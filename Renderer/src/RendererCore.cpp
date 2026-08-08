#include "../include/RendererCore.h"
#include "../include/Renderer2D.h"     // Renderer2D -- PresentFrame collects its draw lists
#include "../include/Renderer3D.h"     // Renderer3D -- PresentFrame records + submits its 3D-world list
#include "../include/Helpers.h"      // ThrowIfFailed
#include <cstdio>                     // sprintf_s (crash diagnostics -> OutputDebugString)
#include "../include/Event.h"        // JLib::Event (SignalAll) for the fence-wait bridge
#include <Thread.h>
#include <TaskScheduler.h> // JLib::TaskScheduler::Instance()/WaitOnEventDirectArmed -- previously pulled in transitively via Renderer2D.h
#include <algorithm>      // std::max
#include <cassert>
#include <cstdio>         // swprintf_s
#include <random>         // RequestSpawn/SeedParticlePool distributions
using namespace JLib;
using namespace Microsoft::WRL;

// D3D12_UNORDERED_ACCESS_VIEW_DESC::Buffer::CounterOffsetInBytes MUST be a multiple of
// D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT (4096) -- rounds dataSizeBytes up to the next 4096
// boundary. Used by both RegisterParticleEffect (creates the buffer + UAV) and SeedParticlePool
// (uploads into it) -- MUST compute the identical offset in both places, hence the shared helper
// instead of duplicating the arithmetic.
static UINT64 AlignUavCounterOffset(UINT64 dataSizeBytes) {
	constexpr UINT64 kAlignment = D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT;
	return (dataSizeBytes + kAlignment - 1) & ~(kAlignment - 1);
}
RendererCore::RendererCore()
{}
RendererCore::~RendererCore()
{}
// ===========================================================================
// Public lifecycle
// ===========================================================================
// Core-only bring-up: device, queue, swap chain, RTV heap, PRE/POST lists, fence, constant
// buffers, and the particle system's shared setup. Renderer2D::Initialize(core) runs AFTER
// this returns and builds its own GPU objects (root sig, PSO, instance buffer, SRV heap, font)
// against the device/queue this created.
void RendererCore::Initialize(HWND hWnd, uint32_t width, uint32_t height, bool useWarp)
{
	m_ClientWidth = width;
	m_ClientHeight = height;
	m_UseWarp = useWarp;
	// Call in ALL configs: the debug layer + GPU-based validation inside are still Debug-only (inner
	// #if _DEBUG), but DRED must be armed in RELEASE too so a device-removed/HUNG there is diagnosable.
	// (Was #if _DEBUG-wrapped, which silently disabled DRED in Release -> empty DRED dumps.)
	EnableDebugLayer();                 // must precede ANY device work
	m_TearingSupported = CheckTearingSupport();

	ComPtr<IDXGIAdapter4> adapter = GetAdapter(m_UseWarp);   // 1. pick a GPU
	m_Device = CreateDevice(adapter);    // 2. the device

	CreateConstantBuffers(); // Creates a buffer for each frame (CPU->GPU)
	m_CommandQueue = CreateCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
	m_SwapChain = CreateSwapChain(hWnd, m_ClientWidth, m_ClientHeight, NumFrames);

	m_CurrentBackBufferIndex = m_SwapChain->GetCurrentBackBufferIndex();
	m_FrameResourceIndex = 0; // app-controlled sync-slot counter, independent of the swap chain

	m_RTVDescriptorHeap = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NumFrames);
	m_RTVDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	UpdateRenderTargetViews();

	// Depth buffer for the (upcoming) 3D pass. One DSV, created once; the resource itself is
	// (re)created here and again on every Resize via CreateDepthBuffer().
	m_DSVDescriptorHeap = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
	CreateDepthBuffer();

	for (int i = 0; i < NumFrames; ++i)
		m_CommandAllocators[i] = CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT);

	m_CommandList = CreateCommandList(                       // PRE list
		m_CommandAllocators[m_FrameResourceIndex], D3D12_COMMAND_LIST_TYPE_DIRECT);

	for (int i = 0; i < NumFrames; ++i)
		m_PostAllocators[i] = CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT);
	m_PostList = CreateCommandList(
		m_PostAllocators[m_FrameResourceIndex], D3D12_COMMAND_LIST_TYPE_DIRECT);

	m_CommandQueue->SetName(L"MainQueue");
	m_CommandList->SetName(L"PRE (clear+barrier)");
	m_PostList->SetName(L"POST (present barrier)");

	m_Fence = CreateFence();
	m_ComputeFence = CreateFence();   // SEPARATE from m_Fence -- see m_ComputeFence's declaration
	m_FenceEvent = CreateEventHandle();

	// --- Particle system: shared setup only ---
	// Per-effect pools (particle/dead-list/spawn buffers, each pool's OWN update shader) are NOT
	// created here -- call RegisterParticleEffect() after both Initialize() calls return.
	InitializeParticleSystem();
	m_ComputeQueue = CreateCommandQueue(D3D12_COMMAND_LIST_TYPE_COMPUTE);
	m_ComputeQueue->SetName(L"ComputeQueue (particles)");
	for (int i = 0; i < NumFrames; ++i)
		m_ComputeAllocators[i] = CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE);
	m_ComputeList = CreateCommandList(m_ComputeAllocators[m_FrameResourceIndex], D3D12_COMMAND_LIST_TYPE_COMPUTE);
	m_ComputeList->SetName(L"Compute (particle update)");

	m_SettleFrames = 12;   // startup settle: skip the first frames so the swap chain stabilizes before
	                       // the first (possibly heavy) render -- there's no focus TRANSITION to arm off
	                       // of at launch, and the window is "foreground" to Win32 before it's stable.
	m_IsInitialized = true;
}

void RendererCore::BeginFrame(const DirectX::XMFLOAT4& clearColor) {
	// Apply any pending window resize HERE. BeginFrame runs on the MAIN thread at the start of the frame
	// (StartFrame is a DAG main-node) with no frame in flight -- the safe between-frames point. Doing it
	// here makes deferred resize TRANSPARENT to every consumer: no render loop needs to call
	// ApplyPendingResize itself. Game01 broke precisely because its loop didn't call it.
	ApplyPendingResize();

	// TWO DIFFERENT indices, deliberately -- see m_FrameResourceIndex's declaration comment.
	// frame: which rotating sync-slot (allocators, instance buffer) to reclaim/reuse.
	// backBuffer: which actual swap-chain back buffer to transition/clear/render into.
	const int frame = m_FrameResourceIndex;
	const int backBuffer = m_CurrentBackBufferIndex;

	// The allocators/command lists are reset per-frame and safe to reuse once the GPU has
	// finished executing the previous use of that frame slot. But the instance buffer is
	// *persistently mapped* and can be read by in-flight draws from multiple frames back.
	// Wait on an earlier frame's fence to ensure >= 2 frames of latency before we reuse the
	// instance buffer slot.
	int safeFrame = (frame + NumFrames - 2) % NumFrames;
	WaitForFenceValue(m_FrameFenceValues[frame]);        // allocator is safe to reset
	WaitForFenceValue(m_FrameFenceValues[safeFrame]);    // instance buffer is safe to write

	if (m_Renderer2D) m_Renderer2D->ResetWorkerAllocators(frame);

	// PRE list (single-threaded): PRESENT -> RENDER_TARGET, then clear. The worker lists
	// draw after this; the POST list (PresentFrame) transitions back to PRESENT. PRE/POST
	// are SEPARATE lists from the workers so a worker's Reset() can't wipe the clear/barrier.
	m_CommandAllocators[frame]->Reset();
	m_CommandList->Reset(m_CommandAllocators[frame].Get(), nullptr);

	auto toRT = CD3DX12_RESOURCE_BARRIER::Transition(
		m_BackBuffers[backBuffer].Get(),
		D3D12_RESOURCE_STATE_PRESENT,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_CommandList->ResourceBarrier(1, &toRT);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
		m_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), backBuffer, m_RTVDescriptorSize);
	m_CommandList->ClearRenderTargetView(rtv, reinterpret_cast<const FLOAT*>(&clearColor), 0, nullptr);

	// Clear the depth buffer for the 3D pass (depth buffer stays in DEPTH_WRITE, no barrier needed).
	// Harmless while no 3D pass binds it yet; it's the foundation the 3D opaque pass will draw into.
	CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(m_DSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	m_CommandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// Drain any async texture uploads (glTF embedded textures, LoadTextureAsync sprites, atlases) into
	// the PRE list. PRE executes FIRST of all this frame's lists, so a texture decoded since last frame is
	// copied + transitioned to PIXEL_SHADER_RESOURCE before ANY draw list can sample it -- and it flips to
	// IsTextureReady() here, so a Submit later this frame binds it safely. Automatic for every consumer
	// (no per-app PumpAsyncUploads call). No-op when nothing is pending. RM lives on Renderer2D (which also
	// owns the 3D albedo path), so a 2D-less app simply has no async textures to drain.
	if (m_Renderer2D)
		m_Renderer2D->GetResourceManager()->PumpAsyncUploads(m_CommandList.Get());

	m_CommandList->Close(); // PRE complete; submitted first in PresentFrame
}

void RendererCore::PresentFrame() {
	const int frame = m_FrameResourceIndex;       // sync-slot: allocators, cmdLists, fence value
	const int backBuffer = m_CurrentBackBufferIndex; // which actual back buffer to present

	// 2D pipeline: merge worker submissions and launch worker tasks to record draw commands in
	// parallel (FlushBatchParallel handles everything including the GPU buffer upload via memcpy).
	if (m_Renderer2D) m_Renderer2D->FlushBatchParallel();

	// POST list: transition the back buffer back to PRESENT (single-threaded, executed last).
	m_PostAllocators[frame]->Reset();
	m_PostList->Reset(m_PostAllocators[frame].Get(), nullptr);
	// ImGui draws here, at the top of POST -- the back buffer is still RENDER_TARGET (the
	// barrier below hasn't run yet) and every 2D layer has already been submitted before this
	// list, so the UI lands on top of everything. RenderDrawData needs the RTV bound and the
	// ImGui SRV heap set itself; nothing else in POST cares, so no state leaks to un-set.
	// Gated on GetDrawData(): non-null only after the app called ImGui::NewFrame + ImGui::Render
	// this frame, so a consumer that never calls InitImGui skips all of this for free.
	if (ImGui::GetCurrentContext() && ImGui::GetDrawData()) {
		CD3DX12_CPU_DESCRIPTOR_HANDLE imguiRtv(
			m_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), backBuffer, m_RTVDescriptorSize);
		m_PostList->OMSetRenderTargets(1, &imguiRtv, FALSE, nullptr);
		// The SHARED heap -- ImGui's descriptors now live in it (see InitImGui). Binding a private
		// ImGui heap here is what made app-supplied texture handles illegal inside ImGui::Image.
		ID3D12DescriptorHeap* imguiHeaps[] = { m_Renderer2D->GetSrvHeap() };
		m_PostList->SetDescriptorHeaps(1, imguiHeaps);
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_PostList.Get());
	}
	auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
		m_BackBuffers[backBuffer].Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PRESENT);
	m_PostList->ResourceBarrier(1, &toPresent);
	m_PostList->Close();

	// Every pool's physics/spawn dispatch now runs on its OWN async-compute queue, independent
	// of m_CommandQueue's PRE/draw/POST submission below -- lets it overlap with graphics work
	// on hardware with a real async compute engine instead of always serializing on one queue.
	// No cross-queue wait before this dispatch anymore: graphics no longer reads particleBuffer (it
	// draws from the per-frame drawBuffer snapshot instead), so this frame's in-place UAV
	// integration can't race any in-flight graphics read. drawBuffer slot reuse across the NumFrames
	// cycle is gated CPU-side by BeginFrame's WaitForFenceValue on that slot's fence. Dropping the
	// old m_ComputeQueue->Wait(m_LastGraphicsSignalValue) edge is what actually lets compute-N
	// overlap graphics-(N-1) -- the entire point of the snapshot buffer.
	ID3D12CommandList* computeLists[] = { m_ComputeList.Get() };
	m_ComputeQueue->ExecuteCommandLists(1, computeLists);
	uint64_t computeSignalValue = SignalCompute();
	// The particle draws below read particleBuffer as an SRV (ParticleVS.hlsl), so the graphics
	// queue must not start them before this dispatch's UAV writes finish -- a GPU-side Wait() on
	// the compute fence value this signals handles that without a CPU stall. Wait on m_ComputeFence
	// (NOT m_Fence): the wait must key off compute's OWN completion, and keeping compute off m_Fence
	// is what stops it from prematurely satisfying the graphics frame-slot gate (the DEVICE_HUNG bug).
	ThrowIfFailed(m_CommandQueue->Wait(m_ComputeFence.Get(), computeSignalValue));

	// Execute PRE + [particle draws / 2D layers interleaved by zLayer] + POST. Particle pools and
	// Renderer2D's sprite/text layers are submitted together in ascending zLayer order --
	// particles before sprites within the same layer -- so a pool registered at zLayer 2 actually
	// draws over whatever's at zLayer 0/1 and under zLayer 3, instead of always sitting under
	// everything.
	// 3D world pass: record it (binds this back buffer's RTV + the depth DSV, both cleared by PRE)
	// and submit it right AFTER PRE and BEFORE the particle/2D layers, so the world sits UNDER the
	// 2D HUD. No-op when no Renderer3D is attached (2D-only apps leave m_Renderer3D null).
	if (m_Renderer3D) {
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtv3d(
			m_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), backBuffer, m_RTVDescriptorSize);
		CD3DX12_CPU_DESCRIPTOR_HANDLE dsv3d(m_DSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
		m_Renderer3D->RecordCommandList(frame, rtv3d, dsv3d, m_ClientWidth, m_ClientHeight);
	}

	std::vector<ID3D12CommandList*> allLists;
	allLists.push_back(m_CommandList.Get());  // PRE: clear + PRESENT->RENDER_TARGET
	// 3D world (under 2D/particles). Parallel recording produces MULTIPLE lists (one per worker + the
	// skinned list); execute them all, in the order Renderer3D queued them (static slices, then skinned).
	if (m_Renderer3D)
		for (ID3D12CommandList* l3d : m_Renderer3D->GetRecordedLists())
			allLists.push_back(l3d);
	for (int layer = 0; layer < NUM_LAYERS; ++layer) {
		for (auto& pool : m_ParticleEffects)
			if (pool.zLayer == layer) allLists.push_back(pool.drawList.Get());
		if (m_Renderer2D) m_Renderer2D->CollectCommandListsForLayer(layer, frame, allLists);
	}
	allLists.push_back(m_PostList.Get());     // POST: RENDER_TARGET->PRESENT

	// DIAGNOSTIC (device-hung VA=0 hunt): a NULL command list or back buffer reaching submit is the
	// prime suspect for the null render-target fault. Log any before we submit.
	if (!m_BackBuffers[backBuffer])
		OutputDebugStringA("[PresentFrame] *** back buffer is NULL at submit ***\n");
	for (size_t li = 0; li < allLists.size(); ++li)
		if (!allLists[li]) {
			char b[96]; sprintf_s(b, "[PresentFrame] *** NULL command list at index %zu / %zu ***\n", li, allLists.size());
			OutputDebugStringA(b);
		}

	// DIAGNOSTIC (stale-reference hunt, per r/graphicsprogramming lead): the 3D cube pass -- the pass
	// DRED fingered -- writes to THIS back buffer (rtv3d) and the shared depth buffer (dsv3d). A released
	// or size-mismatched depth/RT is the textbook VA=0 draw fault that the per-draw null-checks CANNOT
	// see, because it goes stale AFTER record. So check the two OM targets here, right before submit:
	//   (a) depth buffer resource still alive, and
	//   (b) back buffer, depth buffer, and the logical client size ALL agree on dimensions
	//       (a RT/depth size mismatch device-hangs the GPU on the first draw that touches both).
	// Silent unless something is genuinely wrong -- if this fires on a crashing frame, we found it.
	if (m_Renderer3D) {
		ID3D12Resource* bb = m_BackBuffers[backBuffer].Get();
		ID3D12Resource* db = m_DepthBuffer.Get();
		if (!db) {
			OutputDebugStringA("[Probe] *** depth buffer NULL at submit -- cube pass DSV is dangling ***\n");
		} else if (bb) {
			D3D12_RESOURCE_DESC bd = bb->GetDesc();
			D3D12_RESOURCE_DESC dd = db->GetDesc();
			if (bd.Width != dd.Width || bd.Height != dd.Height ||
				bd.Width != m_ClientWidth || bd.Height != m_ClientHeight) {
				char b[224];
				sprintf_s(b, "[Probe] *** RT/depth/client SIZE MISMATCH before submit: "
					"backbuf=%llux%u  depth=%llux%u  client=%ux%u ***\n",
					(unsigned long long)bd.Width, (unsigned)bd.Height,
					(unsigned long long)dd.Width, (unsigned)dd.Height,
					(unsigned)m_ClientWidth, (unsigned)m_ClientHeight);
				OutputDebugStringA(b);
			}
		}
	}

	m_CommandQueue->ExecuteCommandLists((UINT)allLists.size(), allLists.data());

	// Present, then ONE fence scheme (m_FrameFenceValues + Signal()). BeginFrame waits on
	// m_FrameFenceValues[frame] before reusing this slot's allocators next time around.
	// ALLOW_TEARING is only legal (DXGI validates this) when SyncInterval is 0, i.e. VSync off --
	// the swap chain was already created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING gated on
	// m_TearingSupported, but that alone doesn't make Present() actually tear; this flag is the
	// per-present opt-in. Without it, VSync off didn't behave like VSync off (no visible tearing,
	// still effectively capped) -- this also matters beyond raw tearing: Windows requires this
	// flag for G-Sync/FreeSync variable refresh rate to work correctly in windowed/borderless mode.
	UINT presentFlags = (!m_VSync && m_TearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
	HRESULT hrPresent = m_SwapChain->Present(m_VSync ? 1 : 0, presentFlags);
	if (FAILED(hrPresent)) {
		{ char b[160]; sprintf_s(b, "[PresentFrame] Present FAILED hr=0x%08X\n", (unsigned)hrPresent); OutputDebugStringA(b); }
		// A TDR surfaces here as DEVICE_REMOVED/RESET. Dump DRED (which op/resource killed
		// the GPU) BEFORE throwing, so we get the diagnosis instead of a bare crash.
		if (hrPresent == DXGI_ERROR_DEVICE_REMOVED || hrPresent == DXGI_ERROR_DEVICE_RESET) {
			HRESULT reason = m_Device->GetDeviceRemovedReason();
			char b[160]; sprintf_s(b, "[PresentFrame] GetDeviceRemovedReason=0x%08X\n", (unsigned)reason); OutputDebugStringA(b);
			LogDeviceRemoved();
		}
		ThrowIfFailed(hrPresent);
	}
	m_FrameFenceValues[frame] = Signal();
	m_CurrentBackBufferIndex = m_SwapChain->GetCurrentBackBufferIndex(); // DXGI-controlled, for the NEXT frame's back buffer
	m_FrameResourceIndex = (m_FrameResourceIndex + 1) % NumFrames;     // app-controlled, fully independent rotation

	// CPU-side reset for next frame. Safe now: all worker RECORDING finished above (the GPU
	// reads m_InstanceBuffer, not these vectors).
	if (m_Renderer2D) m_Renderer2D->ClearWorkerBucketsAndResetPool();
}

void RendererCore::Resize(uint32_t width, uint32_t height)
{
	if (!m_IsInitialized) return;                  // ignore WM_SIZE before bring-up
	const uint32_t w = std::max(1u, width), h = std::max(1u, height);   // never a 0-sized back buffer
	if (w == m_ClientWidth && h == m_ClientHeight) return;              // no actual change

	// Update the LOGICAL size IMMEDIATELY. GetScreenSize()/GetClientWidth() feed UI layout + input
	// hit-testing, which consumers read every frame -- lagging these broke Game01's buttons (drawn/
	// hit-tested against a stale coordinate space, because Game01's loop never calls ApplyPendingResize).
	// Only the GPU BUFFER teardown is deferred (ApplyPendingResize), so a WM_SIZE arriving reentrantly
	// mid-frame can't release buffers under an in-flight frame.
	m_ClientWidth  = w;
	m_ClientHeight = h;
	m_ResizePending = true;
}

// Does the actual swap-chain/depth teardown + recreate. MUST be called between frames (no frame in
// flight) -- the render loop calls it at the top of each iteration. Coalesces a drag's storm of
// WM_SIZE calls into a single recreate at the final size. m_ClientWidth/Height were already set in
// Resize(); this just rebuilds the GPU buffers to match.
void RendererCore::ApplyPendingResize()
{
	if (!m_ResizePending) return;
	m_ResizePending = false;

	// SNAPSHOT the target size BEFORE Flush() (which has an undefined delay). In this single-threaded
	// design m_ClientWidth/Height can't change mid-call -- Resize() runs on the same (main) thread and
	// Flush() blocks on WaitForSingleObject, which does NOT pump messages, so no WM_SIZE dispatches
	// during it. Snapshotting anyway guarantees the back buffers AND depth buffer use the SAME size, and
	// stays correct if a separate render thread is ever added (where this WOULD be a live race).
	// (Credit: a PGE3 beta tester flagged this race class.)
	const uint32_t w = m_ClientWidth, h = m_ClientHeight;

	Flush();                                        // GPU fully idle before we release the buffers
	for (int i = 0; i < NumFrames; ++i)
	{
		m_BackBuffers[i].Reset();                   // release before resizing the chain
		// Flush() guaranteed the GPU is FULLY idle, so every fence value is equally "reached".
		m_FrameFenceValues[i] = m_FrameFenceValues[m_FrameResourceIndex];
	}

	DXGI_SWAP_CHAIN_DESC desc = {};
	ThrowIfFailed(m_SwapChain->GetDesc(&desc));
	ThrowIfFailed(m_SwapChain->ResizeBuffers(NumFrames, w, h, desc.BufferDesc.Format, desc.Flags));

	m_ClientWidth = w;   // pin the logical size to the snapshot the buffers were actually created at, so
	m_ClientHeight = h;  // CreateDepthBuffer() (reads m_ClientWidth/Height) matches the back buffers exactly.
	m_CurrentBackBufferIndex = m_SwapChain->GetCurrentBackBufferIndex();
	// Bump the generation so the new back buffers get distinct debug names, and log it. A resize that
	// lands BETWEEN BeginFrame and PresentFrame is the classic cause of "resource state is invalid for
	// use as a render target": PRE recorded its PRESENT->RENDER_TARGET barrier against the buffers that
	// just got released, and the freshly created ones start in COMMON (0x0) with nothing transitioning
	// them. SetWindowPos/ShowWindow dispatch WM_SIZE SYNCHRONOUSLY, so a resize triggered from game
	// code during Update() does exactly that without ever going through the message pump.
	++m_SwapChainGeneration;
	{
		char b[128];
		sprintf_s(b, "[Resize] swap chain -> %ux%u, generation %u, backBufferIndex %u\n",
			w, h, m_SwapChainGeneration, m_CurrentBackBufferIndex);
		OutputDebugStringA(b);
	}
	UpdateRenderTargetViews();
	CreateDepthBuffer();   // depth buffer must match the new client size (Flush() above idled the GPU)
}

bool RendererCore::ShouldSkipFrame()
{
	if (!m_IsInitialized || !m_SwapChain) return false;
	// Minimized (reliable signal from WM_SIZE == SIZE_MINIMIZED): 0/1x1 client area -- never render into
	// it. This is the dependable minimize skip PRESENT_TEST couldn't give us.
	if (m_Minimized) return true;
	// A pending resize (WM_SIZE / minimize / restore): recreate the swap chain NOW (at the loop top),
	// then SKIP this frame and settle -- so the heavy frame renders only AFTER the swap chain has
	// re-allocated + stabilized, not back-to-back with the recreate. (BeginFrame also calls
	// ApplyPendingResize for consumers that don't call ShouldSkipFrame, e.g. Game01, whose light frames
	// don't need the settle; whichever runs first applies it, the other early-returns on the flag.)
	if (m_ResizePending) { ApplyPendingResize(); m_SettleFrames = 8; return true; }
	// DXGI_PRESENT_TEST does NOT present -- it only reports whether a present WOULD succeed, returning
	// DXGI_STATUS_OCCLUDED when the window is minimized / fully covered / not yet visible. In that state
	// rendering + presenting a heavy frame device-hangs the GPU, so skip -- and settle after it clears.
	if (m_SwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
		m_SettleFrames = 8;
		return true;
	}
	// Post-transition settle: keep skipping a few frames so the swap chain fully re-stabilizes (DWM
	// handing the display plane back / re-establishing flip) BEFORE we render a heavy frame into it.
	if (m_SettleFrames > 0) { --m_SettleFrames; return true; }
	return false;
}

void RendererCore::Cleanup()
{
	if (!m_IsInitialized) return;
	Flush();                                       // wait for the GPU to finish
	if (m_FenceEvent) { ::CloseHandle(m_FenceEvent); m_FenceEvent = nullptr; }
	m_IsInitialized = false;
	// ComPtr members release themselves.
}

// ===========================================================================
// Creation steps
// ===========================================================================
void RendererCore::CreateConstantBuffers() {
	for (int i = 0; i < NumFrames; ++i) {
		CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
		CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(ConstantBufferData));
		ThrowIfFailed(m_Device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ConstantBuffers[i])
		));
		m_ConstantBuffers[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_MappedConstantBufferData[i]));
	}
}

ComPtr<IDXGIAdapter4> RendererCore::GetAdapter(bool useWarp)
{
	ComPtr<IDXGIFactory4> dxgiFactory;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
	ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

	ComPtr<IDXGIAdapter1> dxgiAdapter1;
	ComPtr<IDXGIAdapter4> dxgiAdapter4;

	if (useWarp)
	{
		ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
		ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
	}
	else
	{
		SIZE_T maxDedicatedVideoMemory = 0;
		for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			DXGI_ADAPTER_DESC1 desc;
			dxgiAdapter1->GetDesc1(&desc);
			if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
				SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(), D3D_FEATURE_LEVEL_11_0,
					__uuidof(ID3D12Device), nullptr)) &&
				desc.DedicatedVideoMemory > maxDedicatedVideoMemory)
			{
				maxDedicatedVideoMemory = desc.DedicatedVideoMemory;
				ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
			}
		}
	}
	return dxgiAdapter4;
}

ComPtr<ID3D12Device2> RendererCore::CreateDevice(ComPtr<IDXGIAdapter4> adapter)
{
	ComPtr<ID3D12Device2> device;
	ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
#if defined(_DEBUG)
	ComPtr<ID3D12InfoQueue> pInfoQueue;
	if (SUCCEEDED(device.As(&pInfoQueue)))
	{
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		// WARNING is deliberately NOT a break. D3D12 emits a warning during normal process
		// TEARDOWN ("Process is terminating. Using simple reporting.") which carries no message ID,
		// so it cannot be filtered by ID and it breaks into the debugger on every clean exit --
		// looking exactly like a crash despite the process returning 0. Warnings are also routinely
		// benign (driver hints, redundant state). CORRUPTION and ERROR still break, which is what
		// actually catches API misuse.
		//
		// Turn this back on temporarily when bringing up a new pass and you want every warning to
		// stop the world; just expect the exit-time break to come with it.
		// pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

		D3D12_MESSAGE_SEVERITY Severities[] = { D3D12_MESSAGE_SEVERITY_INFO };
		D3D12_MESSAGE_ID DenyIds[] = {
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
			D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
			D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,
		};
		D3D12_INFO_QUEUE_FILTER NewFilter = {};
		NewFilter.DenyList.NumSeverities = _countof(Severities);
		NewFilter.DenyList.pSeverityList = Severities;
		NewFilter.DenyList.NumIDs = _countof(DenyIds);
		NewFilter.DenyList.pIDList = DenyIds;
		ThrowIfFailed(pInfoQueue->PushStorageFilter(&NewFilter));
	}
#endif
	return device;
}

ComPtr<ID3D12CommandQueue> RendererCore::CreateCommandQueue(D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12CommandQueue> queue;
	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type = type;
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 0;

	HRESULT hr = m_Device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
	if (FAILED(hr)) {
		char buf[256];
		sprintf_s(buf, "CreateCommandQueue failed with HRESULT 0x%08X", hr);
		MessageBoxA(NULL, buf, "Error", MB_OK);
		throw std::runtime_error(buf);
	}
	return queue;
}

bool RendererCore::CheckTearingSupport()
{
	BOOL allowTearing = FALSE;
	ComPtr<IDXGIFactory4> factory4;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
	{
		ComPtr<IDXGIFactory5> factory5;
		if (SUCCEEDED(factory4.As(&factory5)))
		{
			if (FAILED(factory5->CheckFeatureSupport(
				DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
				allowTearing = FALSE;
		}
	}
	return allowTearing == TRUE;
}

ComPtr<IDXGISwapChain4> RendererCore::CreateSwapChain(HWND hWnd, uint32_t width, uint32_t height, uint32_t bufferCount)
{
	ComPtr<IDXGISwapChain4> dxgiSwapChain4;
	ComPtr<IDXGIFactory4> dxgiFactory4;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
	ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.Stereo = FALSE;
	desc.SampleDesc = { 1, 0 };
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = bufferCount;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	desc.Flags = m_TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	ComPtr<IDXGISwapChain1> swapChain1;
	ThrowIfFailed(dxgiFactory4->CreateSwapChainForHwnd(
		m_CommandQueue.Get(), hWnd, &desc, nullptr, nullptr, &swapChain1));

	// We handle Alt+Enter fullscreen ourselves.
	ThrowIfFailed(dxgiFactory4->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));
	ThrowIfFailed(swapChain1.As(&dxgiSwapChain4));
	return dxgiSwapChain4;
}

ComPtr<ID3D12DescriptorHeap> RendererCore::CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors)
{
	ComPtr<ID3D12DescriptorHeap> heap;
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.NumDescriptors = numDescriptors;
	desc.Type = type;

	if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	else
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	ThrowIfFailed(m_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)));
	return heap;
}

void RendererCore::UpdateRenderTargetViews()
{
	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	for (int i = 0; i < NumFrames; ++i)
	{
		ComPtr<ID3D12Resource> backBuffer;
		ThrowIfFailed(m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));
		// EXPLICIT desc, not nullptr: passing nullptr inherits the resource's own UNORM format, which
		// is exactly the "no encode on write" behaviour we are fixing. Naming the _SRGB variant here is
		// what makes the hardware convert linear shader output back to display space. See
		// BackBufferRTVFormat in the header for why the resource itself stays UNORM.
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format        = BackBufferRTVFormat;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		m_Device->CreateRenderTargetView(backBuffer.Get(), &rtvDesc, rtvHandle);
		m_BackBuffers[i] = backBuffer;
		// Name them: an unnamed resource in a D3D12 debug-layer message ("Unnamed ID3D12Resource
		// Object") tells you nothing, and a wrong-resource-state error is exactly the case where you
		// need to know WHICH buffer, and from which swap-chain generation, was involved.
		{
			wchar_t nm[64];
			swprintf_s(nm, L"BackBuffer[%d] gen%u", i, m_SwapChainGeneration);
			backBuffer->SetName(nm);
		}
		rtvHandle.Offset(m_RTVDescriptorSize);
	}
}

void RendererCore::CreateDepthBuffer()
{
	m_DepthBuffer.Reset();   // release the previous one first (Resize calls this after Flush())

	// The optimized clear value MUST match the value the PRE list clears with (1.0), or the debug
	// layer warns and the driver can't fast-clear.
	D3D12_CLEAR_VALUE clear = {};
	clear.Format = DepthFormat;
	clear.DepthStencil.Depth = 1.0f;
	clear.DepthStencil.Stencil = 0;

	D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DepthFormat, m_ClientWidth, m_ClientHeight,
		1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

	ThrowIfFailed(m_Device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&m_DepthBuffer)));
	m_DepthBuffer->SetName(L"DepthBuffer (3D)");

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Format = DepthFormat;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	m_Device->CreateDepthStencilView(m_DepthBuffer.Get(), &dsvDesc,
		m_DSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
}

ComPtr<ID3D12CommandAllocator> RendererCore::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12CommandAllocator> allocator;
	ThrowIfFailed(m_Device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)));
	return allocator;
}

ComPtr<ID3D12GraphicsCommandList> RendererCore::CreateCommandList(ComPtr<ID3D12CommandAllocator> allocator, D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12GraphicsCommandList> list;
	ThrowIfFailed(m_Device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
	ThrowIfFailed(list->Close());   // created in the "recording" state; close so Render can Reset it
	return list;
}

ComPtr<ID3D12Fence> RendererCore::CreateFence()
{
	ComPtr<ID3D12Fence> fence;
	ThrowIfFailed(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	return fence;
}

HANDLE RendererCore::CreateEventHandle()
{
	HANDLE fenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	assert(fenceEvent && "Failed to create fence event.");
	return fenceEvent;
}

// ===========================================================================
// Synchronization
// ===========================================================================
uint64_t RendererCore::Signal()
{
	uint64_t valueForSignal = ++m_FenceValue;
	ThrowIfFailed(m_CommandQueue->Signal(m_Fence.Get(), valueForSignal));
	return valueForSignal;
}

uint64_t RendererCore::SignalCompute()
{
	// SEPARATE fence + counter from Signal(). Sharing one fence across both queues was a real bug:
	// a fence reports only the MAX value signaled, so a fast compute Signal could push m_Fence past a
	// graphics frame value BEFORE the graphics queue actually finished it -- making BeginFrame's
	// WaitForFenceValue(m_FrameFenceValues[slot]) return early and recycle a command list the GPU was
	// still drawing from (DEVICE_HUNG / VA=0, mid-list, load-gated). m_ComputeFence is advanced only
	// here; the graphics queue Wait()s on it GPU-side (see PresentFrame) to order particle reads.
	uint64_t valueForSignal = ++m_ComputeFenceValue;
	ThrowIfFailed(m_ComputeQueue->Signal(m_ComputeFence.Get(), valueForSignal));
	return valueForSignal;
}

namespace {
	// Carries the per-wait state for the GPU-fence -> fiber bridge. One Win32 event per wait
	// (not a shared one) so overlapping waits can't stomp each other. The DirectEvent* is the
	// pooled rendezvous handed to us by WaitOnEventDirectArmed -- we signal it by pointer, so
	// there is no name lookup / registry / global lock on the wakeup path.
	struct FenceWaitCtx {
		HANDLE      win32Event = nullptr;
		HANDLE      waitHandle = nullptr;
		JLib::DirectEvent* evt = nullptr;
	};
	VOID CALLBACK FenceWaitCallback(PVOID param, BOOLEAN /*timedOut*/) {
		auto* c = static_cast<FenceWaitCtx*>(param);
		c->evt->Signal();
		::UnregisterWaitEx(c->waitHandle, nullptr); // one-shot, non-blocking cleanup
		::CloseHandle(c->win32Event);
		delete c;
	}
}

void RendererCore::WaitForFenceValue(uint64_t value, std::chrono::milliseconds dur, ID3D12Fence* fence)
{
	if (!fence) fence = m_Fence.Get();   // default: the graphics frame fence

	if (fence->GetCompletedValue() >= value)
		return; // already complete -- no wait

	auto& sched = JLib::TaskScheduler::Instance();
	if (sched.IsOnFiber()) {
		// On a fiber: SUSPEND it (freeing the worker to run other jobs) until the GPU
		// reaches 'value'. Direct/handle event: no name, no eventRegistry, no registryMtx.
		while (fence->GetCompletedValue() < value) {
			sched.WaitOnEventDirectArmed([fence, value](JLib::DirectEvent* e) {
				auto* c = new FenceWaitCtx();
				c->evt        = e;
				c->win32Event = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
				ThrowIfFailed(fence->SetEventOnCompletion(value, c->win32Event));
				::RegisterWaitForSingleObject(&c->waitHandle, c->win32Event,
					FenceWaitCallback, c, INFINITE, WT_EXECUTEONLYONCE);
			});
		}
		return;
	}

	// Off-fiber (main thread, Resize/Cleanup): plain blocking wait -- no spin, 0% CPU.
	ThrowIfFailed(fence->SetEventOnCompletion(value, m_FenceEvent));
	::WaitForSingleObject(m_FenceEvent, static_cast<DWORD>(dur.count()));
}

void RendererCore::Flush()
{
	// Idle BOTH queues, each on its OWN fence (they no longer share one -- see SignalCompute). Both waits
	// go through WaitForFenceValue, so if Flush() runs on a scheduler fiber BOTH drains SUSPEND the fiber
	// via the DirectEvent bridge (freeing the worker) instead of parking the thread -- no spinning either way.
	WaitForFenceValue(Signal());                                                          // graphics (m_Fence)
	if (m_ComputeQueue)
		WaitForFenceValue(SignalCompute(), std::chrono::milliseconds::max(), m_ComputeFence.Get()); // compute
}

// ===========================================================================
// Debug + diagnostics
// ===========================================================================
void RendererCore::EnableDebugLayer()
{
#if defined(_DEBUG)
	// The BASE debug layer validates EVERY D3D12 API call on the CPU. With a heavy 3D scene (~16k draws
	// -> ~80k validated calls/frame) THAT validation, not /Od codegen, is the dominant cost -- it's what
	// makes a Debug build a ~17 fps slideshow while Release (no layer at all) runs 120+. So it's a toggle:
	// leave it ON for correctness work (it catches most usage errors -- bad barriers, wrong states); flip
	// it OFF to actually ITERATE in Debug at a usable framerate. You keep the full C++ debugger AND DRED
	// (armed below, outside this block, in all configs) either way -- you only lose live D3D usage
	// validation until you turn it back on. /Od is still there but is the smaller, secondary factor.
	constexpr bool kEnableDebugLayer = true;   // <-- set false for a fast, still-breakpointable Debug build
	if (kEnableDebugLayer)
	{
		ComPtr<ID3D12Debug> debugInterface;
		ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
		debugInterface->EnableDebugLayer();

		// GPU-Based Validation instruments EVERY shader and validates resource states / descriptors on the
		// GPU TIMELINE -- a further 10-50x on top of the base layer (this was the original ~4 fps). OPT-IN:
		// flip true only when hunting a specific GPU-timeline bug (bad barrier / stale descriptor / OOB
		// sample). Its companion SynchronizedCommandQueueValidation SERIALIZES the queues -- which is what
		// historically MASKED the async-compute shared-fence race in Debug ("never crashed"); with it off
		// Debug faithfully mirrors Release timing (safe now that the race is fixed -- see m_ComputeFence).
		constexpr bool kEnableGpuBasedValidation = false;
		if (kEnableGpuBasedValidation)
		{
			ComPtr<ID3D12Debug1> debug1;
			if (SUCCEEDED(debugInterface.As(&debug1)))
			{
				debug1->SetEnableGPUBasedValidation(TRUE);
				debug1->SetEnableSynchronizedCommandQueueValidation(TRUE);
			}
		}
	}
#endif
	// DRED (Device Removed Extended Data): on a TDR / device-removed, this records the GPU's
	// command "breadcrumbs" and the faulting virtual address. MUST be set before the device is
	// created. Enabled in all configs so a Release crash is diagnosable too.
	ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
	{
		dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
		dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
	}
}

// Dump DRED data after a device-removed. Call this when a GPU submit/present/fence returns
// DXGI_ERROR_DEVICE_REMOVED/HUNG -- the output goes to the VS Output window.
void RendererCore::LogDeviceRemoved()
{
	char line[256];
	HRESULT reason = m_Device ? m_Device->GetDeviceRemovedReason() : E_FAIL;
	sprintf_s(line, "\n*** DEVICE REMOVED. reason = 0x%08lX\n", (unsigned long)reason);
	OutputDebugStringA(line);

	ComPtr<ID3D12DeviceRemovedExtendedData> dred;
	if (!m_Device || FAILED(m_Device->QueryInterface(IID_PPV_ARGS(&dred))))
	{
		OutputDebugStringA("  (DRED interface unavailable)\n");
		return;
	}

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
	{
		const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
		while (node)
		{
			UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
			if (last == node->BreadcrumbCount) { node = node->pNext; continue; }
			sprintf_s(line, "  [breadcrumb] list='%S' queue='%S' completedOps=%u / %u\n",
				node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"?",
				node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"?",
				last, node->BreadcrumbCount);
			OutputDebugStringA(line);
			if (node->pCommandHistory && last < node->BreadcrumbCount)
			{
				sprintf_s(line, "        died on op #%u = %d\n", last, (int)node->pCommandHistory[last]);
				OutputDebugStringA(line);
			}
			node = node->pNext;
		}
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)))
	{
		sprintf_s(line, "  [pagefault] VA = 0x%llX\n", (unsigned long long)pageFault.PageFaultVA);
		OutputDebugStringA(line);
		for (auto* n = pageFault.pHeadExistingAllocationNode; n; n = n->pNext)
		{
			sprintf_s(line, "        live   alloc '%S' type=%d\n",
				n->ObjectNameW ? n->ObjectNameW : L"?", (int)n->AllocationType);
			OutputDebugStringA(line);
		}
		for (auto* n = pageFault.pHeadRecentFreedAllocationNode; n; n = n->pNext)
		{
			sprintf_s(line, "        freed  alloc '%S' type=%d  <-- recently freed near the fault\n",
				n->ObjectNameW ? n->ObjectNameW : L"?", (int)n->AllocationType);
			OutputDebugStringA(line);
		}
	}
	OutputDebugStringA("*** end DRED dump\n");
}

void RendererCore::ExecuteUploadCommand(std::function<void(ID3D12GraphicsCommandList*)> task) {
	// PRIVATE list, NOT m_CommandList.
	//
	// m_CommandList is the PRE list: BeginFrame records the back buffer's PRESENT -> RENDER_TARGET
	// barrier plus the RTV/DSV clears into it, closes it, and PresentFrame submits it first. This
	// function used to Reset() that same list -- which silently DISCARDED the barrier and the clears
	// whenever an upload happened mid-frame.
	//
	// That is exactly what a scene constructor does: pushing a scene from Update() runs between
	// BeginFrame and PresentFrame, and loading its textures called straight through to here. The
	// frame then submitted a PRE list containing only an already-executed upload, so every worker
	// list drew into a back buffer still in PRESENT:
	//   "Using Draw on Command List 'Worker[layer0][t0][f1]': Resource state
	//    (D3D12_RESOURCE_STATE_[COMMON|PRESENT]) of resource 'BackBuffer[1] gen0' is invalid for use
	//    as a render target" -- EXECUTION ERROR #538: INVALID_SUBRESOURCE_STATE.
	// Debug-only symptom purely because the debug layer is the only thing that checks; in Release the
	// same frame ran with a missing barrier and a skipped clear, undefined rather than correct.
	//
	// Created lazily so this has no ordering dependency on the rest of device init.
	if (!m_UploadList) {
		ThrowIfFailed(m_Device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_UploadAllocator)));
		ThrowIfFailed(m_Device->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_UploadAllocator.Get(), nullptr,
			IID_PPV_ARGS(&m_UploadList)));
		ThrowIfFailed(m_UploadList->Close());
		m_UploadList->SetName(L"UploadList (ExecuteUploadCommand)");
	}

	// Safe to reset both: the Flush() at the end of the previous call guarantees the GPU is done
	// with them, and this path is main-thread-only (scene construction / asset loading).
	ThrowIfFailed(m_UploadAllocator->Reset());
	ThrowIfFailed(m_UploadList->Reset(m_UploadAllocator.Get(), nullptr));
	task(m_UploadList.Get());
	ThrowIfFailed(m_UploadList->Close());
	ID3D12CommandList* ppCommandLists[] = { m_UploadList.Get() };
	m_CommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
	Flush(); // Wait for GPU to finish the upload
}

void RendererCore::UpdateGlobalUniforms(DirectX::XMFLOAT2 screenSize, DirectX::XMFLOAT2 cameraPos, float cameraZoom) {
	float ar = screenSize.x / (screenSize.y > 0.0f ? screenSize.y : 1.0f);
	// Sync-rotation slot (m_FrameResourceIndex), not the swap-chain back-buffer index -- this
	// constant buffer is a CPU/GPU-sync resource, unrelated to which back buffer is on screen.
	ConstantBufferData* pData = reinterpret_cast<ConstantBufferData*>(m_MappedConstantBufferData[m_FrameResourceIndex]);
	pData->screenSize = screenSize;
	pData->aspectRatio = ar;
	pData->cameraPos = cameraPos;
	pData->cameraZoom = cameraZoom;
}

DirectX::XMFLOAT2 RendererCore::GetScreenSize() const {
	return DirectX::XMFLOAT2(static_cast<float>(m_ClientWidth), static_cast<float>(m_ClientHeight));
}

DirectX::XMFLOAT2 RendererCore::GetNDC(float targetX, float targetY)
{
	DirectX::XMFLOAT2 ndc;
	ndc.x = (targetX / (m_ClientWidth / 2.0f)) - 1.0f;
	ndc.y = 1.0f - (targetY / (m_ClientHeight / 2.0f));
	return ndc;
}

// ===========================================================================
// Particle system
// ===========================================================================
void RendererCore::InitializeParticleSystem() {
	m_ComputeRootSignature = CreateComputeRootSignature();
	m_SpawnPSO = CreateComputePipelineState(L"shaders\\SpawnParticles.cso"); // shared -- popping a dead slot is identical regardless of pool
	m_ResetAliveCounterPSO = CreateComputePipelineState(L"shaders\\ResetAliveCounter.cso"); // shared
	m_CompactPSO = CreateComputePipelineState(L"shaders\\CompactParticles.cso"); // shared
	m_ParticleRootSignature = CreateParticleRootSignature();
	m_ParticlePSO = CreateParticlePipelineState(L"shaders\\ParticleVS.cso", L"shaders\\ParticlePS.cso"); // shared draw shader for now

	m_ArgsRootSignature = CreateArgsRootSignature();
	m_BuildArgsPSO = CreateComputePipelineState(L"shaders\\BuildDispatchArgs.cso", m_ArgsRootSignature.Get());

	D3D12_INDIRECT_ARGUMENT_DESC dispatchArg = {};
	dispatchArg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
	D3D12_COMMAND_SIGNATURE_DESC cmdSigDesc = {};
	cmdSigDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
	cmdSigDesc.NumArgumentDescs = 1;
	cmdSigDesc.pArgumentDescs = &dispatchArg;
	// pRootSignature is only required when the command signature also patches root
	// constants/descriptors indirectly -- a pure DISPATCH signature doesn't touch the root
	// signature at all, so nullptr is correct here.
	ThrowIfFailed(m_Device->CreateCommandSignature(&cmdSigDesc, nullptr, IID_PPV_ARGS(&m_DispatchCommandSignature)));
}

uint32_t RendererCore::RegisterParticleEffect(const std::wstring& updateCsPath, uint32_t maxParticles, bool screenSpace, int zLayer,
	TextureHandle texture, uint32_t atlasFramesX, uint32_t atlasFramesY, DirectX::XMFLOAT4 colorEnd, float gravityScale) {
	ParticleEffectPool pool;
	pool.maxParticles = maxParticles;
	pool.zLayer = zLayer;
	pool.atlasFramesX = atlasFramesX;
	pool.atlasFramesY = atlasFramesY;
	pool.colorEnd = colorEnd;
	pool.screenSpace = screenSpace;
	pool.gravityScale = gravityScale;

	D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(
		(UINT64)maxParticles * sizeof(Particle2D), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	ThrowIfFailed(m_Device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pool.particleBuffer)
	));

	// Per-frame draw snapshots (see ParticleEffectPool::drawBuffer). Same size/stride as
	// particleBuffer but NO UAV flag -- these are only ever a CopyBufferRegion destination and a
	// vertex-shader SRV source, never unordered-access written. Created in COMMON (DEFAULT-heap
	// buffers MUST be created in COMMON -- the debug layer rejects a read state here -- same as
	// every sibling buffer below) and they REST in COMMON: UpdateParticles transitions each
	// COMMON->COPY_DEST for the snapshot and back to COMMON, then the particle draw reads it via
	// buffer implicit promotion (COMMON auto-promotes to NON_PIXEL_SHADER_RESOURCE on the SRV read
	// and decays back after the graphics ExecuteCommandLists -- guaranteed for buffers). COMMON is
	// also the correct neutral state for the compute->graphics cross-queue handoff.
	D3D12_RESOURCE_DESC drawBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(
		(UINT64)maxParticles * sizeof(Particle2D), D3D12_RESOURCE_FLAG_NONE);
	for (int i = 0; i < NumFrames; ++i) {
		ThrowIfFailed(m_Device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &drawBufferDesc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pool.drawBuffer[i])
		));
	}

	const UINT64 deadListDataSize = (UINT64)maxParticles * sizeof(uint32_t);
	const UINT64 deadListCounterOffset = AlignUavCounterOffset(deadListDataSize);
	D3D12_RESOURCE_DESC deadListDesc = CD3DX12_RESOURCE_DESC::Buffer(
		deadListCounterOffset + sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	ThrowIfFailed(m_Device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &deadListDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pool.deadListBuffer)
	));

	// aliveListBuffer: same shape as deadListBuffer (maxParticles uints + a hidden UAV counter) --
	// rebuilt from scratch every frame (Reset then Compact), unlike deadList which persists/
	// recycles across frames.
	const UINT64 aliveListDataSize = (UINT64)maxParticles * sizeof(uint32_t);
	const UINT64 aliveListCounterOffset = AlignUavCounterOffset(aliveListDataSize);
	D3D12_RESOURCE_DESC aliveListDesc = CD3DX12_RESOURCE_DESC::Buffer(
		aliveListCounterOffset + sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	ThrowIfFailed(m_Device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &aliveListDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pool.aliveListBuffer)
	));
	pool.aliveListCounterOffset = aliveListCounterOffset;

	D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
	uavHeapDesc.NumDescriptors = 4;
	uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(m_Device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&pool.uavHeap)));
	const UINT uavDescSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.Buffer.NumElements = maxParticles;
	uavDesc.Buffer.StructureByteStride = sizeof(Particle2D);
	m_Device->CreateUnorderedAccessView(pool.particleBuffer.Get(), nullptr, &uavDesc, pool.uavHeap->GetCPUDescriptorHandleForHeapStart());

	D3D12_UNORDERED_ACCESS_VIEW_DESC deadListUavDesc = {};
	deadListUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	deadListUavDesc.Format = DXGI_FORMAT_UNKNOWN;
	deadListUavDesc.Buffer.NumElements = maxParticles;
	deadListUavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
	deadListUavDesc.Buffer.CounterOffsetInBytes = deadListCounterOffset;
	CD3DX12_CPU_DESCRIPTOR_HANDLE deadListHandle(pool.uavHeap->GetCPUDescriptorHandleForHeapStart(), 1, uavDescSize);
	m_Device->CreateUnorderedAccessView(pool.deadListBuffer.Get(), pool.deadListBuffer.Get(), &deadListUavDesc, deadListHandle);

	// u2: counted view over aliveListBuffer (AliveList in ParticleCommon.hlsli) -- Append semantics
	// via IncrementCounter, used by CompactParticles.hlsl, and plain indexed reads by the update shaders.
	D3D12_UNORDERED_ACCESS_VIEW_DESC aliveListUavDesc = {};
	aliveListUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	aliveListUavDesc.Format = DXGI_FORMAT_UNKNOWN;
	aliveListUavDesc.Buffer.NumElements = maxParticles;
	aliveListUavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
	aliveListUavDesc.Buffer.CounterOffsetInBytes = aliveListCounterOffset;
	CD3DX12_CPU_DESCRIPTOR_HANDLE aliveListHandle(pool.uavHeap->GetCPUDescriptorHandleForHeapStart(), 2, uavDescSize);
	m_Device->CreateUnorderedAccessView(pool.aliveListBuffer.Get(), pool.aliveListBuffer.Get(), &aliveListUavDesc, aliveListHandle);

	// u3: RAW, uncounted view over the SAME aliveListBuffer resource -- lets AliveCounterRaw.Load
	// read the counter as a plain uint (ResetAliveCounter.hlsl/BuildDispatchArgs.hlsl/the update
	// shaders' ResolveAliveIndex) without touching u2's Increment/DecrementCounter semantics.
	D3D12_UNORDERED_ACCESS_VIEW_DESC aliveListRawDesc = {};
	aliveListRawDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	aliveListRawDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	aliveListRawDesc.Buffer.NumElements = (UINT)((aliveListCounterOffset + sizeof(uint32_t)) / sizeof(uint32_t));
	aliveListRawDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	CD3DX12_CPU_DESCRIPTOR_HANDLE aliveListRawHandle(pool.uavHeap->GetCPUDescriptorHandleForHeapStart(), 3, uavDescSize);
	m_Device->CreateUnorderedAccessView(pool.aliveListBuffer.Get(), nullptr, &aliveListRawDesc, aliveListRawHandle);

	// argsBuffer: 3 UINTs (ThreadGroupCountX/Y/Z), read directly by ExecuteIndirect every frame --
	// written by BuildDispatchArgs.hlsl, never touched by the CPU after creation.
	D3D12_RESOURCE_DESC argsBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(
		3 * sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	ThrowIfFailed(m_Device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &argsBufferDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&pool.argsBuffer)
	));

	D3D12_DESCRIPTOR_HEAP_DESC argsHeapDesc = {};
	argsHeapDesc.NumDescriptors = 2;
	argsHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	argsHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(m_Device->CreateDescriptorHeap(&argsHeapDesc, IID_PPV_ARGS(&pool.argsHeap)));
	const UINT argsDescSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// u0: RAW view over the WHOLE aliveListBuffer (data + counter region) -- deliberately NOT the
	// counted view (that's aliveListUavDesc above, and only one counted view can exist per
	// resource). BuildDispatchArgs.hlsl only ever Load()s the counter's bytes through this one.
	D3D12_UNORDERED_ACCESS_VIEW_DESC argsAliveRawDesc = {};
	argsAliveRawDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	argsAliveRawDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	argsAliveRawDesc.Buffer.NumElements = (UINT)((aliveListCounterOffset + sizeof(uint32_t)) / sizeof(uint32_t));
	argsAliveRawDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	m_Device->CreateUnorderedAccessView(pool.aliveListBuffer.Get(), nullptr, &argsAliveRawDesc, pool.argsHeap->GetCPUDescriptorHandleForHeapStart());

	D3D12_UNORDERED_ACCESS_VIEW_DESC argsRawDesc = {};
	argsRawDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	argsRawDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	argsRawDesc.Buffer.NumElements = 3;
	argsRawDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	CD3DX12_CPU_DESCRIPTOR_HANDLE argsHandle(pool.argsHeap->GetCPUDescriptorHandleForHeapStart(), 1, argsDescSize);
	m_Device->CreateUnorderedAccessView(pool.argsBuffer.Get(), nullptr, &argsRawDesc, argsHandle);

	auto spawnBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(1024 * sizeof(Particle2D));
	CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
	ThrowIfFailed(m_Device->CreateCommittedResource(
		&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &spawnBufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&pool.spawnBuffer)
	));

	pool.updatePSO = CreateComputePipelineState(updateCsPath);

	// This pool's OWN draw allocator/list -- separate from m_ComputeList (compute-only) so its
	// draw can be submitted independently, interleaved with Renderer2D's zLayers in PresentFrame.
	for (int i = 0; i < NumFrames; ++i)
		pool.drawAllocators[i] = CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT);
	pool.drawList = CreateCommandList(pool.drawAllocators[0], D3D12_COMMAND_LIST_TYPE_DIRECT);

	m_ParticleEffects.push_back(std::move(pool));
	uint32_t effectID = (uint32_t)m_ParticleEffects.size() - 1;

	ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		SeedParticlePool(m_ParticleEffects[effectID], cmd);
		// Invalid handle -> flat-colored quads via a 1x1 white texture, so the draw pipeline
		// always has a real, valid SRV to bind (no separate "untextured" PSO/root-sig variant needed).
		m_ParticleEffects[effectID].texture = texture.IsValid()
			? texture
			: m_Renderer2D->GetResourceManager()->CreateSolidColorTexture(cmd);
	});
	m_ParticleEffects[effectID].particleUploadBuffer.Reset();
	m_ParticleEffects[effectID].deadListUploadBuffer.Reset();

	return effectID;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> RendererCore::CreateComputeRootSignature() {
	D3D12_DESCRIPTOR_RANGE uavRange = {};
	uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	// u0=particleBuffer, u1=deadList(counted), u2=aliveList(counted), u3=aliveList(raw) --
	// see ParticleCommon.hlsli. Shared by every update shader, SpawnParticles.hlsl,
	// ResetAliveCounter.hlsl, and CompactParticles.hlsl, even though most of them only touch a
	// subset -- a PSO's shader is allowed to use fewer resources than the root signature declares.
	uavRange.NumDescriptors = 4;
	uavRange.BaseShaderRegister = 0;

	D3D12_ROOT_PARAMETER params[3];
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[0].DescriptorTable.NumDescriptorRanges = 1;
	params[0].DescriptorTable.pDescriptorRanges = &uavRange;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
	params[1].Descriptor.ShaderRegister = 1; // t1
	params[1].Descriptor.RegisterSpace = 0;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[2].Constants.ShaderRegister = 0; // b0
	params[2].Constants.RegisterSpace = 0;
	params[2].Constants.Num32BitValues = 5; // DeltaTime, spawnCount, maxParticles, aliveListCounterOffset, GravityScale
	params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC desc = { _countof(params), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
	ComPtr<ID3DBlob> signature, error;
	D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);

	ComPtr<ID3D12RootSignature> rootSig;
	m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSig));
	return rootSig;
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> RendererCore::CreateComputePipelineState(const std::wstring& csPath, ID3D12RootSignature* rootSig) {
	auto csBlob = ReadFile(ExeRelative(csPath));
	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = rootSig ? rootSig : m_ComputeRootSignature.Get();
	psoDesc.CS = { csBlob.data(), csBlob.size() };
	ComPtr<ID3D12PipelineState> pso;
	ThrowIfFailed(m_Device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
	return pso;
}

// Descriptor table (2 RAW UAVs: u0 = a pool's aliveListBuffer counter region, u1 = its argsBuffer) +
// 1 32-bit constant (aliveListCounterOffset) at b0. Deliberately separate from
// m_ComputeRootSignature -- that one's table binds COUNTED UAVs (particleBuffer/deadList with
// Append/Consume semantics) which BuildDispatchArgs.hlsl must NOT touch; this one binds plain
// RAW views instead so reading the counter's bytes can't be confused with Increment/DecrementCounter.
Microsoft::WRL::ComPtr<ID3D12RootSignature> RendererCore::CreateArgsRootSignature() {
	D3D12_DESCRIPTOR_RANGE uavRange = {};
	uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	uavRange.NumDescriptors = 2;
	uavRange.BaseShaderRegister = 0;

	D3D12_ROOT_PARAMETER params[2];
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[0].DescriptorTable.NumDescriptorRanges = 1;
	params[0].DescriptorTable.pDescriptorRanges = &uavRange;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[1].Constants.ShaderRegister = 0; // b0
	params[1].Constants.RegisterSpace = 0;
	params[1].Constants.Num32BitValues = 1;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC desc = { _countof(params), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
	ComPtr<ID3DBlob> signature, error;
	D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);

	ComPtr<ID3D12RootSignature> rootSig;
	m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSig));
	return rootSig;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> RendererCore::CreateParticleRootSignature() {
	// param2: a 1-SRV descriptor table for this pool's texture (register t1 -- t0 is the raw
	// root SRV to the particle buffer above). Every pool always has a real texture bound (a
	// solid-color fallback if none was given -- see RegisterParticleEffect), so there's no
	// "no texture" variant of this root signature/PSO to maintain.
	CD3DX12_DESCRIPTOR_RANGE texRange;
	texRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // 1 SRV, register t1

	// param3: this pool's animation params (atlasFramesX, atlasFramesY, colorEnd) -- register
	// b1 (b0 is GlobalUniforms, param0). ALL-visibility since the frame/lerp math lives in the
	// vertex shader but conceivably a future pixel-shader effect might also want it.
	CD3DX12_ROOT_PARAMETER rootParameters[4];
	rootParameters[0].InitAsConstantBufferView(0);
	rootParameters[1].InitAsShaderResourceView(0, 0);
	rootParameters[2].InitAsDescriptorTable(1, &texRange, D3D12_SHADER_VISIBILITY_PIXEL);
	rootParameters[3].InitAsConstants(8, 1, 0, D3D12_SHADER_VISIBILITY_ALL); // 8 DWORDs: 2 uint + screenSpace + pad + float4

	// Same sampler every pool's texture uses -- no per-pool sampler variation needed yet.
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.ShaderRegister = 0; // register(s0)
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
	rsDesc.Init(_countof(rootParameters), rootParameters, 1, &sampler,
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> signature, error;
	ThrowIfFailed(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));

	ComPtr<ID3D12RootSignature> rootSig;
	ThrowIfFailed(m_Device->CreateRootSignature(0, signature->GetBufferPointer(),
		signature->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
	return rootSig;
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> RendererCore::CreateParticlePipelineState(
	const std::wstring& vsPath, const std::wstring& psPath) {
	auto vsBlob = ReadFile(ExeRelative(vsPath));
	auto psBlob = ReadFile(ExeRelative(psPath));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = m_ParticleRootSignature.Get();
	psoDesc.VS = { vsBlob.data(), vsBlob.size() };
	psoDesc.PS = { psBlob.data(), psBlob.size() };
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
	psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	// TRIANGLE, not POINT -- ParticleVS.hlsl now generates a procedural quad (6 verts via
	// SV_VertexID) per particle instead of a fixed-size point-list dot, which is what gives
	// particles a controllable size at all (POINTLIST has no size/UV support in D3D12).
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = RendererCore::BackBufferRTVFormat;
	psoDesc.SampleDesc.Count = 1;

	ComPtr<ID3D12PipelineState> pso;
	ThrowIfFailed(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
	return pso;
}

void RendererCore::SeedParticlePool(ParticleEffectPool& pool, ID3D12GraphicsCommandList* cmd) {
	std::vector<Particle2D> initial(pool.maxParticles);
	std::mt19937 rng(12345);
	std::uniform_real_distribution<float> posX(0.0f, (float)m_ClientWidth);
	std::uniform_real_distribution<float> posY(0.0f, (float)m_ClientHeight);
	std::uniform_real_distribution<float> vel(-50.0f, 50.0f);
	for (auto& p : initial) {
		p.position = { posX(rng), posY(rng) };
		p.velocity = { vel(rng), vel(rng) };
		p.lifetime = 5.0f;
		p.age = 0.0f;
		p.isActive = 0; // inactive until a spawn claims this slot
		p.size = 4.0f;
		p.color = { 1,1,1,1 };
	}

	const UINT64 bufferSize = (UINT64)pool.maxParticles * sizeof(Particle2D);
	CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
	ThrowIfFailed(m_Device->CreateCommittedResource(
		&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&pool.particleUploadBuffer)));

	void* mapped = nullptr;
	ThrowIfFailed(pool.particleUploadBuffer->Map(0, nullptr, &mapped));
	memcpy(mapped, initial.data(), bufferSize);
	pool.particleUploadBuffer->Unmap(0, nullptr);

	auto toDest = CD3DX12_RESOURCE_BARRIER::Transition(pool.particleBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->ResourceBarrier(1, &toDest);
	cmd->CopyBufferRegion(pool.particleBuffer.Get(), 0, pool.particleUploadBuffer.Get(), 0, bufferSize);
	auto toSRV = CD3DX12_RESOURCE_BARRIER::Transition(pool.particleBuffer.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	cmd->ResourceBarrier(1, &toSRV);

	const UINT64 deadListDataSize = (UINT64)pool.maxParticles * sizeof(uint32_t);
	const UINT64 deadListCounterOffset = AlignUavCounterOffset(deadListDataSize);
	const UINT64 deadListUploadSize = deadListCounterOffset + sizeof(uint32_t);

	std::vector<uint32_t> deadListInit(deadListUploadSize / sizeof(uint32_t), 0); // zero-fills the padding gap
	for (uint32_t idx = 0; idx < pool.maxParticles; ++idx) deadListInit[idx] = idx;
	deadListInit[deadListCounterOffset / sizeof(uint32_t)] = pool.maxParticles; // the counter value
	CD3DX12_HEAP_PROPERTIES counterUploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC counterUploadDesc = CD3DX12_RESOURCE_DESC::Buffer(deadListUploadSize);
	ThrowIfFailed(m_Device->CreateCommittedResource(
		&counterUploadHeapProps, D3D12_HEAP_FLAG_NONE, &counterUploadDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&pool.deadListUploadBuffer)));
	void* counterMapped = nullptr;
	ThrowIfFailed(pool.deadListUploadBuffer->Map(0, nullptr, &counterMapped));
	memcpy(counterMapped, deadListInit.data(), deadListUploadSize);
	pool.deadListUploadBuffer->Unmap(0, nullptr);

	auto deadListToDest = CD3DX12_RESOURCE_BARRIER::Transition(pool.deadListBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->ResourceBarrier(1, &deadListToDest);
	cmd->CopyBufferRegion(pool.deadListBuffer.Get(), 0, pool.deadListUploadBuffer.Get(), 0, deadListUploadSize);
	auto deadListToUAV = CD3DX12_RESOURCE_BARRIER::Transition(pool.deadListBuffer.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->ResourceBarrier(1, &deadListToUAV);

	// No data to seed for either -- both get fully (re)written every frame (aliveListBuffer by
	// Reset+Compact, argsBuffer by BuildDispatchArgs) before anything ever reads them. Just get
	// them out of COMMON so the first frame's UAV writes are legal.
	auto aliveListToUAV = CD3DX12_RESOURCE_BARRIER::Transition(pool.aliveListBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->ResourceBarrier(1, &aliveListToUAV);

	auto argsToUAV = CD3DX12_RESOURCE_BARRIER::Transition(pool.argsBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->ResourceBarrier(1, &argsToUAV);
}

// Compute-only: advances every pool's physics/spawn dispatch on the SHARED m_ComputeList. This
// never touches a render target, so pool order here doesn't matter and it always runs as one
// unit, before ANY particle or 2D draw -- only the DRAW half (RecordParticleDraw) needs to be
// ordered relative to Renderer2D's zLayers, which is why it's split out onto each pool's own list.
void RendererCore::UpdateParticles() {
	const int frame = m_FrameResourceIndex;
	const int backBuffer = m_CurrentBackBufferIndex;

	m_ComputeAllocators[frame]->Reset();
	m_ComputeList->Reset(m_ComputeAllocators[frame].Get(), nullptr); // PSO set per pool below

	for (auto& pool : m_ParticleEffects) {
		const UINT spawnCount = (UINT)pool.pendingSpawns.size();
		FlushSpawns(pool);

		// --- Compute pass: advance every particle in THIS pool (ONE dispatch) ---
		auto toUAV = CD3DX12_RESOURCE_BARRIER::Transition(pool.particleBuffer.Get(),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		m_ComputeList->ResourceBarrier(1, &toUAV);

		float dt = GetLastDeltaTime();
		UINT computeConstants[5] = { *reinterpret_cast<UINT*>(&dt), spawnCount, pool.maxParticles,
			(UINT)pool.aliveListCounterOffset, *reinterpret_cast<UINT*>(&pool.gravityScale) };

		// --- Rebuild the compacted alive-index list: Reset the counter, then re-Append every
		// currently-active particle's index (see ParticleCommon.hlsli / CompactParticles.hlsl).
		// Must happen every frame BEFORE the update dispatch, since the update dispatch is sized
		// and indexed entirely off what this pass just wrote.
		ID3D12DescriptorHeap* computeHeaps[] = { pool.uavHeap.Get() };
		m_ComputeList->SetDescriptorHeaps(1, computeHeaps);
		m_ComputeList->SetComputeRootSignature(m_ComputeRootSignature.Get());
		m_ComputeList->SetComputeRootDescriptorTable(0, pool.uavHeap->GetGPUDescriptorHandleForHeapStart());
		m_ComputeList->SetComputeRootShaderResourceView(1, pool.spawnBuffer->GetGPUVirtualAddress());
		m_ComputeList->SetComputeRoot32BitConstants(2, 5, computeConstants, 0);

		m_ComputeList->SetPipelineState(m_ResetAliveCounterPSO.Get());
		m_ComputeList->Dispatch(1, 1, 1);

		auto aliveResetBarrier = CD3DX12_RESOURCE_BARRIER::UAV(pool.aliveListBuffer.Get());
		m_ComputeList->ResourceBarrier(1, &aliveResetBarrier);

		m_ComputeList->SetPipelineState(m_CompactPSO.Get());
		const UINT compactGroupCount = (pool.maxParticles + 255) / 256;
		m_ComputeList->Dispatch(compactGroupCount, 1, 1);

		auto aliveCompactBarrier = CD3DX12_RESOURCE_BARRIER::UAV(pool.aliveListBuffer.Get());
		m_ComputeList->ResourceBarrier(1, &aliveCompactBarrier);

		// --- Build this pool's indirect dispatch args (aliveCount -> ThreadGroupCountX) ---
		// Entirely GPU-side: reads the alive list's counter (just rebuilt above), writes
		// {groupCount,1,1} into argsBuffer. Uses the SEPARATE args root sig/heap (RAW UAVs) --
		// never touches the counted AliveList view the compact/update shaders use.
		ID3D12DescriptorHeap* argsHeaps[] = { pool.argsHeap.Get() };
		m_ComputeList->SetDescriptorHeaps(1, argsHeaps);
		m_ComputeList->SetComputeRootSignature(m_ArgsRootSignature.Get());
		m_ComputeList->SetPipelineState(m_BuildArgsPSO.Get());
		m_ComputeList->SetComputeRootDescriptorTable(0, pool.argsHeap->GetGPUDescriptorHandleForHeapStart());
		UINT argsConstants[1] = { (UINT)pool.aliveListCounterOffset };
		m_ComputeList->SetComputeRoot32BitConstants(1, 1, argsConstants, 0);
		m_ComputeList->Dispatch(1, 1, 1);

		// argsBuffer must be INDIRECT_ARGUMENT for ExecuteIndirect to read it, and the write above
		// must be visible first -- a plain UAV barrier + state transition covers both.
		D3D12_RESOURCE_BARRIER argsBarriers[2] = {
			CD3DX12_RESOURCE_BARRIER::UAV(pool.argsBuffer.Get()),
			CD3DX12_RESOURCE_BARRIER::Transition(pool.argsBuffer.Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
		};
		m_ComputeList->ResourceBarrier(2, argsBarriers);

		// Back to the main compute heap/root sig/bindings (BuildDispatchArgs above switched to
		// the args heap) -- constants (dt/spawnCount/maxParticles/aliveListCounterOffset) and the
		// SRV/table bindings are all still exactly what the update dispatch needs, set once above.
		ID3D12DescriptorHeap* updateHeaps[] = { pool.uavHeap.Get() };
		m_ComputeList->SetDescriptorHeaps(1, updateHeaps);
		m_ComputeList->SetComputeRootSignature(m_ComputeRootSignature.Get());
		m_ComputeList->SetComputeRootDescriptorTable(0, pool.uavHeap->GetGPUDescriptorHandleForHeapStart());
		m_ComputeList->SetComputeRootShaderResourceView(1, pool.spawnBuffer->GetGPUVirtualAddress());
		m_ComputeList->SetComputeRoot32BitConstants(2, 5, computeConstants, 0);
		m_ComputeList->SetPipelineState(pool.updatePSO.Get()); // THIS pool's own behavior shader

		m_ComputeList->ExecuteIndirect(m_DispatchCommandSignature.Get(), 1, pool.argsBuffer.Get(), 0, nullptr, 0);

		// Back to UNORDERED_ACCESS so next frame's BuildDispatchArgs can write it again.
		auto argsBackToUAV = CD3DX12_RESOURCE_BARRIER::Transition(pool.argsBuffer.Get(),
			D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		m_ComputeList->ResourceBarrier(1, &argsBackToUAV);

		if (spawnCount > 0) {
			auto killToSpawnBarrier = CD3DX12_RESOURCE_BARRIER::UAV(pool.particleBuffer.Get());
			m_ComputeList->ResourceBarrier(1, &killToSpawnBarrier);

			m_ComputeList->SetPipelineState(m_SpawnPSO.Get());
			const UINT spawnGroupCount = (spawnCount + 255) / 256;
			m_ComputeList->Dispatch(spawnGroupCount, 1, 1);
		}

		auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(pool.particleBuffer.Get());
		m_ComputeList->ResourceBarrier(1, &uavBarrier);

		// Snapshot this frame's integrated state into drawBuffer[frame] so the graphics queue draws
		// from a SEPARATE resource -- that's what lets compute-N overlap graphics-(N-1) instead of
		// the two queues serializing on a shared particleBuffer (the old
		// m_ComputeQueue->Wait(m_LastGraphicsSignalValue) edge, now removed from PresentFrame).
		// particleBuffer is no longer read by graphics at all: it only ping-pongs UAV<->COPY_SOURCE
		// here and rests back in NON_PIXEL_SHADER_RESOURCE, exactly the state this loop's own
		// start-of-pass toUAV transition (top of the loop) and the seeding upload both expect --
		// that invariant is preserved unchanged; the copy is simply inserted in the middle.
		D3D12_RESOURCE_BARRIER preCopy[2] = {
			CD3DX12_RESOURCE_BARRIER::Transition(pool.particleBuffer.Get(),
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(pool.drawBuffer[frame].Get(),
				D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
		};
		m_ComputeList->ResourceBarrier(2, preCopy);

		m_ComputeList->CopyBufferRegion(pool.drawBuffer[frame].Get(), 0,
			pool.particleBuffer.Get(), 0, (UINT64)pool.maxParticles * sizeof(Particle2D));

		// drawBuffer goes back to COMMON (its rest/creation state), NOT explicitly to
		// NON_PIXEL_SHADER_RESOURCE: the particle draw promotes it from COMMON implicitly on the
		// SRV read and it decays back after that graphics ExecuteCommandLists, so every frame's
		// preCopy sees it in COMMON again -- no first-use special case, and COMMON is the correct
		// neutral state for handing this resource from the compute queue to the graphics queue.
		D3D12_RESOURCE_BARRIER postCopy[2] = {
			CD3DX12_RESOURCE_BARRIER::Transition(pool.particleBuffer.Get(),
				D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			CD3DX12_RESOURCE_BARRIER::Transition(pool.drawBuffer[frame].Get(),
				D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
		};
		m_ComputeList->ResourceBarrier(2, postCopy);
	}

	// 3D GPU particles ride this SAME async-compute list, so they share m_ComputeQueue + m_ComputeFence with
	// the 2D pools above (the graphics queue's Wait(m_ComputeFence) in PresentFrame gates their draw too).
	// Appended after the 2D pools, before Close: their compute integrate + buffer->drawBuffer[frame] snapshot.
	if (m_Renderer3D)
		m_Renderer3D->RecordParticleCompute(m_ComputeList.Get(), frame, GetLastDeltaTime());

	ThrowIfFailed(m_ComputeList->Close());

	// Draw half: one small list PER POOL, recorded here but submitted by PresentFrame wherever
	// this pool's zLayer falls relative to Renderer2D's layers.
	for (auto& pool : m_ParticleEffects)
		RecordParticleDraw(pool, frame, backBuffer);
}

// Binds straight to the back buffer -- no clear (PRE already did that) and no state transition
// (PRE already moved it to RENDER_TARGET; POST moves it back to PRESENT later). Safe to record
// AFTER the pool's compute dispatch above has closed m_ComputeList: the particleBuffer SRV
// transition already happened there, so this list just reads it.
void RendererCore::RecordParticleDraw(ParticleEffectPool& pool, int frame, int backBuffer) {
	pool.drawAllocators[frame]->Reset();
	pool.drawList->Reset(pool.drawAllocators[frame].Get(), m_ParticlePSO.Get());

	// pool.texture's gpuHandle lives in Renderer2D's SRV heap (that's where every texture --
	// sprite art, font atlas, and now particle textures -- gets loaded into), so THAT heap must
	// be bound here even though this list otherwise has nothing to do with 2D batching.
	ID3D12DescriptorHeap* srvHeaps[] = { m_Renderer2D->GetSrvHeap() };
	pool.drawList->SetDescriptorHeaps(1, srvHeaps);
	pool.drawList->SetGraphicsRootSignature(m_ParticleRootSignature.Get());
	pool.drawList->SetGraphicsRootConstantBufferView(0, m_ConstantBuffers[frame]->GetGPUVirtualAddress());
	// Draw from this frame's SNAPSHOT (UpdateParticles copied particleBuffer -> drawBuffer[frame]),
	// NOT particleBuffer itself -- reading a separate resource is what decouples this draw from the
	// compute queue's in-place integration and allows the two queues to overlap. Slot reuse across
	// the NumFrames cycle is gated by BeginFrame's WaitForFenceValue(m_FrameFenceValues[frame]).
	pool.drawList->SetGraphicsRootShaderResourceView(1, pool.drawBuffer[frame]->GetGPUVirtualAddress());
	pool.drawList->SetGraphicsRootDescriptorTable(2, m_Renderer2D->GetResourceManager()->Resolve(pool.texture).gpuHandle);

	// {atlasFramesX, atlasFramesY, screenSpace, pad, colorEnd.rgba} packed as raw DWORDs -- MUST
	// match ParticleVS.hlsl's AnimParams field order exactly (screenSpace/_pad0 before colorEnd,
	// not after -- see that cbuffer's comment on HLSL's 16-byte register packing rules).
	struct { uint32_t framesX, framesY, screenSpace, pad0; DirectX::XMFLOAT4 colorEnd; } animConsts = {
		pool.atlasFramesX, pool.atlasFramesY, pool.screenSpace ? 1u : 0u, 0, pool.colorEnd
	};
	pool.drawList->SetGraphicsRoot32BitConstants(3, 8, &animConsts, 0);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
		m_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), backBuffer, m_RTVDescriptorSize);
	pool.drawList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)m_ClientWidth, (float)m_ClientHeight, 0.0f, 1.0f };
	D3D12_RECT scissor = { 0, 0, (LONG)m_ClientWidth, (LONG)m_ClientHeight };
	pool.drawList->RSSetViewports(1, &viewport);
	pool.drawList->RSSetScissorRects(1, &scissor);
	// 6 verts/instance -- ParticleVS.hlsl generates a quad (2 triangles) per particle via
	// SV_VertexID, not a single point (see CreateParticlePipelineState's topology comment).
	pool.drawList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pool.drawList->DrawInstanced(6, pool.maxParticles, 0, 0);

	ThrowIfFailed(pool.drawList->Close());
}

void RendererCore::RequestSpawn(uint32_t effectID, Particle2D p) {
	auto& pool = m_ParticleEffects[effectID];
	if (pool.pendingSpawns.size() < 1024) {
		pool.pendingSpawns.push_back(p);
	}
}

void RendererCore::RequestSpawn(uint32_t effectID, DirectX::XMFLOAT2 basePosition, float offsetRadius,
	DirectX::XMFLOAT2 velocity, float lifetime, DirectX::XMFLOAT4 color, float size) {
	static std::mt19937 rng(std::random_device{}());
	std::uniform_real_distribution<float> jitter(-offsetRadius, offsetRadius);

	Particle2D p{};
	p.position = { basePosition.x + jitter(rng), basePosition.y + jitter(rng) };
	p.velocity = velocity;
	p.lifetime = lifetime;
	p.color = color;
	p.size = size;
	RequestSpawn(effectID, p);
}

void RendererCore::FlushSpawns(ParticleEffectPool& pool) {
	if (pool.pendingSpawns.empty()) return;
	void* mapped = nullptr;
	pool.spawnBuffer->Map(0, nullptr, &mapped);
	memcpy(mapped, pool.pendingSpawns.data(), pool.pendingSpawns.size() * sizeof(Particle2D));
	pool.spawnBuffer->Unmap(0, nullptr);
	pool.pendingSpawns.clear();
}
// Slot allocator over the ImGui SRV heap -- the backend requests a descriptor through the
// Alloc callback whenever it creates a texture (font atlas, future ImGui::Image() textures)
// and returns it through Free when the texture dies. File-scope static because the InitInfo
// callbacks are plain function pointers (no captures allowed), so they can't reach instance
// members; fine since ImGui only ever has one context/backend alive at a time.
static struct ImGuiSrvHeapAlloc {
	D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
	D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};
	UINT increment = 0;
	std::vector<int> freeSlots;
} s_imguiHeapAlloc;

void RendererCore::InitImGui(HWND hwnd)
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	// Do NOT set ImGuiBackendFlags_RendererHasTextures by hand -- the struct-based
	// ImGui_ImplDX12_Init below sets it itself, and setting it here does nothing anyway.
	// The LEGACY 6-arg Init overload must never be used with this ImGui version (1.92.9 WIP):
	// it CLEARS RendererHasTextures internally (imgui_impl_dx12.cpp, io.BackendFlags &= ~...),
	// and this backend has no font-upload path outside the textures protocol anymore -- so the
	// font atlas never reaches the GPU and the first NewFrame/text measurement crashes on null
	// font/texture data. That single line is what every "crashes in the font atlas" failure in
	// this integration traced back to.
	io.Fonts->AddFontDefault();

	ImGui::StyleColorsDark();

	// LINEARISE THE IMGUI STYLE. ImGui authors its colours in sRGB/display space, but the back
	// buffer RTV is ..._UNORM_SRGB so the hardware ENCODES on write -- meaning ImGui's colours get
	// transfer-encoded a SECOND time and every mid-tone washes out (0.5 -> ~0.73; endpoints are
	// unaffected, which is why it reads as "faded" rather than obviously broken).
	//
	// Exactly the same bug as app-specified 2D colours, which Renderer2D::Submit handles with
	// SrgbToLinear. ImGui does not go through Submit -- imgui_impl_dx12 has its own pipeline and
	// knows nothing about our colour convention -- so its style has to be converted here instead.
	// Done ONCE at init rather than per-frame: the style is a fixed table.
	//
	// Alpha is NOT converted; it is not a colour channel and is never transfer-encoded.
	{
		auto srgbToLinear = [](float v) {
			return (v <= 0.04045f) ? (v / 12.92f) : std::pow((v + 0.055f) / 1.055f, 2.4f);
		};
		ImGuiStyle& style = ImGui::GetStyle();
		for (int i = 0; i < ImGuiCol_COUNT; ++i) {
			ImVec4& c = style.Colors[i];
			c.x = srgbToLinear(c.x);
			c.y = srgbToLinear(c.y);
			c.z = srgbToLinear(c.z);
		}
	}

	ImGui_ImplWin32_Init(hwnd);

	// ImGui allocates out of the SHARED SRV heap, NOT a private one.
	//
	// It used to own a separate 64-descriptor heap, and that was a real bug: only ONE shader-visible
	// CBV/SRV/UAV heap can be bound at a time, so as soon as an app passed a texture handle from the
	// shared heap into ImGui::Image/ImageButton -- which Game01's tile palette does -- ImGui rendered
	// with ITS heap bound and bound a foreign handle:
	//   "The descriptor heap containing handle ... is different from currently set descriptor heap"
	//   EXECUTION ERROR #708: SET_DESCRIPTOR_TABLE_INVALID.
	// It only fired once those buttons were actually visible (BeginChild clips them until the window
	// is big enough), which made it look like a resize bug rather than a heap bug.
	//
	// Reserving a contiguous block rather than routing the callbacks straight at AllocateSrvSlot:
	// that is a bump allocator with no free (deliberately -- its clients are engine-lifetime
	// resources), while ImGui's textures protocol genuinely frees and reallocates. Taking a block
	// once and running ImGui's own free list inside it satisfies both without weakening the shared
	// allocator. 16 is ample: ImGui only allocates for textures IT owns (the font atlas), because
	// app textures arrive as handles that already live in this same heap.
	auto* rm = m_Renderer2D ? m_Renderer2D->GetResourceManager() : nullptr;
	if (!rm) {
		OutputDebugStringA("InitImGui: no Renderer2D/ResourceManager yet -- call it after 2D init.\n");
		return;
	}
	constexpr int kImGuiSrvSlots = 16;
	D3D12_CPU_DESCRIPTOR_HANDLE blockCpu{}, unusedCpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE blockGpu{}, unusedGpu{};
	if (!rm->AllocateSrvSlot(blockCpu, blockGpu)) {
		OutputDebugStringA("InitImGui: shared SRV heap full -- ImGui disabled.\n");
		return;
	}
	for (int i = 1; i < kImGuiSrvSlots; ++i) {          // the rest of the contiguous run
		if (!rm->AllocateSrvSlot(unusedCpu, unusedGpu)) {
			OutputDebugStringA("InitImGui: shared SRV heap full mid-reservation -- ImGui disabled.\n");
			return;
		}
	}

	s_imguiHeapAlloc.cpuStart = blockCpu;
	s_imguiHeapAlloc.gpuStart = blockGpu;
	s_imguiHeapAlloc.increment = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	s_imguiHeapAlloc.freeSlots.clear();
	for (int i = kImGuiSrvSlots - 1; i >= 0; --i)       // slots are RELATIVE to the reserved block
		s_imguiHeapAlloc.freeSlots.push_back(i);

	// DX12 Init -- struct-based path, the only one that works on this ImGui version.
	ImGui_ImplDX12_InitInfo info{};
	info.Device = m_Device.Get();
	info.CommandQueue = m_CommandQueue.Get(); // real graphics queue -- texture uploads sync against it
	info.NumFramesInFlight = NumFrames;       // 3, matching the swapchain
	info.RTVFormat = BackBufferRTVFormat;
	info.DSVFormat = DXGI_FORMAT_UNKNOWN;
	info.SrvDescriptorHeap = m_Renderer2D->GetSrvHeap();   // the SHARED heap (see the reservation above)
	info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*,
		D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
		D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
			IM_ASSERT(!s_imguiHeapAlloc.freeSlots.empty() && "ImGui SRV heap exhausted");
			int slot = s_imguiHeapAlloc.freeSlots.back();
			s_imguiHeapAlloc.freeSlots.pop_back();
			outCpu->ptr = s_imguiHeapAlloc.cpuStart.ptr + (SIZE_T)slot * s_imguiHeapAlloc.increment;
			outGpu->ptr = s_imguiHeapAlloc.gpuStart.ptr + (UINT64)slot * s_imguiHeapAlloc.increment;
		};
	info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
		D3D12_CPU_DESCRIPTOR_HANDLE cpu,
		D3D12_GPU_DESCRIPTOR_HANDLE) {
			int slot = (int)((cpu.ptr - s_imguiHeapAlloc.cpuStart.ptr) / s_imguiHeapAlloc.increment);
			s_imguiHeapAlloc.freeSlots.push_back(slot);
		};
	if (!ImGui_ImplDX12_Init(&info)) {
		OutputDebugStringA("ImGui_ImplDX12_Init failed!\n");
		return;
	}

	// PSO etc. -- fonts are NOT built here on this ImGui version; the backend creates and
	// uploads the font texture during ImGui_ImplDX12_RenderDrawData (textures protocol).
	if (!ImGui_ImplDX12_CreateDeviceObjects()) {
		OutputDebugStringA("ImGui_ImplDX12_CreateDeviceObjects FAILED!\n");
	}
	else {
		OutputDebugStringA("ImGui initialized successfully!\n");
	}
}
