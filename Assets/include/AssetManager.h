#pragma once
#include <TaskScheduler.h>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace JLib {

// Generational handle to a slot in an AssetManager<T>. A handle from a slot that was Unload()'d
// can never accidentally validate against a different asset that later reused the same index
// (the classic ABA problem with plain indices) -- see AssetManager::generation's comment.
template<typename T>
struct AssetHandle {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    bool IsValid() const { return index != UINT32_MAX; }
    bool operator==(const AssetHandle& other) const {
        return index == other.index && generation == other.generation;
    }
};

// Generic, format-agnostic handle-based asset cache with optional async loading. Deliberately
// knows NOTHING about what T is or how to load it -- every caller supplies its own loader
// function (Renderer's texture loader uses DirectXTex + records a GPU upload; Sound's audio
// loader uses miniaudio's ma_decoder; a future model/level loader would supply its own). This
// class only owns the two things that are genuinely the same problem regardless of asset type:
// generational-handle bookkeeping (so a stale handle can never resolve to the wrong asset) and,
// for LoadAsync, dispatching the loader onto a JLib::TaskScheduler worker so the calling thread
// never blocks on file I/O/decoding.
//
// Thread safety: every public method is safe to call from any thread. The loader function itself
// runs OUTSIDE the internal lock (on the calling thread for Load(), on a pool worker for
// LoadAsync()) so a slow load never blocks unrelated Load()/Resolve() calls on this same
// manager -- only the brief slot-bookkeeping steps around it are locked.
//
// Lifetime: the AssetManager instance must outlive any LoadAsync() call still in flight -- the
// worker task captures `this` and writes back into the slot on completion.
template<typename T>
class AssetManager {
public:
    enum class LoadState : uint8_t { Loading, Ready, Failed };
    using Loader = std::function<bool(T&)>; // returns false on load failure

    // Synchronous load (or cache hit by key) -- loaderFn runs on the CALLING thread; the handle
    // is fully Ready or Failed by the time this returns. An invalid handle (!IsValid()) means
    // loaderFn returned false; a valid handle whose LoadState() is Failed can happen if a
    // concurrent LoadAsync() for the same key is still in flight when this is called (see
    // LoadState()).
    AssetHandle<T> Load(const std::string& key, Loader loaderFn) {
        std::unique_lock<std::mutex> lock(m_Mutex);
        AssetHandle<T> cached;
        if (TryGetCachedLocked(key, cached)) return cached;

        AssetHandle<T> handle = AllocateSlotLocked(key);
        Slot& slot = m_Slots[handle.index];
        lock.unlock();
        bool ok = loaderFn(slot.data);
        lock.lock();

        slot.state.store(ok ? LoadState::Ready : LoadState::Failed, std::memory_order_release);
        if (!ok) {
            m_KeyToHandle.erase(key); // don't let a failed load poison future Load() calls for this key
            return AssetHandle<T>{};
        }
        return handle;
    }

    // Asynchronous load (or cache hit by key) -- returns a handle immediately; loaderFn runs on
    // a JLib::TaskScheduler worker. Check LoadState(handle) before Resolve()'ing -- it starts as
    // Loading and transitions to Ready/Failed once the worker finishes. Requires
    // JLib::TaskScheduler::Init() to have already been called.
    AssetHandle<T> LoadAsync(const std::string& key, Loader loaderFn) {
        std::unique_lock<std::mutex> lock(m_Mutex);
        AssetHandle<T> cached;
        if (TryGetCachedLocked(key, cached)) return cached;

        AssetHandle<T> handle = AllocateSlotLocked(key);
        lock.unlock();

        // fastJob (the CreateTask default) is correct here: loaderFn is expected to be
        // synchronous, blocking work (file I/O/decode) that never calls WaitOnEvent*/suspends --
        // exactly the fastJob contract, so it runs inline on whichever worker claims it with no
        // fiber overhead.
        JLib::TaskScheduler::Instance().Push([this, handle, key, loaderFn]() {
            Slot& slot = m_Slots[handle.index]; // stable address -- see AllocateSlotLocked's comment
            bool ok = loaderFn(slot.data);
            std::lock_guard<std::mutex> lock2(m_Mutex);
            slot.state.store(ok ? LoadState::Ready : LoadState::Failed, std::memory_order_release);
            if (!ok) m_KeyToHandle.erase(key);
            });

        return handle;
    }

    // Reserves a slot (or returns the existing handle for `key` if already cached/reserved)
    // WITHOUT loading anything -- the slot starts Loading and stays that way until a later
    // Complete() call. For assets whose load has a phase that can't run inside a loader
    // closure -- e.g. Renderer's textures, where decoding can happen on any thread but the GPU
    // upload must happen on whichever thread owns that frame's command list. The caller gets a
    // real, usable handle immediately (safe to store in a BatchItem right away) while the actual
    // work happens later, however many steps that takes.
    AssetHandle<T> Reserve(const std::string& key) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        AssetHandle<T> cached;
        if (TryGetCachedLocked(key, cached)) return cached;
        return AllocateSlotLocked(key);
    }

    // Fills in a slot reserved by Reserve() (or created by Load/LoadAsync, though those normally
    // complete themselves). ok=false marks the slot Failed and evicts it from the key cache, same
    // as a failed Load()/LoadAsync(). No-ops (does NOT overwrite) if handle is stale/invalid --
    // e.g. Unload() was called on it before the async work finished.
    void Complete(AssetHandle<T> handle, T value, bool ok) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!IsValidIndexLocked(handle) || m_Slots[handle.index].generation != handle.generation) return;
        Slot& slot = m_Slots[handle.index];
        if (ok) slot.data = std::move(value);
        slot.state.store(ok ? LoadState::Ready : LoadState::Failed, std::memory_order_release);
        if (!ok) EraseKeyForHandleLocked(handle);
    }

    // Returns the handle for `key` WITHOUT triggering a load -- an invalid handle means "never
    // loaded" (or already Unload()'d). Useful when a caller wants "give me the handle if it's
    // already cached, otherwise I'll decide myself whether to load it."
    AssetHandle<T> TryGet(const std::string& key) const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        AssetHandle<T> cached;
        return TryGetCachedLocked(key, cached) ? cached : AssetHandle<T>{};
    }

    LoadState GetLoadState(AssetHandle<T> handle) const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!IsValidIndexLocked(handle)) return LoadState::Failed;
        return m_Slots[handle.index].state.load(std::memory_order_acquire);
    }

    // Only meaningful once GetLoadState(handle) == Ready. Resolving a Loading/Failed/invalid/
    // stale handle throws rather than silently handing back an incomplete or reused T -- the
    // same "fail loudly, not with a stale-memory read" philosophy as Renderer's
    // ResourceManager::Resolve() (a bug that shipped silently once before).
    T& Resolve(AssetHandle<T> handle) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!IsValidLocked(handle)) {
            throw std::runtime_error("AssetManager::Resolve called with an invalid/stale/"
                "not-yet-loaded AssetHandle -- check GetLoadState() first.");
        }
        return m_Slots[handle.index].data;
    }
    // const overload -- for a caller (like Renderer's ResourceManager::Resolve) whose own
    // Resolve() is itself const. m_Mutex is mutable, so locking here is fine.
    const T& Resolve(AssetHandle<T> handle) const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!IsValidLocked(handle)) {
            throw std::runtime_error("AssetManager::Resolve called with an invalid/stale/"
                "not-yet-loaded AssetHandle -- check GetLoadState() first.");
        }
        return m_Slots[handle.index].data;
    }

    // Frees the slot and invalidates every handle referring to it (generation bump). Safe to
    // call with an already-invalid/stale handle -- it's just checked and ignored. Does NOT wait
    // for an in-flight LoadAsync() to finish -- calling Unload() while that load is still
    // running leaves the worker task to write into a slot nobody holds a valid handle to
    // anymore (harmless: T's memory stays alive since m_Slots only ever grows, never shrinks/
    // reallocates-away a live index -- see AllocateSlotLocked).
    void Unload(AssetHandle<T> handle) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!IsValidIndexLocked(handle)) return;
        if (m_Slots[handle.index].generation != handle.generation) return;
        m_Slots[handle.index].generation++;
        m_Slots[handle.index].occupied = false;
    }

private:
    struct Slot {
        T data{};
        std::string key; // so Complete()'s failure path can evict this slot's cache entry
        std::atomic<LoadState> state{ LoadState::Loading };
        uint32_t generation = 0;
        bool occupied = false;
    };
    void EraseKeyForHandleLocked(AssetHandle<T> handle) {
        m_KeyToHandle.erase(m_Slots[handle.index].key);
    }

    bool IsValidIndexLocked(AssetHandle<T> handle) const {
        return handle.index < m_Slots.size() && m_Slots[handle.index].occupied;
    }
    bool IsValidLocked(AssetHandle<T> handle) const {
        return IsValidIndexLocked(handle)
            && m_Slots[handle.index].generation == handle.generation
            && m_Slots[handle.index].state.load(std::memory_order_acquire) == LoadState::Ready;
    }
    bool TryGetCachedLocked(const std::string& key, AssetHandle<T>& out) const {
        auto it = m_KeyToHandle.find(key);
        if (it == m_KeyToHandle.end()) return false;
        const AssetHandle<T>& h = it->second;
        if (h.index >= m_Slots.size() || m_Slots[h.index].generation != h.generation) return false;
        out = h;
        return true;
    }
    // m_Slots is a deque, not a vector: growing it NEVER moves/reallocates existing elements
    // (unlike vector, which would require Slot -- holding a std::atomic -- to be move-
    // constructible). An occupied slot is never removed, only marked !occupied and generation-
    // bumped by Unload(), so a Slot's address, once handed to an in-flight LoadAsync() lambda by
    // index, stays valid for the manager's whole lifetime. The lambda re-reads
    // m_Slots[handle.index] itself rather than caching a Slot*, so this holds regardless.
    AssetHandle<T> AllocateSlotLocked(const std::string& key) {
        for (size_t i = 0; i < m_Slots.size(); ++i) {
            if (!m_Slots[i].occupied) {
                Slot& slot = m_Slots[i];
                slot.data = T{};
                slot.key = key;
                slot.state.store(LoadState::Loading, std::memory_order_relaxed);
                slot.occupied = true;
                AssetHandle<T> handle{ (uint32_t)i, slot.generation };
                m_KeyToHandle[key] = handle;
                return handle;
            }
        }
        m_Slots.emplace_back();
        Slot& slot = m_Slots.back();
        slot.key = key;
        slot.occupied = true;
        AssetHandle<T> handle{ (uint32_t)(m_Slots.size() - 1), slot.generation };
        m_KeyToHandle[key] = handle;
        return handle;
    }

    mutable std::mutex m_Mutex;
    std::deque<Slot> m_Slots;
    std::unordered_map<std::string, AssetHandle<T>> m_KeyToHandle;
};

} // namespace JLib
