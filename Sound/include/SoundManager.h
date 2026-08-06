#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <AssetManager.h>
#include "../include/miniaudio.h"

// winmm.h/Windows.h #defines PlaySound as PlaySoundA/PlaySoundW -- if a consumer includes
// Windows.h before this header (or, like SoundManager.cpp, after it), that macro silently
// mangles every "PlaySound" token below into "PlaySoundW", breaking declaration/definition
// matching. #undef it defensively; harmless if Windows.h was never included in this TU.
#ifdef PlaySound
#undef PlaySound
#endif
namespace JLib {
    // Generational handle to a playing voice -- index into SoundManager's slot array plus a
    // generation counter, so a handle from a voice that already finished/was stopped can't
    // accidentally refer to a DIFFERENT voice that later reused the same slot (classic ABA problem
    // with plain indices). IsValid() only means "was constructed with a real index," not "still
    // playing" -- use SoundManager::IsPlaying(handle) for that.
    struct SoundHandle {
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;
        bool IsValid() const { return index != UINT32_MAX; }
    };

    // Mixing is DEMAND-DRIVEN POOL WORK, not a dedicated thread. (The previous design parked a whole
    // pinned worker in a forever-MixLoop at TIME_CRITICAL with a 1000Hz Sleep(1) backoff -- a full
    // core spent on ~1% duty-cycle work, and the loudest self-owned waker in the process. The
    // architecture that mattered -- decode/mix OFF the real-time thread, behind a ring buffer -- is
    // kept; only the tenancy changed: the mixer deserved a task, not a core.)
    //
    //   1. miniaudio's WASAPI device thread (spun up internally by ma_device_start(); the ONE foreign
    //      thread audio contributes -- event-driven, ~100 wakes/s, so it needs no reserved core).
    //      Its data callback does the absolute minimum: drain the ring buffer into the WASAPI buffer,
    //      then, if the ring has >= one chunk of space and no refill is already in flight
    //      (m_RefillInFlight exchange-guard), push ONE hiPri fastJob that runs RefillRing(). Pushing
    //      from a foreign thread is fine -- CreateTask/Push are any-thread-safe (MPSC inboxes), and
    //      the same-address-space cost is a slab alloc + queue push, microseconds.
    //
    //   2. RefillRing() -- the actual decode/mix work, as an ordinary pool task. Tops the ring all
    //      the way up (usually one WASAPI period's worth, ~512 frames) and exits; ~100 short tasks/s
    //      replace the old permanent worker. hiPri so it jumps queues under load; the ~170ms ring is
    //      the real underrun protection (the pool would have to be saturated for that long before
    //      audio starves -- and the callback underruns to SILENCE, never garbage).
    //
    // Latency note (unchanged from the old design): a freshly-started voice mixes in behind whatever
    // is already in the ring, so a new sound can take up to kRingBufferFrames (~170ms) to become
    // audible. Fine for music/ambience; if tight SFX sync is ever needed, shrink the ring or mix
    // one-shots into a shorter side-channel -- don't move mixing back into the callback.
    //
    // Shutdown ordering: set m_ShouldStop (any running refill bails at its next chunk) -> uninit the
    // device (stops the callback source, so no NEW refills get pushed) -> wait for m_RefillInFlight
    // to clear (a queued-but-unstarted task still runs and self-clears; workers must therefore still
    // be alive -- Shutdown() before TaskScheduler Join, same contract as the old design) -> teardown.
    class SoundManager
    {
    public:
        ~SoundManager();

        // Must be called after JLib::TaskScheduler::Init() (refill tasks run on the pool). No core
        // parameter anymore -- mixing is demand-driven pool work, nothing is pinned.
        bool Initialize();
        // Stops refills and tears down the WASAPI device/context. Must run while the scheduler is
        // still alive (before Join) -- see the shutdown-ordering note above. Safe to call even if
        // Initialize() failed partway through.
        void Shutdown();

        // Fire-and-forget one-shot playback (sound effects) -- decodes whatever miniaudio's built-in
        // decoders support (WAV/MP3/FLAC) via ma_decoder, converted to the device's format/channels/
        // sample rate at load time so the mix loop never needs to resample per-voice. The file is
        // opened/probed on the CALLING thread (real disk I/O, not real-time-safe) -- only the fully-
        // initialized decoder gets handed to the mix thread. Returns an invalid handle
        // (!handle.IsValid()) if the file couldn't be opened/decoded (bad path, unsupported format).
        // The handle is still usable to Stop()/SetVolume() after a one-shot finishes on its own --
        // those calls just silently no-op once IsPlaying() would return false.
        SoundHandle PlaySound(const char* filePath, float volume = 1.0f);
        // Same as PlaySound, but loops forever (seeks back to frame 0 on EOF) -- for music/ambience.
        SoundHandle PlayLoop(const char* filePath, float volume = 1.0f);
        // Stops and frees a voice immediately (no fade). Safe to call with an already-invalid/
        // already-stopped/stale handle -- it's just checked and ignored.
        void Stop(SoundHandle handle);
        // Safe to call every frame from game logic (e.g. music ducking, distance attenuation) --
        // just a locked float write, no reallocation.
        void SetVolume(SoundHandle handle, float volume);
        // False for an invalid handle, a stopped/finished voice, or a stale handle whose slot got
        // reused by a different voice since.
        bool IsPlaying(SoundHandle handle) const;
        // True if ANY voice is currently active, no handle needed -- for "is something already
        // playing" checks that don't care which specific sound (e.g. don't stack a second music
        // track on top of whatever's already going). Distinct from IsPlaying(SoundHandle): that
        // answers "is THIS specific voice still alive," this answers "is the mixer doing anything
        // at all right now."
        bool IsAnythingPlaying() const;

    private:
        SoundHandle PlayFile(const char* filePath, float volume, bool loop);
        // Caller must hold m_VoicesMutex.
        bool IsValidLocked(SoundHandle handle) const;

        // The mixing work: tops the ring buffer up to full, then returns. Runs as a hiPri fastJob on
        // whatever pool worker picks it up (also called synchronously once in Initialize to prefill).
        // Never suspends (fastJob contract). Clears m_RefillInFlight on exit.
        void RefillRing();

        // miniaudio's device data callback -- runs on miniaudio's OWN thread, not the JLib pool.
        // Drains m_RingBuffer into pOutput (underrun = silence, never garbage) and kicks a RefillRing
        // task when the ring has room -- the callback is the DEMAND signal that drives all mixing.
        static void DataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

        ma_context m_Context{};
        ma_device m_Device{};
        ma_pcm_rb m_RingBuffer{};
        std::atomic<bool> m_ShouldStop{ false }; 
        bool m_ContextInitialized = false;
        bool m_DeviceInitialized = false;
        bool m_RingBufferInitialized = false;

        // True while a RefillRing task is queued or running -- the DataCallback's exchange-guard, so
        // at most ONE refill is ever in flight (no task pileup if the pool lags a few periods).
        std::atomic<bool> m_RefillInFlight{ false };

        // One playing sound. Heap-allocated (via unique_ptr) so growing m_VoiceSlots never
        // invalidates a decoder's address, even though every access still goes through
        // m_VoicesMutex regardless.
        struct Voice {
            ma_decoder decoder{};
            float volume = 1.0f;
            bool loop = false;
            bool decoderInitialized = false;
            ~Voice() { if (decoderInitialized) ma_decoder_uninit(&decoder); }
        };
        // nullptr voice = empty/free slot. generation increments every time a slot's occupant is
        // removed (finished naturally or Stop()'d) -- NOT when a slot is (re)allocated -- so a
        // SoundHandle captured for one voice can never validate against a different voice that
        // later reused the same index (see SoundHandle's comment).
        struct VoiceSlot {
            std::unique_ptr<Voice> voice;
            uint32_t generation = 0;
        };
        // Guards m_VoiceSlots against PlaySound()/PlayLoop()/Stop()/SetVolume() (called from
        // arbitrary game-logic threads) racing with MixLoop()'s read/mix/erase-finished pass. Held
        // only for the duration of a slot lookup or one chunk's worth of mixing -- short enough not
        // to meaningfully threaten the mix thread's real-time budget. mutable so IsPlaying() can
        // stay const.
        mutable std::mutex m_VoicesMutex;
        std::vector<VoiceSlot> m_VoiceSlots;

        // Shared cache of raw file bytes, keyed by path -- NOT per-voice decoders. Two simultaneous
        // PlaySound() calls for the SAME file (e.g. two gunshots) need INDEPENDENT decode positions,
        // so caching a stateful ma_decoder per key would be wrong (the second play would silently
        // fight the first over one shared read cursor). Caching the raw bytes instead is exactly
        // right: they're genuinely shareable read-only data. Each PlayFile() call decodes its OWN
        // ma_decoder via ma_decoder_init_memory() over this shared buffer -- repeated plays of the
        // same file skip disk I/O entirely after the first load, and LoadAsync() (unused today, but
        // available) lets a caller prefetch a level's sounds ahead of time.
        //
        // Lifetime note: ma_decoder_init_memory does NOT copy pData -- it just stores the pointer
        // (see miniaudio.h) -- so every live Voice decoder holds a raw, unowned pointer into this
        // cache's storage for as long as it plays. This is safe ONLY because: (1) AssetManager's
        // slots live in a deque (stable addresses, never moved/reallocated -- see AssetManager.h),
        // and (2) file-byte assets are never Unload()'d here, so a slot's bytes, once loaded, live
        // for m_FileBytes' entire lifetime (which itself must outlive every Voice -- true here since
        // it's a SoundManager member destructed after Shutdown() has already stopped the mix thread
        // and cleared every Voice).
        AssetManager<std::vector<uint8_t>> m_FileBytes;

        static constexpr ma_uint32 kSampleRate = 48000;
        static constexpr ma_uint32 kChannels = 2;
        static constexpr ma_format kFormat = ma_format_f32;
        // ~170ms of headroom at 48kHz stereo float -- comfortably more than one WASAPI callback
        // period. This is the underrun budget: the pool would have to be too saturated to run one
        // hiPri microsecond-scale task for THIS long before audio goes silent.
        static constexpr ma_uint32 kRingBufferFrames = 8192;
        // One refill/mix granule; also the callback's "worth pushing a refill" threshold.
        static constexpr ma_uint32 kChunkFrames = 512;
    };
}