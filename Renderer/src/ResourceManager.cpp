// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#include "../include/ResourceManager.h"
#include "../include/Helpers.h"
#include <DirectXTex.h>   // ScratchImage/LoadFromWICFile (Renderer3D.cpp also uses it, for the IBL HDR bake)

// d3dx12.h's UpdateSubresources (used below to upload whole mip chains) calls GetDevice with
// IID_PPV_ARGS, which pulls in IID_ID3D12Device from dxguid.lib. Declaring it here rather than in
// each app's linker settings means the dependency travels inside Renderer.lib -- consumers link the
// renderer and are done, instead of hitting LNK2001 and having to know why.
#pragma comment(lib, "dxguid.lib")

#include <Windows.h>      // WideCharToMultiByte -- narrow keys for AssetManager<T>'s std::string cache
#include <fstream>        // atlas sidecar parsing (LoadAtlas)
#include <cassert>        // SRV heap exhaustion guards below
#include <vector>
#include <cmath>          // std::sqrt (mesh bounding-sphere radius)
using namespace JLib;

// Force a decoded format into the right COLOUR SPACE for how the texture will be used.
//
// This replaces a blanket "strip sRGB from everything", which was wrong in a way that hid itself:
// with no decode on load AND no encode on output, unlit 2D sprites looked correct (the two errors
// cancel), while every lit 3D surface did its lighting maths on gamma-encoded values. Now the RTV
// encodes on write (RendererCore::BackBufferRTVFormat), so textures must decode on read.
//
//   srgb = true  -> COLOUR data: base colour, sprites, emissive. Hardware converts to linear on
//                   sample, and the sRGB back-buffer view converts back on write, so a sprite drawn
//                   1:1 is byte-identical to before -- decode then encode is identity.
//   srgb = false -> NON-colour data: normal maps, metallic-roughness, occlusion. These are numbers
//                   that happen to live in an image. Gamma-decoding a normal map bends every normal;
//                   gamma-decoding roughness makes everything look wetter than authored.
static DXGI_FORMAT ApplyColorSpace(DXGI_FORMAT fmt, bool srgb) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB : DXGI_FORMAT_B8G8R8X8_UNORM;
    default:
        return fmt;   // float/HDR and block-compressed formats carry their own convention
    }
}

// "key=value" / "key=\"value\"" line parsing -- shared by LoadAtlas (AtlasPacker's sidecar) and
// LoadFont (BMFont's .fnt) below, since both are the same plain-text-directive convention.
static float FindField(const std::string& line, const char* key) {
    // Bare line.find(key) used to grab the FIRST substring match anywhere in the line and give up
    // immediately if it wasn't followed by '=' -- which breaks the moment a region/font NAME
    // happens to contain the same letter as a single-character key. E.g. region name="mystery"
    // x=0 y=102 ...: line.find("y") matches the 'y' inside "mystery" (not followed by '='), so
    // FindField(line, "y") returned 0 instead of ever reaching the real "y=102" later in the line
    // -- the mystery ship's parsed region silently became {x=0,y=0,...}, exactly overlapping a
    // DIFFERENT region ("alien_3") that legitimately starts at 0,0. Now: keep scanning forward
    // past any match that isn't a real "key=" token (preceded by start-of-line or whitespace),
    // instead of stopping at the first substring hit anywhere in the line.
    std::string keyEq = std::string(key) + "=";
    size_t searchStart = 0;
    while (true) {
        size_t pos = line.find(keyEq, searchStart);
        if (pos == std::string::npos) return 0.0f;
        if (pos == 0 || line[pos - 1] == ' ') {
            return static_cast<float>(atof(line.c_str() + pos + keyEq.size()));
        }
        searchStart = pos + 1;
    }
}
static std::string FindNameField(const std::string& line) {
    size_t pos = line.find("name=\"");
    if (pos == std::string::npos) return {};
    pos += strlen("name=\"");
    size_t end = line.find('"', pos);
    if (end == std::string::npos) return {};
    return line.substr(pos, end - pos);
}

// AssetManager<T>'s cache key is std::string; texture paths here are std::wstring. A real
// WideCharToMultiByte round-trip (not a naive truncating cast) so non-ASCII paths still work as
// cache keys instead of silently colliding/mojibake-ing.
static std::string NarrowKey(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string narrow(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), narrow.data(), size, nullptr, nullptr);
    return narrow;
}

ResourceManager::ResourceManager(ID3D12Device* device, ID3D12DescriptorHeap* sharedSrvHeap)
    : m_Device(device), m_SrvHeap(sharedSrvHeap) {}
void ResourceManager::SetSrvHeap(ID3D12DescriptorHeap* heap) {
    m_SrvHeap = heap; // If m_SrvHeap is a ComPtr, this will AddRef/Release correctly
}
TextureHandle ResourceManager::LoadTexture(const std::wstring& filename, ID3D12GraphicsCommandList* cmdList, bool srgb) {
    JLib::AssetHandle<TextureResource> assetHandle = m_TextureAssets.Load(NarrowKey(filename),
        [&](TextureResource& tex) -> bool {
    // Load the image (WIC needs COM initialized on this thread -- done in wWinMain).
    DirectX::ScratchImage image;
    ThrowIfFailed(DirectX::LoadFromWICFile(filename.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image));

    // Build the mip chain BEFORE the sRGB strip below, deliberately: DirectXTex reads sRGB-ness off
    // the source format and downsamples in linear space when it's set, which is the only correct way
    // to filter colour data. Stripping first would make every mip subtly wrong.
    DirectX::ScratchImage mipChain;
    const bool haveMips = SUCCEEDED(DirectX::GenerateMipMaps(
        *image.GetImages(), DirectX::TEX_FILTER_DEFAULT, 0, mipChain, false));
    const DirectX::ScratchImage& srcImages = haveMips ? mipChain : image;
    const auto& meta = srcImages.GetMetadata();

    // Describe the GPU texture. DepthOrArraySize and MipLevels MUST be set (>=1) -- leaving
    // them 0 made CreateCommittedResource fail with E_INVALIDARG and (with the ignored
    // HRESULT) produced a null resource that the GPU later faulted on.
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = meta.width;
    texDesc.Height = (UINT)meta.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = (UINT16)meta.mipLevels;
    // Colour space by USAGE, not blanket-stripped. See ApplyColorSpace at the top of this file: the
    // back-buffer view now encodes on write, so colour textures must decode on read or everything
    // renders washed out. Data textures (normal / metallic-roughness / occlusion) stay linear.
    texDesc.Format = ApplyColorSpace(meta.format, srgb);
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    // Create the DEFAULT-heap texture in COPY_DEST (we're about to copy into it).
    Microsoft::WRL::ComPtr<ID3D12Resource> newResource;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&newResource)));

    // --- Upload: stage every mip in one UPLOAD buffer, then let UpdateSubresources do the
    //     footprint maths and issue a CopyTextureRegion per level. (This was a hand-rolled
    //     single-subresource copy before mips existed; UpdateSubresources handles a chain of
    //     any length, so it replaces it rather than being looped around it.) ---
    std::vector<D3D12_SUBRESOURCE_DATA> subs(meta.mipLevels);
    for (size_t i = 0; i < meta.mipLevels; ++i) {
        const DirectX::Image* mip = srcImages.GetImage(i, 0, 0);
        subs[i].pData      = mip->pixels;
        subs[i].RowPitch   = (LONG_PTR)mip->rowPitch;
        subs[i].SlicePitch = (LONG_PTR)mip->slicePitch;
    }

    const UINT64 requiredSize = GetRequiredIntermediateSize(newResource.Get(), 0, (UINT)subs.size());
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    CD3DX12_HEAP_PROPERTIES uploadProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredSize);
    ThrowIfFailed(m_Device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)));

    UpdateSubresources(cmdList, newResource.Get(), uploadBuffer.Get(), 0, 0,
                       (UINT)subs.size(), subs.data());

    CD3DX12_RESOURCE_BARRIER toSRV = CD3DX12_RESOURCE_BARRIER::Transition(
        newResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toSRV);

    // Keep the upload buffer alive until the caller has executed+waited on cmdList.
    m_UploadBuffers.push_back(uploadBuffer);

    // BOUNDS CHECK. m_CurrentDescriptorIndex is a bump allocator into a 256-slot heap and slots are
    // NEVER FREED (see AllocateSrvSlot's comment: "engine-lifetime resources, not streamed assets").
    // Three texture paths used to increment it with no check at all, so once a session loaded more
    // than kSrvHeapCapacity textures - which repeated scene loads do, since nothing evicts - they
    // wrote descriptors PAST THE END OF THE HEAP. The debug layer catches that as an error, so it
    // presented as a Debug-only crash on "new game" while Release silently tolerated it.
    if (m_CurrentDescriptorIndex >= kSrvHeapCapacity) {
        assert(false && "ResourceManager SRV heap exhausted (256 slots, never freed). Textures are "
                        "not evicted between scenes - see AllocateSrvSlot.");
        return false;
    }

    // 2. Setup the SRV
    UINT descriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(
        m_SrvHeap->GetCPUDescriptorHandleForHeapStart(),
        m_CurrentDescriptorIndex,
        descriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = (UINT)meta.mipLevels;   // must expose the whole chain, or sampling
                                                          // silently clamps back to the top mip

    // Create the view using newResource
    m_Device->CreateShaderResourceView(newResource.Get(), &srvDesc, cpuHandle);

    // 3. Fill in the TextureResource AssetManager<T>::Load() gave us -- this IS the slot's
    // storage, not a temporary that then gets copied/stored elsewhere.
    tex.resource = newResource;
    tex.gpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_SrvHeap->GetGPUDescriptorHandleForHeapStart(),
        m_CurrentDescriptorIndex,
        descriptorSize);

    m_CurrentDescriptorIndex++;
    return true;
        });
    // AssetManager<T>::Load() is synchronous, so assetHandle is fully Ready (or invalid, on
    // failure) by the time we get here -- generation is always 0 (see TextureHandle's comment).
    return TextureHandle{ assetHandle.index };
}

// Shared by LoadAtlas and LoadAtlasAsync -- parsing the sidecar is pure CPU work with no
// GPU/thread-affinity constraint either path needs to worry about (unlike the texture upload
// itself). Returns false (regions left empty) if the file can't be opened or has no region
// lines. Sidecar format matches AtlasPacker's output exactly:
//   atlas width=.. height=..
//   region name="key" x=.. y=.. width=.. height=..
static bool ParseAtlasRegions(const std::wstring& regionsPath,
    std::unordered_map<std::string, AtlasRegion>& outRegions) {
    std::ifstream file(regionsPath);
    if (!file) return false;

    float atlasWidth = 1.0f, atlasHeight = 1.0f;
    // Two passes, not one: the "atlas width/height" directive isn't guaranteed to appear
    // before every "region" line, so raw pixel rects are collected first and only converted
    // to uvOffset/uvScale after the whole file's been read and atlasWidth/atlasHeight are final.
    struct RawRegion { std::string name; float x, y, w, h; };
    std::vector<RawRegion> raw;

    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("atlas", 0) == 0) {
            atlasWidth = FindField(line, "width");
            atlasHeight = FindField(line, "height");
        } else if (line.rfind("region", 0) == 0) {
            std::string name = FindNameField(line);
            if (name.empty()) continue; // malformed line -- skip rather than fail the whole load
            raw.push_back({ name, FindField(line, "x"), FindField(line, "y"),
                FindField(line, "width"), FindField(line, "height") });
        }
    }
    if (atlasWidth <= 0.0f) atlasWidth = 1.0f;
    if (atlasHeight <= 0.0f) atlasHeight = 1.0f;
    if (raw.empty()) return false;

    for (const auto& r : raw) {
        AtlasRegion region;
        region.uvOffset = { r.x / atlasWidth, r.y / atlasHeight };
        region.uvScale = { r.w / atlasWidth, r.h / atlasHeight };
        outRegions[r.name] = region;
    }
    return true;
}

JLib::AssetHandle<AtlasResource> ResourceManager::LoadAtlas(const std::wstring& regionsPath,
    const std::wstring& atlasImagePath, ID3D12GraphicsCommandList* cmdList) {
    return m_Atlases.Load(NarrowKey(regionsPath), [&](AtlasResource& out) -> bool {
        if (!ParseAtlasRegions(regionsPath, out.regions)) return false;
        // Atlas texture -- ordinary LoadTexture(), independently cached by atlasImagePath.
        out.texture = LoadTexture(atlasImagePath, cmdList);
        return out.texture.IsValid();
        });
}

JLib::AssetHandle<AtlasResource> ResourceManager::LoadAtlasAsync(const std::wstring& regionsPath,
    const std::wstring& atlasImagePath) {
    std::string key = NarrowKey(regionsPath);
    JLib::AssetHandle<AtlasResource> existing = m_Atlases.TryGet(key);
    if (existing.IsValid()) return existing;

    std::unordered_map<std::string, AtlasRegion> regions;
    if (!ParseAtlasRegions(regionsPath, regions)) return JLib::AssetHandle<AtlasResource>{};

    JLib::AssetHandle<AtlasResource> handle = m_Atlases.Reserve(key);
    // Hands the actual image off to the SAME decode-on-worker + PumpAsyncUploads machinery
    // ordinary async textures already use -- no new decode path, just a new completion hookup.
    TextureHandle texHandle = LoadTextureAsync(atlasImagePath);

    std::lock_guard<std::mutex> lock(m_PendingAtlasesMutex);
    m_PendingAtlases.push_back({ handle, texHandle, std::move(regions) });
    return handle;
}

JLib::AssetHandle<FontResource> ResourceManager::LoadFont(const std::wstring& fntPath,
    const std::wstring& atlasPath, ID3D12GraphicsCommandList* cmdList) {
    return m_Fonts.Load(NarrowKey(fntPath), [&](FontResource& out) -> bool {
        // .fnt is plain text (AngelCode BMFont text format) -- read and parse line by line.
        // Same parser/format Font::Load used before this was centralized into ResourceManager.
        std::ifstream file(fntPath);
        if (!file) return false;

        std::string line;
        while (std::getline(file, line)) {
            if (line.rfind("common", 0) == 0) {
                out.scaleW = FindField(line, "scaleW");
                out.scaleH = FindField(line, "scaleH");
                out.lineHeight = FindField(line, "lineHeight");
            } else if (line.rfind("char ", 0) == 0) {
                int id = static_cast<int>(FindField(line, "id"));
                Glyph g;
                g.x = FindField(line, "x");
                g.y = FindField(line, "y");
                g.width = FindField(line, "width");
                g.height = FindField(line, "height");
                g.xoffset = FindField(line, "xoffset");
                g.yoffset = FindField(line, "yoffset");
                g.xadvance = FindField(line, "xadvance");
                out.glyphs[id] = g;
            } else if (line.rfind("kerning ", 0) == 0) {
                int first = static_cast<int>(FindField(line, "first"));
                int second = static_cast<int>(FindField(line, "second"));
                float amount = FindField(line, "amount");
                out.kerning[{first, second}] = amount;
            }
        }
        if (out.scaleW <= 0.0f) out.scaleW = 1.0f;
        if (out.scaleH <= 0.0f) out.scaleH = 1.0f;

        // Atlas texture -- ordinary LoadTexture(), independently cached by atlasPath.
        out.texture = LoadTexture(atlasPath, cmdList);
        return out.texture.IsValid();
        });
}

TextureHandle ResourceManager::GetTexture(const std::wstring& filename) {
    JLib::AssetHandle<TextureResource> assetHandle = m_TextureAssets.TryGet(NarrowKey(filename));
    return assetHandle.IsValid() ? TextureHandle{ assetHandle.index } : TextureHandle{};
}

TextureHandle ResourceManager::LoadTextureAsync(const std::wstring& filename, bool srgb) {
    std::string key = NarrowKey(filename);

    // Already reserved/loading/loaded (whether by a prior LoadTextureAsync, or LoadTexture) --
    // don't kick off a duplicate decode for the same file.
    JLib::AssetHandle<TextureResource> existing = m_TextureAssets.TryGet(key);
    if (existing.IsValid()) {
        return TextureHandle{ existing.index };
    }

    JLib::AssetHandle<TextureResource> textureHandle = m_TextureAssets.Reserve(key);

    // Runs on a JLib::TaskScheduler worker -- decode ONLY, no D3D12 calls (this thread doesn't
    // own any frame's command list). Extracts everything the later GPU-upload step needs into
    // plain fields (DecodedImage), rather than keeping the DirectXTex ScratchImage itself, so
    // DirectXTex.h stays confined to this .cpp.
    JLib::AssetHandle<DecodedImage> decodeHandle = m_DecodedImages.LoadAsync(key,
        [filename, srgb](DecodedImage& img) -> bool {
            DirectX::ScratchImage image;
            if (FAILED(DirectX::LoadFromWICFile(filename.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image))) {
                return false;
            }
            const auto& meta = image.GetMetadata();
            img.width = (UINT)meta.width;
            img.height = (UINT)meta.height;
            img.format = ApplyColorSpace(meta.format, srgb);
            img.rowPitch = image.GetImages()->rowPitch;
            const uint8_t* src = image.GetPixels();
            img.pixels.assign(src, src + (size_t)img.rowPitch * img.height);
            return true;
        });

    {
        std::lock_guard<std::mutex> lock(m_PendingUploadsMutex);
        m_PendingUploads.push_back({ TextureHandle{ textureHandle.index }, decodeHandle });
    }
    return TextureHandle{ textureHandle.index };
}

TextureHandle ResourceManager::LoadTextureFromMemoryAsync(const std::string& cacheKey,
                                                          const uint8_t* compressed, size_t size, bool srgb) {
    // Same slot/dedup semantics as LoadTextureAsync, keyed by the caller's cacheKey instead of a path.
    JLib::AssetHandle<TextureResource> existing = m_TextureAssets.TryGet(cacheKey);
    if (existing.IsValid())
        return TextureHandle{ existing.index };

    JLib::AssetHandle<TextureResource> textureHandle = m_TextureAssets.Reserve(cacheKey);

    // COPY the compressed bytes into the decode task: the glTF buffer they point into is freed by the
    // caller (cgltf_free) as soon as the loader returns, long before this worker runs. Decode from
    // memory (LoadFromWICMemory) instead of a file -- everything downstream (DecodedImage -> upload) is
    // identical to LoadTextureAsync, so it flows through the same m_PendingUploads / PumpAsyncUploads drain.
    std::vector<uint8_t> blob(compressed, compressed + size);
    JLib::AssetHandle<DecodedImage> decodeHandle = m_DecodedImages.LoadAsync(cacheKey,
        [blob = std::move(blob), srgb](DecodedImage& img) -> bool {
            DirectX::ScratchImage image;
            if (FAILED(DirectX::LoadFromWICMemory(blob.data(), blob.size(),
                                                  DirectX::WIC_FLAGS_NONE, nullptr, image))) {
                return false;
            }
            const auto& meta = image.GetMetadata();
            img.width    = (UINT)meta.width;
            img.height   = (UINT)meta.height;
            img.format   = ApplyColorSpace(meta.format, srgb);
            img.rowPitch = image.GetImages()->rowPitch;
            const uint8_t* src = image.GetPixels();
            img.pixels.assign(src, src + (size_t)img.rowPitch * img.height);
            return true;
        });

    {
        std::lock_guard<std::mutex> lock(m_PendingUploadsMutex);
        m_PendingUploads.push_back({ TextureHandle{ textureHandle.index }, decodeHandle });
    }
    return TextureHandle{ textureHandle.index };
}

void ResourceManager::PumpAsyncUploads(ID3D12GraphicsCommandList* cmdList) {
    // Split the pending list under the lock (cheap, just moving handles around), then do the
    // actual GPU work outside it -- PumpAsyncUploads is called from the render thread, which
    // shouldn't hold this mutex any longer than necessary against a LoadTextureAsync() call
    // arriving concurrently from elsewhere.
    std::vector<PendingUpload> ready;
    {
        std::lock_guard<std::mutex> lock(m_PendingUploadsMutex);
        auto it = m_PendingUploads.begin();
        while (it != m_PendingUploads.end()) {
            if (m_DecodedImages.GetLoadState(it->decodeHandle) == JLib::AssetManager<DecodedImage>::LoadState::Loading) {
                ++it; // still decoding -- try again next PumpAsyncUploads() call
            }
            else {
                ready.push_back(*it);
                it = m_PendingUploads.erase(it);
            }
        }
    }

    for (auto& pending : ready) {
        JLib::AssetHandle<TextureResource> assetHandle{ pending.textureHandle.id, 0 };
        if (m_DecodedImages.GetLoadState(pending.decodeHandle) == JLib::AssetManager<DecodedImage>::LoadState::Failed) {
            m_TextureAssets.Complete(assetHandle, TextureResource{}, false);
            continue;
        }
        const DecodedImage& img = m_DecodedImages.Resolve(pending.decodeHandle);
        TextureResource tex = UploadDecodedImage(img, cmdList);
        m_TextureAssets.Complete(assetHandle, tex, true);
    }

    // Atlases queued via LoadAtlasAsync -- their regions are already parsed (see
    // LoadAtlasAsync), just waiting on the underlying texture's upload above (this call or an
    // earlier one). No GPU work of its own; this is pure bookkeeping, but it belongs in the same
    // once-per-frame drain point since that's what actually advances the underlying texture.
    std::vector<PendingAtlas> atlasesReady;
    {
        std::lock_guard<std::mutex> lock(m_PendingAtlasesMutex);
        auto it = m_PendingAtlases.begin();
        while (it != m_PendingAtlases.end()) {
            auto state = m_TextureAssets.GetLoadState(JLib::AssetHandle<TextureResource>{ it->textureHandle.id, 0 });
            if (state == JLib::AssetManager<TextureResource>::LoadState::Loading) {
                ++it; // texture still decoding/uploading -- try again next call
            } else {
                atlasesReady.push_back(std::move(*it));
                it = m_PendingAtlases.erase(it);
            }
        }
    }
    for (auto& pending : atlasesReady) {
        auto state = m_TextureAssets.GetLoadState(JLib::AssetHandle<TextureResource>{ pending.textureHandle.id, 0 });
        bool ok = state == JLib::AssetManager<TextureResource>::LoadState::Ready;
        AtlasResource res;
        res.texture = pending.textureHandle;
        res.regions = std::move(pending.regions);
        m_Atlases.Complete(pending.atlasHandle, std::move(res), ok);
    }
}

TextureResource ResourceManager::UploadDecodedImage(const DecodedImage& img, ID3D12GraphicsCommandList* cmdList) {
    // This is the path model textures take (glTF external URIs and embedded images are decoded on a
    // worker, then land here). It has to build mips too -- doing it only in LoadTexture would have
    // fixed sprites and left every Sponza texture aliasing, which is where the sparkle actually was.
    DirectX::Image top = {};
    top.width      = img.width;
    top.height     = img.height;
    top.format     = img.format;
    top.rowPitch   = (size_t)img.rowPitch;
    top.slicePitch = (size_t)img.rowPitch * img.height;
    top.pixels     = const_cast<uint8_t*>(img.pixels.data());   // DirectXTex reads only

    DirectX::ScratchImage mipChain;
    const bool haveMips = SUCCEEDED(DirectX::GenerateMipMaps(
        top, DirectX::TEX_FILTER_DEFAULT, 0, mipChain, false));
    const UINT mipCount = haveMips ? (UINT)mipChain.GetMetadata().mipLevels : 1u;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = img.width;
    texDesc.Height = img.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = (UINT16)mipCount;
    texDesc.Format = img.format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> newResource;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&newResource)));

    std::vector<D3D12_SUBRESOURCE_DATA> subs(mipCount);
    for (UINT i = 0; i < mipCount; ++i) {
        const DirectX::Image* mip = haveMips ? mipChain.GetImage(i, 0, 0) : &top;
        subs[i].pData      = mip->pixels;
        subs[i].RowPitch   = (LONG_PTR)mip->rowPitch;
        subs[i].SlicePitch = (LONG_PTR)mip->slicePitch;
    }

    const UINT64 requiredSize = GetRequiredIntermediateSize(newResource.Get(), 0, mipCount);
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    CD3DX12_HEAP_PROPERTIES uploadProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredSize);
    ThrowIfFailed(m_Device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)));

    UpdateSubresources(cmdList, newResource.Get(), uploadBuffer.Get(), 0, 0, mipCount, subs.data());

    CD3DX12_RESOURCE_BARRIER toSRV = CD3DX12_RESOURCE_BARRIER::Transition(
        newResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toSRV);

    m_UploadBuffers.push_back(uploadBuffer);

    // Same bump-allocator bounds check as the synchronous path: the heap is 256 slots and nothing
    // is ever freed, so an unchecked increment writes descriptors outside it. Returns a
    // default-constructed (null resource) TextureResource on exhaustion.
    if (m_CurrentDescriptorIndex >= kSrvHeapCapacity) {
        assert(false && "ResourceManager SRV heap exhausted (256 slots, never freed). Textures are "
                        "not evicted between scenes - see AllocateSrvSlot.");
        return TextureResource{};
    }

    UINT descriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(
        m_SrvHeap->GetCPUDescriptorHandleForHeapStart(), m_CurrentDescriptorIndex, descriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = mipCount;
    m_Device->CreateShaderResourceView(newResource.Get(), &srvDesc, cpuHandle);

    TextureResource tex;
    tex.resource = newResource;
    tex.gpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_SrvHeap->GetGPUDescriptorHandleForHeapStart(), m_CurrentDescriptorIndex, descriptorSize);
    m_CurrentDescriptorIndex++;
    return tex;
}

TextureHandle ResourceManager::CreateSolidColorTexture(ID3D12GraphicsCommandList* cmdList,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // Synthetic cache key encodes the color so different solid colors don't collide/alias.
    // Real textures are always loaded from an actual file path (see LoadTexture), so this
    // distinctive, non-path-shaped prefix can't collide with one.
    wchar_t key[32];
    swprintf_s(key, L"__solid_%02x%02x%02x%02x__", r, g, b, a);
    std::string keyStr = NarrowKey(key);

    JLib::AssetHandle<TextureResource> assetHandle = m_TextureAssets.Load(keyStr,
        [&](TextureResource& tex) -> bool {
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> newResource;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&newResource)));

    UINT64 requiredSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT numRows; UINT64 rowSizeInBytes;
    m_Device->GetCopyableFootprints(&texDesc, 0, 1, 0, &layout, &numRows, &rowSizeInBytes, &requiredSize);

    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    CD3DX12_HEAP_PROPERTIES uploadProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(requiredSize);
    ThrowIfFailed(m_Device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)));

    uint8_t pixel[4] = { r, g, b, a };
    BYTE* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    memcpy(mapped + layout.Offset, pixel, sizeof(pixel));
    uploadBuffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = newResource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = layout;
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);

    CD3DX12_RESOURCE_BARRIER toSRV = CD3DX12_RESOURCE_BARRIER::Transition(
        newResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toSRV);

    m_UploadBuffers.push_back(uploadBuffer);

    // Third of the three bump-allocated paths; same 256-slot, never-freed heap.
    if (m_CurrentDescriptorIndex >= kSrvHeapCapacity) {
        assert(false && "ResourceManager SRV heap exhausted (256 slots, never freed). Textures are "
                        "not evicted between scenes - see AllocateSrvSlot.");
        return false;
    }

    UINT descriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(
        m_SrvHeap->GetCPUDescriptorHandleForHeapStart(), m_CurrentDescriptorIndex, descriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_Device->CreateShaderResourceView(newResource.Get(), &srvDesc, cpuHandle);
    // NOTE: the bounds check for this path is above, before cpuHandle is computed.

    tex.resource = newResource;
    tex.gpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_SrvHeap->GetGPUDescriptorHandleForHeapStart(), m_CurrentDescriptorIndex, descriptorSize);

    m_CurrentDescriptorIndex++;
    return true;
        });
    return TextureHandle{ assetHandle.index };
}

Mesh ResourceManager::CreateMeshRaw(const void* vertices, uint32_t vCount, uint32_t stride,
                                    const uint32_t* indices, uint32_t iCount) {
    Mesh mesh;
    mesh.vertexCount = vCount;
    mesh.indexCount = iCount;
    mesh.meshID = meshCtr;
    meshCtr++;

    // Object-space bounding sphere for frustum culling (Renderer3D transforms it per Submit). Position is
    // the first 3 floats of EVERY vertex type here (Vertex.x/y/z, Vertex3D::pos, SkinnedVertex3D::pos --
    // all XMFLOAT3-equivalent at offset 0), so this stays vertex-type-blind like the rest of the function.
    // AABB center + half-diagonal radius: one pass, conservative (never culls a visible object).
    if (vCount > 0 && vertices) {
        const uint8_t* base = static_cast<const uint8_t*>(vertices);
        const float* p0 = reinterpret_cast<const float*>(base);
        float mn[3] = { p0[0], p0[1], p0[2] };
        float mx[3] = { p0[0], p0[1], p0[2] };
        for (uint32_t i = 1; i < vCount; ++i) {
            const float* p = reinterpret_cast<const float*>(base + (size_t)i * stride);
            for (int k = 0; k < 3; ++k) { mn[k] = (p[k] < mn[k]) ? p[k] : mn[k]; mx[k] = (p[k] > mx[k]) ? p[k] : mx[k]; }
        }
        mesh.boundsCenter = { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f };
        float ex = mx[0] - mesh.boundsCenter.x, ey = mx[1] - mesh.boundsCenter.y, ez = mx[2] - mesh.boundsCenter.z;
        mesh.boundsRadius = std::sqrt(ex * ex + ey * ey + ez * ez);
    }
    // --- Vertex Buffer --- (stride comes from the templated CreateMesh<VertexT> wrapper; this
    // function is deliberately vertex-type-blind so 2D and 3D share one implementation.)
    const UINT64 vBytes = (UINT64)stride * vCount;
    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC vBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vBytes);

    ThrowIfFailed(m_Device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &vBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mesh.vertexBuffer)));

    void* pVData;
    mesh.vertexBuffer->Map(0, nullptr, &pVData);
    memcpy(pVData, vertices, (size_t)vBytes);
    mesh.vertexBuffer->Unmap(0, nullptr);

    mesh.vertexBufferView.BufferLocation = mesh.vertexBuffer->GetGPUVirtualAddress();
    mesh.vertexBufferView.StrideInBytes = stride;
    mesh.vertexBufferView.SizeInBytes = (UINT)vBytes;

    // --- Index Buffer ---
    CD3DX12_RESOURCE_DESC iBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t) * iCount);

    ThrowIfFailed(m_Device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &iBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mesh.indexBuffer)));

    void* pIData;
    mesh.indexBuffer->Map(0, nullptr, &pIData);
    memcpy(pIData, indices, sizeof(uint32_t) * iCount);
    mesh.indexBuffer->Unmap(0, nullptr);

    mesh.indexBufferView.BufferLocation = mesh.indexBuffer->GetGPUVirtualAddress();
    mesh.indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    mesh.indexBufferView.SizeInBytes = sizeof(uint32_t) * iCount;
    uint32_t newID = (uint32_t)m_Meshes.size();
    m_Meshes.push_back({ mesh, newID });
    return mesh;
}


