#pragma once
#include <unordered_map>
#include <map>
#include <vector>
#include <string>
#include <stdexcept>
#include <mutex>
#include <d3d12.h>
#include <wrl.h>
#include <d3dx12.h>
#include <DirectXMath.h>
#include <AssetManager.h>
#include "TextureHandle.h"   // TextureHandle now lives here (shared with Mesh/Material -- see that header)
#include "Mesh.h"
#include "Vertex.h"
namespace JLib {
    struct TextureResource {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle; // For creating the view
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle; // For binding to the command list
        uint32_t texID = 0;
    };
    // Lightweight, cheap-to-copy reference to a texture -- an INDEX into ResourceManager's own
    // storage, never a pointer. Client code (BatchItem, ParticleEffectPool, etc.) carries this
    // around by value; only code that actually needs to bind/read the texture calls Resolve().
    // TextureHandle moved to TextureHandle.h (included above) so Mesh/Material can reference it
    // without a circular include back through ResourceManager.
    struct MeshResource {
        Mesh mesh; // Your existing Mesh struct
        uint32_t meshID = 0;
    };
    // A region within one shared atlas texture -- {uvOffset, uvScale} match BatchItem's own
    // fields exactly (see Renderer2D.h), so a lookup here can be assigned straight into
    // item.uvOffset/item.uvScale with no conversion.
    struct AtlasRegion {
        DirectX::XMFLOAT2 uvOffset = { 0.0f, 0.0f };
        DirectX::XMFLOAT2 uvScale = { 1.0f, 1.0f };
    };
    // One packed atlas texture (built offline by the AtlasPacker tool -- see its header comment)
    // plus the named region map parsed from its sidecar text file. Lives in AssetManager<T> like
    // every other resource here instead of its own bespoke class -- LoadAtlas() below is cached
    // by sidecar path exactly the same way LoadTexture() is cached by filename, so loading the
    // same atlas twice is a free cache hit, not a re-parse + re-decode.
    struct AtlasResource {
        TextureHandle texture;
        std::unordered_map<std::string, AtlasRegion> regions;
    };
    // One BMFont glyph -- x/y/width/height are its pixel rect within the font's atlas texture
    // (converted to uvOffset/uvScale at draw time using FontResource::scaleW/scaleH, same as
    // AtlasRegion does at load time -- kept raw here instead since width/height are ALSO used
    // directly as this glyph's on-screen size, not just for the UV rect).
    struct Glyph {
        float x = 0, y = 0, width = 0, height = 0;
        float xoffset = 0, yoffset = 0, xadvance = 0;
    };
    // A loaded BMFont (.fnt + atlas texture pair) -- see ResourceManager::LoadFont. Font (in
    // Font.h) is a thin drawing/layout wrapper around a handle to one of these; this struct owns
    // the actual glyph/kerning/texture data, cached by AssetManager<T> like every other resource
    // here instead of living directly on Font.
    struct FontResource {
        std::unordered_map<int, Glyph> glyphs;
        std::map<std::pair<int, int>, float> kerning;
        TextureHandle texture;
        float scaleW = 1.0f, scaleH = 1.0f;
        float lineHeight = 0.0f;
    };
    // Plain CPU-side decode result -- deliberately holds no DirectXTex type (ScratchImage etc.),
    // so DirectXTex.h stays confined to ResourceManager.cpp (see that file's comment) even though
    // this struct has to be a complete type here for AssetManager<DecodedImage> to work as a
    // member. Everything the GPU-upload step needs, extracted from the ScratchImage right after
    // decoding, on whatever thread did that decode.
    struct DecodedImage {
        std::vector<uint8_t> pixels;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT64 rowPitch = 0;
    };
    class ResourceManager {
    public:
        ResourceManager(ID3D12Device* device, ID3D12DescriptorHeap* sharedSrvHeap);
        void SetSrvHeap(ID3D12DescriptorHeap* heap);
        // Loads a texture (or returns the cached handle). The upload is RECORDED into cmdList --
        // the caller must Execute that list and wait for the GPU before the texture is usable.
        // A manager with only a device cannot upload anything to the GPU; it needs a command
        // list to record the CPU->GPU copy. That missing piece is what broke the renderer.
        TextureHandle LoadTexture(const std::wstring& filename, ID3D12GraphicsCommandList* cmdList, bool srgb = true);
        // Reserves a TextureHandle immediately (safe to assign to BatchItem::tex/submit right
        // away) and decodes the file on a JLib::TaskScheduler worker instead of blocking the
        // calling thread. The GPU upload itself CAN'T run on that worker (it needs a command
        // list belonging to whichever thread is recording the current frame), so it happens
        // later -- call PumpAsyncUploads() once per frame (wherever a command list is available)
        // to actually finish any decodes that completed since the last call. IMPORTANT: unlike a
        // fully-loaded handle, Resolve() on this handle THROWS (same as an invalid handle always
        // has) until the upload actually completes -- there's no free, GPU-safe way to hand back
        // a placeholder without binding an uninitialized descriptor-heap slot, which is a real
        // correctness hazard, not just an inconvenience. Check IsTextureReady(handle) before
        // submitting/drawing a BatchItem that uses a LoadTextureAsync() handle, and skip (or
        // substitute your own already-loaded placeholder, e.g. CreateSolidColorTexture's) until
        // it returns true.
        TextureHandle LoadTextureAsync(const std::wstring& filename, bool srgb = true);
        // Same async decode+upload machinery as LoadTextureAsync, but the source is an ALREADY-IN-MEMORY
        // compressed image blob (PNG/JPEG) rather than a file -- for glTF/GLB textures embedded in the
        // BIN chunk (see ModelLoader). cacheKey is a caller-supplied unique string (e.g.
        // "models\\CesiumMan.glb#img0") that plays the role the filename does for caching/dedup. The
        // bytes are COPIED internally, so the caller may free the glTF buffer immediately after. Same
        // deferred contract: check IsTextureReady() before drawing with the returned handle.
        TextureHandle LoadTextureFromMemoryAsync(const std::string& cacheKey, const uint8_t* compressed, size_t size, bool srgb = true);
        // Call once per frame, wherever a command list is available (matches ReleaseUploadBuffers'
        // usage pattern) -- finishes the GPU-upload half of any LoadTextureAsync() decodes that
        // completed since the last call. Safe to call even if nothing is pending.
        void PumpAsyncUploads(ID3D12GraphicsCommandList* cmdList);
        // True once a LoadTextureAsync() (or ordinary LoadTexture()) handle's GPU upload has
        // actually completed and Resolve() is safe to call.
        bool IsTextureReady(TextureHandle handle) const {
            return m_TextureAssets.GetLoadState(JLib::AssetHandle<TextureResource>{ handle.id, 0 })
                == JLib::AssetManager<TextureResource>::LoadState::Ready;
        }
        // Invalid handle (IsValid() == false) if filename hasn't been loaded.
        TextureHandle GetTexture(const std::wstring& filename);
        // Same upload path as LoadTexture, minus the WIC file load -- a 1x1 RGBA texture of the
        // given color. Cached under a synthetic key so repeated calls with the same color are free
        // after the first. Used as the particle system's untextured-pool fallback (draws flat-
        // colored quads through the SAME textured draw PSO, no separate no-texture shader variant).
        TextureHandle CreateSolidColorTexture(ID3D12GraphicsCommandList* cmdList,
            uint8_t r = 255, uint8_t g = 255, uint8_t b = 255, uint8_t a = 255);
        // Resolves a handle to the real GPU resource data. Only code that actually needs to bind
        // the texture (Renderer2D::FlushBatchTask, RendererCore::RecordParticleDraw) should call
        // this -- everything else should just carry the handle around by value.
        const TextureResource& Resolve(TextureHandle handle) const {
            // An invalid/out-of-range handle used to be a silent out-of-bounds vector read (a
            // BatchItem submitted before its .tex was ever assigned, e.g.) -- that's exactly the
            // kind of bug that's easy to submit once and hard to trace back from a raw memory
            // fault. Fail loudly and immediately instead, with a message naming the actual problem.
            // Generation is always 0 here (see TextureHandle's comment on why that's safe) --
            // AssetManager::Resolve still throws its own clear message for an out-of-range/
            // not-yet-loaded index, same failure philosophy as before this used AssetManager.
            return m_TextureAssets.Resolve(JLib::AssetHandle<TextureResource>{ handle.id, 0 });
        }
        // Loads (or returns the cached handle for) an atlas built by the offline AtlasPacker
        // tool -- regionsPath is its sidecar text file, atlasImagePath the packed PNG next to it.
        // Cached by regionsPath, same caching semantics as LoadTexture. The underlying texture
        // itself goes through the ordinary LoadTexture() path (and so is ALSO independently
        // cached/deduplicated by atlasImagePath), so re-using the same atlas image under two
        // different sidecars (unusual, but not prevented) still shares one GPU texture.
        JLib::AssetHandle<AtlasResource> LoadAtlas(const std::wstring& regionsPath,
            const std::wstring& atlasImagePath, ID3D12GraphicsCommandList* cmdList);
        // Throws if handle is invalid/not-yet-loaded, or if key isn't in the atlas's region map --
        // same "fail loudly" philosophy as every other Resolve() in this file.
        const AtlasRegion& GetAtlasRegion(JLib::AssetHandle<AtlasResource> handle, const std::string& key) const {
            return m_Atlases.Resolve(handle).regions.at(key);
        }
        TextureHandle GetAtlasTexture(JLib::AssetHandle<AtlasResource> handle) const {
            return m_Atlases.Resolve(handle).texture;
        }
        // Async counterpart to LoadAtlas -- returns a handle immediately. The sidecar itself is
        // parsed synchronously right here (pure CPU, no GPU/thread-affinity involved, so there's
        // no reason to defer it -- same reasoning LoadFont uses for its own .fnt parsing); only
        // the underlying atlas TEXTURE actually needs to be async, since ITS upload is what has
        // the GPU-thread constraint. Internally just Reserve()s the atlas slot and hands the
        // image off to the EXISTING LoadTextureAsync()/PumpAsyncUploads() machinery -- no new
        // decode path, just a new completion hookup (see m_PendingAtlases). Check
        // IsAtlasReady(handle) before GetAtlasRegion/GetAtlasTexture/Resolve, same as
        // IsTextureReady() gates a LoadTextureAsync() handle.
        JLib::AssetHandle<AtlasResource> LoadAtlasAsync(const std::wstring& regionsPath,
            const std::wstring& atlasImagePath);
        bool IsAtlasReady(JLib::AssetHandle<AtlasResource> handle) const {
            return m_Atlases.GetLoadState(handle) == JLib::AssetManager<AtlasResource>::LoadState::Ready;
        }
        // Loads (or returns the cached handle for) a BMFont .fnt + atlas texture pair, cached by
        // fntPath. Font::Load calls this and caches the returned FontResource's address directly
        // (see Font.h) rather than re-resolving every draw.
        JLib::AssetHandle<FontResource> LoadFont(const std::wstring& fntPath,
            const std::wstring& atlasPath, ID3D12GraphicsCommandList* cmdList);
        const FontResource& ResolveFont(JLib::AssetHandle<FontResource> handle) const {
            return m_Fonts.Resolve(handle);
        }
        // Call AFTER the upload command list has been executed and waited on. Frees the
        // temporary upload buffers (they must stay alive until the GPU finishes the copy).
        void ReleaseUploadBuffers() { m_UploadBuffers.clear(); }
        // Templated on the vertex type so ONE mesh path serves both 2D (Vertex) and 3D (Vertex3D):
        // the type only affects byte size + stride here; Mesh itself is type-agnostic (GPU buffers +
        // a stride-carrying VBV). VertexT deduces from the pointer, so existing CreateMesh(verts, n,
        // ...) calls compile unchanged. All D3D12 work stays in the non-template CreateMeshRaw().
        template <class VertexT>
        Mesh CreateMesh(const VertexT* vertices, uint32_t vCount, const uint32_t* indices, uint32_t iCount) {
            return CreateMeshRaw(vertices, vCount, (uint32_t)sizeof(VertexT), indices, iCount);
        }

        // Reserve one slot in the SHARED shader-visible SRV heap for a resource this manager doesn't
        // own -- Renderer3D's shadow map is the first such case. It has to come from this heap rather
        // than a private one because only one shader-visible heap can be bound at a time; a separate
        // heap would force a rebind mid-command-list. The slot is permanent (no free list): these are
        // engine-lifetime resources, not streamed assets. Returns false if the heap is full.
        bool AllocateSrvSlot(D3D12_CPU_DESCRIPTOR_HANDLE& outCpu, D3D12_GPU_DESCRIPTOR_HANDLE& outGpu) {
            if (m_CurrentDescriptorIndex >= kSrvHeapCapacity) return false;
            const UINT sz = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            outCpu = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_SrvHeap->GetCPUDescriptorHandleForHeapStart(),
                                                   m_CurrentDescriptorIndex, sz);
            outGpu = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_SrvHeap->GetGPUDescriptorHandleForHeapStart(),
                                                   m_CurrentDescriptorIndex, sz);
            ++m_CurrentDescriptorIndex;
            return true;
        }
    private:
        // Matches the NumDescriptors the heap was created with in Renderer2D::Initialize.
        static constexpr UINT kSrvHeapCapacity = 256;
        Mesh CreateMeshRaw(const void* vertices, uint32_t vCount, uint32_t stride,
                           const uint32_t* indices, uint32_t iCount);
        // Shared GPU-upload step for both LoadTexture's synchronous decode and
        // LoadTextureAsync/PumpAsyncUploads' worker-thread decode -- must run on the thread that
        // owns cmdList.
        TextureResource UploadDecodedImage(const DecodedImage& img, ID3D12GraphicsCommandList* cmdList);
        int meshCtr = 0;
        ID3D12Device* m_Device;
        ID3D12DescriptorHeap* m_SrvHeap; // Just a pointer, not a ComPtr
        // Backed by the shared JLib::AssetManager<T> (see AssetManager.h) instead of a hand-rolled
        // vector+unordered_map -- same underlying idea (index-based, append-only, key-cached
        // storage) but the bookkeeping (and, if ever needed, async loading via
        // AssetManager::LoadAsync) is now shared with Sound's asset loading instead of duplicated.
        // TextureHandle stays a plain {id} (unchanged public API for every existing caller --
        // BatchItem::tex, ParticleEffectPool::texture, etc.) and maps to
        // AssetHandle<TextureResource>{id, generation=0}: textures are never Unload()'d here, so
        // the slot's real generation never advances past 0, making that reconstruction always
        // valid. If per-texture unloading is ever added, TextureHandle needs its own generation
        // field too -- reconstructing {id, 0} would then silently validate against a REUSED slot.
        JLib::AssetManager<TextureResource> m_TextureAssets;
        // CPU-side decode cache for LoadTextureAsync() -- separate from m_TextureAssets because
        // decoding (any thread) and GPU upload (must be the frame-recording thread) are two
        // different steps with two different threading constraints. Never Unload()'d, same as
        // m_TextureAssets.
        JLib::AssetManager<DecodedImage> m_DecodedImages;
        // Atlases built by the offline AtlasPacker tool -- see AtlasResource's comment. Never
        // Unload()'d, same as m_TextureAssets/m_DecodedImages.
        JLib::AssetManager<AtlasResource> m_Atlases;
        // Fonts loaded via LoadFont -- see FontResource's comment. Never Unload()'d: Font.h
        // caches a raw pointer into this manager's slot storage for the lifetime of the Font
        // object, which is only safe because a slot's address never changes and is never
        // invalidated/reused (see AssetManager<T>::m_Slots' comment) as long as Unload() is never
        // called on it.
        JLib::AssetManager<FontResource> m_Fonts;
        // One entry per LoadTextureAsync() call whose GPU upload hasn't been finished by
        // PumpAsyncUploads() yet. Guarded by its own mutex -- LoadTextureAsync() can be called
        // from any thread, PumpAsyncUploads() runs on whichever thread owns the frame's cmdList.
        struct PendingUpload {
            TextureHandle textureHandle;
            JLib::AssetHandle<DecodedImage> decodeHandle;
        };
        std::mutex m_PendingUploadsMutex;
        std::vector<PendingUpload> m_PendingUploads;
        // One entry per LoadAtlasAsync() call whose underlying texture hasn't finished its GPU
        // upload yet. regions is already fully parsed (see LoadAtlasAsync's comment) -- it just
        // travels alongside textureHandle until PumpAsyncUploads() can Complete() the atlas slot
        // in one shot, same "nothing partially visible before Ready" contract LoadTextureAsync's
        // own DecodedImage/TextureResource split already uses.
        struct PendingAtlas {
            JLib::AssetHandle<AtlasResource> atlasHandle;
            TextureHandle textureHandle;
            std::unordered_map<std::string, AtlasRegion> regions;
        };
        std::mutex m_PendingAtlasesMutex;
        std::vector<PendingAtlas> m_PendingAtlases;
        std::vector<MeshResource> m_Meshes;
        // Upload heaps are read by the GPU during the copy, so they must outlive the recorded
        // command list until it has finished executing. Held here, freed by ReleaseUploadBuffers.
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_UploadBuffers;
        UINT m_CurrentDescriptorIndex = 0;
    };
};
