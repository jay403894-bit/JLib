#include "../include/Renderer2D.h"
#include "../include/Helpers.h"      // ThrowIfFailed
#include <Thread.h>     // Thread::GetCurrent()->qIndex for worker-local storage
#include <TaskScheduler.h> // JLib::TaskScheduler::Instance()/CreateTask -- previously pulled in transitively via Renderer2D.h
#include <cstdio>         // swprintf_s
#include <cmath>          // sinf/cosf/atan2f/sqrtf -- 2D primitive mesh generation
#include <vector>         // primitive mesh build buffers
using namespace JLib;
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
	m_PipelineState = CreatePipelineState(L"shaders\\VertexShader.cso", L"shaders\\PixelShader.cso", DefaultAlphaBlend());
	m_Effects.push_back({ m_PipelineState });
	CreateInstanceBuffer(device, kMaxInstances);   // per-frame instance capacity (see Renderer2D.h)
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
	font.Load(ExeRelative(L"fonts\\Aldrich-Regular.fnt"), ExeRelative(L"fonts\\Aldrich-Regular.png"), *this);
}

ComPtr<ID3D12RootSignature> Renderer2D::CreateRootSignature() {
	CD3DX12_ROOT_PARAMETER rootParameters[3];
	CD3DX12_DESCRIPTOR_RANGE srvRange;
	srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // 1 SRV, register t0
	rootParameters[0].InitAsConstantBufferView(0); // b0
	rootParameters[1].InitAsShaderResourceView(1, 0); // 1 is the register (t1), 0 is the space
	rootParameters[2].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	// POINT, not LINEAR -- bilinear filtering blends across texel boundaries, which for a densely-
	// packed pixel-art atlas (no padding between cells) bleeds neighboring tiles' edge pixels into
	// whatever's sampled right at a UV rect boundary -- the thin stray lines seen over enemies/
	// platforms once real sprite sheets were in use. Point sampling never blends between texels
	// regardless of exact UV precision, which is also just the correct look for pixel art in general
	// (LINEAR would blur every sprite, not only cause seams).
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
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

uint64_t Renderer2D::GetMaterialID(Mesh* mesh, TextureHandle tex, uint32_t effectID) {
	// Used as an unordered_map KEY (see Submit/m_MaterialHandles).
	return ((uint64_t)mesh ^ ((uint64_t)tex.id << 1)) ^ ((uint64_t)effectID << 48);
}

// sRGB -> linear for a colour the APP specified.
//
// Textures take care of themselves: the hardware decodes an _SRGB texture on sample and the sRGB
// back-buffer view re-encodes on write, so a sprite round-trips byte-for-byte. A colour written in
// code has no decode step -- it goes into the shader as a literal and gets encoded on the way out,
// which pushes mid-tones UP (0.5 lands near 0.73). Endpoints are unaffected, which is exactly why
// this hid: white text and pure-red sprites looked fine while every tint in between washed out.
//
// Colours in this API are DISPLAY-SPACE by convention, the same as raylib's -- {0.5,0.5,0.5} should
// look like mid grey, not like whatever 0.5 means after a transfer function. So they are converted
// here, at the one point every primitive, sprite and glyph funnels through.
//
// NOTE this does NOT apply to Renderer3D's baseColorFactor: glTF defines those as already linear,
// so converting them would be wrong. Hand-picked tints in 3D scene code are the grey area -- they
// are authored by eye against the rendered result, so they self-correct.
static DirectX::XMFLOAT4 SrgbToLinear(DirectX::XMFLOAT4 c) {
	auto ch = [](float v) {
		return (v <= 0.04045f) ? (v / 12.92f) : std::pow((v + 0.055f) / 1.055f, 2.4f);
	};
	return { ch(c.x), ch(c.y), ch(c.z), c.w };   // alpha is NOT a colour channel -- never transfer-encoded
}

void Renderer2D::Submit(
	const BatchItem& caller)
{
	// COPY, and take the original by const ref. This used to take BatchItem& and mutate it, which
	// made Submit NOT IDEMPOTENT for a reused item: the sRGB conversion below wrote back into the
	// caller's struct, so a loop that fills one BatchItem and submits it per cell converted the
	// colour AGAIN each call. Cell 1 once, cell 2 twice, cell 3 three times -- same hue,
	// progressively darker, which is exactly how it presented (Tetris pieces shaded differently
	// per cell). The flipX/flipY handling below had the identical bug: a reused item flipped its
	// UVs again on every submit.
	//
	// Reusing one BatchItem across a loop is the OBVIOUS way to write a tile/grid renderer, so the
	// API has to be safe under it. Const ref also lets callers pass a temporary, which the old
	// non-const reference rejected.
	BatchItem item = caller;

	item.color = SrgbToLinear(item.color);

	// Mirroring is just reversing which edge of the UV sub-rect the mesh's 0->1 edge lands on --
	// negate the scale and shift the offset to the far edge, so the shader's existing
	// uv = mesh.uv * uvScale + uvOffset (VertexShader.hlsl) needs no changes at all.
	if (item.flipX) { item.uvOffset.x += item.uvScale.x; item.uvScale.x = -item.uvScale.x; }
	if (item.flipY) { item.uvOffset.y += item.uvScale.y; item.uvScale.y = -item.uvScale.y; }

	DirectX::XMFLOAT2 posNDC = GetNDC(item.position.x, item.position.y);
	DirectX::XMFLOAT2 screen = GetScreenSize();
	DirectX::XMFLOAT2 sizeNDC =
	{
		(item.size.x / (screen.x / 2.0f)),
		(item.size.y / (screen.y / 2.0f))
	};

	// Look up (or assign, on first use) this material's bucket HANDLE -- a small sequential
	// index into m_Buckets[zLayer]. Handles are assigned ONCE and PERSIST across frames
	// (meshes/textures/effects live for the program's lifetime) -- only per-frame INSTANCE data
	// gets cleared each frame.
	uint64_t key = GetMaterialID(item.mesh, item.tex, item.effectID);
	auto& handleMap = m_MaterialHandles[item.zLayer];
	auto it = handleMap.find(key);
	uint32_t index;
	if (it != handleMap.end()) {
		index = it->second;
	} else {
		index = (uint32_t)m_Buckets[item.zLayer].size();
		Batch b;
		b.mesh = item.mesh;
		b.tex = item.tex;
		b.effectID = item.effectID;
		b.depth = (float)item.zLayer;
		m_Buckets[item.zLayer].push_back(b);
		handleMap[key] = index;

		// Grow EVERY worker's buckets[zLayer] to match, right now -- not just the discovering
		// worker's. Without this, a worker that never happens to submit to this NEW handle
		// keeps a SHORTER vector indefinitely.
		if (m_WorkerLocalStorage) {
			for (auto& worker : *m_WorkerLocalStorage) {
				auto& layerBuckets = worker.buckets[item.zLayer];
				if (index >= layerBuckets.size()) layerBuckets.resize((size_t)index + 1);
			}
		}
	}

	// Push to THIS WORKER'S local bucket, not the shared one. This eliminates concurrent
	// vector modification races.
	auto* thread = JLib::Thread::GetCurrent();
	float hasTex = item.tex.IsValid() ? 1.0f : 0.0f;
	float alphaFromRGB = item.useAlphaFromRGB ? 1.0f : 0.0f;
	size_t workerIdx = (thread && thread->qIndex < MAX_WORKERS) ? (size_t)thread->qIndex : 0;
	if (m_WorkerLocalStorage) {
		auto& workerLayerBuckets = (*m_WorkerLocalStorage)[workerIdx].buckets[item.zLayer];
		if (index >= workerLayerBuckets.size()) workerLayerBuckets.resize((size_t)index + 1);
		workerLayerBuckets[index].push_back(
			// Shear converts like size.x (both scale the local X axis), so it goes through the
			// same screen->NDC divisor. 0 for everything except DrawTriangle.
			ObjectData(posNDC, sizeNDC, item.color, hasTex, item.rotation, item.uvOffset, item.uvScale,
			           alphaFromRGB, item.effectParams, item.shear / (screen.x / 2.0f))
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
	if (!m_FpsVisible) return;   // timing still runs -- GetFPS() stays valid for an app-drawn readout

	// Negative coordinates anchor from the opposite edge, resolved against the CURRENT screen size
	// so a bottom-right overlay stays bottom-right after a resize.
	//
	// Right-anchoring uses TextAlign::Right rather than subtracting an assumed text width: the
	// reading changes width as the number does ("FPS: 60.0" vs "FPS: 1234.5"), so any fixed guess
	// clips the last digit off the edge at some framerate. With Right, the offset means exactly
	// "this many pixels of margin", whatever the string turns out to be.
	DirectX::XMFLOAT2 screen = GetScreenSize();
	const bool fromRight = m_FpsPos.x < 0.0f;
	float x = fromRight ? screen.x + m_FpsPos.x : m_FpsPos.x;
	float y = m_FpsPos.y >= 0.0f ? m_FpsPos.y : screen.y + m_FpsPos.y - 20.0f * m_FpsScale;

	SubmitText(font, x, y, "FPS: {:.1f}", m_FpsScale, m_FpsColor, 0.0f,
	           fromRight ? TextAlign::Right : TextAlign::Left, m_FpsLayer, m_LastFPS);
}

void Renderer2D::SetFpsOverlay(DirectX::XMFLOAT2 position, DirectX::XMFLOAT4 color,
                               float scale, int zLayer, bool visible)
{
	m_FpsPos = position;
	m_FpsColor = color;
	m_FpsScale = scale;
	m_FpsLayer = zLayer;
	m_FpsVisible = visible;
}

void Renderer2D::SetFpsVisible(bool visible) { m_FpsVisible = visible; }

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

	// (taskCount is the hw/pool CEILING here; the actual dispatched count is scaled DOWN to the real
	//  workload after the sprite merge below, once `running` is known -- see the kSpritesPerTask block.)

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
	const UINT MAX_INSTANCE_COUNT = kMaxInstances;
	UINT droppedInstances = 0;   // reported once at the end, not per bucket

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
					// OVERFLOW: draw what fits and drop the remainder, rather than discarding the
					// WHOLE frame. Bailing out produced a black screen with no on-screen explanation,
					// which reads as a crash; a frame that's missing some sprites is both far more
					// usable and immediately diagnosable by eye.
					if (running + count > MAX_INSTANCE_COUNT) {
						const UINT room = (running < MAX_INSTANCE_COUNT) ? (MAX_INSTANCE_COUNT - running) : 0;
						droppedInstances += count - room;
						count = room;
						if (count == 0) continue;
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

	// Report an overflow ONCE, not every frame -- a per-frame message at 120fps buries the debug
	// output and makes the real cause harder to find, which was half the original problem.
	if (droppedInstances > 0 && !m_WarnedInstanceOverflow) {
		m_WarnedInstanceOverflow = true;
		char b[288];
		sprintf_s(b, "[Renderer2D] INSTANCE BUFFER FULL: dropped %u sprites this frame (cap %u).\n"
		             "  Some geometry will be MISSING. Raise Renderer2D::kMaxInstances and rebuild,\n"
		             "  or submit fewer items per frame. This warning prints once per run.\n",
		          droppedInstances, MAX_INSTANCE_COUNT);
		OutputDebugStringA(b);
	}

	m_ActiveLayersThisFrame.clear();
	for (int layer = 0; layer < NUM_LAYERS; ++layer)
		if (layerHasContent[layer]) m_ActiveLayersThisFrame.push_back(layer);

	// Scale the DISPATCHED task count to the actual work now that the total sprite count (`running`) is
	// known. A worker task costs a command-list Reset/Close/Execute + dispatch + WaitGroup sync, so
	// fanning out to hw-1 tasks for a few hundred sprites is pure overhead -- Game01's ~520-sprite scene
	// measured 2.0ms with ~20 tasks vs 0.25ms with ~3 (one per layer). Use ~one task per kSpritesPerTask
	// sprites, never EXCEEDING the ceiling set above (only reduces, so pool indexing stays within stride).
	// Heavy 2D scenes still parallelize; light ones stay near-serial. kSpritesPerTask is a tuning knob.
	{
		constexpr UINT kSpritesPerTask = 4000;
		int workTasks = 1 + (int)(running / kSpritesPerTask);
		if (workTasks < taskCount) taskCount = workTasks;
	}

	for (int layer : m_ActiveLayersThisFrame)
		ProvisionLayerContexts(layer);

	auto& sched = JLib::TaskScheduler::Instance();
	JLib::WaitGroup wg;
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

	// NOTE (measured, question closed): the whole 2D record path was timed at Game01's real scene --
	// ~0.3ms avg for 523 sprites / 3 active layers / 3 tasks, with ~0.2ms frame-to-frame VARIANCE. The
	// record cost is sub-millisecond and the noise floor is larger than any serial-vs-parallel delta, so a
	// true-serial path is NOT worth building (same conclusion as the instanced 3D pass). The kSpritesPerTask
	// scaling above is the real crossover: light scenes drop to ~1 task/layer, heavy scenes fan out. Don't
	// re-litigate this without a scene where the [2D record] time is a genuine multi-ms chunk.
	return launched;
}

// Called by RendererCore::PresentFrame once per layer, in ascending zLayer order, interleaved
// with particle-pool draws registered at that same layer. m_TasksDispatchedPerLayer is sized
// NUM_LAYERS and re-assigned to all-0 every frame in FlushBatchParallel, so indexing it directly
// for an inactive layer is safe -- the inner loop just doesn't run.
void Renderer2D::CollectCommandListsForLayer(int layer, int frame, std::vector<ID3D12CommandList*>& outLists) {
	const int taskCount = m_FlushTaskContextPool.taskCount;
	const int dispatched = m_TasksDispatchedPerLayer[layer];
	for (int t = 0; t < dispatched; ++t) {
		outLists.push_back(m_CommandContextPool[(size_t)layer * taskCount + t].cmdLists[frame].Get());
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


// ============================== 2D primitives ==============================
// Every primitive is a GENERATED MESH pushed through the ordinary BatchItem/Submit path, so shapes
// batch alongside sprites and text and need no new shader, no new PSO, and no extra .cso beside the
// exe. See Renderer2D.h for the conventions (rectangles take a top-left corner, circles/polys take
// a center, angles in degrees -- matching raylib so ported tutorial code reads the same).

void Renderer2D::EnsurePrimitives()
{
	if (m_Prim.ready) return;

	// Unit quad centered on the origin. BatchItem positions by CENTER and scales by `size`, so this
	// one mesh serves every rectangle AND every line (a line is a rotated, stretched quad).
	const Vertex quadVerts[] = {
		{ -0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 0.0f },
		{  0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 0.0f },
		{ -0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f },
		{  0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f },
	};
	const uint32_t quadIdx[] = { 0, 1, 2, 1, 3, 2 };
	m_Prim.quad = m_ResourceManager->CreateMesh(quadVerts, 4, quadIdx, 6);

	// Reference triangle at the origin and the two unit axes. DrawTriangle solves for the affine
	// map taking (0,0)->a, (1,0)->b, (0,1)->c, so these exact vertices ARE the basis it solves in.
	const Vertex triVerts[] = {
		{ 0.0f, 0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 0.0f },
		{ 1.0f, 0.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f },
	};
	const uint32_t triIdx[] = { 0, 1, 2 };
	m_Prim.triangle = m_ResourceManager->CreateMesh(triVerts, 3, triIdx, 3);

	// 1x1 opaque white -- primitives take their color from BatchItem::color, so the texture must
	// contribute nothing. Same upload path main.cpp uses for its solid tile texture.
	ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		m_Prim.white = m_ResourceManager->CreateSolidColorTexture(cmd, 255, 255, 255, 255);
		});

	// SDF circle effect: circles are drawn as ONE quad whose pixel shader computes coverage from
	// the distance to the center, so they stay mathematically perfect at any zoom instead of
	// showing the flat edges of an N-gon. Registration is guarded because it loads a .cso from
	// beside the exe -- a project whose shaders folder predates CirclePS.cso keeps working and
	// silently falls back to the cached-mesh discs (see DrawCircle).
	try {
		m_Prim.circleEffect = RegisterEffect(L"shaders\\VertexShader.cso",
			L"shaders\\CirclePS.cso", DefaultAlphaBlend());
	}
	catch (...) {
		m_Prim.circleEffect = 0;   // 0 = the default sprite effect => "unavailable", use meshes
		OutputDebugStringA("[Renderer2D] CirclePS.cso not found -- circles fall back to N-gon "
			"meshes (visible faceting when zoomed). Redeploy the renderer's shaders.\n");
	}

	// SDF rounded rectangle: the UI shape. Same guard -- missing .cso degrades to sharp corners
	// rather than failing, so an app with an older shaders folder still runs.
	try {
		m_Prim.roundRectEffect = RegisterEffect(L"shaders\\VertexShader.cso",
			L"shaders\\RoundRectPS.cso", DefaultAlphaBlend());
	}
	catch (...) {
		m_Prim.roundRectEffect = 0;
		OutputDebugStringA("[Renderer2D] RoundRectPS.cso not found -- rounded rectangles fall back "
			"to square corners. Redeploy the renderer's shaders.\n");
	}

	m_Prim.ready = true;
}

Mesh& Renderer2D::GetDiscMesh(int sides)
{
	EnsurePrimitives();
	if (sides < 3) sides = 3;
	auto it = m_Prim.discs.find(sides);
	if (it != m_Prim.discs.end()) return it->second;

	// Triangle fan around a center vertex at radius 0.5, i.e. a UNIT disc: callers scale by
	// (radius * 2) and get the right size, exactly like the unit quad.
	std::vector<Vertex> verts;
	verts.reserve((size_t)sides + 1);
	verts.push_back({ 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.5f });
	for (int i = 0; i < sides; ++i) {
		const float t = (float)i / (float)sides * 6.28318530718f;
		const float cx = 0.5f * cosf(t), cy = 0.5f * sinf(t);
		verts.push_back({ cx, cy, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, cx + 0.5f, 0.5f - cy });
	}
	std::vector<uint32_t> idx;
	idx.reserve((size_t)sides * 3);
	for (int i = 0; i < sides; ++i) {
		idx.push_back(0u);
		idx.push_back((uint32_t)(1 + i));
		idx.push_back((uint32_t)(1 + (i + 1) % sides));
	}
	auto res = m_Prim.discs.emplace(sides,
		m_ResourceManager->CreateMesh(verts.data(), (uint32_t)verts.size(),
			idx.data(), (uint32_t)idx.size()));
	return res.first->second;
}

Mesh& Renderer2D::GetRingMesh(int sides)
{
	EnsurePrimitives();
	if (sides < 3) sides = 3;
	auto it = m_Prim.rings.find(sides);
	if (it != m_Prim.rings.end()) return it->second;

	// A band between the unit radius (0.5) and 92% of it. NOTE: because one transform cannot scale
	// radius and band width independently, this mesh's outline THICKENS with the shape. It exists
	// for callers who want that proportional look; DrawCircleLines/DrawPolyLines instead emit N
	// line quads so their thickness stays in pixels regardless of radius.
	const float kInner = 0.5f * 0.92f;
	std::vector<Vertex> verts;
	verts.reserve((size_t)sides * 2);
	for (int i = 0; i < sides; ++i) {
		const float t = (float)i / (float)sides * 6.28318530718f;
		const float c = cosf(t), s = sinf(t);
		verts.push_back({ 0.5f * c,   0.5f * s,   0.0f, 1.0f,1.0f,1.0f,1.0f, 0.5f + 0.5f * c,   0.5f - 0.5f * s });
		verts.push_back({ kInner * c, kInner * s, 0.0f, 1.0f,1.0f,1.0f,1.0f, 0.5f + kInner * c, 0.5f - kInner * s });
	}
	std::vector<uint32_t> idx;
	idx.reserve((size_t)sides * 6);
	for (int i = 0; i < sides; ++i) {
		const uint32_t o0 = (uint32_t)(i * 2), i0 = o0 + 1;
		const uint32_t o1 = (uint32_t)(((i + 1) % sides) * 2), i1 = o1 + 1;
		idx.push_back(o0); idx.push_back(o1); idx.push_back(i0);
		idx.push_back(i0); idx.push_back(o1); idx.push_back(i1);
	}
	auto res = m_Prim.rings.emplace(sides,
		m_ResourceManager->CreateMesh(verts.data(), (uint32_t)verts.size(),
			idx.data(), (uint32_t)idx.size()));
	return res.first->second;
}

// Shared submit path for every primitive: unit mesh + center + size + rotation, drawn with the
// white texture so BatchItem::color is the only thing tinting it. effectID/effectParams carry the
// SDF circle parameters when the circle effect is in use.
void Renderer2D::SubmitPrimitive(Mesh& mesh, DirectX::XMFLOAT2 center, DirectX::XMFLOAT2 size,
	DirectX::XMFLOAT4 color, float rotationDeg, int zLayer,
	uint32_t effectID, DirectX::XMFLOAT4 effectParams, float shear)
{
	BatchItem item;
	item.mesh = &mesh;
	item.tex = m_Prim.white;
	item.position = center;
	item.size = size;
	item.zLayer = zLayer;
	item.color = color;
	item.rotation = rotationDeg;
	item.effectID = effectID;
	item.effectParams = effectParams;
	item.shear = shear;
	Submit(item);
}

void Renderer2D::DrawLine(DirectX::XMFLOAT2 a, DirectX::XMFLOAT2 b,
	DirectX::XMFLOAT4 color, float thickness, int zLayer)
{
	EnsurePrimitives();
	const float dx = b.x - a.x, dy = b.y - a.y;
	const float len = sqrtf(dx * dx + dy * dy);
	if (len <= 0.0001f) return;                    // degenerate segment -- nothing to draw
	// The unit quad stretched to the segment length, rotated to its angle, centered on its
	// midpoint. The angle is negated because screen Y grows downward while atan2 assumes Y up.
	const DirectX::XMFLOAT2 mid{ (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
	const float angleDeg = -atan2f(dy, dx) * (180.0f / 3.14159265359f);
	SubmitPrimitive(m_Prim.quad, mid, { len, thickness }, color, angleDeg, zLayer);
}

void Renderer2D::DrawRectangle(DirectX::XMFLOAT2 topLeft, DirectX::XMFLOAT2 size,
	DirectX::XMFLOAT4 color, float rotationDeg, int zLayer)
{
	EnsurePrimitives();
	const DirectX::XMFLOAT2 center{ topLeft.x + size.x * 0.5f, topLeft.y + size.y * 0.5f };
	SubmitPrimitive(m_Prim.quad, center, size, color, rotationDeg, zLayer);
}

void Renderer2D::DrawRectangleLines(DirectX::XMFLOAT2 topLeft, DirectX::XMFLOAT2 size,
	DirectX::XMFLOAT4 color, float thickness, int zLayer)
{
	// Four line quads. The horizontal edges are extended by half the thickness at each end so the
	// corners close cleanly instead of leaving notches.
	const float x0 = topLeft.x, y0 = topLeft.y;
	const float x1 = topLeft.x + size.x, y1 = topLeft.y + size.y;
	const float h = thickness * 0.5f;
	DrawLine({ x0 - h, y0 }, { x1 + h, y0 }, color, thickness, zLayer);   // top
	DrawLine({ x0 - h, y1 }, { x1 + h, y1 }, color, thickness, zLayer);   // bottom
	DrawLine({ x0, y0 }, { x0, y1 }, color, thickness, zLayer);           // left
	DrawLine({ x1, y0 }, { x1, y1 }, color, thickness, zLayer);           // right
}

// Segment count scales with radius so small circles stay cheap and large ones stay smooth, then
// rounds to a multiple of 4 so counts CLUSTER -- in practice a whole game reuses a handful of
// cached meshes instead of generating one per distinct radius.
static int PrimCircleSides(float radius)
{
	int sides = (int)(radius * 0.8f);
	if (sides < 16) sides = 16;
	if (sides > 96) sides = 96;
	return (sides + 3) & ~3;
}

void Renderer2D::DrawCircle(DirectX::XMFLOAT2 center, float radius,
	DirectX::XMFLOAT4 color, int zLayer)
{
	EnsurePrimitives();
	const DirectX::XMFLOAT2 size{ radius * 2.0f, radius * 2.0f };
	if (m_Prim.circleEffect != 0) {
		// One quad, perfect circle at any zoom. effectParams.x = 0 => filled disc.
		SubmitPrimitive(m_Prim.quad, center, size, color, 0.0f, zLayer,
			m_Prim.circleEffect, { 0.0f, 0.0f, 0.0f, 0.0f });
		return;
	}
	SubmitPrimitive(GetDiscMesh(PrimCircleSides(radius)), center, size, color, 0.0f, zLayer);
}

void Renderer2D::DrawCircleLines(DirectX::XMFLOAT2 center, float radius,
	DirectX::XMFLOAT4 color, float thickness, int zLayer)
{
	EnsurePrimitives();
	if (m_Prim.circleEffect != 0 && radius > 0.0f) {
		// Ring via the same shader: effectParams.x is the inner radius in UV units, so a pixel
		// thickness converts to (radius - thickness) / (radius * 2). Clamped so a thickness larger
		// than the radius degenerates to a filled disc instead of inverting.
		float inner = (radius - thickness) / (radius * 2.0f);
		if (inner < 0.0f) inner = 0.0f;
		if (inner > 0.4999f) inner = 0.4999f;
		SubmitPrimitive(m_Prim.quad, center, { radius * 2.0f, radius * 2.0f }, color, 0.0f, zLayer,
			m_Prim.circleEffect, { inner, 0.0f, 0.0f, 0.0f });
		return;
	}
	DrawPolyLines(center, PrimCircleSides(radius), radius, 0.0f, color, thickness, zLayer);
}

void Renderer2D::DrawPoly(DirectX::XMFLOAT2 center, int sides, float radius,
	float rotationDeg, DirectX::XMFLOAT4 color, int zLayer)
{
	if (sides < 3) sides = 3;
	Mesh& disc = GetDiscMesh(sides);
	SubmitPrimitive(disc, center, { radius * 2.0f, radius * 2.0f }, color, rotationDeg, zLayer);
}

void Renderer2D::DrawPolyLines(DirectX::XMFLOAT2 center, int sides, float radius,
	float rotationDeg, DirectX::XMFLOAT4 color,
	float thickness, int zLayer)
{
	if (sides < 3) sides = 3;
	// N line quads rather than the ring mesh, so `thickness` stays in PIXELS and independent of
	// radius -- which is what callers expect. (A scaled ring mesh would thicken with the shape.)
	const float step = 6.28318530718f / (float)sides;
	const float base = rotationDeg * (3.14159265359f / 180.0f);
	DirectX::XMFLOAT2 prev{ center.x + radius * cosf(base), center.y - radius * sinf(base) };
	for (int i = 1; i <= sides; ++i) {
		const float t = base + step * (float)i;
		const DirectX::XMFLOAT2 cur{ center.x + radius * cosf(t), center.y - radius * sinf(t) };
		DrawLine(prev, cur, color, thickness, zLayer);
		prev = cur;
	}
}

void Renderer2D::DrawTriangleLines(DirectX::XMFLOAT2 a, DirectX::XMFLOAT2 b, DirectX::XMFLOAT2 c,
	DirectX::XMFLOAT4 color, float thickness, int zLayer)
{
	DrawLine(a, b, color, thickness, zLayer);
	DrawLine(b, c, color, thickness, zLayer);
	DrawLine(c, a, color, thickness, zLayer);
}

void Renderer2D::DrawRectangleRounded(DirectX::XMFLOAT2 topLeft, DirectX::XMFLOAT2 size,
	float cornerRadius, DirectX::XMFLOAT4 color, float rotationDeg, int zLayer)
{
	EnsurePrimitives();
	const DirectX::XMFLOAT2 center{ topLeft.x + size.x * 0.5f, topLeft.y + size.y * 0.5f };
	if (m_Prim.roundRectEffect == 0 || cornerRadius <= 0.0f) {
		// No shader (or nothing to round) -- a plain quad is exactly right, not an approximation.
		SubmitPrimitive(m_Prim.quad, center, size, color, rotationDeg, zLayer);
		return;
	}
	// Half-extents go to the shader in PIXELS so corners stay circular on non-square rects; the
	// shader clamps the radius itself, but clamping here too keeps the value meaningful if a
	// caller reads it back. w = 0 -> filled.
	const DirectX::XMFLOAT2 half{ size.x * 0.5f, size.y * 0.5f };
	const float r = (cornerRadius > (half.x < half.y ? half.x : half.y))
		? (half.x < half.y ? half.x : half.y) : cornerRadius;
	SubmitPrimitive(m_Prim.quad, center, size, color, rotationDeg, zLayer,
		m_Prim.roundRectEffect, { half.x, half.y, r, 0.0f });
}

void Renderer2D::DrawRectangleRoundedLines(DirectX::XMFLOAT2 topLeft, DirectX::XMFLOAT2 size,
	float cornerRadius, DirectX::XMFLOAT4 color,
	float thickness, float rotationDeg, int zLayer)
{
	EnsurePrimitives();
	if (m_Prim.roundRectEffect == 0 || cornerRadius <= 0.0f) {
		// Sharp-cornered outline via the four-line path (which also handles rotation == 0 well).
		// A rotated fallback outline isn't supported -- callers wanting rotation need the shader.
		DrawRectangleLines(topLeft, size, color, thickness, zLayer);
		return;
	}
	const DirectX::XMFLOAT2 center{ topLeft.x + size.x * 0.5f, topLeft.y + size.y * 0.5f };
	const DirectX::XMFLOAT2 half{ size.x * 0.5f, size.y * 0.5f };
	const float maxR = half.x < half.y ? half.x : half.y;
	const float r = cornerRadius > maxR ? maxR : cornerRadius;
	// w = border thickness in pixels; the shader subtracts an inset copy of the shape to leave a
	// band of exactly this width, corners included -- which four line quads cannot do.
	SubmitPrimitive(m_Prim.quad, center, size, color, rotationDeg, zLayer,
		m_Prim.roundRectEffect, { half.x, half.y, r, thickness });
}

void Renderer2D::DrawTriangle(DirectX::XMFLOAT2 a, DirectX::XMFLOAT2 b, DirectX::XMFLOAT2 c,
	DirectX::XMFLOAT4 color, int zLayer)
{
	EnsurePrimitives();

	// Solve for the affine transform that carries the reference triangle -- (0,0),(1,0),(0,1) --
	// onto (a,b,c), then express it in the (translate, rotate, scale, shear) form the instance
	// data can carry. Translation is trivially `a`; the linear part must map the unit X axis onto
	// (b-a) and the unit Y axis onto (c-a).
	//
	// The wrinkle is that the vertex shader doesn't apply its rotation in raw NDC -- it stretches
	// X by the aspect ratio, rotates, then un-stretches (so rotations stay circular on a
	// non-square window). The decomposition therefore has to be done in that same
	// aspect-corrected space, or the recovered angle/scales are wrong on any window that isn't 1:1.
	const DirectX::XMFLOAT2 screen = GetScreenSize();
	if (screen.x <= 0.0f || screen.y <= 0.0f) return;
	const float k = screen.x / screen.y;             // same aspectRatio the shader receives

	// Screen-space edge vectors -> NDC. Y is negated because screen Y grows downward while NDC
	// Y grows upward (the same flip GetNDC applies to positions).
	const float sx2 = 2.0f / screen.x, sy2 = 2.0f / screen.y;
	const DirectX::XMFLOAT2 e1{ (b.x - a.x) * sx2, -(b.y - a.y) * sy2 };
	const DirectX::XMFLOAT2 e2{ (c.x - a.x) * sx2, -(c.y - a.y) * sy2 };

	// Into aspect-corrected space, where the shader's rotation is a true rotation.
	const DirectX::XMFLOAT2 u{ e1.x * k, e1.y };
	const DirectX::XMFLOAT2 v{ e2.x * k, e2.y };

	// Rotation is whatever angle carries the unit X axis onto the first edge.
	const float theta = atan2f(u.y, u.x);
	const float lenU = sqrtf(u.x * u.x + u.y * u.y);
	if (lenU <= 1e-6f) return;                        // degenerate: a and b coincide

	// Undo that rotation on the second edge; what remains is exactly the upper-triangular
	// [[sx, shear], [0, sy]] part -- x picks up the shear, y the vertical scale.
	const float ct = cosf(-theta), st = sinf(-theta);
	const DirectX::XMFLOAT2 vr{ v.x * ct - v.y * st, v.x * st + v.y * ct };

	// Back out of aspect-corrected space and into the pixel units Submit expects (it divides by
	// screen/2 again on the way to NDC, so this round-trips exactly).
	const float halfW = screen.x * 0.5f, halfH = screen.y * 0.5f;
	const DirectX::XMFLOAT2 sizePx{ (lenU / k) * halfW, vr.y * halfH };
	const float shearPx = (vr.x / k) * halfW;

	SubmitPrimitive(m_Prim.triangle, a, sizePx, color,
		theta * (180.0f / 3.14159265359f), zLayer, 0, { 0.0f, 0.0f, 0.0f, 0.0f }, shearPx);
}

// ---- Curves and polylines: flattened into DrawLine segments, so they batch like everything
// else and their thickness stays in pixels. ----

void Renderer2D::DrawSplineLinear(const DirectX::XMFLOAT2* points, int pointCount,
	DirectX::XMFLOAT4 color, float thickness, int zLayer)
{
	if (!points || pointCount < 2) return;
	for (int i = 1; i < pointCount; ++i)
		DrawLine(points[i - 1], points[i], color, thickness, zLayer);
}

void Renderer2D::DrawLineBezierCubic(DirectX::XMFLOAT2 start, DirectX::XMFLOAT2 c1,
	DirectX::XMFLOAT2 c2, DirectX::XMFLOAT2 end,
	DirectX::XMFLOAT4 color, float thickness, int segments, int zLayer)
{
	if (segments < 1) segments = 1;
	DirectX::XMFLOAT2 prev = start;
	for (int i = 1; i <= segments; ++i) {
		const float t = (float)i / (float)segments;
		const float it = 1.0f - t;
		// Bernstein basis: (1-t)^3, 3(1-t)^2 t, 3(1-t) t^2, t^3
		const float b0 = it * it * it;
		const float b1 = 3.0f * it * it * t;
		const float b2 = 3.0f * it * t * t;
		const float b3 = t * t * t;
		const DirectX::XMFLOAT2 cur{
			b0 * start.x + b1 * c1.x + b2 * c2.x + b3 * end.x,
			b0 * start.y + b1 * c1.y + b2 * c2.y + b3 * end.y
		};
		DrawLine(prev, cur, color, thickness, zLayer);
		prev = cur;
	}
}

void Renderer2D::DrawLineBezierQuad(DirectX::XMFLOAT2 start, DirectX::XMFLOAT2 control,
	DirectX::XMFLOAT2 end, DirectX::XMFLOAT4 color,
	float thickness, int segments, int zLayer)
{
	if (segments < 1) segments = 1;
	DirectX::XMFLOAT2 prev = start;
	for (int i = 1; i <= segments; ++i) {
		const float t = (float)i / (float)segments;
		const float it = 1.0f - t;
		const float b0 = it * it, b1 = 2.0f * it * t, b2 = t * t;
		const DirectX::XMFLOAT2 cur{
			b0 * start.x + b1 * control.x + b2 * end.x,
			b0 * start.y + b1 * control.y + b2 * end.y
		};
		DrawLine(prev, cur, color, thickness, zLayer);
		prev = cur;
	}
}

void Renderer2D::DrawLineBezier(DirectX::XMFLOAT2 start, DirectX::XMFLOAT2 end,
	DirectX::XMFLOAT4 color, float thickness, int segments, int zLayer)
{
	// raylib's DrawLineBezier is a cubic whose controls are the OPPOSITE corners of the
	// start/end bounding box -- (end.x, start.y) and (start.x, end.y). That's what produces its
	// characteristic ease-in-out S shape rather than a straight line, and matching it here means
	// ported raylib code draws the same curve.
	DrawLineBezierCubic(start, { end.x, start.y }, { start.x, end.y }, end,
		color, thickness, segments, zLayer);
}

