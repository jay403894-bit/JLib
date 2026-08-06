#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <mutex>            // m_BurstMtx (RequestBurst3D is cross-task)
#include "RendererCore.h"   // RendererCore::NumFrames + GetDevice()
#include "Mesh.h"           // Mesh (vertex/index buffers a draw item points at)
#include "ResourceManager.h" // TextureHandle + Resolve().gpuHandle for per-object albedo binding

namespace JLib {
    // One light for the PBR pixel shader. Layout MUST match `Light` in Basic3D_PS.hlsl EXACTLY (four
    // float4 rows = 64 bytes). type: 0 = directional (position/range unused, `direction` = light dir),
    // 1 = point, 2 = spot. spotCos* are the cone-edge cosines (only used when type == 2).
    struct GpuLight {
        DirectX::XMFLOAT3 position;   float range;
        DirectX::XMFLOAT3 direction;  float intensity;
        DirectX::XMFLOAT3 color;      float type;
        float spotCosInner; float spotCosOuter; float pad0; float pad1;
    };
    static_assert(sizeof(GpuLight) == 64, "GpuLight must match Basic3D_PS.hlsl's Light (64 bytes)");

    // Peer of Renderer2D: the 3D-world pass. Attaches to RendererCore via SetRenderer3D() and owns
    // its own per-frame draw command list, which RendererCore::PresentFrame submits right AFTER the
    // PRE list and BEFORE the particle/2D layers -- so the 3D world sits UNDER the 2D HUD. Draws
    // with depth test+write against RendererCore's DSV (both RTV and DSV are cleared by the PRE
    // list). Currently an EMPTY pass: it binds the target + viewport and draws nothing. Step 3 adds
    // a root signature + depth-enabled PSO, a camera (view/proj) constant buffer, and a cube mesh.
    class Renderer3D {
    public:
        void Initialize(RendererCore& core);

        // Sets this frame's camera: the combined View * Projection matrix that EVERY object shares
        // (the camera doesn't change per object). Call once per frame before PresentFrame. You don't
        // pass a frame index -- RecordCommandList uploads it into the correct per-frame slot for you.
        // Per-object Model matrices are separate (set per-draw as root constants in recordItems).
        // cameraWorldPos is NEW: the PBR pixel shader needs the camera position for the view vector
        // (specular is view-dependent). Pass the same eye point you built the view matrix from.
        void SetCamera(DirectX::FXMMATRIX viewProj, DirectX::XMFLOAT3 cameraWorldPos);

        // --- Light rig (call per frame before PresentFrame, like SetCamera; uploaded at record time) ---
        // Flat ambient: one constant added to every surface regardless of which way it faces. Cheap,
        // and fine outdoors where a directional light supplies the orientation cue.
        void SetAmbient(DirectX::XMFLOAT3 ambient) { m_Ambient = ambient; m_HemiMix = 0.0f; }
        // Hemisphere ambient: `sky` lights upward-facing surfaces, `ground` lights downward-facing
        // ones, blended by the surface normal. Use this for INTERIORS -- an enclosed room blocks the
        // sun, so with flat ambient the floor, walls and ceiling all receive the identical fill and
        // the scene reads flat no matter how the shadows are tuned. A dim cool sky over a warmer,
        // dimmer ground bounce is the usual starting point. Still not GI: it responds to which way a
        // surface faces, not to what is actually near it -- occlusion is SSAO's job.
        void SetHemisphereAmbient(DirectX::XMFLOAT3 sky, DirectX::XMFLOAT3 ground, float mix = 1.0f) {
            m_Ambient = sky; m_AmbientGround = ground; m_HemiMix = mix;
        }
        void ClearLights() { m_LightCount = 0; }
        void AddLight(const GpuLight& l);   // silently ignored once kMaxLights are queued
        // Convenience builders. `dir` is the direction the light travels (world space); it's normalized here.
        void AddDirectionalLight(DirectX::XMFLOAT3 dir, DirectX::XMFLOAT3 color, float intensity);
        void AddPointLight(DirectX::XMFLOAT3 pos, DirectX::XMFLOAT3 color, float intensity, float range);
        // Cone light. `dir` is the direction it points; inner/outer are the cone half-angles in
        // DEGREES -- full brightness inside inner, falling to nothing at outer. A spot is the only
        // local light that can cast a shadow cheaply (one perspective pass, like a directional's one
        // orthographic pass); a point light would need six.
        void AddSpotLight(DirectX::XMFLOAT3 pos, DirectX::XMFLOAT3 dir, DirectX::XMFLOAT3 color,
                          float intensity, float range, float innerDeg = 20.0f, float outerDeg = 30.0f);

        // ---- Shadows (one caster: directional or spot) --------------------------------------------
        // Renders the scene once from the light's point of view into a depth map, then the pixel
        // shader tests each surface against it. Off by default -- a scene opts in, because the extra
        // pass costs a second traversal of the geometry.
        //
        // The caster may be:
        //   DIRECTIONAL -- orthographic projection, bounded by SetShadowBounds
        //   SPOT        -- one perspective projection from its position, bounded by cone and range
        //   POINT       -- SIX perspective projections into a cube map, one per axis direction
        // A point caster costs six depth passes instead of one, which is why engines cap shadow-
        // casting point lights aggressively. Those six passes are independent, so they're recorded
        // in parallel on the scheduler rather than serially on the main thread.
        //
        // By default the caster is auto-picked: the first directional light, else the first spot.
        // SetShadowCaster pins a specific light index when a scene has several and wants a particular
        // one casting.
        void SetShadowCaster(int lightIndex) { m_ShadowCasterOverride = lightIndex; }
        //
        // `center` / `extent` define the box the shadow map covers, in world units. A directional
        // light has no position, so something has to bound where shadows are computed; everything
        // outside this box renders unshadowed rather than wrong. SMALLER IS SHARPER -- the same
        // texture stretched over a smaller area means more texels per metre, so fit it to the play
        // area rather than the whole level (a 40-unit box on a 2048 map is ~51 texels/unit).
        void EnableShadows(bool on) { m_ShadowsEnabled = on; }

        // Screen-space ambient occlusion. Darkens AMBIENT only (never direct light), so it deepens
        // creases, contact points and recesses without touching what the sun or a lamp lights. This is
        // the pass that makes an INTERIOR read: hemisphere ambient varies with which way a surface
        // faces, but a column base and the open floor beside it share a normal and only differ in what
        // is NEAR them. Costs a depth prepass plus one fullscreen pass; off by default.
        // Image-based lighting from an HDR environment map (equirectangular .hdr / .exr -- Poly Haven
        // is the usual CC0 source). Bakes an irradiance cube and a prefiltered specular chain ONCE,
        // synchronously, then ambient comes from the environment instead of a constant: fill light
        // that varies with direction, and metals that actually reflect something.
        //
        // Returns false if the file is missing or can't be decoded, leaving the hemisphere/flat path
        // in place -- so a scene shipped without the asset still renders.
        //
        // Caveat worth knowing: one environment map assumes every surface can see the sky. That is
        // right outdoors and wrong inside an enclosed room, where the correct answer is probes or
        // baked lightmaps. Expect interiors to read too bright until those exist.
        bool LoadEnvironment(const std::wstring& hdrPath) { return BakeEnvironment(hdrPath); }
        bool HasEnvironment() const { return m_EnvReady; }

        void EnableSSAO(bool on) { m_SsaoEnabled = on; }
        // radius    -- world units; the SCALE of detail it finds. Roughly the size of the gap you want
        //              darkened. Too small and it only shades hairline seams; too large and it turns
        //              into a soft global dimming that reads as haze.
        // intensity -- multiplies the occlusion. 1.0 is the physical-ish baseline; interiors usually
        //              want a bit more.
        void SetSSAOParams(float radius, float intensity, float bias = 0.025f) {
            m_SsaoRadius = radius; m_SsaoIntensity = intensity; m_SsaoBias = bias;
        }
        bool ShadowsEnabled() const { return m_ShadowsEnabled; }
        void SetShadowBounds(DirectX::XMFLOAT3 center, float extent) {
            m_ShadowCenter = center; m_ShadowExtent = extent > 1.0f ? extent : 1.0f;
        }

        // Queue one mesh to be drawn this frame with the given world transform. Like Renderer2D's
        // Submit, the app calls this each frame for every object; RecordCommandList draws them all
        // and then clears the queue. `mesh` must outlive the frame (the app owns it) -- we only
        // keep a pointer, not a copy.
        // The mesh carries its own Material (albedo) now, so no texture is passed here -- a loaded
        // asset is self-describing. Set mesh.material.albedo once after creation, then just Submit.
        void Submit(const Mesh& mesh, DirectX::FXMMATRIX model);

        // Queue a SKINNED mesh (a SkinnedModel::mesh, built from SkinnedVertex3D) for this frame.
        // Recorded in a separate pass with the skinning PSO/root sig. For now the bone palette is
        // IDENTITY (bind pose) -- the verification step; a per-instance animated palette argument
        // is added with animation. The mesh's material.albedo is bound the same as the static path.
        void SubmitSkinned(const Mesh& mesh, DirectX::FXMMATRIX model,
                           const std::vector<DirectX::XMFLOAT4X4>& bonePalette);

        // Records this frame's 3D pass into m_DrawList: binds the back-buffer RTV + depth DSV, sets
        // a full-window viewport/scissor, draws (nothing yet), and Closes. Called by
        // RendererCore::PresentFrame, which passes the current back buffer's RTV, the depth DSV, and
        // the client size.
        void RecordCommandList(int frame,
                               D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                               D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                               uint32_t width, uint32_t height);

        ID3D12CommandList* GetCommandList() const { return m_DrawList[0].Get(); }  // legacy; PresentFrame uses GetRecordedLists()

        // Parallel command-list recording: the static draws are split across scheduler workers, each
        // recording its OWN command list. RendererCore::PresentFrame executes this whole set instead of
        // a single list. Toggle at runtime to A/B against serial; GetLastRecordMs is the record wall time.
        const std::vector<ID3D12CommandList*>& GetRecordedLists() const { return m_RecordedLists; }
        void   SetParallelRecording(bool on) { m_ParallelRecording = on; }
        bool   IsParallelRecording() const { return m_ParallelRecording; }
        double GetLastRecordMs() const { return m_LastRecordMs; }

        // Real draw-call count = number of instanced batches issued last frame (NOT the object count --
        // instancing collapses many objects into one DrawIndexedInstanced). GetLastInstanceCount is the
        // total objects drawn across all batches.
        size_t GetLastDrawCallCount() const { return m_LastBatchCount; }
        size_t GetLastInstanceCount() const { return m_LastInstanceCount; }

        // Frustum culling: objects whose world-space bounding sphere is fully outside the camera frustum
        // are rejected at Submit time (never batched/drawn). GetLastCulledCount is how many were skipped
        // last frame -- pair it with GetLastInstanceCount to see the win. Toggle to A/B against no culling.
        void   SetCullingEnabled(bool on) { m_CullingEnabled = on; }
        bool   IsCullingEnabled() const   { return m_CullingEnabled; }
        size_t GetLastCulledCount() const { return m_LastCulledCount; }

        // A/B profiler for the parallel-vs-serial crossover: when ON, RecordCommandList ALTERNATES the two
        // modes every frame and keeps a rolling average (EMA) of the record time for each. Read both at the
        // current scene's batch count (GetLastDrawCallCount) to see which wins -- vary scene complexity to
        // find the batch count where parallel starts beating serial (the threshold to gate on). Overrides
        // SetParallelRecording while active.
        void   SetAutoBenchmark(bool on) { m_AutoBench = on; }
        bool   IsAutoBenchmark() const   { return m_AutoBench; }
        double GetSerialAvgMs() const     { return m_SerialAvgMs; }
        double GetParallelAvgMs() const   { return m_ParallelAvgMs; }

        // Procedural skybox: a VISUAL-ONLY gradient sky drawn behind the world (contributes nothing to
        // lighting). On by default; toggle to compare against a plain cleared background.
        void SetSkyEnabled(bool on) { m_SkyEnabled = on; }
        bool IsSkyEnabled() const   { return m_SkyEnabled; }

        // --- 3D GPU particles (PHASE 1) ------------------------------------------------------------------
        // A self-recycling fountain: simulated in a compute pass and drawn as camera-facing billboards in
        // the 3D pass (depth-tested, so geometry occludes them). Register an emitter ONCE (after Initialize);
        // it then runs every frame. Compute currently rides the GRAPHICS queue; the async compute queue +
        // real spawn/dead-list machinery are phase 2. All world-space units.
        struct ParticleEmitterDesc {
            DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
            float             spread   = 0.6f;                       // 0 = tight jet; larger = wider cone
            float             speed    = 6.0f;                       // base launch speed
            float             gravity  = -9.8f;                      // added to velocity.y each tick
            float             lifetime = 2.0f;                       // seconds per particle
            float             size     = 0.15f;                      // billboard half-extent
            DirectX::XMFLOAT4 color    = { 0.6f, 0.8f, 1.0f, 1.0f }; // spawn color (fades to transparent by age)
            uint32_t          count    = 4096;                       // pool size (particles alive at once)
        };
        using ParticleEmitterHandle = size_t;
        ParticleEmitterHandle AddParticleEmitter(const ParticleEmitterDesc& desc);

        // --- Burst pools (PHASE 2A): on-demand one-shot puffs (impact effects), vs the always-on fountain.
        // A RING buffer: the CPU owns the ring head, RequestBurst3D reserves [head, head+count) and a tiny
        // spawn CS fills those slots next compute pass; particles die by lifetime and STAY dead (no respawn)
        // until the ring wraps over them. Size capacity >= peak bursts/sec x count x lifetime so wraps only
        // ever overwrite dead slots. Same async-compute + drawBuffer path as the fountain. Create pools at
        // init (before the frame loop); RequestBurst3D may be called from any frame task that runs BEFORE
        // RendererCore::UpdateParticles in the frame DAG (it's mutex-guarded, cheap).
        ParticleEmitterHandle AddBurstPool(uint32_t capacity, float gravity = -9.8f);
        struct BurstDesc {
            DirectX::XMFLOAT3 position   = { 0.0f, 0.0f, 0.0f };
            DirectX::XMFLOAT3 normal     = { 0.0f, 1.0f, 0.0f };  // burst direction (e.g. contact normal)
            uint32_t          count      = 24;
            float             speed      = 3.0f;
            float             lifetime   = 0.6f;
            float             size       = 0.08f;
            float             normalBias = 0.6f;                   // 0 = isotropic puff, 1 = fully along normal
            DirectX::XMFLOAT4 color      = { 1.0f, 0.8f, 0.4f, 1.0f };
        };
        void RequestBurst3D(ParticleEmitterHandle pool, const BurstDesc& burst);

        void SetParticlesEnabled(bool on) { m_ParticlesEnabled = on; }
        bool AreParticlesEnabled() const  { return m_ParticlesEnabled; }
        // Records the 3D particle COMPUTE (integrate each emitter + snapshot buffer->drawBuffer[frame]) onto
        // the caller's ASYNC-COMPUTE command list. Called by RendererCore::UpdateParticles so it rides the
        // SAME compute queue + m_ComputeFence as the 2D pools -- NOT for app use. `dt` is the frame delta.
        void RecordParticleCompute(ID3D12GraphicsCommandList* computeList, int frame, float dt);

    private:
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature();
        void CreatePipelineState();   // bakes shaders + Vertex3D layout + depth-on into m_PSO
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateSkinnedRootSignature(); // adds b2 bone palette
        void CreateSkinnedPipelineState();   // SkinnedVertex3D layout + Skinned3D_VS + Basic3D_PS -> m_SkinnedPSO
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateSkyRootSignature();   // b0 = InvViewProj + camPos
        void CreateSkyPipeline();            // Skybox_VS/PS, no input layout, depth OFF -> m_SkyPSO
        // Records the fullscreen sky pass into m_SkyList[frame] (RTV only, no depth). Pushed FIRST into
        // m_RecordedLists so it draws before the geometry lists (which then overwrite it where depth wins).
        void RecordSkybox(int frame, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height);
        // --- 3D particle pipelines + record (phase 1) ---
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateParticleComputeRootSignature(); // u0 buffer + b0 SimCB
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateParticleDrawRootSignature();    // b0 cam + t0 buffer + b1 params
        void CreateParticleComputePipeline();  // UpdateParticles3D.cso -> m_ParticleComputePSO
        void CreateParticleDrawPipeline();      // Particle3D_VS/PS (alpha blend, depth test/no-write) -> m_ParticleDrawPSO
        // Records this frame's particle pass: one compute Dispatch per emitter (writes its buffer), a
        // UAV->SRV transition, then a billboard DrawInstanced per emitter into the 3D RTV+DSV, then SRV->UAV
        // back. All on the graphics queue in one list; pushed AFTER the geometry so it depth-tests against it.
        void RecordParticles(int frame, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                             uint32_t width, uint32_t height);

        // One queued object: a mesh + where it goes in the world. Model is stored as plain floats
        // (not XMMATRIX) so it can be handed straight to SetGraphicsRoot32BitConstants.
        struct DrawItem {
            const Mesh* mesh;
            DirectX::XMFLOAT4X4 model;
            // All material textures resolved ONCE at Submit (main thread) -- resolving per-draw in the
            // record loop locks the AssetManager mutex per object and serializes the parallel workers.
            // Absent/not-ready maps get `albedo` as a harmless filler (a valid bound texture); mapFlags
            // tells the shader which of metalRough/emissive/occlusion are REAL vs filler (bit0/1/2).
            D3D12_GPU_DESCRIPTOR_HANDLE albedo;
            D3D12_GPU_DESCRIPTOR_HANDLE metalRough;
            D3D12_GPU_DESCRIPTOR_HANDLE emissive;
            D3D12_GPU_DESCRIPTOR_HANDLE occlusion;
            D3D12_GPU_DESCRIPTOR_HANDLE normal;      // t4: normal map (filler = albedo when absent)
            uint32_t mapFlags;
        };
        std::vector<DrawItem> m_Items;
        // One instanced draw: every queued object sharing this exact mesh + albedo. Their model matrices
        // sit contiguously in m_InstanceBuffer at [firstInstance, firstInstance+count), so a single
        // DrawIndexedInstanced(mesh->indexCount, count, 0, 0, firstInstance) draws the whole group.
        // Built each frame in RecordCommandList by sorting m_Items on (mesh, albedo) and finding runs.
        struct InstanceBatch {
            const Mesh* mesh;
            D3D12_GPU_DESCRIPTOR_HANDLE albedo;
            D3D12_GPU_DESCRIPTOR_HANDLE metalRough;   // material maps are per-mesh, so all instances in a
            D3D12_GPU_DESCRIPTOR_HANDLE emissive;     // batch share these -- copied from the batch's first item
            D3D12_GPU_DESCRIPTOR_HANDLE occlusion;
            D3D12_GPU_DESCRIPTOR_HANDLE normal;       // t4: normal map
            uint32_t mapFlags;
            uint32_t firstInstance;
            uint32_t count;
        };
        std::vector<InstanceBatch> m_Batches;
        // A skinned draw carries its own bone palette (this instance's animated pose), copied here so
        // the caller's vector needn't outlive the frame. Uploaded to a per-instance region of m_BoneCB.
        struct SkinnedItem {
            const Mesh* mesh;
            DirectX::XMFLOAT4X4 model;
            std::vector<DirectX::XMFLOAT4X4> palette;
            D3D12_GPU_DESCRIPTOR_HANDLE albedo;      // material maps resolved once at SubmitSkinned
            D3D12_GPU_DESCRIPTOR_HANDLE metalRough;  // (see DrawItem) -- filler = albedo when absent
            D3D12_GPU_DESCRIPTOR_HANDLE emissive;
            D3D12_GPU_DESCRIPTOR_HANDLE occlusion;
            D3D12_GPU_DESCRIPTOR_HANDLE normal;      // t4: normal map
            uint32_t mapFlags;
        };
        std::vector<SkinnedItem> m_SkinnedItems;

        // Records draw calls for items [start, end) into `cmd`. This is the RANGE-BASED core: the
        // serial path (RecordCommandList) calls it once with the whole range; the future parallel
        // path calls it on each worker's OWN command list with that worker's slice -- SAME code,
        // which is exactly why the cube is built this way from the start. Assumes the caller has
        // already bound the PSO, root signature, camera CBV, and render targets on `cmd`.
        // Records instanced draws for BATCHES [startBatch, endBatch) into `cmd` (m_Batches is built in
        // RecordCommandList). Assumes the caller already bound the PSO, root sig, camera CBV, render
        // targets, AND the per-frame instance buffer at vertex slot 1. One DrawIndexedInstanced per
        // batch -- still lock-free (albedo pre-resolved at Submit; no shared lock touched here).
        void recordBatches(ID3D12GraphicsCommandList* cmd, size_t startBatch, size_t endBatch);
        // Skinned peer of recordItems: assumes the skinning PSO/root sig + camera (b0) + bone palette
        // (b2) are already bound on `cmd`; binds per-object model (b1) + albedo table (t0=param 3).
        void recordSkinnedItems(ID3D12GraphicsCommandList* cmd, size_t start, size_t end, int frame);

        // --- parallel recording ---
        struct RecordWorker {   // one per scheduler task: per-frame allocators AND per-frame lists
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    alloc[RendererCore::NumFrames];
            // PER-FRAME lists (not one shared): a single list Reset every frame gets Reset while the GPU
            // is still executing the PREVIOUS frame's recording of it under heavy load -> torn list memory
            // -> null-VA draw fault (DEVICE_HUNG). Per-frame slot is fence-gated, so never reset in flight.
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list[RendererCore::NumFrames];
        };
        struct RecordTaskCtx {  // args for one range-record task (POD; lives in m_RecordCtxs across WaitFor)
            Renderer3D* self;
            ID3D12GraphicsCommandList* list;
            ID3D12CommandAllocator*    alloc;
            size_t start, end;
            int frame;
            D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
            uint32_t width, height;
            ID3D12DescriptorHeap* srvHeap;
            ResourceManager* rm;
        };
        static void RecordRangeTask(void* p);        // scheduler entry point -> self->RecordRange(*ctx)
        void RecordRange(const RecordTaskCtx& c);    // reset+bind one list, recordItems its slice, Close
        void RecordSkinnedList(int frame, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                               uint32_t width, uint32_t height, ID3D12DescriptorHeap* srvHeap);

        std::vector<RecordWorker>       m_Workers;        // sized to (hw-1) in Initialize
        std::vector<RecordTaskCtx>      m_RecordCtxs;     // per-frame task contexts (outlive the WaitFor)
        std::vector<ID3D12CommandList*> m_RecordedLists;  // what PresentFrame executes this frame
        // Records the mode actually used last frame (for the HUD/getter). Normally AUTO-selected each
        // frame by RecordCommandList: serial below the measured crossover, parallel above it. The
        // benchmark forces this to alternate; otherwise RecordCommandList overwrites it with its decision.
        bool   m_ParallelRecording = false;
        // Measured parallel-vs-serial crossover: instanced batches are so cheap to record that serial
        // wins until ~15-16k of them -- below that, the parallel dispatch overhead (task creation + N
        // command-list Reset/Close + WaitGroup sync) outweighs the work. Above it, splitting wins. No real
        // scene has this many DISTINCT (mesh,material) batches, so in practice this is always serial.
        static constexpr size_t kParallelBatchThreshold = 15000;
        double m_LastRecordMs = 0.0;
        double m_QpcFreq      = 1.0;   // QueryPerformanceFrequency, cached in Initialize
        size_t m_LastBatchCount    = 0;   // instanced draw calls issued last frame (== m_Batches.size())
        size_t m_LastInstanceCount = 0;   // objects drawn last frame (across all batches)
        // Auto-benchmark state (see SetAutoBenchmark): alternate mode each frame, EMA per mode.
        bool   m_AutoBench       = false;
        bool   m_BenchUseParallel = false;   // which mode THIS auto-bench frame runs (flips each frame)
        double m_SerialAvgMs     = 0.0;
        double m_ParallelAvgMs   = 0.0;

        ID3D12Device2* m_Device = nullptr;
        RendererCore*  m_Core   = nullptr;  // for GetRenderer2D()->GetSrvHeap()/GetResourceManager() at record
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_Allocators[RendererCore::NumFrames];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_DrawList[RendererCore::NumFrames];   // serial static path (per-frame)
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_SkinnedAllocators[RendererCore::NumFrames];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_SkinnedList[RendererCore::NumFrames]; // skinned pass (per-frame)
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_SkyAllocators[RendererCore::NumFrames];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_SkyList[RendererCore::NumFrames];      // procedural sky pass (per-frame)

        // The "shape" of a 3D draw: which resources the shaders can read. Two slots --
        //   b0 = camera constant buffer (View*Projection), shared by every object this frame
        //   b1 = 16 root constants = one Model matrix, pushed per-draw (cheap, no descriptor)
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_PSO;   // the baked 3D pipeline (see CreatePipelineState)
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_SkinnedRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_SkinnedPSO;   // the skinned pipeline (bone-blended VS)
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_SkyRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_SkyPSO;       // fullscreen procedural sky (depth off)
        bool m_SkyEnabled = true;

        // --- 3D particles (phase 1) ---
        // Per-frame CB layouts (MUST match the particle shaders). SimCB = sim + emitter params (compute b0);
        // ParticleCamCB = ViewProj + camera world right/up for the billboards (draw b0).
        struct SimCB {
            float dt; float time; uint32_t maxParticles; float gravity;
            DirectX::XMFLOAT3 emitterPos; float spread;
            float speed; float lifetime; float size;
            uint32_t respawn;   // 1 = fountain (recycle at emitter), 0 = burst pool (dead stays dead)
            DirectX::XMFLOAT4 color;
        };
        // Root constants for SpawnBurst3D.hlsl (20 DWORDs) -- MUST match its BurstCB layout exactly.
        struct BurstConsts {
            float pos[3];    uint32_t startSlot;
            float normal[3]; uint32_t count;
            uint32_t poolSize; float speed; float lifetime; float size;
            float color[4];
            float normalBias; uint32_t seed; float pad[2];
        };
        struct ParticleCamCB {
            DirectX::XMFLOAT4X4 viewProj;
            DirectX::XMFLOAT3 camRight; float _p0;
            DirectX::XMFLOAT3 camUp;    float _p1;
        };
        struct BurstReq { BurstDesc d; uint32_t startSlot; };
        struct ParticleEmitter {
            ParticleEmitterDesc desc;
            bool     isBurst  = false;   // burst pool: update runs with respawn=0; SpawnBurst3D fills slots
            uint32_t ringHead = 0;       // CPU-owned ring cursor (single logical writer, see RequestBurst3D)
            std::vector<BurstReq> pending;   // bursts requested this frame; drained in RecordParticleCompute
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;                       // DEFAULT-heap RWStructuredBuffer<Particle3D>; COMPUTE-QUEUE ONLY now
            // Per-frame SNAPSHOT the GRAPHICS queue draws from (mirrors the 2D pools' drawBuffer): the compute
            // queue integrates `buffer` in place and copies it here, so graphics never touches `buffer` and the
            // two queues can overlap. DEFAULT heap, NO UAV, rests in COMMON (copy-dest + SRV-read only).
            Microsoft::WRL::ComPtr<ID3D12Resource> drawBuffer[RendererCore::NumFrames];
            Microsoft::WRL::ComPtr<ID3D12Resource> simCB[RendererCore::NumFrames];
            void* simCBMapped[RendererCore::NumFrames] = {};
        };
        std::vector<ParticleEmitter> m_Emitters;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_ParticleComputeRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ParticleComputePSO;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_BurstSpawnRootSig;   // u0 + 20 root constants (BurstConsts)
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_BurstSpawnPSO;       // SpawnBurst3D.cso
        std::mutex m_BurstMtx;   // guards every emitter's pending/ringHead (RequestBurst3D vs compute record)
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_ParticleDrawRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ParticleDrawPSO;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_ParticleAllocators[RendererCore::NumFrames];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_ParticleList[RendererCore::NumFrames];
        Microsoft::WRL::ComPtr<ID3D12Resource> m_ParticleCamCB[RendererCore::NumFrames];
        void* m_ParticleCamCBMapped[RendererCore::NumFrames] = {};
        bool    m_ParticlesEnabled = true;
        float   m_ParticleTime     = 0.0f;   // running clock (seeds the respawn hash); advanced in RecordParticleCompute

        // Camera constant buffer, one per frame slot (like RendererCore::m_ConstantBuffers): an
        // UPLOAD-heap buffer kept persistently mapped so SetCamera() is just a memcpy. Holds one
        // float4x4. D3D12 requires CBs be 256-byte aligned, so each is a full 256-byte buffer even
        // though the matrix is only 64.
        Microsoft::WRL::ComPtr<ID3D12Resource> m_CameraCB[RendererCore::NumFrames];
        void* m_CameraCBMapped[RendererCore::NumFrames] = {};
        // Sky CB per frame: inverse ViewProj (for the view-ray unprojection) + camPos. 256-byte aligned like
        // the camera CB; RecordCommandList uploads it before recording the sky pass.
        Microsoft::WRL::ComPtr<ID3D12Resource> m_SkyCB[RendererCore::NumFrames];
        void* m_SkyCBMapped[RendererCore::NumFrames] = {};
        DirectX::XMFLOAT4X4 m_ViewProj;   // this frame's camera, set by SetCamera, uploaded at record time
        DirectX::XMFLOAT3   m_CamPos = { 0.0f, 0.0f, 0.0f };  // camera world pos (b0, PS view vector)

        // Frustum culling state. The 6 world-space frustum planes are re-extracted from ViewProj in
        // SetCamera; Submit/SubmitSkinned test each object's world bounding sphere against them. Planes are
        // (a,b,c,d) normalized, "inside" = a*x+b*y+c*z+d >= -radius. See CullSphere() in the .cpp.
        DirectX::XMFLOAT4 m_FrustumPlanes[6] = {};
        bool   m_FrustumValid   = false;   // false until SetCamera runs once (cull nothing before then)
        bool   m_CullingEnabled = true;
        size_t m_CulledThisFrame = 0;      // accumulates across this frame's Submits
        size_t m_LastCulledCount = 0;      // snapshot of the above, taken in RecordCommandList
        // True if the mesh's world bounding sphere is at least partially inside the frustum (i.e. keep it).
        bool CullSphere(DirectX::FXMMATRIX model, const Mesh& mesh) const;

        // Light rig, uploaded to m_LightsCB (b3) each frame. Mirrors the Lights cbuffer in Basic3D_PS.hlsl.
        static constexpr uint32_t kMaxLights = 16;    // MUST match MAX_LIGHTS in Basic3D_PS.hlsl
        DirectX::XMFLOAT3 m_Ambient = { 0.03f, 0.03f, 0.03f };   // doubles as the SKY colour when m_HemiMix > 0
        DirectX::XMFLOAT3 m_AmbientGround = { 0.03f, 0.03f, 0.03f };
        float m_HemiMix = 0.0f;   // 0 = flat constant ambient (legacy default), 1 = full hemisphere
        GpuLight m_Lights[kMaxLights] = {};
        uint32_t m_LightCount = 0;
        // MUST match `cbuffer Lights : register(b3)` in Basic3D_PS.hlsl field-for-field. The shadow
        // fields are APPENDED after the array so the existing layout is untouched.
        struct GpuLightsCB {
            DirectX::XMFLOAT3 ambient; uint32_t count;
            GpuLight lights[kMaxLights];
            DirectX::XMFLOAT4X4 lightViewProj;   // the caster's camera (what the depth map was drawn with)
            int32_t shadowIndex;                 // which light casts; -1 = shadows off
            float   shadowTexel;                 // 1 / shadow map resolution (PCF tap spacing)
            float   shadowNormalOffset;          // ~one shadow texel in WORLD units (anti-acne offset)
            int32_t shadowIsPoint;               // 1 = sample the CUBE map instead of the 2D map
            // Point-caster extras: the cube's projection near/far, needed to reproduce a face's
            // projected depth from a world-space direction (see ShadowFactorCube).
            DirectX::XMFLOAT3 shadowLightPos;  float shadowFar;
            float   shadowNear;                float _pad[3];
            // Hemisphere ambient (see SetHemisphereAmbient). `ambient` above doubles as the sky
            // colour; hemiMix = 0 means "ignore these and use flat ambient", which is the default.
            DirectX::XMFLOAT3 ambientGround;   float hemiMix;
            // SSAO: ssaoTexel = 1/screen size (blur tap spacing); ssaoOn = 0 skips the taps.
            DirectX::XMFLOAT2 ssaoTexel;       float ssaoOn;   float _pad2;
            // IBL: iblOn = 0 -> the shader falls back to hemisphere/flat ambient.
            float iblOn;  float iblMipCount;   float _pad3[2];
        };
        Microsoft::WRL::ComPtr<ID3D12Resource> m_LightsCB[RendererCore::NumFrames];
        void* m_LightsCBMapped[RendererCore::NumFrames] = {};

        // ---- Shadow map (see EnableShadows) ----
        // 2048 is the sweet spot here: 16MB of depth, and combined with a tight SetShadowBounds it
        // gives crisp contact shadows without cascades. Cascaded shadow maps are the answer for
        // large outdoor scenes; they're overkill while a scene fits in a few tens of metres.
        static constexpr UINT kShadowMapSize = 2048;
        bool               m_ShadowsEnabled = false;
        DirectX::XMFLOAT3  m_ShadowCenter = { 0.0f, 0.0f, 0.0f };
        float              m_ShadowExtent = 40.0f;
        int                m_ShadowCasterOverride = -1;   // -1 = auto-pick (see SetShadowCaster)
        DirectX::XMFLOAT4X4 m_LightViewProj = {};
        int32_t            m_ShadowLightIndex = -1;          // which gLights entry casts this frame
        Microsoft::WRL::ComPtr<ID3D12Resource>       m_ShadowMap;        // D32 depth, also read as an SRV
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_ShadowDsvHeap;    // 1 DSV for the depth pass
        D3D12_GPU_DESCRIPTOR_HANDLE                  m_ShadowSrvGpu = {}; // t5 handle in the shared SRV heap
        Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_ShadowPso;        // depth-only, no pixel shader
        Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_ShadowRootSig;    // just b0 (the light matrix)
        Microsoft::WRL::ComPtr<ID3D12Resource>       m_ShadowCB[RendererCore::NumFrames];
        void*                                        m_ShadowCBMapped[RendererCore::NumFrames] = {};
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_ShadowAlloc[RendererCore::NumFrames];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_ShadowList[RendererCore::NumFrames];
        void CreateShadowResources();
        void RecordShadowPass(int frame);

        // ---- SSAO (see EnableSSAO) ----
        // Runs as two passes ahead of the geometry pass: a camera DEPTH PREPASS (reusing the shadow
        // pass's depth-only shader and root signature, just with the camera's matrix instead of the
        // light's), then a fullscreen pass that turns that depth into an occlusion factor the geometry
        // pass multiplies into its ambient term.
        //
        // It owns its own depth texture rather than sharing RendererCore's: the core's buffer is
        // written by the 3D pass and then by Renderer2D/ImGui on top, and its state machine is tuned
        // for that. A private one keeps this pass self-contained -- the same reasoning that gave the
        // shadow map its own. The prepass costs one extra geometry replay, and pays some of that back
        // by giving the geometry pass early-Z.
        bool  m_SsaoEnabled   = false;
        float m_SsaoRadius    = 0.5f;    // world units -- the SCALE of crevice it finds (see SetSSAOParams)
        float m_SsaoBias      = 0.025f;
        float m_SsaoIntensity = 1.0f;
        Microsoft::WRL::ComPtr<ID3D12Resource>       m_SsaoDepth;        // R32_TYPELESS: DSV to write, SRV to read
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SsaoDepthDsvHeap;
        D3D12_GPU_DESCRIPTOR_HANDLE                  m_SsaoDepthSrvGpu = {};
        Microsoft::WRL::ComPtr<ID3D12Resource>       m_SsaoTarget;       // R8_UNORM occlusion factor
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SsaoRtvHeap;
        D3D12_GPU_DESCRIPTOR_HANDLE                  m_SsaoSrvGpu = {};  // t7 in the geometry pass
        Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_SsaoPso;
        Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_SsaoRootSig;
        Microsoft::WRL::ComPtr<ID3D12Resource>       m_SsaoCB[RendererCore::NumFrames];
        void*                                        m_SsaoCBMapped[RendererCore::NumFrames] = {};
        Microsoft::WRL::ComPtr<ID3D12Resource>       m_PrepassCB[RendererCore::NumFrames];
        void*                                        m_PrepassCBMapped[RendererCore::NumFrames] = {};
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_SsaoAlloc[RendererCore::NumFrames];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_SsaoList[RendererCore::NumFrames];
        UINT m_SsaoWidth = 0, m_SsaoHeight = 0;   // rebuilt on resize
        void CreateSsaoResources(UINT width, UINT height);
        void RecordSsaoPass(int frame);           // depth prepass + the occlusion pass, one list

        // ---- IBL (see LoadEnvironment) ----
        // Baked ONCE at load, never per frame: equirectangular HDR -> environment cube -> a cosine-
        // convolved irradiance cube (diffuse) + a roughness mip chain (specular). At shading time that
        // is two texture reads, which is what makes image-based lighting affordable at all.
        static constexpr UINT kEnvCubeSize    = 512;   // source environment, mipped for prefiltering
        static constexpr UINT kIrradianceSize = 32;    // cosine irradiance is very low-frequency
        static constexpr UINT kPrefilterSize  = 128;   // mip 0; the chain runs down from here
        bool m_EnvReady      = false;
        UINT m_PrefilterMips = 1;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_EnvCube;        // full-res environment (intermediate)
        Microsoft::WRL::ComPtr<ID3D12Resource> m_IrradianceCube;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_PrefilterCube;
        // Irradiance and prefiltered are allocated BACK-TO-BACK in the shared SRV heap so one
        // descriptor range (t8..t9) covers both; only the first handle is needed to bind them.
        D3D12_GPU_DESCRIPTOR_HANDLE m_IrradianceSrvGpu = {};
        // The equirect source is KEPT after the bake, not discarded with the other intermediates: the
        // visible skybox samples it directly at full resolution. Drawing the sky from the baked cube
        // instead would stretch a 512-texel face across the screen and look smeared.
        Microsoft::WRL::ComPtr<ID3D12Resource> m_EquirectTex;
        D3D12_GPU_DESCRIPTOR_HANDLE m_EquirectSrvGpu = {};
        float m_SkyIntensity = 1.0f;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_BakeRootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_EquirectPso, m_IrradiancePso, m_PrefilterPso;
        bool BakeEnvironment(const std::wstring& hdrPath);

        // ---- Point-light shadows: a depth CUBE map, six faces rendered from the light along
        // +X,-X,+Y,-Y,+Z,-Z with a 90-degree perspective (six 90-degree frustums exactly tile a
        // sphere). Six DSVs (one per face slice) but ONE SRV over the whole cube, because the pixel
        // shader samples it by DIRECTION rather than by face. ----
        static constexpr UINT kCubeShadowSize = 1024;   // per face; 6 faces at 1024 = 24MB of depth
        Microsoft::WRL::ComPtr<ID3D12Resource>       m_CubeShadowMap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_CubeDsvHeap;      // 6 DSVs, one per face
        D3D12_GPU_DESCRIPTOR_HANDLE                  m_CubeSrvGpu = {};  // t6, the whole cube
        Microsoft::WRL::ComPtr<ID3D12Resource>       m_CubeCB[RendererCore::NumFrames];
        void*                                        m_CubeCBMapped[RendererCore::NumFrames] = {};
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_CubeAlloc[RendererCore::NumFrames][6];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CubeList[RendererCore::NumFrames][6];
        bool  m_ShadowIsPoint = false;
        float m_ShadowFar = 25.0f, m_ShadowNear = 0.1f;
        void CreateCubeShadowResources();
        void RecordCubeShadowPass(int frame);
        // One face's worth of work, so the six can run as scheduler tasks instead of serially.
        struct CubeFaceCtx { Renderer3D* self; int frame; int face; };
        static void RecordCubeFaceTask(void* data);
        void RecordCubeFace(int frame, int face);
        CubeFaceCtx m_CubeFaceCtx[6] = {};

        // Per-frame INSTANCE buffer: an UPLOAD-heap array of model matrices (one XMFLOAT4X4 per queued
        // object), bound as vertex slot 1 and read per-instance by Basic3D_VS (INSTMAT0..3). Persistently
        // mapped like m_CameraCB, so writing this frame's matrices is a plain memcpy. kMaxInstances caps
        // how many objects a single frame can draw; anything past the cap is dropped that frame.
        static constexpr UINT kMaxInstances = 65536;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_InstanceBuffer[RendererCore::NumFrames];
        void* m_InstanceMapped[RendererCore::NumFrames] = {};

        // Bone palette constant buffer, one per frame slot: kMaxBones row_major float4x4 the skinning VS
        // reads at b2. Filled with IDENTITY at Initialize (bind-pose verification) and left there for
        // now; animation will re-upload per-instance poses here. kMaxBones MUST match Skinned3D_VS.hlsl.
        static constexpr UINT kMaxBones = 128;             // MUST match Skinned3D_VS.hlsl MAX_BONES
        static constexpr UINT kMaxSkinnedInstances = 16;   // per-frame bone CB is split into this many regions
        Microsoft::WRL::ComPtr<ID3D12Resource> m_BoneCB[RendererCore::NumFrames];
        void* m_BoneCBMapped[RendererCore::NumFrames] = {};
    };
}
