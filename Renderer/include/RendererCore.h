#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <d3dx12.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <vector>
#include <functional>
#include <GetTime.h> // JLib::Time -- QueryPerformanceCounter-backed clock, shared with GetFrameTime()
#include "ResourceManager.h" // TextureHandle -- carried by value, needs the full definition
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx12.h"
namespace JLib {
    // One particle, shared GPU-buffer layout across every .hlsl behavior/spawn shader and every
    // particle pool. MUST stay in sync with the Particle struct in UpdateParticles.hlsl,
    // WaveParticles.hlsl, SpawnParticles.hlsl, and ParticleVS.hlsl (via ParticleCommon.hlsli).
    struct Particle2D
    {
        DirectX::XMFLOAT2 position;
        DirectX::XMFLOAT2 velocity;
        float lifetime;
        // Seconds since this particle last (re)spawned -- lets per-particle behavior branch on how
        // long it's been alive. Nothing branches on it yet outside the update shaders themselves.
        float age;
        uint32_t isActive; // 1 if alive, 0 if dead
        // Quad side length in pixels -- ParticleVS.hlsl now draws a procedural billboarded quad
        // (SV_VertexID-generated corners) sized by this instead of a fixed-size POINTLIST dot
        // (D3D12 point-list topology has no size or texture-coordinate support at all, so it
        // couldn't ever have supported this -- this field is what actually made size the property
        // of a per-particle draw rather than a GPU/driver-fixed 1px dot).
        float size;
        // Per-particle color -- ParticleVS.hlsl reads this instead of a hardcoded color, so each
        // RequestSpawn() call can look different.
        DirectX::XMFLOAT4 color;
    };

    class Renderer2D; // forward decl -- PresentFrame calls into it to collect the frame's draw lists
    class Renderer3D; // forward decl -- PresentFrame submits its 3D-world list (after PRE, under 2D)

    // Owns every GPU object that ISN'T specific to 2D sprite/text batching: device, swap chain,
    // command queue, fences, back buffers, PRE/POST command lists, and the whole particle-compute
    // system (which is independent of 2D vs. a future 3D renderer -- both could drive particles).
    // Public API is the lifecycle: Initialize -> (Render each frame / Resize on WM_SIZE) -> Cleanup.
    //
    // PresentFrame is the seam where a Renderer2D (and, eventually, a Renderer3D) contributes its
    // own recorded command lists to be submitted alongside Core's PRE/POST/particle lists -- see
    // SetRenderer2D() and Renderer2D::CollectCommandLists().
    class RendererCore {
    private:
        // ---- DAG task wrapper context types -- never named outside this class. The static
        // Task wrappers below and the m_*Ctx member variables that use them stay public (main.cpp
        // assigns to the members directly and passes the wrappers to CreateTask by name), but a
        // public MEMBER can have a private TYPE just fine -- nothing outside ever needs to spell
        // "RendererCore::StartFrameContext" to use core.m_StartFrameCtx = {...}. ----
        // Context instances are SINGLE, reused every frame -- safe ONLY because frames stay
        // serialized (frame N+1's StartFrame node is never built/fired until frame N's tail node
        // has completed). If frames are ever allowed to overlap, these MUST become real per-in-
        // flight-frame pools -- reusing a single instance across genuinely concurrent frames would
        // be a data race on the context fields.
        struct StartFrameContext {
            RendererCore* core;
            DirectX::XMFLOAT4 clearColor;
        };
        struct PresentFrameContext {
            RendererCore* core;
        };
        struct UpdateParticlesContext {
            RendererCore* core;
        };

    public:
        // ---- DAG task wrappers for BeginFrame/PresentFrame/UpdateParticles --------------------
        static void StartFrameTask(void* data) {
            auto* ctx = static_cast<StartFrameContext*>(data);
            // Task::Execute() is noexcept, so any exception thrown in here (e.g. ThrowIfFailed)
            // would otherwise hit std::terminate() before its message ever surfaces -- catch +
            // surface it via OutputDebugStringA (visible in an attached debugger or DebugView, no
            // disk write -- a hardcoded C:\temp path has no business existing in a Release build
            // running on someone else's machine) before rethrowing.
            try { ctx->core->BeginFrame(ctx->clearColor); }
            catch (const std::exception& e) {
                char buf[512];
                sprintf_s(buf, "StartFrameTask threw: %s\n", e.what());
                OutputDebugStringA(buf);
                throw;
            }
        }
        static void PresentFrameTask(void* data) {
            auto* ctx = static_cast<PresentFrameContext*>(data);
            try { ctx->core->PresentFrame(); }
            catch (const std::exception& e) {
                char buf[512];
                sprintf_s(buf, "PresentFrameTask threw: %s\n", e.what());
                OutputDebugStringA(buf);
                throw;
            }
        }
        static void UpdateParticlesTask(void* data) {
            auto* ctx = static_cast<UpdateParticlesContext*>(data);
            ctx->core->UpdateParticles();
        }
        StartFrameContext m_StartFrameCtx{};
        PresentFrameContext m_PresentFrameCtx{};
        UpdateParticlesContext m_UpdateParticlesCtx{};
        // ------------------------------------------------------------------------------------------

        // Lets PresentFrame() pull FlushBatchParallel()'s recorded lists in without Core knowing any
        // 2D-batching internals. Call once, after constructing both objects (a future Renderer3D
        // would get an analogous SetRenderer3D()).
        RendererCore();
        ~RendererCore();
        RendererCore(RendererCore&&) = delete;  // Or implement correctly
        RendererCore& operator=(RendererCore&&) = delete;
        RendererCore(const RendererCore&) = delete;
        RendererCore& operator=(const RendererCore&) = delete;

        void SetRenderer2D(Renderer2D* renderer2D) { m_Renderer2D = renderer2D; }
        // Renderer3D reaches the shared SRV heap + ResourceManager through this at RECORD time (not
        // Initialize) so it doesn't depend on SetRenderer2D being called before Renderer3D::Initialize.
        Renderer2D* GetRenderer2D() const { return m_Renderer2D; }
        void SetRenderer3D(Renderer3D* renderer3D) { m_Renderer3D = renderer3D; }

        void Initialize(HWND hWnd, uint32_t width, uint32_t height, bool useWarp = false);
        void BeginFrame(const DirectX::XMFLOAT4& clearColor);
        void PresentFrame();
        // DEFERRED RESIZE. Resize() now only RECORDS the request (safe to call reentrantly from WndProc
        // mid-frame -- it does no GPU work). ApplyPendingResize() does the actual teardown/recreate and
        // MUST be called between frames (no frame in flight), e.g. at the top of the render loop. Doing
        // the recreate straight from WM_SIZE released the depth buffer under an in-flight 3D frame ->
        // device-removed; deferring it fixes that and also coalesces a drag's WM_SIZE storm into one.
        void Resize(uint32_t width, uint32_t height);
        void ApplyPendingResize();

        // True when the window can't currently be presented to (occluded / minimized / not yet visible).
        // The render loop calls this at the top of each frame and SKIPS the frame when true -- rendering
        // + presenting a heavy frame to an occluded swap chain device-hangs the GPU. Uses DXGI_PRESENT_TEST
        // (queries occlusion WITHOUT presenting), so it's cheap and safe to call before the first present.
        bool ShouldSkipFrame();
        // Request that the next few frames be skipped -- called when a window transition ENDS (focus
        // regained, move/size drag finished) so the swap chain has time to re-settle before we render
        // a heavy frame into it. Rendering the instant a transition starts returning (before it's stable)
        // is what still device-hung on focus-return / move even after the unfocused-skip.
        void RequestSettle() { m_SettleFrames = 8; }

        // Block until the GPU has finished ALL submitted work. Call from the window handler on focus/
        // move transitions: DWM can invalidate the swap-chain buffers when the window loses focus or
        // moves, and if the GPU still has in-flight work referencing those buffers the driver resets
        // the device (DEVICE_HUNG). Idling here means there's no pending work when DWM re-composites.
        void WaitForGpuIdle() { Flush(); }

        // Reliable minimized state, set from WM_SIZE (wParam == SIZE_MINIMIZED) -- more dependable than
        // PRESENT_TEST occlusion detection. ShouldSkipFrame() skips entirely while minimized: a minimized
        // window has a 0/1x1 client area, and rendering a heavy frame into it device-hangs.
        void SetMinimized(bool m) { m_Minimized = m; }
        void Cleanup();
        void SetVSync(bool v) { m_VSync = v; }
        bool VSync() const { return m_VSync; }
        bool IsInitialized() const { return m_IsInitialized; }
        void InitImGui(HWND hwnd);

        void ExecuteUploadCommand(std::function<void(ID3D12GraphicsCommandList*)> task);
        // cameraPos/cameraZoom default to {0,0}/1.0f (camera centered at world origin, no zoom) --
        // callers not using a camera at all get the exact old behavior (world space == screen space).
        void UpdateGlobalUniforms(DirectX::XMFLOAT2 screenSize,
            DirectX::XMFLOAT2 cameraPos = { 0.0f, 0.0f }, float cameraZoom = 1.0f);
        DirectX::XMFLOAT2 GetScreenSize() const;
        DirectX::XMFLOAT2 GetNDC(float targetX, float targetY);

        ID3D12GraphicsCommandList* GetCommandList() {
            if (m_CommandList == nullptr) {
                OutputDebugStringA("ERROR: m_CommandList is NULL! Check initialization order.\n");
            }
            return m_CommandList.Get();
        }
        // Accessors Renderer2D needs to build ITS OWN GPU objects against the same device/queue/
        // swap-chain-derived state, without Core exposing its whole internals.
        ID3D12Device2* GetDevice() const { return m_Device.Get(); }
        ID3D12CommandQueue* GetCommandQueue() const { return m_CommandQueue.Get(); }
        ID3D12DescriptorHeap* GetRTVDescriptorHeap() const { return m_RTVDescriptorHeap.Get(); }
        ID3D12DescriptorHeap* GetImGUIDescriptorHeap() const { return m_imguiDescriptorHeap.Get(); }

        UINT GetRTVDescriptorSize() const { return m_RTVDescriptorSize; }
        ID3D12Resource* GetConstantBuffer(int frame) const { return m_ConstantBuffers[frame].Get(); }
        uint32_t GetClientWidth() const { return m_ClientWidth; }
        uint32_t GetClientHeight() const { return m_ClientHeight; }
        UINT GetFrameResourceIndex() const { return m_FrameResourceIndex; }
        UINT GetCurrentBackBufferIndex() const { return m_CurrentBackBufferIndex; }
        // Compile-time (not just a getter) so Renderer2D can size its own per-frame arrays
        // (m_InstanceBuffer, etc.) against this without duplicating the literal.
        static constexpr uint8_t NumFrames = 3;
        // Depth-stencil format for the 3D pass. D32_FLOAT: full 32-bit depth, no stencil -- switch
        // to DXGI_FORMAT_D24_UNORM_S8_UINT if a stencil buffer is ever needed. 2D never binds this
        // (it draws depth-off, on top of the 3D world); the depth buffer is 3D-only.
        static constexpr DXGI_FORMAT DepthFormat = DXGI_FORMAT_D32_FLOAT;

        // The back buffer RESOURCE is plain UNORM; its RENDER TARGET VIEW is the sRGB variant. That
        // pairing is the whole colour pipeline:
        //   sRGB texture  --hardware DECODE-->  linear shader maths  --hardware ENCODE-->  sRGB screen
        // Lighting is only correct in linear space, and a display expects sRGB, so both conversions
        // have to happen. Previously NEITHER did: ResourceManager stripped sRGB off every texture and
        // the RTV was UNORM, which is two wrongs that cancel for unlit 2D sprites (decode-then-encode
        // is identity, so skipping both looks fine) and do NOT cancel for anything lit -- every 3D
        // frame was doing its lighting on gamma-encoded values.
        //
        // Creating a UNORM swap chain with an _SRGB RTV is the standard way to get this: a swap chain
        // cannot be created _SRGB directly when using FLIP presentation models.
        //
        // Every PSO's RTVFormats[0] MUST equal this or CreateGraphicsPipelineState fails -- which is a
        // feature here, since it makes a missed pipeline a loud error rather than a wrong-looking one.
        static constexpr DXGI_FORMAT BackBufferFormat    = DXGI_FORMAT_R8G8B8A8_UNORM;
        static constexpr DXGI_FORMAT BackBufferRTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

        // Seconds elapsed since the LAST call to GetFrameTime() (not since last frame Present --
        // call this once per game-loop iteration, same spot every time, e.g. right after
        // ProcessMessages()/before Submit() calls, so "once per call" lines up with "once per
        // frame"). First call ever returns 0 (no prior timepoint to diff against) instead of some
        // huge bogus value measured from process startup.
        float GetFrameTime() {
            // JLib::Time (QueryPerformanceCounter-backed), not std::chrono::high_resolution_clock --
            // gives every consumer of "current time" in the app (this, and anything else that later
            // calls JLib::Time::GetTime() directly) the same clock instead of two independently-
            // ticking ones. Time::Initialize() is called lazily here, on the first-ever GetFrameTime()
            // call -- RendererCore is only ever constructed once per process, so this is the one
            // legitimate place to seed the epoch; calling Time::Initialize() again from anywhere else
            // would reset that shared epoch out from under every other consumer.
            if (m_FirstFrameTimeCall) {
                JLib::Time::Initialize();
                m_LastFrameTime = JLib::Time::GetTime();
                m_FirstFrameTimeCall = false;
                m_LastDeltaTime = 0.0f;
                return 0.0f;
            }
            double now = JLib::Time::GetTime();
            float dt = static_cast<float>(now - m_LastFrameTime);
            m_LastFrameTime = now;
            m_LastDeltaTime = dt;
            return dt;
        }
        // The value GetFrameTime() last returned -- lets UpdateParticles() (which runs later in the
        // same frame, after main.cpp already called GetFrameTime() once) reuse that SAME dt instead
        // of calling GetFrameTime() again, which would reset m_LastFrameTime and silently corrupt
        // the value main.cpp is still using for that frame's rotation/etc.
        float GetLastDeltaTime() const { return m_LastDeltaTime; }

        // ============================ Particle system ============================
        // Compiles updateCsPath into a new pool's update PSO, allocates its particle/dead-list/spawn
        // buffers sized for maxParticles, and seeds it (every particle inactive, dead list full).
        // Call AFTER Initialize() (needs m_ComputeRootSignature/m_CommandQueue). An invalid (default-
        // constructed) texture draws flat-colored quads (the 1x1 white fallback) -- pass a real
        // TextureHandle (from Renderer2D::GetResourceManager()->LoadTexture) for a textured/animated
        // look. atlasFramesX/Y: flipbook grid on that texture (1,1 = no animation). colorEnd: every
        // particle in this pool fades from its own spawn color toward this one over its lifetime
        // (default transparent white = fade-to-invisible, matching the old hardcoded behavior but
        // scaled to each particle's real lifetime). screenSpace=true skips the camera transform for
        // this whole pool (see ParticleEffectPool::screenSpace) -- use for view-decoration effects,
        // not effects that exist at a location in the world. Returns the effectID to pass to
        // RequestSpawn.
        uint32_t RegisterParticleEffect(const std::wstring& updateCsPath, uint32_t maxParticles, bool screenSpace = false, int zLayer = 0,
            TextureHandle texture = {}, uint32_t atlasFramesX = 1, uint32_t atlasFramesY = 1,
            DirectX::XMFLOAT4 colorEnd = { 1.0f, 1.0f, 1.0f, 0.0f }, float gravityScale = 0.0f);
        // effectID comes from RegisterParticleEffect() -- routes the spawn to that pool's own
        // spawn buffer/dead list, so different behaviors never share particle slots.
        void RequestSpawn(uint32_t effectID, Particle2D p);
        // Convenience overload: jitters position by up to +/-offsetRadius on each axis (uniform
        // random) so multiple spawns from the same call site scatter instead of stacking into a
        // line/point. Pass offsetRadius=0 for an exact position.
        // size: quad side length in pixels (see Particle::size) -- defaults to a reasonable small dot.
        void RequestSpawn(uint32_t effectID, DirectX::XMFLOAT2 basePosition, float offsetRadius,
            DirectX::XMFLOAT2 velocity, float lifetime, DirectX::XMFLOAT4 color, float size = 4.0f);
        void UpdateParticles();

    private:
        Renderer2D* m_Renderer2D = nullptr;
        Renderer3D* m_Renderer3D = nullptr;   // optional 3D-world pass; null = no 3D this app (2D-only)

        // One independently-updated particle population. Each pool owns its own particle buffer,
        // dead-list free list, spawn buffer, UAV heap, and (crucially) its own UPDATE PSO -- so
        // "gravity", "sin/cos wave", "static", etc. are genuinely different compute shaders, not
        // branches inside one shader. The SPAWN pass, draw root signature, and draw PSO stay SHARED
        // across every pool (see m_SpawnPSO/m_ParticleRootSignature/m_ParticlePSO) -- popping a dead
        // slot and rasterizing a point don't need to differ per behavior. Never touched outside this
        // class -- RegisterParticleEffect/RequestSpawn (public) never expose it in their signatures.
        struct ParticleEffectPool {
            uint32_t maxParticles = 0;
            // Which sprite/text zLayer (see Renderer2D's NUM_LAYERS system) this pool's DRAW
            // interleaves with -- PresentFrame submits particle pools and 2D layers together in
            // ascending zLayer order, particles-before-sprites within the same layer. The physics/
            // spawn COMPUTE dispatch is unaffected by this (it never touches the render target, so
            // it always just runs once for every pool, before any draw).
            int zLayer = 0;
            // Sprite sheet/atlas this pool draws with -- an invalid handle means "use the 1x1 white
            // fallback" (m_WhiteTexture), so untextured pools still render as flat-colored quads
            // through the SAME shared draw PSO/root signature instead of needing a separate
            // no-texture variant.
            TextureHandle texture;
            // Flipbook grid on `texture` -- e.g. 4x1 for a 4-frame animation strip. 1x1 (the
            // default) means "whole texture, no animation", so untextured/unanimated pools behave
            // exactly as before. Frame index is picked by life PROGRESS (age / (age+lifetime), see
            // ParticleVS.hlsl), not by a separate timer -- ties animation speed to how long the
            // particle actually lives, no extra per-particle state needed.
            uint32_t atlasFramesX = 1;
            uint32_t atlasFramesY = 1;
            // Every particle in this pool lerps from its OWN spawn-time Particle2D::color toward
            // THIS pool-wide color as it ages (progress 0 -> 1). Defaults to transparent white so
            // the alpha channel alone reproduces the old hardcoded "fade out near end of life"
            // behavior -- but correctly scaled to each particle's ACTUAL lifetime instead of a
            // fixed assumed 5 seconds.
            DirectX::XMFLOAT4 colorEnd = { 1.0f, 1.0f, 1.0f, 0.0f };
            // false (default): particles live at world-space positions and scroll/rescale with the
            // camera every frame (fountains, smoke, attached emitters -- anything that represents
            // something existing at a location in the scene). true: ParticleVS.hlsl skips the
            // camera transform entirely, treating Particle2D::position as already being in screen
            // pixels -- for effects decorating the VIEW itself rather than the world (a lens-grime
            // overlay, a damage vignette, a menu background) so they never scroll/rescale away.
            bool screenSpace = false;
            // px/s^2 applied to velocity.y every update-shader tick, 0 (default) = no gravity.
            // Per-pool rather than baked into UpdateParticles.hlsl, since not every effect using
            // that shader wants to fall (e.g. a screen-space overlay effect).
            float gravityScale = 0.0f;
            Microsoft::WRL::ComPtr<ID3D12Resource> particleBuffer;
            // Per-frame SNAPSHOT of particleBuffer that the graphics queue draws from, so the
            // compute queue's in-place integration of particleBuffer never shares a resource with
            // an in-flight particle draw -- that shared-resource hazard was forcing compute and
            // graphics to fully serialize (m_ComputeQueue->Wait(m_LastGraphicsSignalValue)) with no
            // real overlap. UpdateParticles copies particleBuffer -> drawBuffer[m_FrameResourceIndex]
            // at the end of the compute pass; RecordParticleDraw binds THIS slot. Sized NumFrames
            // and indexed by the frame-resource slot, so reuse is already gated by BeginFrame's wait
            // on m_FrameFenceValues[frame] -- no new fence needed. No UAV flag (copy-dest + SRV read
            // only); draw binds it as a root SRV by GPU-VA, so no descriptor-heap view either.
            Microsoft::WRL::ComPtr<ID3D12Resource> drawBuffer[NumFrames];
            Microsoft::WRL::ComPtr<ID3D12Resource> deadListBuffer;
            Microsoft::WRL::ComPtr<ID3D12Resource> spawnBuffer;
            // Compacted list of alive particle indices, rebuilt every frame (ResetAliveCounter.hlsl
            // then CompactParticles.hlsl) BEFORE the update dispatch -- lets the update dispatch skip
            // dead slots entirely via ExecuteIndirect instead of scanning maxParticles every frame,
            // without needing particles to be stored contiguously (see ParticleCommon.hlsli).
            Microsoft::WRL::ComPtr<ID3D12Resource> aliveListBuffer;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> uavHeap; // u0=particleBuffer, u1=deadList, u2=aliveList(counted), u3=aliveList(raw)
            Microsoft::WRL::ComPtr<ID3D12PipelineState> updatePSO; // this pool's OWN behavior shader
            // Byte offset of aliveListBuffer's hidden UAV counter (see AlignUavCounterOffset) -- stored
            // here (rather than recomputed) so BuildDispatchArgs.hlsl / ResetAliveCounter.hlsl /
            // ParticleCommon.hlsli's AliveCounterRaw can Load()/Store() it as a plain uint, without
            // touching the counted UAV (u2) path at all.
            UINT64 aliveListCounterOffset = 0;
            // 3 UINTs (ThreadGroupCountX/Y/Z) that ExecuteIndirect reads directly from the GPU --
            // written every frame by a tiny BuildDispatchArgs dispatch AFTER CompactParticles has
            // rebuilt the alive list, from that list's own counter. Lets the update dispatch scale
            // down to however many particles are actually alive instead of always covering
            // maxParticles, without any CPU readback (indirect args never leave the GPU).
            Microsoft::WRL::ComPtr<ID3D12Resource> argsBuffer;
            // u0 = RAW view over aliveListBuffer (covers the counter, no CounterOffsetInBytes attached --
            // that would make it a COUNTED view, and only one counted view can exist per buffer, which
            // uavHeap's u2 view already claims), u1 = RAW view over argsBuffer.
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> argsHeap;
            std::vector<Particle2D> pendingSpawns;
            // Kept alive only until RegisterParticleEffect's seeding Flush().
            Microsoft::WRL::ComPtr<ID3D12Resource> particleUploadBuffer;
            Microsoft::WRL::ComPtr<ID3D12Resource> deadListUploadBuffer;
            // This pool's OWN draw list -- separate from m_ComputeList (which only ever holds the
            // physics/spawn DISPATCHES, never a draw) so a pool's draw can be submitted at whatever
            // point in zLayer order PresentFrame decides, independent of every other pool's.
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> drawAllocators[NumFrames];
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> drawList;
        };

        Microsoft::WRL::ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp);
        Microsoft::WRL::ComPtr<ID3D12Device2> CreateDevice(Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter);
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> CreateCommandQueue(D3D12_COMMAND_LIST_TYPE type);
        Microsoft::WRL::ComPtr<IDXGISwapChain4> CreateSwapChain(HWND hWnd, uint32_t width, uint32_t height, uint32_t bufferCount);
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors);
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type);
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> CreateCommandList(Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator, D3D12_COMMAND_LIST_TYPE type);
        Microsoft::WRL::ComPtr<ID3D12Fence> CreateFence();
        HANDLE CreateEventHandle();
        bool   CheckTearingSupport();
        void   UpdateRenderTargetViews();   // (re)creates an RTV for each swap-chain back buffer
        void   CreateDepthBuffer();         // (re)creates the depth-stencil buffer + DSV at the current client size
        void   EnableDebugLayer();
        void   LogDeviceRemoved();          // dumps DRED breadcrumbs + page fault on device-removed
        void CreateConstantBuffers();
        uint64_t Signal();
        // Same scheme as Signal(), but signals m_ComputeFence (a SEPARATE fence + counter) from
        // m_ComputeQueue -- lets the graphics queue Wait() on this value (GPU-side, no CPU stall)
        // before reading anything the compute dispatch wrote, without a full Flush(). It MUST NOT
        // share m_Fence with the graphics queue: m_Fence gates frame-slot reuse and must reflect
        // graphics completion alone (see m_ComputeFence's declaration for the failure mode).
        uint64_t SignalCompute();
        // fence == nullptr defaults to m_Fence (the graphics frame fence). Pass m_ComputeFence to wait
        // on the async-compute queue -- either way the on-fiber path SUSPENDS the fiber (DirectEvent),
        // it never spins. Only the fence differs.
        void     WaitForFenceValue(uint64_t value, std::chrono::milliseconds dur = std::chrono::milliseconds::max(),
                                   ID3D12Fence* fence = nullptr);
        void     Flush();   // Signal + wait: GPU idle, safe to release/resize resources

        // Layout MUST match every .hlsl file's "GlobalUniforms" cbuffer (VertexShader.hlsl,
        // PixelShader.hlsl, ParticleVS.hlsl) exactly -- this is the SPRITE/TEXT/PARTICLE-DRAW
        // per-frame uniform buffer, shared by all three pipelines. Compute-only data (dt,
        // spawnCount) does NOT belong here -- it goes through the compute root signature's own
        // 32-bit-constants parameter instead, so it can't desync this shared layout.
        // cameraPos/cameraZoom repurpose what used to be plain padding floats (byte layout is
        // UNCHANGED -- still 8 floats/32 bytes/2 float4 rows -- just two of the spares now carry
        // real data). Lets ParticleVS.hlsl do camera-aware world->NDC conversion every frame on the
        // GPU, since particles are simulated entirely server-side (GPU compute) and have no CPU-side
        // per-frame "reproject through the camera" step the way BatchItem/Submit()'s sprites do.
        struct alignas(256) ConstantBufferData {
            float time;
            DirectX::XMFLOAT2 cameraPos; // world-space point the camera is centered on
            float padding1;               // still 1 spare float
            DirectX::XMFLOAT2 screenSize;
            float aspectRatio;
            float cameraZoom;
        };

        Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateComputeRootSignature();
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CreateComputePipelineState(const std::wstring& csPath,
            ID3D12RootSignature* rootSig = nullptr); // nullptr -> m_ComputeRootSignature (the common case)
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateArgsRootSignature();
        Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateParticleRootSignature();
        Microsoft::WRL::ComPtr<ID3D12PipelineState> CreateParticlePipelineState(
            const std::wstring& vsPath, const std::wstring& psPath);
        void SeedParticlePool(ParticleEffectPool& pool, ID3D12GraphicsCommandList* cmd);
        void FlushSpawns(ParticleEffectPool& pool);
        void InitializeParticleSystem();
        // Records ONLY this pool's draw (bind RTV/viewport/PSO/SRV, DrawInstanced) onto its own
        // drawList -- split out from the physics/spawn dispatch so PresentFrame can interleave each
        // pool's draw with Renderer2D's zLayers independently of the others.
        void RecordParticleDraw(ParticleEffectPool& pool, int frame, int backBuffer);

        std::vector<ParticleEffectPool> m_ParticleEffects;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_ComputeRootSignature; // shared by every pool's updatePSO + m_SpawnPSO
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_SpawnPSO;   // SpawnParticles.hlsl: pop dead slots -- SHARED
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ResetAliveCounterPSO; // ResetAliveCounter.hlsl -- SHARED
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_CompactPSO;           // CompactParticles.hlsl -- SHARED
        // Indirect-dispatch plumbing (shared across every pool -- only argsBuffer/argsHeap are per-pool).
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_ArgsRootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_BuildArgsPSO;      // BuildDispatchArgs.hlsl
        Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_DispatchCommandSignature; // ExecuteIndirect(Dispatch)
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_ComputeList;
        // Draws the particle buffer directly (point list, no vertex buffer -- SV_InstanceID indexes
        // the StructuredBuffer). Separate root signature/PSO from the sprite pipeline. SHARED draw
        // PSO -- every pool draws with the same point-list VS/PS for now.
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_ParticleRootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ParticlePSO;

        // ============================ persistent state ============================
        // Separate allocator per frame-resource slot -- the compute dispatch runs on the SAME
        // m_CommandQueue as everything else, but must never share an allocator/list with the
        // PRE/POST/worker lists.
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_ComputeAllocators[NumFrames];

        // Core devices
        Microsoft::WRL::ComPtr<ID3D12Device2>      m_Device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_CommandQueue;
        // Async-compute queue -- ONLY the particle update/spawn dispatch (m_ComputeList) submits
        // here now, separate from m_CommandQueue's PRE/draw/POST submission in PresentFrame. On
        // hardware with a real async compute engine this lets particle sim overlap with the
        // previous frame's graphics work instead of serializing behind it on one queue.
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_ComputeQueue;
        Microsoft::WRL::ComPtr<IDXGISwapChain4>    m_SwapChain;

        // Per-frame data (the triple-buffer set)
        Microsoft::WRL::ComPtr<ID3D12Resource>         m_BackBuffers[NumFrames];
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_CommandAllocators[NumFrames];
        uint64_t m_FrameFenceValues[NumFrames] = {};
        Microsoft::WRL::ComPtr<ID3D12Resource> m_UploadHeap; // Temporary buffer for texture upload
        // Pipeline helpers
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CommandList; // PRE list: clear + PRESENT->RT
        // POST list: RENDER_TARGET->PRESENT barrier, executed last each frame.
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_PostList;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_PostAllocators[NumFrames];
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_RTVDescriptorHeap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_imguiDescriptorHeap = nullptr;
        // Depth-stencil buffer + its DSV heap (1 descriptor). Lives in DEPTH_WRITE for its whole
        // lifetime (never read as SRV yet), so no per-frame state transitions -- just a clear in the
        // PRE list. Recreated on Resize alongside the back buffers. See CreateDepthBuffer().
        Microsoft::WRL::ComPtr<ID3D12Resource>            m_DepthBuffer;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_DSVDescriptorHeap;

        UINT m_RTVDescriptorSize = 0;
        // m_CurrentBackBufferIndex: WHICH SWAP-CHAIN BACK BUFFER to render into this frame --
        // DXGI-controlled (via GetCurrentBackBufferIndex()). ONLY use this for things tied to the
        // actual back buffer resource: m_BackBuffers[x], and the RTV descriptor offset.
        UINT m_CurrentBackBufferIndex = 0;
        // Incremented on every swap-chain resize, and baked into each back buffer's debug name
        // ("BackBuffer[i] genN"). Purely diagnostic: it makes a D3D12 debug-layer complaint say WHICH
        // generation of buffer was involved, which is the difference between "a barrier is missing"
        // and "a barrier was recorded against buffers that have since been released and replaced".
        UINT m_SwapChainGeneration = 0;

        // Dedicated list for ExecuteUploadCommand. It MUST NOT share m_CommandList (the PRE list):
        // an upload triggered mid-frame -- e.g. a scene constructor loading textures from Update() --
        // would Reset it and throw away the frame's PRESENT->RENDER_TARGET barrier and clears.
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_UploadAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_UploadList;
        // m_FrameResourceIndex: WHICH ROTATING SET of per-frame CPU/GPU-sync resources to use --
        // app-controlled, advanced by exactly 1 (mod NumFrames) once per PresentFrame call,
        // INDEPENDENT of the swap chain. With DXGI_SWAP_EFFECT_FLIP_DISCARD (+ tearing support,
        // both used by this swap chain), GetCurrentBackBufferIndex() is NOT documented to follow a
        // strict round-robin sequence -- tying CPU/GPU sync-slot selection to it risks reusing a
        // sync resource the GPU is still using. Keeping this counter fully app-controlled removes
        // that dependency entirely.
        UINT m_FrameResourceIndex = 0;

        // Synchronization
        Microsoft::WRL::ComPtr<ID3D12Fence> m_Fence;
        uint64_t m_FenceValue = 0;
        HANDLE   m_FenceEvent = nullptr;
        // SEPARATE fence for the async-compute (particle) queue. m_Fence must be advanced ONLY by the
        // graphics queue, because m_FrameFenceValues[] (the per-slot allocator/list reuse gate) are
        // GRAPHICS Signal() values. A single fence signaled by BOTH queues reports only the MAX value,
        // so a fast compute Signal masks an incomplete graphics Signal -- and BeginFrame would then
        // reuse a frame slot whose graphics work is still in flight, recycling a command list the GPU
        // is mid-draw on (DEVICE_HUNG, VA=0, mid-list). Splitting the fence keeps the graphics gate honest.
        Microsoft::WRL::ComPtr<ID3D12Fence> m_ComputeFence;
        uint64_t m_ComputeFenceValue = 0;
        // Constant buffers (one per frame)
        Microsoft::WRL::ComPtr<ID3D12Resource> m_ConstantBuffers[NumFrames];
        // You also need to keep the pointer to the "mapped" memory so you can write to it every
        // frame without calling Map/Unmap
        uint8_t* m_MappedConstantBufferData[NumFrames];

        // Config / dimensions
        uint32_t m_ClientWidth = 1280;
        uint32_t m_ClientHeight = 720;
        // Deferred-resize state: Resize() sets these; ApplyPendingResize() consumes them between frames.
        bool     m_ResizePending = false;
        uint32_t m_PendingWidth  = 1280;
        uint32_t m_PendingHeight = 720;
        int      m_SettleFrames  = 0;   // >0 => skip this many frames (post-transition swap-chain settle)
        bool     m_Minimized     = false;  // set from WM_SIZE(SIZE_MINIMIZED); ShouldSkipFrame skips while true
        bool m_VSync = true;
        bool m_TearingSupported = false;
        bool m_UseWarp = false;
        bool m_IsInitialized = false;

        double m_LastFrameTime = 0.0;
        bool m_FirstFrameTimeCall = true;
        float m_LastDeltaTime = 0.0f;
    };
};