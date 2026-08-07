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
#include <thread>   // std::thread::hardware_concurrency() -- FlushTaskContextPool
#include <atomic>   // std::atomic<size_t> -- FlushTaskContextPool
#include <array>    // std::array -- WorkerLocalSubmissionData::buckets
#include "Colors.h"
#include "ResourceManager.h"
#include "Mesh.h"
#include "Vertex.h"
#include "Font.h"
#include "RendererCore.h"
#define NUM_LAYERS 10
namespace JLib {
    struct BatchItem {
        // Pointer to an externally-owned, STABLE Mesh -- NOT a value copy. Submit() caches
        // &item.mesh's pointee in a Batch that persists across every future frame (see
        // Renderer2D::Submit/GetMaterialID), so the Mesh this points to must outlive the item
        // itself. A fresh SpriteBatchItem built per-call (e.g. Font::SubmitLine, one per glyph per
        // frame) is fine as long as `mesh` points at a long-lived Mesh (e.g. Font::unitQuad) --
        // it must never point at a Mesh embedded in the transient item itself.
        Mesh* mesh = nullptr;
        // Lightweight handle, not a pointer -- see TextureHandle's comment in ResourceManager.h.
        // Default-constructed (invalid) means "no texture"; Submit() checks IsValid(), same as the
        // old nullptr check.
        TextureHandle tex;
        DirectX::XMFLOAT2 position;
        DirectX::XMFLOAT2 size;
        int zLayer = 0;
        DirectX::XMFLOAT4 color = { 1.0f,1.0f,1.0f,1.0f };
        float rotation = 0.0f;
        DirectX::XMFLOAT2 uvOffset = { 0.0f,0.0f };
        DirectX::XMFLOAT2 uvScale = { 1.0f,1.0f };
        bool useAlphaFromRGB = false;
        uint32_t effectID = 0;
        DirectX::XMFLOAT4 effectParams = { 0.0f,0.0f,0.0f,0.0f };
        bool flipX = false;
        bool flipY = false;
        // Horizontal shear in the same pixel units as `size` (x' = x*size.x + y*shear). 0 for
        // every ordinary sprite; DrawTriangle sets it so one reference triangle mesh can be
        // affine-mapped onto three arbitrary points. See ObjectData::shear.
        float shear = 0.0f;
    };

    // Owns the 2D sprite/text batching + drawing pipeline: root signature/PSOs, the shared instance
    // buffer, per-worker submission buffers, and the parallel FlushBatchTask/FlushBatchParallel
    // machinery. Does NOT own the device/swap chain/fences/particle system -- those live in
    // RendererCore (see m_Core), which drives this class's per-frame work via
    // CollectCommandLists()/ResetWorkerAllocators()/ClearWorkerBucketsAndResetPool(), called from
    // RendererCore::BeginFrame/PresentFrame. This is the seam a future Renderer3D would mirror.
    class Renderer2D {
    private:
        // ---- 2D primitive resources (see the public primitive block above). Built lazily by
        // EnsurePrimitives() on the first primitive call, so projects that draw none pay nothing.
        struct PrimitiveResources {
            Mesh quad;                            // unit quad (-0.5..0.5): rectangles AND lines
            // Reference triangle with vertices at (0,0),(1,0),(0,1). DrawTriangle solves for the
            // affine transform carrying THIS onto the caller's three points -- which is why its
            // vertices sit at the origin and the unit axes rather than being centered like the quad.
            Mesh triangle;
            std::unordered_map<int, Mesh> discs;  // filled unit N-gon fans, keyed by side count
            std::unordered_map<int, Mesh> rings;  // proportional-band N-gon outlines, by side count
            TextureHandle white;                  // 1x1 opaque white -- color comes per-instance
            // SDF circle effect (CirclePS.cso). Circles draw as ONE quad whose shader derives
            // coverage from the distance to the center, so they stay perfectly round at any zoom
            // rather than showing an N-gon's flat edges. 0 means "not available" (the .cso wasn't
            // found) and the cached-mesh discs are used instead.
            uint32_t circleEffect = 0;
            // SDF rounded-rectangle effect (RoundRectPS.cso) -- the UI shape: panels, buttons,
            // pills. Its parameters are in PIXELS rather than UV because a UV-space corner radius
            // stretches into an ellipse on any non-square rect. 0 = unavailable, callers fall back
            // to sharp-cornered rectangles.
            uint32_t roundRectEffect = 0;
            bool ready = false;
        };
        PrimitiveResources m_Prim;
        void  EnsurePrimitives();                 // idempotent: quad + white texture + circle effect
        Mesh& GetDiscMesh(int sides);             // cached filled N-gon (radius 0.5)
        Mesh& GetRingMesh(int sides);             // cached N-gon band (thickness scales with size)
        void  SubmitPrimitive(Mesh& mesh, DirectX::XMFLOAT2 center, DirectX::XMFLOAT2 size,
                              DirectX::XMFLOAT4 color, float rotationDeg, int zLayer,
                              uint32_t effectID = 0,
                              DirectX::XMFLOAT4 effectParams = { 0.0f, 0.0f, 0.0f, 0.0f },
                              float shear = 0.0f);

        // ---- Internal batching types -- never touched outside this class. BatchItem (above) is
        // the only client-facing payload type; everything below is implementation detail. ----
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
            // SHEAR (was the spare alignment float). Turns the instance transform from
            // scale+rotate+translate (5 degrees of freedom) into a full 2D AFFINE map (6), which is
            // exactly what an arbitrary triangle needs -- 3 points = 6 DOF. The vertex shader
            // applies it as x' = x*size.x + y*shear, so 0 reproduces the old behavior bit-for-bit
            // and every existing sprite/glyph is unaffected. Used only by DrawTriangle today.
            float shear;
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
                float alphaFromRGB = 0.0f, DirectX::XMFLOAT4 effParams = { 0.0f, 0.0f, 0.0f, 0.0f },
                float shearNDC = 0.0f)
                : pos(p), size(s), color(c), hasTexture(hasTex), rotation(rot),
                useAlphaFromRGB(alphaFromRGB), shear(shearNDC), uvOffset(uvOff), uvScale(uvScl),
                effectParams(effParams)
            {}
        };

        struct Batch {
            Mesh* mesh = nullptr;
            TextureHandle tex;
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

        CommandContext* m_CurrentContext;
        bool m_IsRenderTargetState = false; // Start as false
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

        // Per-worker (fiber) submission buffers. Index by Thread::GetCurrent()->qIndex.
        // Heap-allocated to avoid stack overflow (2MB+ of vectors).
        static constexpr int MAX_WORKERS = 64;
        // Per-frame instance capacity. ONE definition -- the buffer allocation and the overflow check
        // both read this, because when they were two separate 65536 literals with a "must match"
        // comment, nothing enforced that they did.
        //
        // Raised 64K -> 256K: a CPU-side particle simulator submits one instance PER PARTICLE and
        // blew through 64K easily, which (before the overflow fix) silently dropped whole frames.
        // Cost is ObjectData(80 bytes) x capacity x NumFrames(3) = ~61MB of upload-heap memory,
        // allocated once at startup. That is a real but acceptable trade against sprites vanishing.
        //
        // If you find yourself wanting to raise this again, prefer the GPU PARTICLE PATH instead
        // (RendererCore::RegisterParticleEffect / Renderer3D::AddParticleEmitter): those simulate on
        // the compute queue and never touch this buffer, so their particle count is bounded by VRAM
        // rather than by per-frame CPU upload. This buffer is for SPRITES the game logic controls.
        static constexpr UINT kMaxInstances = 262144;
        bool m_WarnedInstanceOverflow = false;   // one message per run, not per frame
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
        // Built-in FPS readout placement (see SetFpsOverlay). Negative components anchor from the
        // far edge, resolved per-frame against the current screen size so it survives resizes.
        DirectX::XMFLOAT2 m_FpsPos   = { 10.0f, 10.0f };
        DirectX::XMFLOAT4 m_FpsColor = { 0.22f, 1.0f, 0.08f, 1.0f };
        float             m_FpsScale = 1.0f;
        int               m_FpsLayer = 2;
        bool              m_FpsVisible = true;
        RendererCore* m_Core = nullptr;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature();
        // vsPath/psPath/blend are explicit (no silent "no blending" default) -- Initialize() and
        // RegisterEffect() both pass them. Everything else about the PSO (input layout, root
        // signature, rasterizer/depth state, RTV format) stays fixed across effects.
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CreatePipelineState(
            const std::wstring& vsPath, const std::wstring& psPath, D3D12_BLEND_DESC blend);
        void CreateInstanceBuffer(ID3D12Device* device, UINT maxInstances);
        uint64_t GetMaterialID(Mesh* mesh, TextureHandle tex, uint32_t effectID);
        void ProvisionLayerContexts(int layer);

    public:
        // Standard alpha "over" blend -- used by effectID 0, and reusable by any RegisterEffect()
        // caller that just wants normal transparency (e.g. an outline effect with a transparent
        // interior) without hand-rolling the same D3D12_BLEND_DESC fields themselves.
        static D3D12_BLEND_DESC DefaultAlphaBlend();
        // Call AFTER core.Initialize() -- builds root signature/PSO/instance buffer/worker storage/
        // SRV heap/resource manager/font against core's already-created device/queue.
        void Initialize(RendererCore& core);

        // Takes a const reference and copies internally: Submit never modifies your item, so the
        // same BatchItem can be filled once and submitted repeatedly in a loop (the natural way to
        // draw a tile grid). It previously took BatchItem& and mutated it, which made repeated
        // submits of one item apply the sRGB conversion cumulatively.
        void Submit(const BatchItem& item);
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
        inline ImTextureID GetImGuiTextureID(TextureHandle tex) const
        {
            // If it's an integer type, return 0 for an invalid handle
            if (!tex.IsValid()) return 0;

            // Use the descriptor the texture ACTUALLY got, rather than recomputing it as
            // heapStart + tex.id * descriptorSize. That old arithmetic assumed TextureHandle::id and
            // the descriptor's index in the shared heap were the same number -- true only while every
            // slot in the heap was consumed by a texture load, in load order, with nothing else ever
            // allocating from it. The moment anything reserved slots (Renderer3D's shadow map and IBL
            // cubes already did; ImGui's 16-slot block finally made it visible), every id past that
            // point pointed at the wrong descriptor and ImGui::Image drew a blank.
            const TextureResource& res = m_ResourceManager->Resolve(tex);
            return static_cast<ImTextureID>(res.gpuHandle.ptr);
        }
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
            auto& cl = myCtx.cmdLists[frameResourceIdx];

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
                cl->SetGraphicsRootDescriptorTable(2, r->GetResourceManager()->Resolve(b.tex).gpuHandle);
                cl->SetGraphicsRootShaderResourceView(1, r->m_InstanceBuffer[frameResourceIdx]->GetGPUVirtualAddress() + byteOffset);
                cl->DrawIndexedInstanced(b.mesh->indexCount, count, 0, 0, 0);
            }
            cl->Close();
        }
        int  FlushBatchParallel(); // records worker draw lists; returns how many were recorded
        // ---- Called by RendererCore's BeginFrame/PresentFrame -- see the class comment above. ----
        void ResetWorkerAllocators(int frame);
        // Appends ONLY this one layer's recorded lists -- lets RendererCore::PresentFrame interleave
        // them with particle-pool draws in ascending zLayer order instead of dumping every 2D layer
        // in one block.
        void CollectCommandListsForLayer(int layer, int frame, std::vector<ID3D12CommandList*>& outLists);
        void ClearWorkerBucketsAndResetPool();
        // ------------------------------------------------------------------------------------------
        ResourceManager* GetResourceManager() const;
        // Lets RendererCore bind a TextureResource's descriptor for particle draws -- textures live
        // in THIS heap (loaded via ResourceManager), so a particle pool's texture SRV handle is only
        // valid once this exact heap is bound via SetDescriptorHeaps.
        ID3D12DescriptorHeap* GetSrvHeap() const { return m_SrvHeap.Get(); }
        // Ticks the frame counter and draws the built-in FPS readout. Position/colour/scale/layer are
        // configurable (see SetFpsOverlay) because a library has no business owning a fixed corner of
        // the app's screen -- the old hardcoded 10,10 collided with any HUD that used the same spot.
        void   UpdateFPS();
        // Configure the built-in readout. `visible=false` keeps the timing running (so GetFPS stays
        // valid) but draws nothing -- the right call for a game that renders FPS in its own HUD.
        // Negative x/y anchor from the RIGHT/BOTTOM edge instead, so SetFpsOverlay({-24,-24}) pins it
        // to the bottom-right and stays correct across window resizes. A right-anchored readout is
        // drawn right-ALIGNED, so the offset is a true margin regardless of how many digits the
        // number has. Allow ~20px of margin rather than the obvious 8-10: Windows 11 rounds window
        // corners, and text tucked tight into a corner gets visibly clipped by the rounding.
        void   SetFpsOverlay(DirectX::XMFLOAT2 position,
                             DirectX::XMFLOAT4 color = { 0.22f, 1.0f, 0.08f, 1.0f },
                             float scale = 1.0f, int zLayer = 2, bool visible = true);
        void   SetFpsVisible(bool visible);
        // The averaged value UpdateFPS computes (refreshed once/sec). Exposed so an app can draw the
        // number wherever it likes -- pair with SetFpsVisible(false).
        double GetFPS() const { return m_LastFPS; }

        // ===================== 2D primitives (raylib-shaped convenience) =====================
        // Immediate-style shape drawing on top of the ordinary sprite batch: every primitive is a
        // GENERATED MESH submitted with a transform, so these need no new shader, no new PSO and
        // no extra .cso beside the exe -- they batch alongside sprites and text like anything
        // else. Resources (unit quad, 1x1 white texture, cached N-gon meshes) are built LAZILY on
        // the first primitive call, so a project that draws none pays nothing.
        //
        // Coordinate conventions match raylib deliberately, so ported tutorial code reads the
        // same: rectangles take their TOP-LEFT corner, circles/polys take their CENTER (Submit()
        // itself is center-based; the conversion happens inside). Angles are in DEGREES.
        void DrawLine(DirectX::XMFLOAT2 start, DirectX::XMFLOAT2 end,
                      DirectX::XMFLOAT4 color, float thickness = 1.0f, int zLayer = 0);
        void DrawRectangle(DirectX::XMFLOAT2 topLeft, DirectX::XMFLOAT2 size,
                           DirectX::XMFLOAT4 color, float rotationDeg = 0.0f, int zLayer = 0);
        void DrawRectangleLines(DirectX::XMFLOAT2 topLeft, DirectX::XMFLOAT2 size,
                                DirectX::XMFLOAT4 color, float thickness = 1.0f, int zLayer = 0);
        void DrawCircle(DirectX::XMFLOAT2 center, float radius,
                        DirectX::XMFLOAT4 color, int zLayer = 0);
        void DrawCircleLines(DirectX::XMFLOAT2 center, float radius,
                             DirectX::XMFLOAT4 color, float thickness = 1.0f, int zLayer = 0);
        // sides >= 3 (3 = triangle, 4 = diamond, 6 = hexagon...). rotationDeg spins it about the
        // center. Meshes are cached per side-count, so repeated calls reuse one mesh.
        void DrawPoly(DirectX::XMFLOAT2 center, int sides, float radius,
                      float rotationDeg, DirectX::XMFLOAT4 color, int zLayer = 0);
        void DrawPolyLines(DirectX::XMFLOAT2 center, int sides, float radius,
                           float rotationDeg, DirectX::XMFLOAT4 color,
                           float thickness = 1.0f, int zLayer = 0);
        // Rounded rectangle -- the UI primitive (panels, buttons, pills). cornerRadius is in
        // PIXELS and is clamped to half the shorter side, where the shape becomes a capsule. Drawn
        // by an SDF shader, so corners stay perfectly circular at any width/height ratio and any
        // zoom; falls back to sharp corners if RoundRectPS.cso isn't beside the exe.
        void DrawRectangleRounded(DirectX::XMFLOAT2 topLeft, DirectX::XMFLOAT2 size,
                                  float cornerRadius, DirectX::XMFLOAT4 color,
                                  float rotationDeg = 0.0f, int zLayer = 0);
        void DrawRectangleRoundedLines(DirectX::XMFLOAT2 topLeft, DirectX::XMFLOAT2 size,
                                       float cornerRadius, DirectX::XMFLOAT4 color,
                                       float thickness = 1.0f, float rotationDeg = 0.0f,
                                       int zLayer = 0);
        // ---- Curves and polylines. All of these are just N DrawLine segments, so they inherit
        // pixel-width thickness and batch with everything else. `segments` trades smoothness for
        // draw calls; 24 matches raylib's default and looks smooth at typical sizes. ----
        //
        // Connects points in order (an open polyline -- pass the first point again at the end to
        // close it). Fewer than 2 points draws nothing.
        void DrawSplineLinear(const DirectX::XMFLOAT2* points, int pointCount,
                              DirectX::XMFLOAT4 color, float thickness = 1.0f, int zLayer = 0);
        // raylib-compatible S-curve: a cubic bezier whose control points are implied as
        // (end.x, start.y) and (start.x, end.y), giving the ease-in-out shape raylib draws.
        void DrawLineBezier(DirectX::XMFLOAT2 start, DirectX::XMFLOAT2 end,
                            DirectX::XMFLOAT4 color, float thickness = 1.0f,
                            int segments = 24, int zLayer = 0);
        // Explicit-control beziers, for when you want to place the curve yourself.
        void DrawLineBezierQuad(DirectX::XMFLOAT2 start, DirectX::XMFLOAT2 control,
                                DirectX::XMFLOAT2 end, DirectX::XMFLOAT4 color,
                                float thickness = 1.0f, int segments = 24, int zLayer = 0);
        void DrawLineBezierCubic(DirectX::XMFLOAT2 start, DirectX::XMFLOAT2 c1,
                                 DirectX::XMFLOAT2 c2, DirectX::XMFLOAT2 end,
                                 DirectX::XMFLOAT4 color, float thickness = 1.0f,
                                 int segments = 24, int zLayer = 0);

        // Filled triangle through three ARBITRARY points -- no constraint on the shape. Works
        // because the instance transform is a full affine map (see ObjectData::shear): three
        // points are 6 degrees of freedom, and translate(2) + rotate(1) + scale(2) + shear(1) is
        // exactly 6, so one reference mesh covers every triangle with no per-call GPU allocation
        // and no separate draw path -- these still batch with sprites and text.
        void DrawTriangle(DirectX::XMFLOAT2 a, DirectX::XMFLOAT2 b, DirectX::XMFLOAT2 c,
                          DirectX::XMFLOAT4 color, int zLayer = 0);
        void DrawTriangleLines(DirectX::XMFLOAT2 a, DirectX::XMFLOAT2 b, DirectX::XMFLOAT2 c,
                               DirectX::XMFLOAT4 color, float thickness = 1.0f, int zLayer = 0);
        // =====================================================================================

        // ---- Thin forwarding wrappers to m_Core, so existing call sites (main.cpp/Font.cpp) don't
        // need to know a RendererCore exists at all. ----
        void ExecuteUploadCommand(std::function<void(ID3D12GraphicsCommandList*)> task) { m_Core->ExecuteUploadCommand(task); }
        void UpdateGlobalUniforms(DirectX::XMFLOAT2 screenSize,
            DirectX::XMFLOAT2 cameraPos = { 0.0f, 0.0f }, float cameraZoom = 1.0f) {
            m_Core->UpdateGlobalUniforms(screenSize, cameraPos, cameraZoom);
        }
        DirectX::XMFLOAT2 GetScreenSize() const { return m_Core->GetScreenSize(); }
        DirectX::XMFLOAT2 GetNDC(float targetX, float targetY) { return m_Core->GetNDC(targetX, targetY); }
        float GetFrameTime() { return m_Core->GetFrameTime(); }
        float GetLastDeltaTime() const { return m_Core->GetLastDeltaTime(); }
        ID3D12GraphicsCommandList* GetCommandList() { return m_Core->GetCommandList(); }
        uint32_t RegisterParticleEffect(const std::wstring& updateCsPath, uint32_t maxParticles) {
            return m_Core->RegisterParticleEffect(updateCsPath, maxParticles);
        }
        void RequestSpawn(uint32_t effectID, Particle2D p) { m_Core->RequestSpawn(effectID, p); }
        void RequestSpawn(uint32_t effectID, DirectX::XMFLOAT2 basePosition, float offsetRadius,
            DirectX::XMFLOAT2 velocity, float lifetime, DirectX::XMFLOAT4 color, float size = 4.0f) {
            m_Core->RequestSpawn(effectID, basePosition, offsetRadius, velocity, lifetime, color, size);
        }
    };
};