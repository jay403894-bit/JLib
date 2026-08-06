#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <d3dx12.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <cstdint>
#include <chrono>
#include <unordered_map>
#include "Colors.h"
#include "ResourceManager.h"
#include "DirectXTex.h"
#include "TaskScheduler.h"
#include "Mesh.h"
#include "Vertex.h"
#include "Font.h"
#include "RendererCore.h"
#define MAX_BUCKETS_PER_LAYER 512
#define NUM_LAYERS 4

struct ObjectData {
    DirectX::XMFLOAT2 pos;
    DirectX::XMFLOAT2 size;
    DirectX::XMFLOAT4 color;
	float hasTexture; // 1.0f if the object has a texture, 0.0f otherwise
    float rotation;
    // useAlphaFromRGB: 1.0f = this instance's texture has no usable alpha channel (e.g. a
    // BMFont atlas with outline=0, where the glyph shape is baked into RGB as white-ink-on-
    // black instead of real transparency). The pixel shader derives coverage/alpha from RGB
    // luminance instead of texture.a, and uses this instance's requested `color` for RGB
    // instead of the texture's. 0.0f = normal texture.a-based sampling (repurposes one of
    // the two former alignment-padding floats -- struct size unchanged).
    float useAlphaFromRGB;
    float padding; // still 1 spare float for 16-byte alignment
    // Per-instance UV sub-rect (0..1 of the bound texture): final UV = mesh.uv * uvScale + uvOffset.
    // Appended as its own 16-byte row so the existing layout above is untouched. Identity
    // (offset=0,0 scale=1,1) reproduces the old whole-texture behavior for every existing
    // sprite. Lets text glyphs share ONE unit-quad mesh + the font atlas texture, varying
    // only this per-instance rect -- an entire string batches into one instanced draw call.
    DirectX::XMFLOAT2 uvOffset;
    DirectX::XMFLOAT2 uvScale;
    // Free-form per-effect payload: each effectID's own shader (see ShaderEffect/RegisterEffect)
    // defines what these 4 floats mean -- e.g. a glow shader might read .x as intensity and
    // .yzw as a tint. Prevents ObjectData growing a new named field per effect the way
    // useAlphaFromRGB did for text; the DEFAULT shader (effectID 0) ignores this entirely.
    DirectX::XMFLOAT4 effectParams;
    ObjectData(DirectX::XMFLOAT2 p, DirectX::XMFLOAT2 s, DirectX::XMFLOAT4 c, float hasTex, float rot,
               DirectX::XMFLOAT2 uvOff = { 0.0f, 0.0f }, DirectX::XMFLOAT2 uvScl = { 1.0f, 1.0f },
               float alphaFromRGB = 0.0f, DirectX::XMFLOAT4 effParams = { 0.0f, 0.0f, 0.0f, 0.0f })
        : pos(p), size(s), color(c), hasTexture(hasTex), rotation(rot),
          useAlphaFromRGB(alphaFromRGB), padding(0.0f), uvOffset(uvOff), uvScale(uvScl),
          effectParams(effParams)
    {
    }
};

struct Batch {
    Mesh* mesh = nullptr;
    TextureResource* tex = nullptr;
    uint32_t effectID = 0;   // which PSO (see ShaderEffect) draws this bucket
    float depth = 0.0f;
    // (instance data lives in WorkerLocalSubmissionData, not here -- this is metadata only)
};

// One registered shader/PSO. effectID 0 is always the default (sprites + text) shader,
// registered automatically in Initialize(). Root signature is shared across effects unless
// one is explicitly given -- only give an effect its own if it needs different bound
// resources; a different blend/rasterizer state or VS/PS pair does NOT require a new root sig.
struct ShaderEffect {
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
};

// Per-worker (fiber) submission buffers. Each worker accumulates ObjectData locally.
// At flush time, all worker buffers are merged into the GPU instance buffer (single-threaded).
// This eliminates concurrent vector modification races.
// Indexed by [layer][bucketHandle]. bucketHandle is a small sequential index assigned the
// first time a (mesh,tex,effect) combo is submitted (see Renderer2D::Submit) -- NOT a hash, so
// there's no collision/probing and no fixed cap. Each layer's inner vector grows lazily to fit
// however many distinct materials THAT layer actually uses.
struct alignas(64) WorkerLocalSubmissionData {
    std::array<std::vector<std::vector<ObjectData>>, NUM_LAYERS> buckets;
};

struct CommandContext {
    std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> allocators;
    std::vector<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>> cmdLists;    bool isBusy = false;
};

// Owns the 2D sprite/text batching + drawing pipeline: root signature/PSOs, the shared instance
// buffer, per-worker submission buffers, and the parallel FlushBatchTask/FlushBatchParallel
// machinery. Does NOT own the device/swap chain/fences/particle system -- those live in
// RendererCore (see m_Core), which drives this class's per-frame work via
// CollectCommandLists()/ResetWorkerAllocators()/ClearWorkerBucketsAndResetPool(), called from
// RendererCore::BeginFrame/PresentFrame. This is the seam a future Renderer3D would mirror.
class Renderer2D {
public:
    struct FlushTaskContext {
        Renderer2D* renderer;                         // Access to m_Buckets + constant buffers
        // Indexed simply by bucket handle WITHIN 'layer' (see below) -- each task only ever
        // processes ONE layer now, so these hold just THAT layer's counts/offsets, not a
        // flat all-layers array. (A bucket handle used in two layers is still two entirely
        // separate m_Buckets[layer] vectors, so no cross-layer collision is possible.)
        std::vector<UINT> bucketInstanceCounts;    // Per-bucket instance count, for THIS layer only
        std::vector<UINT> bucketInstanceOffsets;   // Per-bucket ABSOLUTE start index in the buffer, THIS layer only
        int layer;                                   // THIS task processes ONLY this one layer
        int taskIndex;                              // The ID used to index into the pool (0..taskCount-1)
        int startBucket;                            // The range of buckets (within 'layer') this task processes
        int endBucket;
    };
    struct FlushTaskContextPool {
        unsigned hw = std::thread::hardware_concurrency();
        int taskCount = (hw > 1) ? (int)(hw - 1) : 1;
        std::vector<FlushTaskContext> pool;
        std::atomic<size_t> nextIndex{ 0 };

        // One slot per (layer, task) pair -- worst case every layer is active. Empty layers
        // are skipped at dispatch time (see FlushBatchParallel), so unused slots just sit
        // idle; this is a one-time size, not a per-frame cost.
        FlushTaskContextPool() {
            pool.resize((size_t)taskCount * NUM_LAYERS);
        }
        FlushTaskContext* Acquire() {
            size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
            // Add a safety check here if you exceed your pool size
            return &pool[index];
        }

        void Reset() {
            nextIndex.store(0, std::memory_order_relaxed);
        }
    };

    // Call AFTER core.Initialize() -- builds root signature/PSO/instance buffer/worker storage/
    // SRV heap/resource manager/font against core's already-created device/queue.
    void Initialize(RendererCore& core);

    void Submit(const Mesh& mesh, TextureResource* tex, DirectX::XMFLOAT2 offset, float width, float height, int zLayer=0, DirectX::XMFLOAT4 color = {1.0f,1.0f,1.0f,1.0f}, float rotation = 0.0f, DirectX::XMFLOAT2 uvOffset = {0.0f,0.0f}, DirectX::XMFLOAT2 uvScale = {1.0f,1.0f}, bool useAlphaFromRGB = false, uint32_t effectID = 0, DirectX::XMFLOAT4 effectParams = {0.0f,0.0f,0.0f,0.0f});
    template<typename... Args>
    void SubmitText(Font& font, float x, float y, const std::string& fmt,
        float scale = 1.0f,
        DirectX::XMFLOAT4 color = { 1.0f,1.0f,1.0f,1.0f },
        float rotation = 0.0f,
        TextAlign align = TextAlign::Left,
        int zLayer = 2,
        Args&&... args)
    {
        std::string text = std::vformat(fmt, std::make_format_args(args...));
        font.SubmitText(*this, text, x, y, scale, color, rotation, align, zLayer);
    }
    inline Font* GetSysFont() { return &font; }
    // Compiles a VS/PS pair + blend state into a new PSO and returns its effectID (index into
    // m_Effects). effectID 0 is the default sprite/text shader, registered by Initialize().
    // Call AFTER Initialize() (needs m_Core's device + m_RootSignature). Shares the existing
    // root signature -- only add an override param for a new one if an effect needs different
    // bound resources (most effects just need a different VS/PS/blend, not a new root sig).
    uint32_t RegisterEffect(const std::wstring& vsPath, const std::wstring& psPath, D3D12_BLEND_DESC blend);
    static void FlushBatchTask(void* data) {
        FlushTaskContext* ctx = static_cast<FlushTaskContext*>(data);
        Renderer2D* r = ctx->renderer;
        RendererCore* core = r->m_Core;
        // TWO DIFFERENT indices, deliberately: frameResourceIdx picks which rotating CPU/GPU
        // sync-slot set to use (allocator, cmdList, constant buffer, instance buffer);
        // backBufferIdx picks which actual swap-chain back buffer / RTV to render into.
        const int frameResourceIdx = core->GetFrameResourceIndex();
        const int backBufferIdx = core->GetCurrentBackBufferIndex();

        // Each task owns its OWN context (cmd list + allocator for this frame). NEVER touch
        // the shared/main context here -- two threads recording one list = corruption, and
        // resetting the main list would wipe the clear/barrier recorded in BeginFrame.
        // Indexed by (layer, taskIndex): PresentFrame submits these lists LAYER-MAJOR, so a
        // (mesh,tex) pair at zLayer 9 is GUARANTEED to execute on the GPU after everything at
        // zLayer 0, regardless of which task's bucket-range either one happened to hash into.
        const int taskCount = r->m_FlushTaskContextPool.taskCount;
        CommandContext& myCtx = r->m_CommandContextPool[(size_t)ctx->layer * taskCount + ctx->taskIndex];
        auto& alloc = myCtx.allocators[frameResourceIdx];
        auto& cl    = myCtx.cmdLists[frameResourceIdx];

        // Safe to reset: BeginFrame waited on this frame-resource slot's fence before dispatch.
        alloc->Reset();
        cl->Reset(alloc.Get(), r->m_PipelineState.Get());

        // State only -- NO clear, NO back-buffer barrier (those live in the PRE/POST lists).
        ID3D12DescriptorHeap* ppHeaps[] = { r->m_SrvHeap.Get() };
        cl->SetDescriptorHeaps(1, ppHeaps);
        cl->SetGraphicsRootSignature(r->m_RootSignature.Get());

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
            core->GetRTVDescriptorHeap()->GetCPUDescriptorHandleForHeapStart(),
            backBufferIdx, core->GetRTVDescriptorSize()); // must match the ACTUAL back buffer, not the sync slot
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr); // bind only; PRE already cleared it
        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)core->GetClientWidth(), (float)core->GetClientHeight(), 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, (LONG)core->GetClientWidth(), (LONG)core->GetClientHeight() };
        cl->RSSetViewports(1, &viewport);
        cl->RSSetScissorRects(1, &scissor);
        cl->SetGraphicsRootConstantBufferView(0, core->GetConstantBuffer(frameResourceIdx)->GetGPUVirtualAddress());

        // Tracks the PSO currently bound on THIS list, so we only call SetPipelineState when
        // a bucket's effect actually differs from the previous one -- buckets are already
        // grouped by (mesh,tex,effect) via the bucket key, so same-effect buckets in a row
        // cost nothing extra; a real switch only happens at an effect boundary.
        uint32_t lastEffect = UINT32_MAX;
        const int layer = ctx->layer; // this task's ONE assigned layer -- see the pool-indexing comment above
        for (int i = ctx->startBucket; i < ctx->endBucket; ++i) {
            const Batch& b = r->m_Buckets[layer][i];
            if (b.mesh == nullptr) continue;

            // ctx->bucketInstanceCounts/Offsets hold ONLY this task's layer, indexed directly
            // by bucket handle (no flat all-layers math needed -- each layer's m_Buckets is
            // its own vector, so there's no cross-layer index collision to disambiguate).
            UINT count = ctx->bucketInstanceCounts[i];
            if (count == 0) continue;

            if (b.effectID != lastEffect) {
                cl->SetPipelineState(r->m_Effects[b.effectID].pso.Get());
                lastEffect = b.effectID;
            }

            // ABSOLUTE offset the merge placed this bucket at -- no sequential accumulation,
            // so disjoint buffer regions across layers are addressed correctly.
            UINT64 byteOffset = (UINT64)ctx->bucketInstanceOffsets[i] * sizeof(ObjectData);

            cl->IASetVertexBuffers(0, 1, &b.mesh->vertexBufferView);
            cl->IASetIndexBuffer(&b.mesh->indexBufferView);
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl->SetGraphicsRootDescriptorTable(2, b.tex->gpuHandle);
            cl->SetGraphicsRootShaderResourceView(1, r->m_InstanceBuffer[frameResourceIdx]->GetGPUVirtualAddress() + byteOffset);
            cl->DrawIndexedInstanced(b.mesh->indexCount, count, 0, 0, 0);
        }
        cl->Close();
    }
    int  FlushBatchParallel(); // records worker draw lists; returns how many were recorded
    // ---- Called by RendererCore's BeginFrame/PresentFrame -- see the class comment above. ----
    void ResetWorkerAllocators(int frame);
    void CollectCommandLists(int frame, std::vector<ID3D12CommandList*>& outLists);
    void ClearWorkerBucketsAndResetPool();
    // ------------------------------------------------------------------------------------------
    ResourceManager* GetResourceManager() const;
    void   UpdateFPS();                 // accumulates frames, logs FPS once per second

    // ---- Thin forwarding wrappers to m_Core, so existing call sites (main.cpp/Font.cpp) don't
    // need to know a RendererCore exists at all. ----
    void ExecuteUploadCommand(std::function<void(ID3D12GraphicsCommandList*)> task) { m_Core->ExecuteUploadCommand(task); }
    void UpdateGlobalUniforms(DirectX::XMFLOAT2 screenSize) { m_Core->UpdateGlobalUniforms(screenSize); }
    DirectX::XMFLOAT2 GetScreenSize() const { return m_Core->GetScreenSize(); }
    DirectX::XMFLOAT2 GetNDC(float targetX, float targetY) { return m_Core->GetNDC(targetX, targetY); }
    float GetFrameTime() { return m_Core->GetFrameTime(); }
    float GetLastDeltaTime() const { return m_Core->GetLastDeltaTime(); }
    ID3D12GraphicsCommandList* GetCommandList() { return m_Core->GetCommandList(); }
    uint32_t RegisterParticleEffect(const std::wstring& updateCsPath, uint32_t maxParticles) {
        return m_Core->RegisterParticleEffect(updateCsPath, maxParticles);
    }
    void RequestSpawn(uint32_t effectID, Particle p) { m_Core->RequestSpawn(effectID, p); }
    void RequestSpawn(uint32_t effectID, DirectX::XMFLOAT2 basePosition, float offsetRadius,
        DirectX::XMFLOAT2 velocity, float lifetime, DirectX::XMFLOAT4 color) {
        m_Core->RequestSpawn(effectID, basePosition, offsetRadius, velocity, lifetime, color);
    }
    // ------------------------------------------------------------------------------------------
private:
    RendererCore* m_Core = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature();
    // vsPath/psPath/blend are explicit (no silent "no blending" default) -- Initialize() and
    // RegisterEffect() both pass them. Everything else about the PSO (input layout, root
    // signature, rasterizer/depth state, RTV format) stays fixed across effects.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CreatePipelineState(
        const std::wstring& vsPath, const std::wstring& psPath, D3D12_BLEND_DESC blend);
    static D3D12_BLEND_DESC DefaultAlphaBlend(); // standard alpha "over" blend, used by effectID 0
    void CreateInstanceBuffer(ID3D12Device* device, UINT maxInstances);
    uint64_t GetMaterialID(Mesh* mesh, TextureResource* tex, uint32_t effectID);
    void ProvisionLayerContexts(int layer);

    CommandContext* m_CurrentContext;
    bool m_IsRenderTargetState = false; // Start as false
    std::atomic<bool> m_BarrierIssued = false;
    std::vector<CommandContext> m_CommandContextPool;
    // Which zLayers actually had content THIS frame, in ascending order -- filled by
    // FlushBatchParallel, consumed by CollectCommandLists to submit command lists LAYER-MAJOR
    // (all of layer L's task lists before any of layer L+1's), which is what makes zLayer
    // ordering correct across task boundaries. See FlushBatchTask's pool-indexing comment.
    std::vector<int> m_ActiveLayersThisFrame;
    // How many (task) command lists were ACTUALLY recorded per layer this frame -- may be
    // LESS than taskCount when a layer has few distinct materials. CollectCommandLists must
    // use this, not a blanket taskCount, or it resubmits stale/never-recorded lists.
    std::vector<int> m_TasksDispatchedPerLayer;
    // m_CommandContextPool is sized for NUM_LAYERS up front, but a layer's taskCount worth of
    // allocators/lists are only actually CREATED the first time that layer is used (see
    // ProvisionLayerContexts). Lets NUM_LAYERS stay a generous cap with zero memory cost for
    // layers you never touch, instead of paying for all of them from frame 0.
    std::vector<bool> m_LayerProvisioned;
    // m_Buckets[layer] grows dynamically as new (mesh,tex,effect) combos are discovered --
    // no fixed cap, no hash collisions. m_MaterialHandles[layer] maps a material's key (see
    // GetMaterialID) to its index in m_Buckets[layer], assigned ONCE the first time that
    // material is submitted and PERSISTED across frames (meshes/textures/effects live for the
    // program's lifetime, so there's no need to rediscover them every frame -- only the
    // per-frame INSTANCE data, in WorkerLocalSubmissionData, gets cleared each frame).
    std::vector<Batch> m_Buckets[NUM_LAYERS];
    std::unordered_map<uint64_t, uint32_t> m_MaterialHandles[NUM_LAYERS];

    // Per-worker (fiber) submission buffers. Index by T_Thread::GetCurrent()->qIndex.
    // Heap-allocated to avoid stack overflow (2MB+ of vectors).
    static constexpr int MAX_WORKERS = 32;
    std::unique_ptr<std::array<WorkerLocalSubmissionData, MAX_WORKERS>> m_WorkerLocalStorage;

    static inline const uint32_t indices[] = { 0, 1, 2, 1, 3, 2 };
    Microsoft::WRL::ComPtr<ID3D12Resource> m_IndexBuffer;
    D3D12_INDEX_BUFFER_VIEW ibv;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PipelineState; // effectID 0's PSO (also m_Effects[0])
    std::vector<ShaderEffect> m_Effects;

    FlushTaskContextPool m_FlushTaskContextPool;
    std::unique_ptr<ResourceManager> m_ResourceManager;
    Font font; // Renderer2D's own built-in font, used by UpdateFPS()'s SubmitText convenience wrapper

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SamplerHeap;

    // Instance buffers: ONE PER FRAME. A single shared buffer let the CPU overwrite
    // instance data while the GPU was still reading it for an in-flight frame -> GPU
    // hang / "device ran into a problem" (TDR). Per-frame + the BeginFrame fence wait
    // means we only write a buffer the GPU has finished with.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_InstanceBuffer[RendererCore::NumFrames];
    void* m_MappedData[RendererCore::NumFrames] = {};

    // FPS measurement
    uint64_t m_frameCounter = 0;
    double   m_elapsedSeconds = 0.0;
    std::chrono::high_resolution_clock::time_point m_t0;
    // Last COMPUTED fps value -- updated once/sec inside UpdateFPS()'s averaging window, but
    // the text must be SUBMITTED every frame (this renderer has no persistent draw state --
    // skip a frame's Submit() and it just doesn't draw that frame) or it flickers, visible for
    // one frame per second instead of staying on screen continuously.
    double m_LastFPS = 0.0;
};
