#include "../include/Renderer.h"
#include "../include/Helpers.h"      // ThrowIfFailed
#include <T_Thread.h>     // T_Thread::GetCurrent()->qIndex for worker-local storage
#include <cstdio>         // swprintf_s

using namespace Microsoft::WRL;
static const D3D12_INPUT_ELEMENT_DESC inputLayoutDesc[] = {
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

ResourceManager* Renderer2D::GetResourceManager() const { return m_ResourceManager.get(); }

// Call AFTER core.Initialize() -- builds every 2D-specific GPU object against core's
// already-created device/queue. See the class comment in Renderer.h for the Core/2D split.
void Renderer2D::Initialize(RendererCore& core)
{
	m_Core = &core;
	ID3D12Device2* device = core.GetDevice();

	m_RootSignature = CreateRootSignature();
	// effectID 0: the default sprite/text shader. Registered here so Submit()'s default
	// effectID=0 and FlushBatchTask's m_Effects[b.effectID] lookup both resolve correctly.
	m_PipelineState = CreatePipelineState(L"VertexShader.cso", L"PixelShader.cso", DefaultAlphaBlend());
	m_Effects.push_back({ m_PipelineState });
	CreateInstanceBuffer(device, 65536);  // 64K instances max per frame
	// Allocate per-worker submission buffers (heap-allocated to avoid stack overflow)
	m_WorkerLocalStorage = std::make_unique<std::array<WorkerLocalSubmissionData, MAX_WORKERS>>();

	// One context SLOT per (layer, task) pair -- see FlushBatchTask's pool-indexing comment
	// for why zLayer ordering across tasks requires this instead of one context per task.
	// Slots start EMPTY (no allocators/lists yet) -- ProvisionLayerContexts creates a layer's
	// worth lazily, the first time that layer is actually used (called from
	// FlushBatchParallel). This lets NUM_LAYERS stay a generous cap with zero memory cost for
	// layers the game never touches, instead of eagerly paying for all of them at startup.
	const int taskCount = m_FlushTaskContextPool.taskCount;
	m_CommandContextPool.resize((size_t)taskCount * NUM_LAYERS);
	m_LayerProvisioned.assign(NUM_LAYERS, false);

	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = 256; // 256 is an example size
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvHeap)));
	m_ResourceManager = std::make_unique<ResourceManager>(device, m_SrvHeap.Get());

	// (dead code, kept as-is from before the split -- m_IndexBuffer/ibv are created but never
	// bound anywhere; FlushBatchTask uses each Mesh's OWN indexBufferView instead.)
	CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
	CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(indices));
	device->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_IndexBuffer)
	);
	ibv.BufferLocation = m_IndexBuffer->GetGPUVirtualAddress();
	ibv.Format = DXGI_FORMAT_R32_UINT;
	ibv.SizeInBytes = sizeof(indices);

	// Renderer2D's own built-in Font (used by UpdateFPS()'s SubmitText convenience wrapper).
	// MUST come after m_ResourceManager exists and core's command queue/list are ready (true
	// by the time core.Initialize() returns, which is a precondition of calling this function).
	font.Load(ExeRelative(L"font.fnt"), ExeRelative(L"font.png"), *this);
}

ComPtr<ID3D12RootSignature> Renderer2D::CreateRootSignature() {
	CD3DX12_ROOT_PARAMETER rootParameters[3];
	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // 1 SRV, register t0
	rootParameters[0].InitAsConstantBufferView(0); // b0
	rootParameters[1].InitAsShaderResourceView(1, 0); // 1 is the register (t1), 0 is the space
	rootParameters[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.ShaderRegister = 0; // Matches 'register(s0)'
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
	rsDesc.Init(_countof(rootParameters), rootParameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	ComPtr<ID3DBlob> signature, error;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
	if (FAILED(hr)) {
		OutputDebugStringA((char*)error->GetBufferPointer());
		ThrowIfFailed(hr);
	}

	ComPtr<ID3D12RootSignature> rootSig;
	ThrowIfFailed(m_Core->GetDevice()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
	return rootSig;
}

// Standard alpha "over" blend -- what every sprite/text draw used before effects existed.
// D3D12_DEFAULT leaves BlendEnable=FALSE (GPU writes sampled RGBA straight to the render
// target, ignoring alpha), which is why text showed solid black boxes before this was added:
// the font atlas's non-ink texels still got their RGB (0,0,0) written verbatim instead of
// being blended away.
D3D12_BLEND_DESC Renderer2D::DefaultAlphaBlend() {
	D3D12_BLEND_DESC blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	return blend;
}

ComPtr<ID3D12PipelineState> Renderer2D::CreatePipelineState(
	const std::wstring& vsPath, const std::wstring& psPath, D3D12_BLEND_DESC blend) {
	auto vertexShaderBlob = ReadFile(ExeRelative(vsPath));
	auto pixelShaderBlob = ReadFile(ExeRelative(psPath));
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.VS = { vertexShaderBlob.data(), vertexShaderBlob.size() };
	psoDesc.PS = { pixelShaderBlob.data(), pixelShaderBlob.size() };
	psoDesc.SampleMask = 0xFFFFFFFF;

	// Connect the Root Signature and Input Layout. Input layout, root signature, depth/DSV,
	// RTV format, and topology are shared across EVERY effect -- only VS/PS/blend vary.
	psoDesc.pRootSignature = m_RootSignature.Get();
	psoDesc.InputLayout = { inputLayoutDesc, _countof(inputLayoutDesc) };
	psoDesc.BlendState = blend;
	// No depth buffer in this renderer, so depth/stencil must be OFF.
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.RTVFormats[0] = RendererCore::BackBufferRTVFormat;
	psoDesc.NumRenderTargets = 1;
	psoDesc.SampleDesc.Count = 1;
	D3D12_RASTERIZER_DESC rasterizerDesc = {};
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
	rasterizerDesc.FrontCounterClockwise = FALSE;
	rasterizerDesc.DepthBias = 0;
	rasterizerDesc.SlopeScaledDepthBias = 0.0f;
	rasterizerDesc.DepthBiasClamp = 0.0f;
	rasterizerDesc.DepthClipEnable = TRUE;
	rasterizerDesc.MultisampleEnable = FALSE;
	rasterizerDesc.AntialiasedLineEnable = FALSE;
	rasterizerDesc.ForcedSampleCount = 0;
	rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	psoDesc.RasterizerState = rasterizerDesc;

	ComPtr<ID3D12PipelineState> pso;
	ThrowIfFailed(m_Core->GetDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
	return pso;
}

uint32_t Renderer2D::RegisterEffect(const std::wstring& vsPath, const std::wstring& psPath, D3D12_BLEND_DESC blend) {
	ShaderEffect effect;
	effect.pso = CreatePipelineState(vsPath, psPath, blend);
	m_Effects.push_back(effect);
	return (uint32_t)m_Effects.size() - 1;
}

void Renderer2D::CreateInstanceBuffer(ID3D12Device* device, UINT maxInstances) {
	UINT bufferSize = maxInstances * sizeof(ObjectData);

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

	D3D12_RESOURCE_DESC bufferDesc = {};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	// NOT UAV -- this buffer lives on an UPLOAD heap (CPU-mapped, written every frame, read via
	// a root SRV in FlushBatchTask). D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS on an
	// UPLOAD/READBACK heap is illegal (D3D12 ERROR #638).
	bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	bufferDesc.Width = bufferSize;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	// 3+4. Create + persistently map ONE buffer PER FRAME (avoids the in-flight overwrite).
	for (int i = 0; i < RendererCore::NumFrames; ++i) {
		ThrowIfFailed(device->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_InstanceBuffer[i])
		));
		ThrowIfFailed(m_InstanceBuffer[i]->Map(0, nullptr, &m_MappedData[i]));
	}
}

uint64_t Renderer2D::GetMaterialID(Mesh* mesh, TextureResource* tex, uint32_t effectID) {
	// Used as an unordered_map KEY (see Submit/m_MaterialHandles).
	return ((uint64_t)mesh ^ ((uint64_t)tex << 1)) ^ ((uint64_t)effectID << 48);
}

void Renderer2D::Submit(
	const Mesh& mesh,
	TextureResource* tex,
	DirectX::XMFLOAT2 offset,
	float width,
	float height,
	int zLayer,
	DirectX::XMFLOAT4 color,
	float rotation,
	DirectX::XMFLOAT2 uvOffset,
	DirectX::XMFLOAT2 uvScale,
	bool useAlphaFromRGB,
	uint32_t effectID,
	DirectX::XMFLOAT4 effectParams)
{
	DirectX::XMFLOAT2 posNDC = GetNDC(offset.x, offset.y);
	DirectX::XMFLOAT2 screen = GetScreenSize();
	DirectX::XMFLOAT2 sizeNDC =
	{
		(width / (screen.x / 2.0f)),
		(height / (screen.y / 2.0f))
	};

	// Look up (or assign, on first use) this material's bucket HANDLE -- a small sequential
	// index into m_Buckets[zLayer]. Handles are assigned ONCE and PERSIST across frames
	// (meshes/textures/effects live for the program's lifetime) -- only per-frame INSTANCE data
	// gets cleared each frame.
	uint64_t key = GetMaterialID((Mesh*)&mesh, tex, effectID);
	auto& handleMap = m_MaterialHandles[zLayer];
	auto it = handleMap.find(key);
	uint32_t index;
	if (it != handleMap.end()) {
		index = it->second;
	} else {
		index = (uint32_t)m_Buckets[zLayer].size();
		Batch b;
		b.mesh = (Mesh*)&mesh;
		b.tex = tex;
		b.effectID = effectID;
		b.depth = (float)zLayer;
		m_Buckets[zLayer].push_back(b);
		handleMap[key] = index;

		// Grow EVERY worker's buckets[zLayer] to match, right now -- not just the discovering
		// worker's. Without this, a worker that never happens to submit to this NEW handle
		// keeps a SHORTER vector indefinitely.
		if (m_WorkerLocalStorage) {
			for (auto& worker : *m_WorkerLocalStorage) {
				auto& layerBuckets = worker.buckets[zLayer];
				if (index >= layerBuckets.size()) layerBuckets.resize((size_t)index + 1);
			}
		}
	}

	// Push to THIS WORKER'S local bucket, not the shared one. This eliminates concurrent
	// vector modification races.
	auto* thread = T_Threads::T_Thread::GetCurrent();
	float hasTex = (tex != nullptr) ? 1.0f : 0.0f;
	float alphaFromRGB = useAlphaFromRGB ? 1.0f : 0.0f;
	size_t workerIdx = (thread && thread->qIndex < MAX_WORKERS) ? (size_t)thread->qIndex : 0;
	if (m_WorkerLocalStorage) {
		auto& workerLayerBuckets = (*m_WorkerLocalStorage)[workerIdx].buckets[zLayer];
		if (index >= workerLayerBuckets.size()) workerLayerBuckets.resize((size_t)index + 1);
		workerLayerBuckets[index].push_back(
			ObjectData(posNDC, sizeNDC, color, hasTex, rotation, uvOffset, uvScale, alphaFromRGB, effectParams)
		);
	}
}

void Renderer2D::UpdateFPS()
{
	using clock = std::chrono::high_resolution_clock;
	m_frameCounter++;
	auto t1 = clock::now();
	m_elapsedSeconds += std::chrono::duration<double>(t1 - m_t0).count();
	m_t0 = t1;
	if (m_elapsedSeconds > 1.0)
	{
		// Recompute the DISPLAYED value only once/sec (a proper averaging window) -- but
		// SubmitText() below still runs every call, using this cached value, so the text
		// itself is submitted (and therefore drawn) every frame.
		m_LastFPS = m_frameCounter / m_elapsedSeconds;
		m_frameCounter = 0;
		m_elapsedSeconds = 0.0;
	}
	SubmitText(font, 10, 10, "FPS: {:.1f}", 1.0f, { 57.0f / 255.0f, 255.0f / 255.0f, 20.0f / 255.0f, 1.0f }, 0.0f, TextAlign::Left, 2, m_LastFPS);
}

// Lazily creates the taskCount allocator/list groups for ONE layer, the first time that
// layer is actually used (called from FlushBatchParallel, on the main thread, before any
// worker touches m_CommandContextPool this frame -- no synchronization needed). Idempotent:
// a layer already provisioned is a no-op.
void Renderer2D::ProvisionLayerContexts(int layer) {
	if (m_LayerProvisioned[layer]) return;

	ID3D12Device2* device = m_Core->GetDevice();
	const int taskCount = m_FlushTaskContextPool.taskCount;
	for (int t = 0; t < taskCount; ++t) {
		CommandContext& ctx = m_CommandContextPool[(size_t)layer * taskCount + t];
		for (int i = 0; i < RendererCore::NumFrames; ++i) {
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
			HRESULT hr1 = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
			if (FAILED(hr1)) { /* Handle error */ }

			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
			HRESULT hr2 = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), m_PipelineState.Get(), IID_PPV_ARGS(&list));
			if (FAILED(hr2)) { /* Handle error */ }
			list->Close(); // created in the "recording" state; FlushBatchTask Resets it before use

			wchar_t nm[64];
			swprintf_s(nm, L"Worker[layer%d][t%d][f%d]", layer, t, i);
			list->SetName(nm);

			ctx.allocators.push_back(alloc);
			ctx.cmdLists.push_back(list);
		}
	}
	m_LayerProvisioned[layer] = true;
}

// Called by RendererCore::BeginFrame -- resets all worker allocators to prevent stale commands
// accumulating. (No per-frame bucket-metadata reset here: m_Buckets/m_MaterialHandles PERSIST
// across frames -- a material's handle is assigned once and reused forever.)
void Renderer2D::ResetWorkerAllocators(int frame) {
	for (auto& ctx : m_CommandContextPool) {
		if ((int)ctx.allocators.size() > frame) {
			ctx.allocators[frame]->Reset();
		}
	}
}

int Renderer2D::FlushBatchParallel() {
	const int frame = m_Core->GetFrameResourceIndex();

	unsigned hw = std::thread::hardware_concurrency();
	int taskCount = (hw > 1) ? (int)(hw - 1) : 1;
	if (taskCount > (int)m_CommandContextPool.size()) taskCount = (int)m_CommandContextPool.size();
	if (taskCount < 1) taskCount = 1;

	// SERIAL section (main thread only): merge all worker-local submissions from all layers
	// into the GPU instance buffer.
	std::vector<UINT> bucketInstanceCounts[NUM_LAYERS];
	std::vector<UINT> bucketInstanceOffsets[NUM_LAYERS];
	auto* dst = reinterpret_cast<ObjectData*>(m_MappedData[frame]);
	if (!dst) {
		OutputDebugStringA("FATAL: m_MappedData[frame] is null!\n");
		m_ActiveLayersThisFrame.clear();
		return 0;
	}
	UINT running = 0;
	const UINT MAX_INSTANCE_COUNT = 65536;  // Must match CreateInstanceBuffer size

	bool layerHasContent[NUM_LAYERS] = {};
	for (int layer = 0; layer < NUM_LAYERS; ++layer) {
		const size_t bucketCount = m_Buckets[layer].size();
		bucketInstanceCounts[layer].assign(bucketCount, 0);
		bucketInstanceOffsets[layer].assign(bucketCount, 0);
		if (!m_WorkerLocalStorage) continue;

		for (size_t i = 0; i < bucketCount; ++i) {
			bucketInstanceOffsets[layer][i] = running;

			for (int w = 0; w < MAX_WORKERS; ++w) {
				auto& workerLayerBuckets = (*m_WorkerLocalStorage)[w].buckets[layer];
				if (i >= workerLayerBuckets.size()) continue;
				const auto& workerBucket = workerLayerBuckets[i];
				UINT count = (UINT)workerBucket.size();
				if (count > 0) {
					if (running + count > MAX_INSTANCE_COUNT) {
						char dbgBuf[256];
						sprintf_s(dbgBuf, "ERROR: Would overflow instance buffer! running=%u, count=%u, max=%u\n", running, count, MAX_INSTANCE_COUNT);
						OutputDebugStringA(dbgBuf);
						m_ActiveLayersThisFrame.clear();
						return 0;  // Fail fast instead of clamping
					}
					const void* src = workerBucket.data();
					if (!src) {
						OutputDebugStringA("ERROR: workerBucket.data() is null!\n");
						m_ActiveLayersThisFrame.clear();
						return 0;
					}
					size_t copySize = (size_t)count * sizeof(ObjectData);
					memcpy(dst + running, src, copySize);
					running += count;
					bucketInstanceCounts[layer][i] += count;
					layerHasContent[layer] = true;
				}
			}
		}
	}

	m_ActiveLayersThisFrame.clear();
	for (int layer = 0; layer < NUM_LAYERS; ++layer)
		if (layerHasContent[layer]) m_ActiveLayersThisFrame.push_back(layer);

	for (int layer : m_ActiveLayersThisFrame)
		ProvisionLayerContexts(layer);

	auto& sched = T_Threads::TaskScheduler::Instance();
	T_Threads::WaitGroup wg;
	wg.n.store(0, std::memory_order_relaxed);
	std::vector<FlushTaskContext*> ctxs;
	ctxs.reserve((size_t)m_ActiveLayersThisFrame.size() * taskCount);

	m_TasksDispatchedPerLayer.assign(NUM_LAYERS, 0);

	int launched = 0;
	for (int layer : m_ActiveLayersThisFrame) {
		const int layerBucketCount = (int)bucketInstanceCounts[layer].size();
		const int bucketsPerTask = (layerBucketCount + taskCount - 1) / taskCount; // ceil
		int dispatchedThisLayer = 0;
		for (int t = 0; t < taskCount; ++t) {
			int start = t * bucketsPerTask;
			if (start >= layerBucketCount) break;
			int end = start + bucketsPerTask;
			if (end > layerBucketCount) end = layerBucketCount;

			auto* ctx = m_FlushTaskContextPool.Acquire();
			ctx->renderer = this;
			ctx->bucketInstanceCounts = bucketInstanceCounts[layer];
			ctx->bucketInstanceOffsets = bucketInstanceOffsets[layer];
			ctx->layer = layer;
			ctx->taskIndex = t;
			ctx->startBucket = start;
			ctx->endBucket = end;

			ctxs.push_back(ctx);
			auto* task = sched.CreateTask(FlushBatchTask, ctx, true);
			task->waitGroup = &wg;
			wg.n.fetch_add(1, std::memory_order_release);
			sched.Push(task);
			++launched;
			++dispatchedThisLayer;
		}
		m_TasksDispatchedPerLayer[layer] = dispatchedThisLayer;
	}
	sched.WaitFor(wg);  // all worker lists are recorded + Closed - blocks until all tasks complete
	return launched;
}

// Called by RendererCore::PresentFrame -- appends this frame's 2D draw lists, LAYER-MAJOR (all
// of layer L's task lists before any of layer L+1's), to outLists (which already has PRE +
// particle list ahead of it, and POST will be appended after).
void Renderer2D::CollectCommandLists(int frame, std::vector<ID3D12CommandList*>& outLists) {
	const int taskCount = m_FlushTaskContextPool.taskCount;
	for (int layer : m_ActiveLayersThisFrame) {
		const int dispatched = m_TasksDispatchedPerLayer[layer];
		for (int t = 0; t < dispatched; ++t) {
			outLists.push_back(m_CommandContextPool[(size_t)layer * taskCount + t].cmdLists[frame].Get());
		}
	}
}

// Called by RendererCore::PresentFrame after Present(). Only the per-frame INSTANCE data needs
// clearing -- m_Buckets/m_MaterialHandles (which material maps to which handle) PERSIST across
// frames. .clear() empties each per-bucket vector's CONTENTS but keeps its allocated capacity,
// so steady-state frames don't reallocate.
void Renderer2D::ClearWorkerBucketsAndResetPool() {
	if (m_WorkerLocalStorage) {
		for (int w = 0; w < MAX_WORKERS; ++w) {
			for (int layer = 0; layer < NUM_LAYERS; ++layer) {
				auto& workerLayerBuckets = (*m_WorkerLocalStorage)[w].buckets[layer];
				for (auto& bucket : workerLayerBuckets)
					bucket.clear();
			}
		}
	}
	m_FlushTaskContextPool.Reset();
}

