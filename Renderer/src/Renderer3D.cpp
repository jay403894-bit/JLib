// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#include "../include/Renderer3D.h"
#include "../include/Helpers.h"   // ThrowIfFailed
#include "../include/Vertex.h"    // Vertex3D
#include "../include/Particle3D.h" // Particle3D (GPU particle buffer stride)
#include "../include/Renderer2D.h" // GetSrvHeap() + GetResourceManager() (shared with the 2D pass)
#include <cstring>                 // memcpy (bone palette upload)
#include <cstdint>
#include <Thread.h>                // scheduler (worker command-list recording)
#include <TaskScheduler.h>         // JLib::TaskScheduler::Instance()/CreateTask/WaitFor
#include <thread>                  // (no longer used: worker count now comes from the scheduler)
#include <algorithm>               // std::min/std::max
#include <cstdio>                  // sprintf_s (device-hung binding diagnostics)
#include <DirectXTex.h>            // LoadFromHDRFile -- the IBL environment bake decodes the HDRI here
#include <cmath>                   // std::sqrt (frustum-plane normalize + world sphere radius)

using Microsoft::WRL::ComPtr;

namespace JLib {

// Tells the GPU's Input Assembler how to unpack ONE Vertex3D from the vertex buffer into the
// values the vertex shader's input struct expects. The order/offsets/formats MUST match
// Vertex.h's `struct Vertex3D { XMFLOAT3 pos; XMFLOAT3 normal; XMFLOAT2 uv; }` exactly:
//   POSITION  float3 at byte 0   (12 bytes)
//   NORMAL    float3 at byte 12  (12 bytes)  -- direction the surface faces, for lighting later
//   TEXCOORD  float2 at byte 24  (8  bytes)
//   TANGENT   float4 at byte 32  (16 bytes)  -- surface tangent (xyz) + handedness (w), for normal mapping
// The string names ("POSITION" etc.) are how the shader's input semantics bind to these slots.
static const D3D12_INPUT_ELEMENT_DESC kVertex3DLayout[] = {
    // Slot 0: the mesh's per-vertex data (one Vertex3D each).
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
    { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
    { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
    // Slot 1: the per-INSTANCE model matrix, 4 rows. StepRate 1 => the IA advances one matrix per
    // instance (fetched at StartInstanceLocation + SV_InstanceID). Replaces the old b1 root-constant
    // ModelMat -- one DrawIndexedInstanced now draws every object that shares this mesh+material.
    { "INSTMAT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    { "INSTMAT",  1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    { "INSTMAT",  2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    { "INSTMAT",  3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
};

// SkinnedVertex3D adds the two bone streams after uv (see Vertex.h for the byte offsets). BLENDINDICES
// is UINT (bone indices), BLENDWEIGHT is FLOAT -- these bind to the skinning VS's uint4/float4 inputs.
static const D3D12_INPUT_ELEMENT_DESC kSkinnedVertex3DLayout[] = {
    { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,  0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

// What the STATIC pipeline's shaders may read (see the register-map comment in Basic3D_PS.hlsl):
//   b0 = camera (ViewProj + camPos)   -- VS reads ViewProj, PS reads camPos -> visibility ALL
//   b1 = Model (16 floats)            -- VS; UNUSED now (instancing feeds the model via the vertex stream),
//                                        kept so param indices don't shuffle
//   t0 = albedo                       -- PS (per batch)
//   b3 = lights                       -- PS (per frame)
//   b4 = material (metallic,roughness)-- PS (per batch, root constants)
// ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT is required because we feed vertices through a vertex buffer + layout.
ComPtr<ID3D12RootSignature> Renderer3D::CreateRootSignature() {
    // t0..t3 = the four material maps, each read only by the pixel shader (separate 1-SRV tables since
    // the textures aren't allocated contiguously in the heap; a contiguous-per-material table is a later opt).
    CD3DX12_DESCRIPTOR_RANGE rAlbedo, rMetalRough, rEmissive, rOcclusion, rNormal, rShadow, rShadowCube, rSsao;
    rAlbedo.Init    (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);  // t0
    rMetalRough.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);  // t1
    rEmissive.Init  (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);  // t2
    rOcclusion.Init (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);  // t3
    rNormal.Init    (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);  // t4
    rShadow.Init    (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);  // t5: shadow map (per frame, not per batch)
    rShadowCube.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);  // t6: point-light depth cube
    rSsao.Init      (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7);  // t7: screen-space occlusion (per frame)
    // t8+t9 as ONE range of two: the irradiance and prefiltered cubes are allocated back-to-back in
    // the shared heap, so a single descriptor table and a single root parameter covers both.
    CD3DX12_DESCRIPTOR_RANGE rIbl, rSsgi;
    rIbl.Init       (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 8);  // t8: irradiance, t9: prefiltered
    rSsgi.Init      (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10); // t10: SSGI bounce

    CD3DX12_ROOT_PARAMETER params[14] = {};
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);    // b0: camera (ViewProj+camPos)
    params[1].InitAsConstants(16, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);      // b1: Model (unused w/ instancing)
    params[2].InitAsDescriptorTable(1, &rAlbedo, D3D12_SHADER_VISIBILITY_PIXEL);    // t0: base color (per batch)
    params[3].InitAsConstantBufferView(3, 0, D3D12_SHADER_VISIBILITY_PIXEL);  // b3: lights (per frame)
    params[4].InitAsConstants(12, 4, 0, D3D12_SHADER_VISIBILITY_PIXEL);       // b4: material (factors+flags, per batch)
    params[5].InitAsDescriptorTable(1, &rMetalRough, D3D12_SHADER_VISIBILITY_PIXEL); // t1: metallic-roughness
    params[6].InitAsDescriptorTable(1, &rEmissive,   D3D12_SHADER_VISIBILITY_PIXEL); // t2: emissive
    params[7].InitAsDescriptorTable(1, &rOcclusion,  D3D12_SHADER_VISIBILITY_PIXEL); // t3: occlusion
    params[8].InitAsDescriptorTable(1, &rNormal,     D3D12_SHADER_VISIBILITY_PIXEL); // t4: normal map
    params[9].InitAsDescriptorTable(1, &rShadow,     D3D12_SHADER_VISIBILITY_PIXEL); // t5: shadow map
    params[10].InitAsDescriptorTable(1, &rShadowCube, D3D12_SHADER_VISIBILITY_PIXEL); // t6: shadow cube
    params[11].InitAsDescriptorTable(1, &rSsao,       D3D12_SHADER_VISIBILITY_PIXEL); // t7: SSAO
    params[12].InitAsDescriptorTable(1, &rIbl,        D3D12_SHADER_VISIBILITY_PIXEL); // t8+t9: IBL cubes
    params[13].InitAsDescriptorTable(1, &rSsgi,       D3D12_SHADER_VISIBILITY_PIXEL); // t10: SSGI

    // s0 = a single static linear/wrap sampler baked into the root signature (no descriptor needed).
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    // ANISOTROPIC, not plain trilinear: at grazing angles the pixel footprint in texture space is a
    // long thin ellipse, and isotropic filtering has to pick one mip for both axes -- so it either
    // aliases along the short axis or smears along the long one. Sponza's drapes and floor are almost
    // entirely grazing angles, which is where the sparkle was worst. Only meaningful now that the
    // textures actually have mip chains (see ResourceManager); before this there was nothing to pick.
    samplers[0].Filter           = D3D12_FILTER_ANISOTROPIC;
    samplers[0].MaxAnisotropy    = 16;
    samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].ShaderRegister   = 0;   // s0
    samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1 = the shadow COMPARISON sampler. COMPARISON_MIN_MAG_LINEAR makes SampleCmpLevelZero return a
    // bilinearly-blended pass/fail fraction instead of a raw depth, which is what turns a hard stair-
    // stepped shadow edge into a smooth one. BORDER address with a WHITE (1.0 = farthest) border means
    // anything sampled outside the map reads as "nothing was closer to the light" -> unshadowed, rather
    // than clamping to the edge texel and smearing the boundary shadow across the whole scene.
    samplers[1].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].ShaderRegister   = 1;   // s1
    samplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC desc;
    desc.Init(_countof(params), params, _countof(samplers), samplers,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        if (error) OutputDebugStringA((const char*)error->GetBufferPointer());
        ThrowIfFailed(hr);
    }
    ComPtr<ID3D12RootSignature> rootSig;
    ThrowIfFailed(m_Device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
    return rootSig;
}

void Renderer3D::Initialize(RendererCore& core) {
    m_Device = core.GetDevice();
    m_Core   = &core;   // used at record time to reach the shared SRV heap + ResourceManager
    for (int i = 0; i < RendererCore::NumFrames; ++i)
        ThrowIfFailed(m_Device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_Allocators[i])));

    // PER-FRAME serial static lists (created OPEN -> Close so the first Reset is uniform).
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_Allocators[i].Get(), nullptr, IID_PPV_ARGS(&m_DrawList[i])));
        m_DrawList[i]->Close();
        m_DrawList[i]->SetName(L"3D world pass");
    }

    // Per-frame skinned-pass lists + allocators (separate from the static pass).
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_SkinnedAllocators[i])));
        ThrowIfFailed(m_Device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_SkinnedAllocators[i].Get(), nullptr, IID_PPV_ARGS(&m_SkinnedList[i])));
        m_SkinnedList[i]->Close();
        m_SkinnedList[i]->SetName(L"3D skinned pass");
    }

    // Per-frame SKY-pass lists + allocators (recorded first each frame; depth off, RTV only).
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_SkyAllocators[i])));
        ThrowIfFailed(m_Device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_SkyAllocators[i].Get(), nullptr, IID_PPV_ARGS(&m_SkyList[i])));
        m_SkyList[i]->Close();
        m_SkyList[i]->SetName(L"3D sky pass");
    }

    // Per-frame PARTICLE-pass lists + allocators (one list does the compute update + the billboard draw).
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_ParticleAllocators[i])));
        ThrowIfFailed(m_Device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_ParticleAllocators[i].Get(), nullptr, IID_PPV_ARGS(&m_ParticleList[i])));
        m_ParticleList[i]->Close();
        m_ParticleList[i]->SetName(L"3D particle pass");
    }

    // One worker context per scheduler thread -- each has its own per-frame allocators AND per-frame
    // lists, so tasks record concurrently AND no list is ever Reset while a prior frame's copy is
    // still executing on the GPU.
    //
    // Asked of the scheduler rather than derived from hardware_concurrency()-1. That derivation was
    // right only for the AUTO pool size: Init(poolSize) takes an explicit count, and an app with a
    // persistent audio thread is told to pass hw-2, which would have left a worker context short.
    int taskCount = (int)JLib::TaskScheduler::Instance().GetWorkerCount();
    if (taskCount < 1) taskCount = 1;
    m_Workers.resize(taskCount);
    for (auto& wkr : m_Workers) {
        for (int i = 0; i < RendererCore::NumFrames; ++i) {
            ThrowIfFailed(m_Device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&wkr.alloc[i])));
            ThrowIfFailed(m_Device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, wkr.alloc[i].Get(), nullptr, IID_PPV_ARGS(&wkr.list[i])));
            wkr.list[i]->Close();
            wkr.list[i]->SetName(L"3D world pass (worker)");
        }
    }

    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    m_QpcFreq = (double)qpf.QuadPart;

    m_RootSig = CreateRootSignature();
    CreatePipelineState();
    CreateShadowResources();   // depth map + depth-only PSO; inert until EnableShadows(true)
    CreateCubeShadowResources();   // six-face depth cube, used only when a POINT light casts
    m_SkinnedRootSig = CreateSkinnedRootSignature();
    CreateSkinnedPipelineState();
    m_SkyRootSig = CreateSkyRootSignature();
    CreateSkyPipeline();
    m_ParticleComputeRootSig = CreateParticleComputeRootSignature();
    CreateParticleComputePipeline();
    // Burst spawn (phase 2A): u0 = the pool buffer (root UAV), b0 = BurstConsts as 20 ROOT CONSTANTS --
    // per-burst params ride the command list itself, so no per-burst CB resource management.
    {
        CD3DX12_ROOT_PARAMETER params[2] = {};
        params[0].InitAsUnorderedAccessView(0);   // u0
        params[1].InitAsConstants(20, 0);          // b0: BurstConsts
        CD3DX12_ROOT_SIGNATURE_DESC desc;
        desc.Init(_countof(params), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> blob, error;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(hr)) { if (error) OutputDebugStringA((const char*)error->GetBufferPointer()); ThrowIfFailed(hr); }
        ThrowIfFailed(m_Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&m_BurstSpawnRootSig)));
        auto cs = ReadFile(ExeRelative(L"shaders\\SpawnBurst3D.cso"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = m_BurstSpawnRootSig.Get();
        pso.CS = { cs.data(), cs.size() };
        ThrowIfFailed(m_Device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_BurstSpawnPSO)));
        m_BurstSpawnPSO->SetName(L"Burst3D spawn PSO");
    }
    m_ParticleDrawRootSig = CreateParticleDrawRootSignature();
    CreateParticleDrawPipeline();
    // Tonemap pipeline (the FP16 resolve). Built HERE, with every other pipeline, so a missing
    // shaders\Tonemap_PS.cso throws at startup naming the file -- rather than on the first frame
    // from inside a noexcept scheduler task, where it surfaces as a bare terminate. The FP16 TEXTURE
    // itself is still lazy (it needs the client size); see CreateHdrTarget.
    CreateTonemapPipeline();
    CreateBloomPipeline();   // same reasoning: shaders read at Initialize, targets built lazily
    CreateFxaaPipeline();
    CreateSsgiPipeline();

    // Per-frame camera constant buffer: UPLOAD heap (CPU-writable), 256 bytes (CB alignment rule),
    // mapped once and never unmapped -- SetCamera() then just memcpy's the matrix into the slot.
    const UINT kCbSize = 256;
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC   cbDesc = CD3DX12_RESOURCE_DESC::Buffer(kCbSize);
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_CameraCB[i])));
        ThrowIfFailed(m_CameraCB[i]->Map(0, nullptr, &m_CameraCBMapped[i])); // keep mapped for life
    }

    // Sky CB per frame: same 256-byte upload buffer as the camera CB (holds InvViewProj + camPos).
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_SkyCB[i])));
        ThrowIfFailed(m_SkyCB[i]->Map(0, nullptr, &m_SkyCBMapped[i]));
    }

    // Particle camera CB per frame (ViewProj + camera right/up for the billboards); same 256-byte upload CB.
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ParticleCamCB[i])));
        ThrowIfFailed(m_ParticleCamCB[i]->Map(0, nullptr, &m_ParticleCamCBMapped[i]));
    }

    // Per-frame instance buffer: kMaxInstances model matrices, UPLOAD heap, mapped for life (like the
    // camera CB). RecordCommandList memcpy's this frame's sorted matrices in, then binds it at vertex
    // slot 1; the IA feeds each instance its matrix via StartInstanceLocation. No 256-byte alignment
    // rule here (it's a vertex buffer, not a CB) -- just size = capacity * one matrix.
    CD3DX12_RESOURCE_DESC instDesc =
        CD3DX12_RESOURCE_DESC::Buffer((UINT64)kMaxInstances * sizeof(DirectX::XMFLOAT4X4));
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &instDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_InstanceBuffer[i])));
        ThrowIfFailed(m_InstanceBuffer[i]->Map(0, nullptr, &m_InstanceMapped[i]));
    }

    // Per-frame lights CB (b3): ambient+count header + the light array, mapped for life. RecordCommandList
    // uploads this frame's rig. Size rounded up to 256 (root-CBV buffers don't strictly require it, but
    // it's harmless and keeps the CB well-formed).
    const UINT kLightsCbSize = (UINT)((sizeof(GpuLightsCB) + 255) & ~255u);
    CD3DX12_RESOURCE_DESC lightsDesc = CD3DX12_RESOURCE_DESC::Buffer(kLightsCbSize);
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &lightsDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_LightsCB[i])));
        ThrowIfFailed(m_LightsCB[i]->Map(0, nullptr, &m_LightsCBMapped[i]));
    }

    // Bone palette CB per frame: kMaxBones row_major float4x4, persistently mapped. Filled with
    // IDENTITY now -- with an identity palette the skinning VS reproduces the exact bind pose, so a
    // skinned mesh must render identical to its static LoadGlbMesh load (the verification checkpoint).
    // Animation will re-upload real joint matrices into these slots later.
    // Split into kMaxSkinnedInstances regions of kMaxBones each: one palette per skinned instance so
    // concurrent skinned draws don't clobber each other's pose. Per-instance stride (8192) is 256-aligned
    // as a root-CBV offset requires.
    const UINT kPerInstanceBytes = kMaxBones * (UINT)sizeof(DirectX::XMFLOAT4X4);       // 128*64 = 8192
    const UINT kBoneCbSize       = kMaxSkinnedInstances * kPerInstanceBytes;
    CD3DX12_RESOURCE_DESC boneDesc = CD3DX12_RESOURCE_DESC::Buffer(kBoneCbSize);
    DirectX::XMFLOAT4X4 identity;
    DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &boneDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_BoneCB[i])));
        ThrowIfFailed(m_BoneCB[i]->Map(0, nullptr, &m_BoneCBMapped[i]));
        auto* dst = reinterpret_cast<DirectX::XMFLOAT4X4*>(m_BoneCBMapped[i]);
        for (UINT b = 0; b < kMaxSkinnedInstances * kMaxBones; ++b) dst[b] = identity;  // safe default
    }

    // Safe default until SetCamera is called: identity means "no camera transform", so a cube still
    // shows (as a flat centered square) instead of transforming through uninitialized garbage.
    DirectX::XMStoreFloat4x4(&m_ViewProj, DirectX::XMMatrixIdentity());
}

void Renderer3D::CreatePipelineState() {
    // Load the compiled shader bytecode (FxCompile produced these .cso next to the exe).
    auto vs = ReadFile(ExeRelative(L"shaders\\Basic3D_VS.cso"));
    auto ps = ReadFile(ExeRelative(L"shaders\\Basic3D_PS.cso"));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_RootSig.Get();                        // the b0/b1 contract from above
    pso.VS = { vs.data(), vs.size() };
    pso.PS = { ps.data(), ps.size() };
    pso.InputLayout = { kVertex3DLayout, _countof(kVertex3DLayout) }; // how to unpack a Vertex3D
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // The FP16 scene target, NOT the back buffer -- this pass writes linear HDR and the tonemap pass
    // resolves it later (see RendererCore::HdrFormat). Must match or PSO creation fails outright.
    pso.RTVFormats[0] = RendererCore::HdrFormat;
    pso.NumRenderTargets = 1;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = 0xFFFFFFFF;

    // Opaque geometry: no alpha blending (that D3D12_DEFAULT = "just write the pixel").
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

    // Solid fill. CULL_MODE_NONE on purpose FOR NOW: back-face culling makes triangles wound the
    // "wrong" way invisible, which is a classic first-cube trap -- with NONE the cube shows no
    // matter how its indices are wound. Switch to D3D12_CULL_MODE_BACK later (a ~2x fill win) once
    // the winding is confirmed.
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // THE 3D difference vs the 2D PSO: depth test + write ON. D3D12_DEFAULT already sets
    // DepthEnable=TRUE, write=ALL, func=LESS (nearer pixels win) -- exactly what sorts a 3D scene
    // so you don't hand-order draws like 2D zLayers. DSVFormat must match RendererCore's DSV.
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DSVFormat = RendererCore::DepthFormat;

    ThrowIfFailed(m_Device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_PSO)));
    m_PSO->SetName(L"Basic3D PSO");
}

// The static root sig's registers PLUS the skinning-only b1 Model + b2 bone palette. The shared
// PS reads b0(camPos)/b3(lights)/b4(material)/t0(albedo) here too, so those must appear in BOTH sigs.
// Existing indices (b1=1, b2=2, t0=3) are unchanged; lights(4) + material(5) are appended.
ComPtr<ID3D12RootSignature> Renderer3D::CreateSkinnedRootSignature() {
    CD3DX12_DESCRIPTOR_RANGE rAlbedo, rMetalRough, rEmissive, rOcclusion, rNormal, rShadow, rShadowCube, rSsao;
    rAlbedo.Init    (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);   // t0
    rMetalRough.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);   // t1
    rEmissive.Init  (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);   // t2
    rOcclusion.Init (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);   // t3
    rNormal.Init    (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);   // t4
    rShadow.Init    (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);   // t5: shadow map
    rShadowCube.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);  // t6: point-light depth cube
    rSsao.Init      (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7);  // t7: screen-space occlusion
    CD3DX12_DESCRIPTOR_RANGE rIbl, rSsgi;
    rIbl.Init       (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 8);  // t8: irradiance, t9: prefiltered
    rSsgi.Init      (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 10); // t10: SSGI bounce

    CD3DX12_ROOT_PARAMETER params[15] = {};
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);         // b0: camera (ViewProj+camPos)
    params[1].InitAsConstants(16, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);           // b1: Model (16 floats)
    params[2].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_VERTEX);      // b2: bone palette
    params[3].InitAsDescriptorTable(1, &rAlbedo, D3D12_SHADER_VISIBILITY_PIXEL);   // t0: base color (per draw)
    params[4].InitAsConstantBufferView(3, 0, D3D12_SHADER_VISIBILITY_PIXEL);       // b3: lights (per frame)
    params[5].InitAsConstants(12, 4, 0, D3D12_SHADER_VISIBILITY_PIXEL);            // b4: material (factors+flags, per draw)
    params[6].InitAsDescriptorTable(1, &rMetalRough, D3D12_SHADER_VISIBILITY_PIXEL); // t1: metallic-roughness
    params[7].InitAsDescriptorTable(1, &rEmissive,   D3D12_SHADER_VISIBILITY_PIXEL); // t2: emissive
    params[8].InitAsDescriptorTable(1, &rOcclusion,  D3D12_SHADER_VISIBILITY_PIXEL); // t3: occlusion
    params[9].InitAsDescriptorTable(1, &rNormal,     D3D12_SHADER_VISIBILITY_PIXEL); // t4: normal map
    // t5 + s1 exist here ONLY because this signature shares Basic3D_PS with the static path. A root
    // signature must satisfy everything its shaders declare, so adding a resource to that PS means
    // adding it to BOTH signatures -- omitting it here is what made CreateSkinnedPipelineState fail.
    params[10].InitAsDescriptorTable(1, &rShadow,    D3D12_SHADER_VISIBILITY_PIXEL); // t5: shadow map
    params[11].InitAsDescriptorTable(1, &rShadowCube, D3D12_SHADER_VISIBILITY_PIXEL); // t6: shadow cube
    params[12].InitAsDescriptorTable(1, &rSsao,       D3D12_SHADER_VISIBILITY_PIXEL); // t7: SSAO
    params[13].InitAsDescriptorTable(1, &rIbl,        D3D12_SHADER_VISIBILITY_PIXEL); // t8+t9: IBL cubes
    params[14].InitAsDescriptorTable(1, &rSsgi,       D3D12_SHADER_VISIBILITY_PIXEL); // t10: SSGI

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    // ANISOTROPIC, not plain trilinear: at grazing angles the pixel footprint in texture space is a
    // long thin ellipse, and isotropic filtering has to pick one mip for both axes -- so it either
    // aliases along the short axis or smears along the long one. Sponza's drapes and floor are almost
    // entirely grazing angles, which is where the sparkle was worst. Only meaningful now that the
    // textures actually have mip chains (see ResourceManager); before this there was nothing to pick.
    samplers[0].Filter           = D3D12_FILTER_ANISOTROPIC;
    samplers[0].MaxAnisotropy    = 16;
    samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].ShaderRegister   = 0;   // s0
    samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s1: the shadow comparison sampler -- must match the static signature's exactly.
    samplers[1].Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].ShaderRegister   = 1;   // s1
    samplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC desc;
    desc.Init(_countof(params), params, _countof(samplers), samplers,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        if (error) OutputDebugStringA((const char*)error->GetBufferPointer());
        ThrowIfFailed(hr);
    }
    ComPtr<ID3D12RootSignature> rootSig;
    ThrowIfFailed(m_Device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
    return rootSig;
}

// Same pipeline as the static one but with the skinned input layout and the bone-blended vertex
// shader. The PIXEL shader is REUSED (Basic3D_PS) -- the skinning happens entirely in the VS, and the
// VS->PS contract (clip/normal/uv) is identical, so lighting/albedo code is shared.
void Renderer3D::CreateSkinnedPipelineState() {
    auto vs = ReadFile(ExeRelative(L"shaders\\Skinned3D_VS.cso"));
    auto ps = ReadFile(ExeRelative(L"shaders\\Basic3D_PS.cso"));   // reuse the static pixel shader

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_SkinnedRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { kSkinnedVertex3DLayout, _countof(kSkinnedVertex3DLayout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RTVFormats[0]         = RendererCore::HdrFormat;   // FP16 scene target, not the back buffer
    pso.NumRenderTargets      = 1;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = 0xFFFFFFFF;
    pso.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DSVFormat             = RendererCore::DepthFormat;

    ThrowIfFailed(m_Device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_SkinnedPSO)));
    m_SkinnedPSO->SetName(L"Skinned3D PSO");
}

// Sky root sig: just b0 (InvViewProj + camPos), read by the VS. No textures/sampler/input layout -- the
// sky is a fullscreen triangle generated from SV_VertexID, so ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT is off.
ComPtr<ID3D12RootSignature> Renderer3D::CreateSkyRootSignature() {
    CD3DX12_ROOT_PARAMETER params[3] = {};
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);   // b0: InvViewProj + camPos
    // b1 + t0: the HDR environment, so the visible sky can be the real HDRI rather than the procedural
    // gradient. Root CONSTANTS rather than a CBV -- two floats that change at most once per frame.
    params[1].InitAsConstants(4, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);          // b1: hasEnv + intensity
    CD3DX12_DESCRIPTOR_RANGE rSky;
    rSky.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    params[2].InitAsDescriptorTable(1, &rSky, D3D12_SHADER_VISIBILITY_PIXEL);   // t0: equirect HDRI
    D3D12_STATIC_SAMPLER_DESC skySampler = {};
    skySampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    // WRAP in U so the lat/long seam joins cleanly; CLAMP in V so the poles can't wrap onto each other.
    skySampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    skySampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    skySampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    skySampler.MaxLOD           = D3D12_FLOAT32_MAX;
    skySampler.ShaderRegister   = 0;
    skySampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    CD3DX12_ROOT_SIGNATURE_DESC desc;
    desc.Init(_countof(params), params, 1, &skySampler, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) {
        if (error) OutputDebugStringA((const char*)error->GetBufferPointer());
        ThrowIfFailed(hr);
    }
    ComPtr<ID3D12RootSignature> rootSig;
    ThrowIfFailed(m_Device->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
    return rootSig;
}

// Sky pipeline: Skybox_VS/PS, NO input layout (SV_VertexID triangle), cull none, and depth OFF -- the sky
// writes color but no depth, so the geometry pass (depth on) draws over it. No DSV is bound at record time.
void Renderer3D::CreateSkyPipeline() {
    auto vs = ReadFile(ExeRelative(L"shaders\\Skybox_VS.cso"));
    auto ps = ReadFile(ExeRelative(L"shaders\\Skybox_PS.cso"));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_SkyRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { nullptr, 0 };   // no vertex buffer
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // FP16 scene target. This is also what finally makes the HDRI sky honest: sampled at intensity
    // it routinely exceeds 1.0, and writing it to an 8-bit target clipped the sun to flat white.
    pso.RTVFormats[0]         = RendererCore::HdrFormat;
    pso.NumRenderTargets      = 1;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = 0xFFFFFFFF;
    pso.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState        = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable    = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;   // no depth buffer for this pass

    ThrowIfFailed(m_Device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_SkyPSO)));
    m_SkyPSO->SetName(L"Skybox PSO");
}

// Records the fullscreen sky triangle into this frame's sky list: RTV only (no depth), bind b0, draw 3
// verts. Caller (RecordCommandList) has already uploaded m_SkyCB[frame] and pushes this list FIRST.
void Renderer3D::RecordSkybox(int frame, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height) {
    m_SkyAllocators[frame]->Reset();
    m_SkyList[frame]->Reset(m_SkyAllocators[frame].Get(), m_SkyPSO.Get());
    m_SkyList[frame]->OMSetRenderTargets(1, &rtv, FALSE, nullptr);   // RTV only -- the sky has no depth
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)width, (LONG)height };
    m_SkyList[frame]->RSSetViewports(1, &vp);
    m_SkyList[frame]->RSSetScissorRects(1, &sc);
    m_SkyList[frame]->SetGraphicsRootSignature(m_SkyRootSig.Get());
    m_SkyList[frame]->SetGraphicsRootConstantBufferView(0, m_SkyCB[frame]->GetGPUVirtualAddress());  // b0
    // b1 + t0: when an environment is loaded the sky IS the HDRI, so the background finally matches
    // the light. The descriptor table must be populated either way (D3D12 requires it even when the
    // shader branches past it), so without an environment any valid SRV will do.
    {
        const float skyConsts[4] = { m_EnvReady ? 1.0f : 0.0f, m_SkyIntensity, 0.0f, 0.0f };
        m_SkyList[frame]->SetGraphicsRoot32BitConstants(1, 4, skyConsts, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE srv = m_EquirectSrvGpu.ptr ? m_EquirectSrvGpu : m_ShadowSrvGpu;
        if (srv.ptr) {
            ID3D12DescriptorHeap* heaps[] = { m_Core->GetRenderer2D()->GetSrvHeap() };
            m_SkyList[frame]->SetDescriptorHeaps(1, heaps);
            m_SkyList[frame]->SetGraphicsRootDescriptorTable(2, srv);
        }
    }
    m_SkyList[frame]->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_SkyList[frame]->DrawInstanced(3, 1, 0, 0);   // one oversized triangle covers the screen
    ThrowIfFailed(m_SkyList[frame]->Close());
}

// Particle COMPUTE root sig: u0 = the particle buffer (root UAV, no descriptor heap), b0 = SimCB.
ComPtr<ID3D12RootSignature> Renderer3D::CreateParticleComputeRootSignature() {
    CD3DX12_ROOT_PARAMETER params[2] = {};
    params[0].InitAsUnorderedAccessView(0);      // u0: RWStructuredBuffer<Particle3D>
    params[1].InitAsConstantBufferView(0);        // b0: SimCB
    CD3DX12_ROOT_SIGNATURE_DESC desc;
    desc.Init(_countof(params), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) { if (error) OutputDebugStringA((const char*)error->GetBufferPointer()); ThrowIfFailed(hr); }
    ComPtr<ID3D12RootSignature> rootSig;
    ThrowIfFailed(m_Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
    return rootSig;
}

// Particle DRAW root sig: b0 = ParticleCam (ViewProj + right/up), t0 = particle buffer (root SRV), b1 = 8
// root constants (colorEnd + atlas). All VERTEX-visible; the PS reads only interpolated color/uv.
ComPtr<ID3D12RootSignature> Renderer3D::CreateParticleDrawRootSignature() {
    CD3DX12_ROOT_PARAMETER params[3] = {};
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);   // b0: ParticleCam
    params[1].InitAsShaderResourceView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);   // t0: Particles (root SRV)
    params[2].InitAsConstants(8, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);          // b1: colorEnd + atlas
    CD3DX12_ROOT_SIGNATURE_DESC desc;
    desc.Init(_countof(params), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) { if (error) OutputDebugStringA((const char*)error->GetBufferPointer()); ThrowIfFailed(hr); }
    ComPtr<ID3D12RootSignature> rootSig;
    ThrowIfFailed(m_Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
    return rootSig;
}

void Renderer3D::CreateParticleComputePipeline() {
    auto cs = ReadFile(ExeRelative(L"shaders\\UpdateParticles3D.cso"));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_ParticleComputeRootSig.Get();
    pso.CS = { cs.data(), cs.size() };
    ThrowIfFailed(m_Device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_ParticleComputePSO)));
    m_ParticleComputePSO->SetName(L"Particle3D compute PSO");
}

void Renderer3D::CreateParticleDrawPipeline() {
    auto vs = ReadFile(ExeRelative(L"shaders\\Particle3D_VS.cso"));
    auto ps = ReadFile(ExeRelative(L"shaders\\Particle3D_PS.cso"));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_ParticleDrawRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { nullptr, 0 };   // no vertex buffer -- SV_VertexID quad, SV_InstanceID particle
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RTVFormats[0]         = RendererCore::HdrFormat;   // FP16 scene target, not the back buffer
    pso.NumRenderTargets      = 1;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = 0xFFFFFFFF;
    // Straight alpha blend so translucent particles composite over the scene.
    CD3DX12_BLEND_DESC blend(D3D12_DEFAULT);
    blend.RenderTarget[0].BlendEnable    = TRUE;
    blend.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    pso.BlendState            = blend;
    pso.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    // Depth TEST on, WRITE off: occluded by opaque geometry, but translucent particles don't write depth
    // (so they don't hard-occlude each other). Same DSV as the geometry pass.
    pso.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DSVFormat             = RendererCore::DepthFormat;

    ThrowIfFailed(m_Device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_ParticleDrawPSO)));
    m_ParticleDrawPSO->SetName(L"Particle3D draw PSO");
}

Renderer3D::ParticleEmitterHandle Renderer3D::AddParticleEmitter(const ParticleEmitterDesc& desc) {
    ParticleEmitter e;
    e.desc = desc;
    // DEFAULT-heap RWStructuredBuffer<Particle3D>. Committed resources are zero-initialized by the runtime,
    // so every slot starts isActive=0/lifetime=0 -> the compute Spawns them all on frame 1, then they desync
    // as their randomized lifetimes differ (a brief initial burst, then a steady fountain). No CPU seed pass.
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(
        (UINT64)desc.count * sizeof(Particle3D), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // Create in COMMON -- a DEFAULT-heap buffer MUST be (the debug layer breaks inside
    // CreateCommittedResource otherwise; same gotcha as the 2D pools' drawBuffer, 2026-07-21).
    // First UAV use implicitly promotes COMMON->UNORDERED_ACCESS; after that the explicit
    // UAV<->COPY_SOURCE barrier chain in RecordParticleCompute governs, exactly like the 2D pattern.
    ThrowIfFailed(m_Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&e.buffer)));
    e.buffer->SetName(L"Particle3D buffer");

    // Per-frame graphics-read SNAPSHOT: same size as `buffer` but NO UAV flag; rests in COMMON (only ever a
    // CopyBufferRegion dest + an SRV read, both of which promote from COMMON). The draw binds these, never `buffer`.
    CD3DX12_RESOURCE_DESC drawDesc = CD3DX12_RESOURCE_DESC::Buffer((UINT64)desc.count * sizeof(Particle3D));
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &drawDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&e.drawBuffer[i])));
    }

    const UINT kCbSize = 256;
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(kCbSize);
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&e.simCB[i])));
        ThrowIfFailed(e.simCB[i]->Map(0, nullptr, &e.simCBMapped[i]));
    }
    m_Emitters.push_back(std::move(e));
    return m_Emitters.size() - 1;
}

Renderer3D::ParticleEmitterHandle Renderer3D::AddBurstPool(uint32_t capacity, float gravity) {
    // A burst pool is a regular emitter whose update runs with respawn=0 (dead stays dead) and whose slots
    // are filled on demand by SpawnBurst3D via the CPU-owned ring head. Zero-init => starts fully dead/empty.
    ParticleEmitterDesc d{};
    d.count   = capacity;
    d.gravity = gravity;
    ParticleEmitterHandle h = AddParticleEmitter(d);
    m_Emitters[h].isBurst = true;
    return h;
}

void Renderer3D::RequestBurst3D(ParticleEmitterHandle pool, const BurstDesc& burst) {
    if (pool >= m_Emitters.size() || !m_Emitters[pool].isBurst) return;
    ParticleEmitter& e = m_Emitters[pool];
    std::lock_guard<std::mutex> g(m_BurstMtx);
    BurstReq r{ burst, 0 };
    if (r.d.count == 0) return;
    if (r.d.count > e.desc.count) r.d.count = e.desc.count;   // a single burst can't exceed the pool
    r.startSlot = e.ringHead % e.desc.count;                  // reserve [startSlot, +count) (GPU wraps by %)
    e.ringHead += r.d.count;
    e.pending.push_back(r);
}

// Records the 3D particle COMPUTE onto the ASYNC-COMPUTE list (called by RendererCore::UpdateParticles, so it
// shares m_ComputeQueue + m_ComputeFence with the 2D pools). Per emitter: integrate/recycle in `buffer`, then
// SNAPSHOT buffer -> drawBuffer[frame] so the graphics queue reads a separate resource and the two overlap.
// `buffer` is touched ONLY here (compute queue) and rests in UNORDERED_ACCESS; drawBuffer rests in COMMON.
void Renderer3D::RecordParticleCompute(ID3D12GraphicsCommandList* cmd, int frame, float dt) {
    if (!m_ParticlesEnabled || m_Emitters.empty()) return;
    m_ParticleTime += dt;
    for (auto& e : m_Emitters) {
        // --- burst spawns first (phase 2A): fill this frame's reserved ring slots, then barrier so the
        // update pass below sees them. Drained under the mutex (RequestBurst3D appends from a frame task).
        std::vector<BurstReq> reqs;
        if (e.isBurst) { std::lock_guard<std::mutex> g(m_BurstMtx); reqs.swap(e.pending); }
        if (!reqs.empty()) {
            cmd->SetPipelineState(m_BurstSpawnPSO.Get());
            cmd->SetComputeRootSignature(m_BurstSpawnRootSig.Get());
            cmd->SetComputeRootUnorderedAccessView(0, e.buffer->GetGPUVirtualAddress());
            for (const BurstReq& r : reqs) {
                BurstConsts bc{};
                bc.pos[0] = r.d.position.x; bc.pos[1] = r.d.position.y; bc.pos[2] = r.d.position.z;
                bc.startSlot = r.startSlot;
                bc.normal[0] = r.d.normal.x; bc.normal[1] = r.d.normal.y; bc.normal[2] = r.d.normal.z;
                bc.count = r.d.count; bc.poolSize = e.desc.count;
                bc.speed = r.d.speed; bc.lifetime = r.d.lifetime; bc.size = r.d.size;
                bc.color[0] = r.d.color.x; bc.color[1] = r.d.color.y; bc.color[2] = r.d.color.z; bc.color[3] = r.d.color.w;
                bc.normalBias = r.d.normalBias;
                bc.seed = (uint32_t)(m_ParticleTime * 6791.0f) + r.startSlot;   // decorrelate bursts
                cmd->SetComputeRoot32BitConstants(1, 20, &bc, 0);
                cmd->Dispatch((r.d.count + 63) / 64, 1, 1);
            }
            auto uav = CD3DX12_RESOURCE_BARRIER::UAV(e.buffer.Get());
            cmd->ResourceBarrier(1, &uav);   // spawns visible before the update pass integrates them
        }

        // --- update pass (fountain: recycle; burst: die-in-place) ---
        cmd->SetPipelineState(m_ParticleComputePSO.Get());
        cmd->SetComputeRootSignature(m_ParticleComputeRootSig.Get());
        SimCB s{};
        s.dt = dt; s.time = m_ParticleTime; s.maxParticles = e.desc.count; s.gravity = e.desc.gravity;
        s.emitterPos = e.desc.position; s.spread = e.desc.spread;
        s.speed = e.desc.speed; s.lifetime = e.desc.lifetime; s.size = e.desc.size;
        s.respawn = e.isBurst ? 0u : 1u;
        s.color = e.desc.color;
        memcpy(e.simCBMapped[frame], &s, sizeof(s));
        cmd->SetComputeRootUnorderedAccessView(0, e.buffer->GetGPUVirtualAddress());
        cmd->SetComputeRootConstantBufferView(1, e.simCB[frame]->GetGPUVirtualAddress());
        cmd->Dispatch((e.desc.count + 255) / 256, 1, 1);

        // Snapshot the just-integrated buffer into this frame's graphics-read slot (same pattern as the 2D
        // pools' drawBuffer). UAV barrier so the copy sees the writes; then buffer UAV->COPY_SOURCE +
        // drawBuffer COMMON->COPY_DEST, copy, and back (buffer rests UAV for next frame; drawBuffer COMMON).
        auto uav = CD3DX12_RESOURCE_BARRIER::UAV(e.buffer.Get());
        cmd->ResourceBarrier(1, &uav);
        D3D12_RESOURCE_BARRIER pre[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(e.buffer.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(e.drawBuffer[frame].Get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cmd->ResourceBarrier(2, pre);
        cmd->CopyBufferRegion(e.drawBuffer[frame].Get(), 0, e.buffer.Get(), 0,
            (UINT64)e.desc.count * sizeof(Particle3D));
        D3D12_RESOURCE_BARRIER post[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(e.buffer.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(e.drawBuffer[frame].Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
        };
        cmd->ResourceBarrier(2, post);
    }
}

// DRAW-ONLY now (the compute moved to RecordParticleCompute on the async queue). Camera-facing billboards
// into the 3D RTV + shared depth (test on, write off), reading each emitter's drawBuffer[frame] SNAPSHOT
// (promoted from COMMON on the SRV read). Gated behind the compute by RendererCore's m_ComputeFence wait.
void Renderer3D::RecordParticles(int frame, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                 uint32_t width, uint32_t height) {
    auto* cmd = m_ParticleList[frame].Get();
    m_ParticleAllocators[frame]->Reset();
    cmd->Reset(m_ParticleAllocators[frame].Get(), m_ParticleDrawPSO.Get());
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)width, (LONG)height };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->SetGraphicsRootSignature(m_ParticleDrawRootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_ParticleCamCB[frame]->GetGPUVirtualAddress());   // b0 cam
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (auto& e : m_Emitters) {
        cmd->SetGraphicsRootShaderResourceView(1, e.drawBuffer[frame]->GetGPUVirtualAddress());   // t0 = snapshot
        struct { float colorEnd[4]; uint32_t ax, ay, p0, p1; } dp =
            { { e.desc.color.x, e.desc.color.y, e.desc.color.z, 0.0f }, 1u, 1u, 0u, 0u };          // fade to transparent
        cmd->SetGraphicsRoot32BitConstants(2, 8, &dp, 0);
        cmd->DrawInstanced(6, e.desc.count, 0, 0);   // 6 verts/quad * count instances
    }
    ThrowIfFailed(cmd->Close());
}

void Renderer3D::SetCamera(DirectX::FXMMATRIX viewProj, DirectX::XMFLOAT3 cameraWorldPos) {
    // XMMATRIX is a SIMD register type; store it into plain floats now; RecordCommandList copies it
    // into this frame's constant-buffer slot at record time (so we don't need a frame index here).
    DirectX::XMStoreFloat4x4(&m_ViewProj, viewProj);
    m_CamPos = cameraWorldPos;   // uploaded alongside ViewProj into the b0 camera CB (PS needs it)

    // Extract the 6 world-space frustum planes from ViewProj (Gribb-Hartmann). Row-major, v*M convention:
    // clip.i = dot(v, column_i(M)), with column_i = (m[0][i], m[1][i], m[2][i], m[3][i]). left = col3+col0,
    // right = col3-col0, bottom = col3+col1, top = col3-col1, near = col2 (D3D depth [0,1]), far = col3-col2.
    // Each is normalized so plane.xyz is unit-length -> the dot below is a true signed distance.
    const auto& m = m_ViewProj.m;
    auto setPlane = [&](int idx, float a, float b, float c, float d) {
        float inv = 1.0f / std::sqrt(a * a + b * b + c * c);
        m_FrustumPlanes[idx] = { a * inv, b * inv, c * inv, d * inv };
    };
    setPlane(0, m[0][3]+m[0][0], m[1][3]+m[1][0], m[2][3]+m[2][0], m[3][3]+m[3][0]); // left
    setPlane(1, m[0][3]-m[0][0], m[1][3]-m[1][0], m[2][3]-m[2][0], m[3][3]-m[3][0]); // right
    setPlane(2, m[0][3]+m[0][1], m[1][3]+m[1][1], m[2][3]+m[2][1], m[3][3]+m[3][1]); // bottom
    setPlane(3, m[0][3]-m[0][1], m[1][3]-m[1][1], m[2][3]-m[2][1], m[3][3]-m[3][1]); // top
    setPlane(4, m[0][2],         m[1][2],         m[2][2],         m[3][2]);         // near
    setPlane(5, m[0][3]-m[0][2], m[1][3]-m[1][2], m[2][3]-m[2][2], m[3][3]-m[3][2]); // far
    m_FrustumValid = true;
}

// Returns TRUE if the mesh's world-space bounding sphere is at least partially inside the frustum (keep
// it); FALSE means fully outside (cull). Culling off / no camera yet / no bounds => always keep (safe).
bool Renderer3D::CullSphere(DirectX::FXMMATRIX model, const Mesh& mesh) const {
    using namespace DirectX;
    if (!m_CullingEnabled || !m_FrustumValid || mesh.boundsRadius <= 0.0f) return true;

    // World center = local center * model (w=1). World radius = local radius * the model's largest axis
    // scale (longest of the 3 basis rows) -- conservative under non-uniform scale, never over-culls.
    XMFLOAT3 c;
    XMStoreFloat3(&c, XMVector3Transform(XMLoadFloat3(&mesh.boundsCenter), model));
    XMFLOAT4X4 mm; XMStoreFloat4x4(&mm, model);
    auto len3 = [](float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); };
    float s = std::max(len3(mm.m[0][0], mm.m[0][1], mm.m[0][2]),
              std::max(len3(mm.m[1][0], mm.m[1][1], mm.m[1][2]),
                       len3(mm.m[2][0], mm.m[2][1], mm.m[2][2])));
    float r = mesh.boundsRadius * s;

    for (int i = 0; i < 6; ++i) {
        const XMFLOAT4& p = m_FrustumPlanes[i];
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return false;   // outside this plane -> cull
    }
    return true;
}

void Renderer3D::AddLight(const GpuLight& l) {
    if (m_LightCount < kMaxLights) m_Lights[m_LightCount++] = l;
}

void Renderer3D::AddDirectionalLight(DirectX::XMFLOAT3 dir, DirectX::XMFLOAT3 color, float intensity) {
    DirectX::XMFLOAT3 nd; DirectX::XMStoreFloat3(&nd, DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&dir)));
    GpuLight l{};
    l.direction = nd; l.color = color; l.intensity = intensity; l.type = 0.0f;
    AddLight(l);
}

void Renderer3D::AddPointLight(DirectX::XMFLOAT3 pos, DirectX::XMFLOAT3 color, float intensity, float range) {
    GpuLight l{};
    l.position = pos; l.range = range; l.color = color; l.intensity = intensity; l.type = 1.0f;
    AddLight(l);
}

void Renderer3D::AddSpotLight(DirectX::XMFLOAT3 pos, DirectX::XMFLOAT3 dir, DirectX::XMFLOAT3 color,
                              float intensity, float range, float innerDeg, float outerDeg) {
    GpuLight l{};
    DirectX::XMVECTOR d = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&dir));
    DirectX::XMStoreFloat3(&l.direction, d);
    l.position = pos; l.range = range; l.color = color; l.intensity = intensity; l.type = 2.0f;
    // Stored as COSINES of the half-angles because the shader compares against dot products, which
    // are cosines already -- doing the conversion here keeps an acos out of the per-pixel path.
    // Inner must be the LARGER cosine (the tighter angle) or the smoothstep falloff inverts.
    if (outerDeg < innerDeg) outerDeg = innerDeg;
    l.spotCosInner = cosf(DirectX::XMConvertToRadians(innerDeg));
    l.spotCosOuter = cosf(DirectX::XMConvertToRadians(outerDeg));
    AddLight(l);
}

void Renderer3D::AddRectAreaLight(DirectX::XMFLOAT3 pos, DirectX::XMFLOAT3 normal,
                                  DirectX::XMFLOAT3 color, float intensity,
                                  float halfWidth, float halfHeight, float range,
                                  float rollDeg, bool twoSided) {
    GpuLight l{};
    DirectX::XMVECTOR n = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&normal));
    DirectX::XMStoreFloat3(&l.direction, n);   // the face normal, NOT a travel direction
    l.position = pos; l.range = range; l.color = color; l.intensity = intensity;
    l.type = 3.0f;
    // The spot cosine slots carry the half-extents, and the two spare floats carry the roll and the
    // two-sided flag. Packing into the existing 64 bytes rather than growing GpuLight is deliberate:
    // the struct is static_asserted against the shader's Light, so widening it would force a rebuild
    // of every consumer in every configuration for one new light type.
    l.spotCosInner = halfWidth  > 0.0f ? halfWidth  : 0.0f;
    l.spotCosOuter = halfHeight > 0.0f ? halfHeight : 0.0f;
    l.pad0 = DirectX::XMConvertToRadians(rollDeg);
    l.pad1 = twoSided ? 1.0f : 0.0f;
    AddLight(l);
}

// 1x1 opaque white, built on first use. Created here rather than in Initialize because it needs the
// ResourceManager, which lives on Renderer2D and is not guaranteed to be attached that early --
// exactly the reason SSAO and the HDR target are also lazy.
TextureHandle Renderer3D::WhiteTexture() {
    if (m_WhiteTex.IsValid() || m_WhiteTexTried) return m_WhiteTex;
    m_WhiteTexTried = true;   // one attempt only; a failure must not retry every Submit of every frame
    auto* rm = m_Core->GetRenderer2D()->GetResourceManager();
    m_Core->ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
        m_WhiteTex = rm->CreateSolidColorTexture(cmd, 255, 255, 255, 255);
    });
    return m_WhiteTex;
}

void Renderer3D::Submit(const Mesh& mesh, DirectX::FXMMATRIX model) {
    // Frustum cull FIRST -- an off-screen object skips the albedo lock + Resolve below, not just the draw.
    if (!CullSphere(model, mesh)) { ++m_CulledThisFrame; return; }
    auto* rm = m_Core->GetRenderer2D()->GetResourceManager();

    // NO ALBEDO AT ALL is a legitimate material, not an error: baseColorFactor exists so a mesh can be
    // a flat colour, and every procedural MakeCubeMesh/MakeCapsuleMesh starts that way. Falling through
    // to the IsTextureReady gate below made those meshes SILENTLY INVISIBLE -- no error, no warning,
    // the object simply never drew. White is the identity for `baseColorFactor * albedo`, so
    // substituting it makes the factor mean exactly what a caller expects.
    //
    // Substituting the HANDLE, not a patched Mesh copy: DrawItem stores `const Mesh*` and the batcher
    // sorts on that pointer, so it has to outlive the frame -- a local copy would dangle immediately.
    // The IsTextureReady gate below still applies, and still matters for a VALID handle that is only
    // part-way through its async upload; that case really must skip a frame.
    TextureHandle albedoH = mesh.material.albedo;
    if (!albedoH.IsValid()) {
        albedoH = WhiteTexture();
        if (!albedoH.IsValid()) return;   // could not create it; nothing sensible left to bind
    }
    // Albedo not ready yet (e.g. a glTF embedded texture still uploading, or never assigned)? Skip this
    // draw for now -- Resolve() THROWS until Ready, and binding a half-uploaded descriptor is a GPU
    // hazard. The model pops in fully textured within a few frames once PumpAsyncUploads() completes.
    // This is the same "gate on IsTextureReady before drawing" contract async 2D sprites already follow.
    if (!rm->IsTextureReady(albedoH)) return;
    DrawItem it;
    it.mesh = &mesh;
    DirectX::XMStoreFloat4x4(&it.model, model);   // SIMD matrix -> plain 4x4 floats for root consts
    // Resolve the albedo descriptor ONCE, here on the main thread. Resolve() locks the AssetManager
    // mutex; doing it per-draw inside the parallel record loop serializes the workers on that lock.
    it.albedo = rm->Resolve(albedoH).gpuHandle;
    // Optional maps: resolve each if it's valid AND uploaded; otherwise leave `albedo` as a harmless filler
    // (a valid bound texture) and keep its mapFlags bit 0 so the shader ignores it (falls back to the factor).
    it.metalRough = it.emissive = it.occlusion = it.normal = it.albedo;
    it.mapFlags = 0;
    if (mesh.material.metalRough.IsValid() && rm->IsTextureReady(mesh.material.metalRough)) { it.metalRough = rm->Resolve(mesh.material.metalRough).gpuHandle; it.mapFlags |= 1u; }
    if (mesh.material.emissive.IsValid()   && rm->IsTextureReady(mesh.material.emissive))   { it.emissive   = rm->Resolve(mesh.material.emissive).gpuHandle;   it.mapFlags |= 2u; }
    if (mesh.material.occlusion.IsValid()  && rm->IsTextureReady(mesh.material.occlusion))  { it.occlusion  = rm->Resolve(mesh.material.occlusion).gpuHandle;  it.mapFlags |= 4u; }
    if (mesh.material.normal.IsValid()     && rm->IsTextureReady(mesh.material.normal))     { it.normal     = rm->Resolve(mesh.material.normal).gpuHandle;     it.mapFlags |= 8u; }
    m_Items.push_back(it);
}

void Renderer3D::SubmitSkinned(const Mesh& mesh, DirectX::FXMMATRIX model,
                               const std::vector<DirectX::XMFLOAT4X4>& bonePalette) {
    if (!CullSphere(model, mesh)) { ++m_CulledThisFrame; return; }   // off-screen -> skip (see Submit)
    auto* rm = m_Core->GetRenderer2D()->GetResourceManager();
    if (!rm->IsTextureReady(mesh.material.albedo)) return;   // async albedo still uploading -- skip (see Submit)
    SkinnedItem it;
    it.mesh = &mesh;   // a SkinnedVertex3D mesh; recorded in the skinned pass
    DirectX::XMStoreFloat4x4(&it.model, model);
    it.palette = bonePalette;   // copy this instance's pose -- caller's vector needn't outlive the frame
    it.albedo  = rm->Resolve(mesh.material.albedo).gpuHandle;
    it.metalRough = it.emissive = it.occlusion = it.normal = it.albedo;   // filler; see Submit
    it.mapFlags = 0;
    if (mesh.material.metalRough.IsValid() && rm->IsTextureReady(mesh.material.metalRough)) { it.metalRough = rm->Resolve(mesh.material.metalRough).gpuHandle; it.mapFlags |= 1u; }
    if (mesh.material.emissive.IsValid()   && rm->IsTextureReady(mesh.material.emissive))   { it.emissive   = rm->Resolve(mesh.material.emissive).gpuHandle;   it.mapFlags |= 2u; }
    if (mesh.material.occlusion.IsValid()  && rm->IsTextureReady(mesh.material.occlusion))  { it.occlusion  = rm->Resolve(mesh.material.occlusion).gpuHandle;  it.mapFlags |= 4u; }
    // NOTE: normal mapping is NOT enabled on the skinned path yet -- SkinnedVertex3D carries no tangent, so
    // bit3 stays clear (the shared PS then skips the TBN branch). it.normal keeps the albedo filler so the
    // t4 slot is always a valid bound texture. Skinned normal mapping = the follow-up (add a tangent to
    // SkinnedVertex3D + kSkinnedVertex3DLayout + Skinned3D_VS, mirroring the static path).
    m_SkinnedItems.push_back(std::move(it));
}

// The b4 material root constants -- layout MUST match the Material cbuffer in Basic3D_PS.hlsl EXACTLY
// (12 DWORDs = 3 float4 rows). flags: bit0 = metalRough tex present, bit1 = emissive, bit2 = occlusion.
struct MatConsts {
    float    baseColor[3]; float metallic;
    float    emissive[3];  float roughness;
    uint32_t flags;        float pad[3];
};

// Instanced record loop. Assumes the caller bound PSO / root sig / camera CBV / render targets AND the
// per-frame instance buffer at vertex slot 1 (this function never touches slot 1, so batches recorded on
// different worker lists can't disturb each other's instance range). Per batch: bind the mesh's maps +
// per-vertex buffer (slot 0) + indices, then ONE DrawIndexedInstanced drawing `count` copies, each fed
// its model matrix from the instance buffer at StartInstanceLocation = firstInstance (+ SV_InstanceID).
// Model matrices are no longer root constants -- they ride the instance stream now (see Basic3D_VS).
void Renderer3D::recordBatches(ID3D12GraphicsCommandList* cmd, size_t startBatch, size_t endBatch) {
    bool logged = false;   // one line per worker per frame if a batch has a null binding
    for (size_t b = startBatch; b < endBatch; ++b) {
        const InstanceBatch& batch = m_Batches[b];
        if (!logged && (batch.albedo.ptr == 0 ||
                        batch.mesh->vertexBufferView.BufferLocation == 0 ||
                        batch.mesh->indexBufferView.BufferLocation == 0 ||
                        batch.mesh->indexCount == 0)) {
            char b2[224];
            sprintf_s(b2, "[recordBatches] *** NULL BINDING batch %zu: albedo.ptr=%llu VBV=0x%llX IBV=0x%llX idxCount=%u count=%u ***\n",
                b, (unsigned long long)batch.albedo.ptr,
                (unsigned long long)batch.mesh->vertexBufferView.BufferLocation,
                (unsigned long long)batch.mesh->indexBufferView.BufferLocation, batch.mesh->indexCount, batch.count);
            OutputDebugStringA(b2);
            logged = true;
        }
        // b4 material constants -- MUST match the Material cbuffer in Basic3D_PS.hlsl (12 DWORDs).
        const Material& m = batch.mesh->material;
        const MatConsts mc = { { m.baseColorFactor.x, m.baseColorFactor.y, m.baseColorFactor.z }, m.metallic,
                               { m.emissiveFactor.x,  m.emissiveFactor.y,  m.emissiveFactor.z },  m.roughness,
                               batch.mapFlags, { 0.0f, 0.0f, 0.0f } };
        cmd->SetGraphicsRoot32BitConstants(4, 12, &mc, 0);             // b4 = material (factors + map bitmask)
        cmd->SetGraphicsRootDescriptorTable(2, batch.albedo);          // t0 = base color (pre-resolved at Submit)
        cmd->SetGraphicsRootDescriptorTable(5, batch.metalRough);      // t1 = metallic-roughness
        cmd->SetGraphicsRootDescriptorTable(6, batch.emissive);        // t2 = emissive
        cmd->SetGraphicsRootDescriptorTable(7, batch.occlusion);       // t3 = occlusion
        cmd->SetGraphicsRootDescriptorTable(8, batch.normal);          // t4 = normal map
        cmd->IASetVertexBuffers(0, 1, &batch.mesh->vertexBufferView);  // slot 0 = per-vertex mesh data
        cmd->IASetIndexBuffer(&batch.mesh->indexBufferView);
        cmd->DrawIndexedInstanced(batch.mesh->indexCount, batch.count, 0, 0, batch.firstInstance);
    }
}

// Skinned peer of recordItems. Caller has already bound the skinned PSO + root sig, the camera CBV
// (b0), and the bone palette CBV (b2). Per object: push the Model matrix (b1) + albedo table (t0 =
// root param 3, NOT 2 -- the skinned root sig inserts the bone CBV at 2), then the indexed draw.
void Renderer3D::recordSkinnedItems(ID3D12GraphicsCommandList* cmd, size_t start, size_t end, int frame) {
    const UINT perInstanceBytes = kMaxBones * (UINT)sizeof(DirectX::XMFLOAT4X4);
    for (size_t i = start; i < end; ++i) {
        if (i >= kMaxSkinnedInstances) break;   // out of bone-CB regions; extra skinned draws are dropped
        const SkinnedItem& it = m_SkinnedItems[i];

        // Upload THIS instance's palette into its own CB region, then point b2 at that region. Slots
        // beyond the palette length keep the Initialize identity fill (boneIDs never exceed joint count).
        uint8_t* dst = reinterpret_cast<uint8_t*>(m_BoneCBMapped[frame]) + (size_t)i * perInstanceBytes;
        size_t count = it.palette.size() < kMaxBones ? it.palette.size() : kMaxBones;
        std::memcpy(dst, it.palette.data(), count * sizeof(DirectX::XMFLOAT4X4));
        cmd->SetGraphicsRootConstantBufferView(2,
            m_BoneCB[frame]->GetGPUVirtualAddress() + (UINT64)i * perInstanceBytes);   // b2 = this pose

        cmd->SetGraphicsRoot32BitConstants(1, 16, &it.model, 0);
        const Material& m = it.mesh->material;
        const MatConsts mc = { { m.baseColorFactor.x, m.baseColorFactor.y, m.baseColorFactor.z }, m.metallic,
                               { m.emissiveFactor.x,  m.emissiveFactor.y,  m.emissiveFactor.z },  m.roughness,
                               it.mapFlags, { 0.0f, 0.0f, 0.0f } };
        cmd->SetGraphicsRoot32BitConstants(5, 12, &mc, 0);   // b4 = material (factors + map bitmask)
        cmd->SetGraphicsRootDescriptorTable(3, it.albedo);   // t0 = base color (pre-resolved at SubmitSkinned)
        cmd->SetGraphicsRootDescriptorTable(6, it.metalRough); // t1
        cmd->SetGraphicsRootDescriptorTable(7, it.emissive);   // t2
        cmd->SetGraphicsRootDescriptorTable(8, it.occlusion);  // t3
        cmd->SetGraphicsRootDescriptorTable(9, it.normal);     // t4 = normal map
        cmd->IASetVertexBuffers(0, 1, &it.mesh->vertexBufferView);
        cmd->IASetIndexBuffer(&it.mesh->indexBufferView);
        cmd->DrawIndexedInstanced(it.mesh->indexCount, 1, 0, 0, 0);
    }
}

void Renderer3D::RecordCommandList(int frame,
                                   D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                   D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                   uint32_t width, uint32_t height) {
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    m_RecordedLists.clear();

    // ---- HDR: swap the render target out from under every pass below -------------------------
    // RendererCore hands us the BACK BUFFER's RTV. Nothing in the 3D renderer draws there any more;
    // every pass writes linear HDR into an FP16 intermediate, and the tonemap pass at the bottom of
    // this function resolves that onto the back buffer. Reassigning the by-value `rtv` parameter is
    // how that redirect reaches the sky, geometry, skinned and particle passes without threading a
    // second handle through all of them -- `backRtv` keeps the real one for the resolve.
    //
    // Created lazily on first use (needs the client size, and reaches the shared SRV heap through
    // Renderer2D, which is not guaranteed attached at Initialize) and rebuilt on resize. Safe to
    // drop the old texture without flushing, for the same reason SSAO is: the only way the size
    // changes is RendererCore::Resize, which has already waited on every in-flight frame.
    const D3D12_CPU_DESCRIPTOR_HANDLE backRtv = rtv;
    if (!m_HdrTarget || m_HdrWidth != width || m_HdrHeight != height) {
        m_HdrTarget.Reset();
        if (!CreateHdrTarget(width, height)) {
            // Logged inside; no 3D this frame. Still drain the queues -- Submit() runs every frame
            // whether or not we render, so leaving them would grow without bound.
            m_Items.clear();
            m_SkinnedItems.clear();
            m_CulledThisFrame = 0;
            return;
        }
    }
    rtv = m_HdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
    // Pushed FIRST so the clear lands ahead of the sky/geometry lists.
    RecordHdrBegin(frame);
    m_RecordedLists.push_back(m_HdrBeginList[frame].Get());

    // Auto-benchmark: alternate serial/parallel every frame so BOTH modes get timed at the same scene
    // complexity (see SetAutoBenchmark). Overrides the manual P toggle while active.
    if (m_AutoBench) {
        m_ParallelRecording = m_BenchUseParallel;
        m_BenchUseParallel  = !m_BenchUseParallel;   // flip for next frame
    }

    // Upload this frame's camera ONCE -- every worker list just binds its GPUVA at b0.
    // Camera b0: ViewProj at offset 0, camPos at 64 -- matches the cbuffer Camera layout in the shaders.
    uint8_t* camDst = reinterpret_cast<uint8_t*>(m_CameraCBMapped[frame]);
    memcpy(camDst, &m_ViewProj, sizeof(m_ViewProj));
    memcpy(camDst + sizeof(m_ViewProj), &m_CamPos, sizeof(m_CamPos));
    // Lights b3: ambient + count header, then the live lights (mirrors GpuLightsCB / the PS cbuffer).
    {
        GpuLightsCB* ldst = reinterpret_cast<GpuLightsCB*>(m_LightsCBMapped[frame]);
        ldst->ambient       = m_Ambient;
        ldst->count         = m_LightCount;
        ldst->ambientGround = m_AmbientGround;
        ldst->hemiMix       = m_HemiMix;
        // ssaoOn gates the 9 blur taps in the pixel shader, so it must reflect whether the AO texture
        // actually exists this frame -- not merely whether the user asked for SSAO.
        const bool ssaoLive = m_SsaoEnabled && m_SsaoTarget && m_SsaoWidth && m_SsaoHeight;
        ldst->ssaoTexel = { ssaoLive ? 1.0f / (float)m_SsaoWidth  : 0.0f,
                            ssaoLive ? 1.0f / (float)m_SsaoHeight : 0.0f };
        ldst->ssaoOn    = ssaoLive ? 1.0f : 0.0f;
        // IBL is all-or-nothing: without a baked environment the shader takes the hemisphere path.
        ldst->iblOn       = m_EnvReady ? 1.0f : 0.0f;
        ldst->iblMipCount = (float)m_PrefilterMips;
        // Live only when the gather actually produced a target this frame, not merely when asked.
        // 0 = off, 1 = composited into ambient, 2 = DEBUG (bounce term alone; see Basic3D_PS).
        ldst->giOn = (m_SsgiEnabled && m_SsgiTarget) ? (m_SsgiDebug ? 2.0f : 1.0f) : 0.0f;
        ldst->envIntensity = m_EnvIntensity;
        const uint32_t n = (m_LightCount < kMaxLights) ? m_LightCount : kMaxLights;
        memcpy(ldst->lights, m_Lights, sizeof(GpuLight) * n);

        // Pick the caster. Explicit override wins; otherwise prefer a DIRECTIONAL light (type 0) and
        // fall back to a SPOT (type 2). POINT lights are skipped: omnidirectional casting needs six
        // passes into a cube map, so a point light stays unshadowed rather than being handed a single
        // matrix that can't describe it.
        m_ShadowLightIndex = -1;
        if (m_ShadowsEnabled && m_ShadowMap) {
            if (m_ShadowCasterOverride >= 0 && (uint32_t)m_ShadowCasterOverride < n &&
                m_Lights[m_ShadowCasterOverride].type < 0.5f) {
                m_ShadowLightIndex = m_ShadowCasterOverride;            // pinned directional
            } else if (m_ShadowCasterOverride >= 0 && (uint32_t)m_ShadowCasterOverride < n &&
                       m_Lights[m_ShadowCasterOverride].type > 1.5f && m_Lights[m_ShadowCasterOverride].type < 2.5f) {
                m_ShadowLightIndex = m_ShadowCasterOverride;            // pinned spot
            } else if (m_ShadowCasterOverride >= 0 && (uint32_t)m_ShadowCasterOverride < n) {
                m_ShadowLightIndex = m_ShadowCasterOverride;            // pinned point light
            } else if (m_ShadowCasterOverride < 0) {
                for (uint32_t i = 0; i < n && m_ShadowLightIndex < 0; ++i)
                    if (m_Lights[i].type < 0.5f) m_ShadowLightIndex = (int32_t)i;   // directional first
                for (uint32_t i = 0; i < n && m_ShadowLightIndex < 0; ++i)
                    if (m_Lights[i].type > 1.5f && m_Lights[i].type < 2.5f) m_ShadowLightIndex = (int32_t)i;   // else a spot (type 3 = area: never casts)
                // A POINT light is never auto-picked: it costs six passes instead of one, so opting
                // into that has to be deliberate (SetShadowCaster with its index).
            }
            // A point caster needs the cube resources to exist; fall back to no shadow if they don't.
            if (m_ShadowLightIndex >= 0 && m_Lights[m_ShadowLightIndex].type > 0.5f &&
                m_Lights[m_ShadowLightIndex].type < 1.5f && !m_CubeShadowMap) {
                m_ShadowLightIndex = -1;
            }
        }
        m_ShadowIsPoint = (m_ShadowLightIndex >= 0) &&
                          (m_Lights[m_ShadowLightIndex].type > 0.5f) &&
                          (m_Lights[m_ShadowLightIndex].type < 1.5f);

        if (m_ShadowLightIndex >= 0 && m_ShadowIsPoint) {
            // Point caster: six matrices, built per-face inside RecordCubeShadowPass. Nothing to do
            // here except leave m_LightViewProj alone -- the cube path doesn't use it.
        } else if (m_ShadowLightIndex >= 0) {
            const GpuLight& L = m_Lights[m_ShadowLightIndex];
            DirectX::XMVECTOR dir = DirectX::XMVector3Normalize(
                DirectX::XMVectorSet(L.direction.x, L.direction.y, L.direction.z, 0.0f));
            // Up must not be parallel to the light direction or the look-at matrix degenerates -- a
            // straight-down light is the common case that hits this.
            DirectX::XMVECTOR up = (fabsf(L.direction.y) > 0.99f)
                ? DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
                : DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

            DirectX::XMMATRIX view, proj;
            if (L.type > 1.5f && L.type < 2.5f) {
                // SPOT: a real viewpoint, so this is an ordinary perspective camera sitting at the
                // light and looking down its cone. FOV is the FULL outer cone angle (the stored value
                // is the cosine of the half-angle), widened slightly so the shadow doesn't get clipped
                // right at the cone edge where the light is still faintly contributing.
                DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&L.position);
                DirectX::XMVECTOR target = DirectX::XMVectorAdd(eye, dir);
                view = DirectX::XMMatrixLookAtLH(eye, target, up);
                float halfAngle = acosf(L.spotCosOuter < -1.0f ? -1.0f
                                      : (L.spotCosOuter > 1.0f ? 1.0f : L.spotCosOuter));
                float fov = halfAngle * 2.0f * 1.1f;
                if (fov > 3.0f) fov = 3.0f;              // clamp under pi -- a >180 deg cone isn't a spot
                // NEAR PLANE AS FAR OUT AS POSSIBLE. A perspective projection spends most of its
                // depth precision between near and ~10x near, so a tiny near plane leaves almost no
                // resolution at the far end -- which is exactly where perspective casters show acne.
                // Pushing near from 0.1 to ~2% of range is a several-fold precision gain at distance,
                // and costs nothing as long as no geometry sits closer to the lamp than that.
                const float farP  = L.range > 1.0f ? L.range : 1.0f;
                const float nearP = (farP * 0.02f > 0.5f) ? farP * 0.02f : 0.5f;
                proj = DirectX::XMMatrixPerspectiveFovLH(fov, 1.0f, nearP, farP);
                m_ShadowNear = nearP;
                m_ShadowFar  = farP;
            } else {
                // DIRECTIONAL: parallel rays, so there's no viewpoint to project from -- only a
                // direction and the box we choose to cover (SetShadowBounds). Pull the eye back along
                // the direction far enough that casters OUTSIDE the box still occlude things inside it.
                DirectX::XMVECTOR center = DirectX::XMLoadFloat3(&m_ShadowCenter);
                const float back = m_ShadowExtent * 2.0f;
                DirectX::XMVECTOR eye = DirectX::XMVectorSubtract(center, DirectX::XMVectorScale(dir, back));
                view = DirectX::XMMatrixLookAtLH(eye, center, up);
                proj = DirectX::XMMatrixOrthographicLH(
                    m_ShadowExtent * 2.0f, m_ShadowExtent * 2.0f, 0.1f, back + m_ShadowExtent * 2.0f);
            }
            DirectX::XMStoreFloat4x4(&m_LightViewProj, view * proj);
            memcpy(m_ShadowCBMapped[frame], &m_LightViewProj, sizeof(m_LightViewProj));
        }

        ldst->lightViewProj = m_LightViewProj;
        ldst->shadowIndex   = m_ShadowLightIndex;
        ldst->shadowTexel   = 1.0f / (float)kShadowMapSize;
        // How big one shadow texel is in WORLD units -- the natural scale for the normal offset,
        // because self-shadowing artifacts are exactly a texel-sized problem. For a directional
        // caster that's the ortho box divided by the resolution. A spot's footprint grows with
        // distance, so use a mid-range estimate from its cone rather than a per-pixel calculation.
        if (m_ShadowIsPoint) {
            // A cube face is a 90-degree frustum, so its footprint at distance d is 2d wide. Use
            // mid-range as the representative distance, same idea as the spot case.
            const GpuLight& L = m_Lights[m_ShadowLightIndex];
            ldst->shadowNormalOffset = (L.range) / (float)kCubeShadowSize;
        } else if (m_ShadowLightIndex >= 0 && m_Lights[m_ShadowLightIndex].type > 1.5f && m_Lights[m_ShadowLightIndex].type < 2.5f) {
            const GpuLight& L = m_Lights[m_ShadowLightIndex];
            float half = acosf(L.spotCosOuter < -1.0f ? -1.0f
                             : (L.spotCosOuter > 1.0f ? 1.0f : L.spotCosOuter));
            float midWidth = 2.0f * tanf(half) * (L.range * 0.5f);   // cone width halfway out
            ldst->shadowNormalOffset = midWidth / (float)kShadowMapSize;
        } else {
            ldst->shadowNormalOffset = (m_ShadowExtent * 2.0f) / (float)kShadowMapSize;
        }
        ldst->shadowIsPoint = m_ShadowIsPoint ? 1 : 0;
        ldst->shadowLightPos = (m_ShadowLightIndex >= 0) ? m_Lights[m_ShadowLightIndex].position
                                                         : DirectX::XMFLOAT3{ 0, 0, 0 };
        ldst->shadowFar  = m_ShadowFar;
        ldst->shadowNear = m_ShadowNear;
    }


    // ---- sky pass: recorded before the geometry lists (right after the HDR clear) so it draws behind them ----
    // Upload InvViewProj (to unproject view rays in the sky VS) + camPos, then record the fullscreen pass.
    if (m_SkyEnabled) {
        DirectX::XMFLOAT4X4 invVP;
        DirectX::XMStoreFloat4x4(&invVP, DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&m_ViewProj)));
        uint8_t* sdst = reinterpret_cast<uint8_t*>(m_SkyCBMapped[frame]);
        memcpy(sdst, &invVP, sizeof(invVP));                        // InvViewProj at offset 0
        memcpy(sdst + sizeof(invVP), &m_CamPos, sizeof(m_CamPos));  // camPos at offset 64 (matches SkyCB)
        RecordSkybox(frame, rtv, width, height);
        m_RecordedLists.push_back(m_SkyList[frame].Get());          // index 0 -> executed before the world
    }

    Renderer2D*           r2d     = m_Core->GetRenderer2D();
    ID3D12DescriptorHeap* srvHeap = r2d->GetSrvHeap();
    ResourceManager*      rm      = r2d->GetResourceManager();

    // ---- build instance batches from this frame's queued objects ----
    // Sort m_Items by (mesh, albedo) so identical objects are adjacent, write their matrices into this
    // frame's instance buffer in that order, and collapse each contiguous run into ONE InstanceBatch.
    // The whole 16k-cube grid becomes a single batch -> a single DrawIndexedInstanced. This grouping +
    // 1MB-ish memcpy is the only serial work; the actual draw recording still splits across workers below.
    m_Batches.clear();
    if (!m_Items.empty()) {
        if (m_Items.size() > kMaxInstances) m_Items.resize(kMaxInstances);   // cap: excess dropped this frame
        std::sort(m_Items.begin(), m_Items.end(), [](const DrawItem& a, const DrawItem& b) {
            if (a.mesh != b.mesh) return a.mesh < b.mesh;
            return a.albedo.ptr < b.albedo.ptr;
        });
        auto* inst = reinterpret_cast<DirectX::XMFLOAT4X4*>(m_InstanceMapped[frame]);
        for (size_t i = 0; i < m_Items.size(); ++i) {
            inst[i] = m_Items[i].model;   // matrices in the SAME order the batches' firstInstance indexes
            if (m_Batches.empty() ||
                m_Batches.back().mesh != m_Items[i].mesh ||
                m_Batches.back().albedo.ptr != m_Items[i].albedo.ptr) {
                // Material maps are per-mesh -> same for every item in the run; copy them from this first item.
                const DrawItem& it = m_Items[i];
                m_Batches.push_back(InstanceBatch{ it.mesh, it.albedo, it.metalRough, it.emissive,
                                                   it.occlusion, it.normal, it.mapFlags, (uint32_t)i, 1 });
            } else {
                m_Batches.back().count++;
            }
        }
    }

    // ---- shadow depth pass. Recorded HERE because it replays m_Batches, which only exists after the
    // grouping above. Pushed ahead of the geometry lists so the GPU fills the depth map before any
    // pixel shader samples it -- the resource barrier inside the pass makes that ordering a hard
    // dependency rather than a hope. ----
    // ---- SSAO, ahead of everything: the geometry pass samples its output, and the depth prepass it
    // depends on has to have run first. Created lazily on first use (it needs the back-buffer size,
    // and a scene that never enables SSAO should not pay for a full-screen depth + AO target), and
    // rebuilt whenever the window size changes out from under it. ----
    // SSAO and SSGI SHARE the camera depth prepass, so this list runs when EITHER is on. The
    // resources are still created independently -- a scene wanting only one does not pay for the
    // other's targets.
    if ((m_SsaoEnabled || m_SsgiEnabled) && !m_Batches.empty()) {
        if (!m_SsaoTarget || m_SsaoWidth != width || m_SsaoHeight != height) {
            // Safe to drop the old textures without flushing here: the only way width/height change
            // is through RendererCore::Resize, which has already waited for every in-flight frame
            // before the size it reports differs from ours.
            m_SsaoDepth.Reset(); m_SsaoTarget.Reset();
            CreateSsaoResources(width, height);
        }
        if (m_SsgiEnabled && (!m_SsgiTarget || m_SsgiWidth != width || m_SsgiHeight != height))
            if (!CreateSsgiTargets(width, height)) m_SsgiEnabled = false;   // logged inside
        if (m_SsaoTarget) {   // the prepass lives on this list, so it needs the depth resources
            RecordSsaoPass(frame);
            m_RecordedLists.push_back(m_SsaoList[frame].Get());
        }
    }

    if (m_ShadowLightIndex >= 0 && !m_Batches.empty()) {
        if (m_ShadowIsPoint) {
            // Six faces, recorded in parallel on the scheduler; pushed in face order so the
            // bracketing resource barriers (face 0 opens, face 5 closes) execute correctly.
            RecordCubeShadowPass(frame);
            for (int f = 0; f < 6; ++f) m_RecordedLists.push_back(m_CubeList[frame][f].Get());
        } else {
            RecordShadowPass(frame);
            m_RecordedLists.push_back(m_ShadowList[frame].Get());
        }
    }

    // AUTO-SELECT serial vs parallel from the measured crossover. Below kParallelBatchThreshold the
    // parallel dispatch overhead outweighs the recording work, so serial wins; above it, splitting wins.
    // The benchmark overrides this to force whichever mode it's timing this frame; otherwise we record the
    // decision back into m_ParallelRecording so the HUD/getter reflects what actually ran.
    bool goParallel;
    if (m_AutoBench) {
        goParallel = m_ParallelRecording;                          // benchmark forces the mode
    } else {
        goParallel = m_Batches.size() >= kParallelBatchThreshold;  // automatic policy
        m_ParallelRecording = goParallel;                          // reflect it for the HUD
    }
    goParallel = goParallel && m_Workers.size() > 1;

    // ---- static pass: split the BATCHES across workers (parallel) or one list (serial) ----
    if (!m_Batches.empty()) {
        if (goParallel) {
            // Scale worker count to the batch count -- do NOT blindly split across all hw-1 workers. Each
            // worker is a separate command list (Reset/Close/Execute) + dispatch + sync; fanning out for a
            // few cheap instanced batches is pure overhead (this is the same over-dispatch that cost the 2D
            // pass ~2ms). ~1 worker per kBatchesPerWorker batches, capped at the pool. Makes the parallel
            // path a FAIR comparison against serial instead of a rigged one. kBatchesPerWorker is a knob.
            constexpr size_t kBatchesPerWorker = 64;
            const int    active = (int)std::min(m_Workers.size(), (size_t)1 + m_Batches.size() / kBatchesPerWorker);
            const size_t chunk  = (m_Batches.size() + active - 1) / active;   // ceil

            // Build ALL task contexts first. reserve() => no reallocation => the &c pointers handed to
            // the scheduler stay valid until WaitFor. Then dispatch one task per context.
            m_RecordCtxs.clear();
            m_RecordCtxs.reserve(active);
            for (int t = 0; t < active; ++t) {
                size_t start = (size_t)t * chunk;
                if (start >= m_Batches.size()) break;
                size_t end = std::min(start + chunk, m_Batches.size());
                m_RecordCtxs.push_back(RecordTaskCtx{
                    this, m_Workers[t].list[frame].Get(), m_Workers[t].alloc[frame].Get(),
                    start, end, frame, rtv, dsv, width, height, srvHeap, rm });
            }

            auto& sched = JLib::TaskScheduler::Instance();
            JLib::WaitGroup wg; wg.n.store(0, std::memory_order_relaxed);
            for (auto& c : m_RecordCtxs) {
                auto* task = sched.CreateTask(&Renderer3D::RecordRangeTask, &c, true);
                task->waitGroup = &wg;
                wg.n.fetch_add(1, std::memory_order_release);
                sched.Push(task);
                m_RecordedLists.push_back(c.list);
            }
            sched.WaitFor(wg);   // blocks until every worker list is recorded + Closed
        } else {
            RecordTaskCtx c{ this, m_DrawList[frame].Get(), m_Allocators[frame].Get(),
                             0, m_Batches.size(), frame, rtv, dsv, width, height, srvHeap, rm };
            RecordRange(c);
            m_RecordedLists.push_back(m_DrawList[frame].Get());
        }
    }

    // ---- skinned pass: always serial, its own list (executes after the static lists) ----
    if (!m_SkinnedItems.empty()) {
        RecordSkinnedList(frame, rtv, dsv, width, height, srvHeap);
        m_RecordedLists.push_back(m_SkinnedList[frame].Get());
    }

    // ---- 3D particles: compute update + billboard draw, recorded LAST so it depth-tests against the
    // geometry the earlier lists wrote. Camera right/up for the billboards come from InvViewProj. ----
    if (m_ParticlesEnabled && !m_Emitters.empty()) {
        using namespace DirectX;
        XMMATRIX invVP = XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_ViewProj));
        XMVECTOR o  = XMVector3TransformCoord(XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f), invVP);
        XMVECTOR rx = XMVector3TransformCoord(XMVectorSet(1.0f, 0.0f, 0.0f, 1.0f), invVP);
        XMVECTOR uy = XMVector3TransformCoord(XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f), invVP);
        ParticleCamCB pc;
        pc.viewProj = m_ViewProj;
        XMStoreFloat3(&pc.camRight, XMVector3Normalize(XMVectorSubtract(rx, o)));
        XMStoreFloat3(&pc.camUp,    XMVector3Normalize(XMVectorSubtract(uy, o)));
        memcpy(m_ParticleCamCBMapped[frame], &pc, sizeof(pc));
        RecordParticles(frame, rtv, dsv, width, height);
        m_RecordedLists.push_back(m_ParticleList[frame].Get());
    }

    // ---- bloom: after everything that writes the scene, before the tonemap that consumes it. It
    // reads the finished FP16 target and leaves bloom[0] holding the accumulated chain, which the
    // tonemap pass adds in BEFORE its curve. Targets are lazy + rebuilt on resize, like the HDR
    // target itself; if they can't be created the pass is skipped and intensity 0 disables it. ----
    if (m_BloomEnabled) {
        if (m_BloomLevels == 0 || m_BloomW[0] != (width >> 1) || m_BloomH[0] != (height >> 1))
            CreateBloomTargets(width, height);
        if (m_BloomLevels > 0) {
            RecordBloomPass(frame);
            m_RecordedLists.push_back(m_BloomList[frame].Get());
        }
    }

    // ---- tonemap: LAST of the 3D lists. Reads the finished FP16 scene, maps it to display range and
    // writes the back buffer through its _SRGB view (the one and only gamma encode in the pipeline).
    // RendererCore submits the 2D layers, 2D particles and ImGui after these lists, so all of that
    // composites on top of the resolved image and is NOT tonemapped -- which is what keeps every
    // existing 2D demo pixel-identical. ----
    // With FXAA on, the tonemap pass writes the LDR INTERMEDIATE instead of the back buffer, and the
    // FXAA pass resolves that to the back buffer. FXAA needs a finished gamma-encoded image to find
    // edges in, and a pass cannot read and write the same target -- so the intermediate is forced by
    // the ordering, not a preference. Both PSOs use BackBufferRTVFormat, so the tonemap PSO is valid
    // against either destination and no second pipeline is needed.
    bool fxaaLive = m_FxaaEnabled;
    if (fxaaLive && (!m_LdrTarget || m_LdrWidth != width || m_LdrHeight != height))
        fxaaLive = CreateLdrTarget(width, height);

    RecordTonemapPass(frame,
                      fxaaLive ? m_LdrRtvHeap->GetCPUDescriptorHandleForHeapStart() : backRtv,
                      width, height);
    m_RecordedLists.push_back(m_TonemapList[frame].Get());

    if (fxaaLive) {
        RecordFxaaPass(frame, backRtv, width, height);
        m_RecordedLists.push_back(m_FxaaList[frame].Get());
    }

    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    m_LastRecordMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / m_QpcFreq;

    // Frame stats, captured BEFORE the clear: real draw calls (batches) + objects drawn (instances).
    const size_t prevBatchCount = m_LastBatchCount;
    m_LastBatchCount    = m_Batches.size();
    m_LastInstanceCount = m_Items.size();
    m_LastCulledCount   = m_CulledThisFrame;   // how many Submits the frustum rejected this frame
    m_CulledThisFrame   = 0;                    // reset for the next frame's Submits
    // Auto-benchmark: fold this frame's record time into the running EMA for whichever mode just ran.
    // If the batch count changed (user swept the knob), RESET both averages so each level reads clean.
    if (m_AutoBench) {
        if (m_Batches.size() != prevBatchCount) { m_SerialAvgMs = 0.0; m_ParallelAvgMs = 0.0; }
        double& avg = m_ParallelRecording ? m_ParallelAvgMs : m_SerialAvgMs;
        avg = (avg == 0.0) ? m_LastRecordMs : (avg * 0.9 + m_LastRecordMs * 0.1);
    }

    // Stash the camera for next frame's temporal reprojection. Must happen AFTER every pass that
    // used m_ViewProj, or the denoise would reproject into the frame it is already rendering.
    m_PrevViewProj = m_ViewProj;

    m_Items.clear();          // consumed this frame; the app re-submits next frame
    m_SkinnedItems.clear();
}

// Reset+bind ONE static-pass list and record its slice of m_Batches [c.start, c.end). Called directly
// (serial) or from a scheduler task (parallel). Each task reads a DISJOINT range of batches and binds the
// shared (read-only) instance buffer + camera, so concurrent workers never touch shared mutable state.
void Renderer3D::RecordRange(const RecordTaskCtx& c) {
    c.alloc->Reset();
    c.list->Reset(c.alloc, m_PSO.Get());
    c.list->OMSetRenderTargets(1, &c.rtv, FALSE, &c.dsv);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)c.width, (float)c.height, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)c.width, (LONG)c.height };
    c.list->RSSetViewports(1, &vp);
    c.list->RSSetScissorRects(1, &sc);
    ID3D12DescriptorHeap* heaps[] = { c.srvHeap };
    c.list->SetDescriptorHeaps(1, heaps);
    c.list->SetGraphicsRootSignature(m_RootSig.Get());
    c.list->SetGraphicsRootConstantBufferView(0, m_CameraCB[c.frame]->GetGPUVirtualAddress());   // b0 camera
    c.list->SetGraphicsRootConstantBufferView(3, m_LightsCB[c.frame]->GetGPUVirtualAddress());   // b3 lights (per frame)
    // t5 = shadow map, bound per frame (not per batch -- every draw samples the same map). Bound even
    // when shadows are off: the root signature declares the table, and D3D12 requires every declared
    // descriptor table to be populated before a draw, regardless of whether the shader reads it.
    if (m_ShadowSrvGpu.ptr) c.list->SetGraphicsRootDescriptorTable(9, m_ShadowSrvGpu);
    if (m_CubeSrvGpu.ptr)   c.list->SetGraphicsRootDescriptorTable(10, m_CubeSrvGpu);   // t6
    // t7 = SSAO. Same "must be populated" rule as above, so when SSAO has never been enabled (its
    // textures don't exist) the shadow map stands in as a harmless placeholder -- gSsaoOn is 0 in that
    // case, so the shader never samples it and only the binding requirement is being satisfied.
    {
        D3D12_GPU_DESCRIPTOR_HANDLE ssaoSrv = m_SsaoSrvGpu.ptr ? m_SsaoSrvGpu : m_ShadowSrvGpu;
        if (ssaoSrv.ptr) c.list->SetGraphicsRootDescriptorTable(11, ssaoSrv);
        // t8+t9 = IBL cubes. Falls back to the shadow CUBE (not the 2D map) when no environment is
        // loaded -- the shader declares TextureCube there, and a table's descriptors must match the
        // declared dimension even when gIblOn is 0 and nothing ever samples them.
        D3D12_GPU_DESCRIPTOR_HANDLE iblSrv = m_IrradianceSrvGpu.ptr ? m_IrradianceSrvGpu : m_CubeSrvGpu;
        if (iblSrv.ptr) c.list->SetGraphicsRootDescriptorTable(12, iblSrv);
        // t10: SSGI bounce. Falls back to the SSAO target (a valid 2D SRV) when GI is off -- the
        // table must be populated regardless, since the shader branches past it on gGiOn rather
        // than the binding being absent.
        // The DENOISED target, not the raw gather.
        D3D12_GPU_DESCRIPTOR_HANDLE giSrv = m_SsgiBlurSrvGpu[m_SsgiBlurIndex].ptr ? m_SsgiBlurSrvGpu[m_SsgiBlurIndex]
                                          : (m_SsaoSrvGpu.ptr ? m_SsaoSrvGpu : m_ShadowSrvGpu);
        if (giSrv.ptr) c.list->SetGraphicsRootDescriptorTable(13, giSrv);
    }
    c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Slot 1 = this frame's instance buffer (whole thing). recordBatches selects each batch's slice via
    // StartInstanceLocation, so every worker binds the same buffer here and never re-binds slot 1.
    D3D12_VERTEX_BUFFER_VIEW instVBV{};
    instVBV.BufferLocation = m_InstanceBuffer[c.frame]->GetGPUVirtualAddress();
    instVBV.StrideInBytes  = sizeof(DirectX::XMFLOAT4X4);
    instVBV.SizeInBytes    = kMaxInstances * (UINT)sizeof(DirectX::XMFLOAT4X4);
    c.list->IASetVertexBuffers(1, 1, &instVBV);
    // Frame-level 3D binding sanity (device-hung VA=0 hunt): camera CBV address + the shared SRV heap.
    if (m_CameraCB[c.frame]->GetGPUVirtualAddress() == 0 || c.srvHeap == nullptr) {
        char b[160];
        sprintf_s(b, "[RecordRange] *** cameraCBV_VA=0x%llX srvHeap=%p (frame=%d) ***\n",
            (unsigned long long)m_CameraCB[c.frame]->GetGPUVirtualAddress(), (void*)c.srvHeap, c.frame);
        OutputDebugStringA(b);
    }
    recordBatches(c.list, c.start, c.end);   // c.start/c.end are BATCH indices now
    ThrowIfFailed(c.list->Close());
}

void Renderer3D::RecordRangeTask(void* p) {
    RecordTaskCtx* c = static_cast<RecordTaskCtx*>(p);
    c->self->RecordRange(*c);
}

void Renderer3D::RecordSkinnedList(int frame, D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                   uint32_t width, uint32_t height, ID3D12DescriptorHeap* srvHeap) {
    m_SkinnedAllocators[frame]->Reset();
    m_SkinnedList[frame]->Reset(m_SkinnedAllocators[frame].Get(), m_SkinnedPSO.Get());
    m_SkinnedList[frame]->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)width, (LONG)height };
    m_SkinnedList[frame]->RSSetViewports(1, &vp);
    m_SkinnedList[frame]->RSSetScissorRects(1, &sc);
    ID3D12DescriptorHeap* heaps[] = { srvHeap };
    m_SkinnedList[frame]->SetDescriptorHeaps(1, heaps);
    m_SkinnedList[frame]->SetGraphicsRootSignature(m_SkinnedRootSig.Get());
    m_SkinnedList[frame]->SetGraphicsRootConstantBufferView(0, m_CameraCB[frame]->GetGPUVirtualAddress()); // b0 camera
    m_SkinnedList[frame]->SetGraphicsRootConstantBufferView(4, m_LightsCB[frame]->GetGPUVirtualAddress()); // b3 lights (param 4)
    // t5 shadow map (param 10 in the skinned sig -- it inserts the bone CBV at 2, so every later
    // index is one higher than the static signature's).
    if (m_ShadowSrvGpu.ptr) m_SkinnedList[frame]->SetGraphicsRootDescriptorTable(10, m_ShadowSrvGpu);
    if (m_CubeSrvGpu.ptr)   m_SkinnedList[frame]->SetGraphicsRootDescriptorTable(11, m_CubeSrvGpu);   // t6
    {   // t7 = SSAO (param 12 here, one higher than the static signature -- the bone CBV at 2 shifts
        // everything after it). Falls back to the shadow map as a placeholder, same as the static path.
        D3D12_GPU_DESCRIPTOR_HANDLE ssaoSrv = m_SsaoSrvGpu.ptr ? m_SsaoSrvGpu : m_ShadowSrvGpu;
        if (ssaoSrv.ptr) m_SkinnedList[frame]->SetGraphicsRootDescriptorTable(12, ssaoSrv);
        D3D12_GPU_DESCRIPTOR_HANDLE iblSrv = m_IrradianceSrvGpu.ptr ? m_IrradianceSrvGpu : m_CubeSrvGpu;
        if (iblSrv.ptr) m_SkinnedList[frame]->SetGraphicsRootDescriptorTable(13, iblSrv);
        // The DENOISED target, not the raw gather.
        D3D12_GPU_DESCRIPTOR_HANDLE giSrv = m_SsgiBlurSrvGpu[m_SsgiBlurIndex].ptr ? m_SsgiBlurSrvGpu[m_SsgiBlurIndex]
                                          : (m_SsaoSrvGpu.ptr ? m_SsaoSrvGpu : m_ShadowSrvGpu);
        if (giSrv.ptr) m_SkinnedList[frame]->SetGraphicsRootDescriptorTable(14, giSrv);
    }
    m_SkinnedList[frame]->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    recordSkinnedItems(m_SkinnedList[frame].Get(), 0, m_SkinnedItems.size(), frame);
    ThrowIfFailed(m_SkinnedList[frame]->Close());
}


// ================================= Shadow map (directional) =================================
// One depth-only pass from the caster's point of view, sampled by Basic3D_PS. See EnableShadows in
// Renderer3D.h for the API and why the bounds have to be given explicitly.

void Renderer3D::CreateShadowResources() {
    ID3D12Device2* device = m_Core->GetDevice();

    // --- the depth target. TYPELESS because it's used two ways: written as a DSV (D32_FLOAT) and read
    // as an SRV (R32_FLOAT). A typed depth resource can't be given a colour-readable SRV. ---
    D3D12_RESOURCE_DESC td = {};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = kShadowMapSize;
    td.Height           = kShadowMapSize;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format               = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth   = 1.0f;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_ShadowMap)));

    // --- DSV (its own tiny heap; the swap chain's DSV heap is sized for back buffers) ---
    D3D12_DESCRIPTOR_HEAP_DESC dh = {};
    dh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dh.NumDescriptors = 1;
    ThrowIfFailed(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&m_ShadowDsvHeap)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format        = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_ShadowMap.Get(), &dsv,
                                   m_ShadowDsvHeap->GetCPUDescriptorHandleForHeapStart());

    // --- SRV in the SHARED heap (only one shader-visible heap can be bound at a time) ---
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    if (!m_Core->GetRenderer2D()->GetResourceManager()->AllocateSrvSlot(srvCpu, m_ShadowSrvGpu)) {
        OutputDebugStringA("[Renderer3D] SRV heap full -- shadows disabled.\n");
        m_ShadowMap.Reset();
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                  = DXGI_FORMAT_R32_FLOAT;   // the readable view of the TYPELESS depth
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(m_ShadowMap.Get(), &srv, srvCpu);

    // --- root signature: the depth pass needs nothing but the light matrix at b0 ---
    CD3DX12_ROOT_PARAMETER sp[1] = {};
    sp[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    CD3DX12_ROOT_SIGNATURE_DESC sd;
    sd.Init(1, sp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&sd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); ThrowIfFailed(hr); }
    ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&m_ShadowRootSig)));

    // --- PSO: vertex shader only, NO pixel shader. NumRenderTargets = 0 -- this pass writes depth and
    // nothing else, which is what makes it roughly twice as fast as a colour pass. ---
    auto vs = ReadFile(ExeRelative(L"shaders\\ShadowDepth_VS.cso"));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_ShadowRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.InputLayout           = { kVertex3DLayout, _countof(kVertex3DLayout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    // CULL NONE, and no rasterizer depth bias -- the pixel shader's NORMAL OFFSET is the only
    // anti-acne mechanism, deliberately.
    //
    // Front-face culling is the classic alternative: recording only BACK faces pushes self-shadowing
    // error behind the surface that would reveal it. But it records each caster's FAR side as the
    // occluder, so near a caster's base the ground is closer to the light than that recorded depth
    // and tests as LIT -- a thin texel-wide bright ring around every object's contact point, scaling
    // with the object's thickness. It also breaks outright on open or single-sided geometry, which
    // has no back face to record.
    //
    // With normal-offset doing the acne work, culling buys nothing and costs that ring, so the
    // depth pass records the true nearest surface instead.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.SampleMask            = UINT_MAX;
    pso.NumRenderTargets      = 0;
    pso.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count      = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_ShadowPso)));

    // --- per-frame constant buffer (the light matrix) + command list ---
    const UINT cbSize = (UINT)((sizeof(DirectX::XMFLOAT4X4) + 255) & ~255u);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC   cbDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ShadowCB[i])));
        ThrowIfFailed(m_ShadowCB[i]->Map(0, nullptr, &m_ShadowCBMapped[i]));
        ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_ShadowAlloc[i])));
        ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_ShadowAlloc[i].Get(), nullptr, IID_PPV_ARGS(&m_ShadowList[i])));
        ThrowIfFailed(m_ShadowList[i]->Close());
    }
}

void Renderer3D::RecordShadowPass(int frame) {
    auto* cmd = m_ShadowList[frame].Get();
    ThrowIfFailed(m_ShadowAlloc[frame]->Reset());
    ThrowIfFailed(cmd->Reset(m_ShadowAlloc[frame].Get(), m_ShadowPso.Get()));

    // The map was left readable by the last frame's pixel shader; flip it back to writable depth.
    CD3DX12_RESOURCE_BARRIER toDepth = CD3DX12_RESOURCE_BARRIER::Transition(
        m_ShadowMap.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &toDepth);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_ShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);   // depth only -- no colour target at all

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)kShadowMapSize, (float)kShadowMapSize, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)kShadowMapSize, (LONG)kShadowMapSize };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->SetGraphicsRootSignature(m_ShadowRootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_ShadowCB[frame]->GetGPUVirtualAddress());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Slot 1 = the whole instance buffer, bound once; each batch selects its range via
    // StartInstanceLocation, exactly as RecordRange/recordBatches do for the geometry pass.
    D3D12_VERTEX_BUFFER_VIEW instVBV{};
    instVBV.BufferLocation = m_InstanceBuffer[frame]->GetGPUVirtualAddress();
    instVBV.StrideInBytes  = sizeof(DirectX::XMFLOAT4X4);
    instVBV.SizeInBytes    = kMaxInstances * (UINT)sizeof(DirectX::XMFLOAT4X4);
    cmd->IASetVertexBuffers(1, 1, &instVBV);

    // Replay the SAME batches the geometry pass will draw, minus all material state -- the shadow
    // shader reads only position and the per-instance matrix, so no textures are bound at all.
    for (const InstanceBatch& b : m_Batches) {
        if (!b.mesh || b.count == 0) continue;
        cmd->IASetVertexBuffers(0, 1, &b.mesh->vertexBufferView);
        cmd->IASetIndexBuffer(&b.mesh->indexBufferView);
        cmd->DrawIndexedInstanced(b.mesh->indexCount, b.count, 0, 0, b.firstInstance);
    }

    // Back to readable so the geometry pass's pixel shader can sample it.
    CD3DX12_RESOURCE_BARRIER toRead = CD3DX12_RESOURCE_BARRIER::Transition(
        m_ShadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &toRead);
    ThrowIfFailed(cmd->Close());
}


// ======================================== IBL environment bake ========================================
// Runs ONCE, synchronously, at load. Three stages, each a fullscreen triangle rendered per cube face
// (sharing SSAO_VS, which already produces the UV these shaders want):
//   1. EquirectToCube  -- the HDRI's lat/long rectangle into a cubemap, so runtime lookups are one
//                         hardware sample by direction instead of an atan2 into a distorted image.
//   2. IrradianceConv  -- cosine-convolved diffuse irradiance, 32x32/face (it is very low-frequency,
//                         so resolution buys nothing).
//   3. PrefilterEnv    -- a roughness mip chain, so a rough surface's blurry reflection is also one
//                         sample, with roughness picking the mip.
// The BRDF term that would normally need a fourth bake and a LUT texture is the analytic
// approximation in Basic3D_PS instead.

namespace {
    // The six cube-face bases, in D3D's face order: +X -X +Y -Y +Z -Z. Mirrors CubeFaceCB in the
    // three bake shaders.
    struct BakeCB {
        DirectX::XMFLOAT3 fwd;   float roughness;
        DirectX::XMFLOAT3 right; float mipCount;
        DirectX::XMFLOAT3 up;    float sourceSize;
    };
    struct FaceBasis { DirectX::XMFLOAT3 fwd, right, up; };
    static const FaceBasis kFaces[6] = {
        { {  1,  0,  0 }, {  0,  0, -1 }, { 0, 1,  0 } },   // +X
        { { -1,  0,  0 }, {  0,  0,  1 }, { 0, 1,  0 } },   // -X
        { {  0,  1,  0 }, {  1,  0,  0 }, { 0, 0, -1 } },   // +Y
        { {  0, -1,  0 }, {  1,  0,  0 }, { 0, 0,  1 } },   // -Y
        { {  0,  0,  1 }, {  1,  0,  0 }, { 0, 1,  0 } },   // +Z
        { {  0,  0, -1 }, { -1,  0,  0 }, { 0, 1,  0 } },   // -Z
    };
}

bool Renderer3D::BakeEnvironment(const std::wstring& hdrPath) {
    auto* device = m_Core->GetDevice();
    if (!device) return false;

    // ---- 1. decode the HDR. DirectXTex handles .hdr natively (it is already linked here for WIC),
    //         which is why this needs no third-party HDR parser. ----
    // Resolve the path EXE-RELATIVE when the caller gave a relative one. Every other asset path in
    // this renderer already goes through ExeRelative (shaders, textures, models) for one reason: the
    // CURRENT WORKING DIRECTORY IS NOT THE EXE DIRECTORY when launched from Visual Studio -- it is
    // the project dir. This one call took its string raw, so it worked when run from Explorer and
    // failed under the debugger, which is about the most confusing way for an asset load to break.
    // Try as-given first so an ABSOLUTE path (or a genuinely cwd-relative one) still works.
    auto tryLoad = [](const std::wstring& p, DirectX::ScratchImage& out) {
        if (SUCCEEDED(DirectX::LoadFromHDRFile(p.c_str(), nullptr, out))) return true;
        // .exr and LDR fallbacks: try WIC before giving up, so a .png/.jpg panorama still works
        // (it just has no values above 1 to give the highlights any punch). NOTE: WIC cannot decode
        // .exr -- that needs DirectXTexEXR + OpenEXR, so a .exr beside the .hdr will NOT load.
        return SUCCEEDED(DirectX::LoadFromWICFile(p.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, out));
    };

    DirectX::ScratchImage src;
    const std::wstring exeRel = ExeRelative(hdrPath);
    if (!tryLoad(hdrPath, src) && !tryLoad(exeRel, src)) {
        // Name BOTH paths tried, and say whether the file was even found. "could not decode" on a
        // file that was never there sends you looking at the decoder instead of the path.
        const bool existsCwd = (GetFileAttributesW(hdrPath.c_str()) != INVALID_FILE_ATTRIBUTES);
        const bool existsExe = (GetFileAttributesW(exeRel.c_str())  != INVALID_FILE_ATTRIBUTES);
        std::string narrow(hdrPath.begin(), hdrPath.end());
        std::string narrowExe(exeRel.begin(), exeRel.end());
        char buf[900];
        sprintf_s(buf, "[Renderer3D] LoadEnvironment FAILED -- IBL stays off.\n"
                       "  as given : %s  (%s)\n"
                       "  exe-rel  : %s  (%s)\n"
                       "  %s\n",
                  narrow.c_str(),    existsCwd ? "exists, DECODE FAILED" : "NOT FOUND",
                  narrowExe.c_str(), existsExe ? "exists, DECODE FAILED" : "NOT FOUND",
                  (existsCwd || existsExe)
                      ? "File found but undecodable. .exr is NOT supported (needs OpenEXR) -- use .hdr."
                      : "File not found on either path. Copy the .hdr next to the exe.");
        OutputDebugStringA(buf);
        return false;
    }
    const auto& srcMeta = src.GetMetadata();

    ComPtr<ID3D12CommandAllocator>    alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                            IID_PPV_ARGS(&cmd)));

    auto* rm = m_Core->GetRenderer2D()->GetResourceManager();

    // ---- upload the equirect source as a plain 2D texture ----
    ComPtr<ID3D12Resource>& equirect = m_EquirectTex;   // kept alive: the skybox samples it every frame
    ComPtr<ID3D12Resource> equirectUpload;
    {
        D3D12_RESOURCE_DESC td = {};
        td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width            = srcMeta.width;
        td.Height           = (UINT)srcMeta.height;
        td.DepthOrArraySize = 1;
        td.MipLevels        = 1;
        td.Format           = srcMeta.format;
        td.SampleDesc.Count = 1;
        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&equirect)));

        const DirectX::Image* img = src.GetImage(0, 0, 0);
        D3D12_SUBRESOURCE_DATA sub{};
        sub.pData      = img->pixels;
        sub.RowPitch   = (LONG_PTR)img->rowPitch;
        sub.SlicePitch = (LONG_PTR)img->slicePitch;

        const UINT64 need = GetRequiredIntermediateSize(equirect.Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC   bd = CD3DX12_RESOURCE_DESC::Buffer(need);
        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&equirectUpload)));
        UpdateSubresources(cmd.Get(), equirect.Get(), equirectUpload.Get(), 0, 0, 1, &sub);
        CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(equirect.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &b);
    }

    // ---- cube resources. R16G16B16A16_FLOAT throughout: HDR values well above 1 are the entire
    //      point of an HDRI, and an 8-bit target would clip the sun to white and lose it. ----
    auto makeCube = [&](UINT size, UINT mips, ComPtr<ID3D12Resource>& out, const wchar_t* name) {
        D3D12_RESOURCE_DESC td = {};
        td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width            = size;
        td.Height           = size;
        td.DepthOrArraySize = 6;
        td.MipLevels        = (UINT16)mips;
        td.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&out)));
        out->SetName(name);
    };

    m_PrefilterMips = 5;   // 128 -> 8; below that a face is too small to carry a useful reflection
    makeCube(kEnvCubeSize,    1,               m_EnvCube,        L"IBL env cube");
    makeCube(kIrradianceSize, 1,               m_IrradianceCube, L"IBL irradiance");
    makeCube(kPrefilterSize,  m_PrefilterMips, m_PrefilterCube,  L"IBL prefiltered");

    // RTV heap: 6 env faces + 6 irradiance faces + 6*mips prefilter slices.
    const UINT rtvCount = 6 + 6 + 6 * m_PrefilterMips;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC rh = {};
    rh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = rtvCount;
    ThrowIfFailed(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&rtvHeap)));
    const UINT rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    auto makeFaceRtv = [&](ID3D12Resource* res, UINT face, UINT mip, UINT slot) {
        D3D12_RENDER_TARGET_VIEW_DESC rv = {};
        rv.Format                         = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rv.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rv.Texture2DArray.MipSlice        = mip;
        rv.Texture2DArray.FirstArraySlice = face;
        rv.Texture2DArray.ArraySize       = 1;
        CD3DX12_CPU_DESCRIPTOR_HANDLE h(rtvHeap->GetCPUDescriptorHandleForHeapStart(), slot, rtvStride);
        device->CreateRenderTargetView(res, &rv, h);
        return h;
    };

    // SRVs: the equirect source, then the env cube (read by stages 2 and 3), then the two the
    // GEOMETRY pass keeps -- irradiance and prefiltered, allocated ADJACENTLY so one t8..t9 range
    // covers both.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE equirectSrv{}, envSrv{}, prefilterSrvUnused{};
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    if (!rm->AllocateSrvSlot(cpu, equirectSrv)) return false;
    sv.Format = srcMeta.format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(equirect.Get(), &sv, cpu);
    m_EquirectSrvGpu = equirectSrv;   // the skybox pass reuses this same view

    if (!rm->AllocateSrvSlot(cpu, envSrv)) return false;
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    sv.TextureCube.MipLevels = 1;
    device->CreateShaderResourceView(m_EnvCube.Get(), &sv, cpu);

    if (!rm->AllocateSrvSlot(cpu, m_IrradianceSrvGpu)) return false;
    sv.TextureCube.MipLevels = 1;
    device->CreateShaderResourceView(m_IrradianceCube.Get(), &sv, cpu);

    if (!rm->AllocateSrvSlot(cpu, prefilterSrvUnused)) return false;   // must land right after t8
    sv.TextureCube.MipLevels = m_PrefilterMips;
    device->CreateShaderResourceView(m_PrefilterCube.Get(), &sv, cpu);

    // ---- bake root signature + the three PSOs (all share SSAO_VS and this signature) ----
    if (!m_BakeRootSig) {
        CD3DX12_DESCRIPTOR_RANGE rSrc;
        rSrc.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        CD3DX12_ROOT_PARAMETER bp[2] = {};
        bp[0].InitAsConstants(sizeof(BakeCB) / 4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        bp[1].InitAsDescriptorTable(1, &rSrc, D3D12_SHADER_VISIBILITY_PIXEL);
        D3D12_STATIC_SAMPLER_DESC ss = {};
        ss.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD           = D3D12_FLOAT32_MAX;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        CD3DX12_ROOT_SIGNATURE_DESC sd;
        sd.Init(2, bp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = D3D12SerializeRootSignature(&sd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); return false; }
        ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                  IID_PPV_ARGS(&m_BakeRootSig)));

        auto vs = ReadFile(ExeRelative(L"shaders\\SSAO_VS.cso"));
        auto mkPso = [&](const wchar_t* psFile, ComPtr<ID3D12PipelineState>& out) {
            auto ps = ReadFile(ExeRelative(psFile));
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature        = m_BakeRootSig.Get();
            pd.VS                    = { vs.data(), vs.size() };
            pd.PS                    = { ps.data(), ps.size() };
            pd.InputLayout           = { nullptr, 0 };
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            pd.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            pd.DepthStencilState.DepthEnable = FALSE;
            pd.SampleMask            = UINT_MAX;
            pd.NumRenderTargets      = 1;
            pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
            pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
            pd.SampleDesc.Count      = 1;
            ThrowIfFailed(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out)));
        };
        mkPso(L"shaders\\EquirectToCube_PS.cso", m_EquirectPso);
        mkPso(L"shaders\\IrradianceConv_PS.cso", m_IrradiancePso);
        mkPso(L"shaders\\PrefilterEnv_PS.cso",   m_PrefilterPso);
    }

    ID3D12DescriptorHeap* heaps[] = { m_Core->GetRenderer2D()->GetSrvHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_BakeRootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Every stage is the same shape: set viewport, point at a face RTV, push the face basis, draw 3
    // vertices. Only the PSO, source SRV and target change.
    auto drawFace = [&](UINT size, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const BakeCB& cb) {
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f };
        D3D12_RECT     sc = { 0, 0, (LONG)size, (LONG)size };
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(BakeCB) / 4, &cb, 0);
        cmd->DrawInstanced(3, 1, 0, 0);
    };

    // ---- stage 1: equirect -> env cube ----
    cmd->SetPipelineState(m_EquirectPso.Get());
    cmd->SetGraphicsRootDescriptorTable(1, equirectSrv);
    UINT slot = 0;
    for (UINT f = 0; f < 6; ++f) {
        BakeCB cb{ kFaces[f].fwd, 0.0f, kFaces[f].right, 0.0f, kFaces[f].up, (float)srcMeta.width };
        drawFace(kEnvCubeSize, makeFaceRtv(m_EnvCube.Get(), f, 0, slot++), cb);
    }
    CD3DX12_RESOURCE_BARRIER envToRead = CD3DX12_RESOURCE_BARRIER::Transition(m_EnvCube.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &envToRead);

    // ---- stage 2: env cube -> irradiance ----
    cmd->SetPipelineState(m_IrradiancePso.Get());
    cmd->SetGraphicsRootDescriptorTable(1, envSrv);
    for (UINT f = 0; f < 6; ++f) {
        BakeCB cb{ kFaces[f].fwd, 0.0f, kFaces[f].right, 0.0f, kFaces[f].up, (float)kEnvCubeSize };
        drawFace(kIrradianceSize, makeFaceRtv(m_IrradianceCube.Get(), f, 0, slot++), cb);
    }

    // ---- stage 3: env cube -> prefiltered chain, roughness ramping 0..1 across the mips ----
    cmd->SetPipelineState(m_PrefilterPso.Get());
    for (UINT mip = 0; mip < m_PrefilterMips; ++mip) {
        const UINT  mipSize   = kPrefilterSize >> mip;
        const float roughness = (m_PrefilterMips > 1) ? (float)mip / (float)(m_PrefilterMips - 1) : 0.0f;
        for (UINT f = 0; f < 6; ++f) {
            BakeCB cb{ kFaces[f].fwd, roughness, kFaces[f].right, (float)m_PrefilterMips,
                       kFaces[f].up, (float)kEnvCubeSize };
            drawFace(mipSize, makeFaceRtv(m_PrefilterCube.Get(), f, mip, slot++), cb);
        }
    }

    CD3DX12_RESOURCE_BARRIER done[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_IrradianceCube.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(m_PrefilterCube.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    cmd->ResourceBarrier(2, done);
    ThrowIfFailed(cmd->Close());

    ID3D12CommandList* lists[] = { cmd.Get() };
    m_Core->GetCommandQueue()->ExecuteCommandLists(1, lists);
    // Block until the bake is done. This is load-time work, and everything after it (the upload
    // buffer, the RTV heap, the intermediate env cube) becomes garbage the moment this returns --
    // releasing any of it while the GPU is still reading would fault.
    m_Core->WaitForGpuIdle();

    m_EnvReady = true;
    return true;
}


// ==================================== SSAO (depth prepass + occlusion) ====================================
// Two passes, one command list, both ahead of the geometry pass:
//   1. depth prepass -- the scene from the CAMERA into a private depth texture, reusing the shadow
//      pass's depth-only shader and root signature (it only ever wanted a matrix at b0, and the
//      camera's matrix fits that slot as well as a light's).
//   2. occlusion pass -- one fullscreen triangle turning that depth into an R8 occlusion factor.
// The barriers between them make the ordering a hard GPU dependency rather than a hope, exactly as
// the shadow pass does.

// Mirrors cbuffer SsaoCB in SSAO_PS.hlsl field-for-field.
struct SsaoCBData {
    DirectX::XMFLOAT4X4 viewProj;
    DirectX::XMFLOAT4X4 invViewProj;
    DirectX::XMFLOAT3   eyePos;   float radius;
    float bias; float intensity; float pad0; float pad1;
};

void Renderer3D::CreateSsaoResources(UINT width, UINT height) {
    auto* device = m_Core->GetDevice();
    if (!device || width == 0 || height == 0) return;

    m_SsaoWidth = width; m_SsaoHeight = height;

    // --- private camera depth target (TYPELESS so it can be both written as depth and read as an SRV) ---
    D3D12_RESOURCE_DESC td = {};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = width;
    td.Height           = height;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE dclear = {};
    dclear.Format             = DXGI_FORMAT_D32_FLOAT;
    dclear.DepthStencil.Depth = 1.0f;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &dclear, IID_PPV_ARGS(&m_SsaoDepth)));
    m_SsaoDepth->SetName(L"SSAO camera depth");

    D3D12_DESCRIPTOR_HEAP_DESC dh = {};
    dh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dh.NumDescriptors = 1;
    ThrowIfFailed(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&m_SsaoDepthDsvHeap)));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format        = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_SsaoDepth.Get(), &dsv,
                                   m_SsaoDepthDsvHeap->GetCPUDescriptorHandleForHeapStart());

    // --- occlusion target: R8 is plenty for a 0..1 factor that then gets box-blurred ---
    D3D12_RESOURCE_DESC ad = td;
    ad.Format = DXGI_FORMAT_R8_UNORM;
    ad.Flags  = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE aclear = {};
    aclear.Format = DXGI_FORMAT_R8_UNORM;
    aclear.Color[0] = 1.0f;   // 1 = unoccluded, so a frame that never runs the pass reads as "no AO"
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &ad,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &aclear, IID_PPV_ARGS(&m_SsaoTarget)));
    m_SsaoTarget->SetName(L"SSAO occlusion");

    D3D12_DESCRIPTOR_HEAP_DESC rh = {};
    rh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = 1;
    ThrowIfFailed(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m_SsaoRtvHeap)));
    device->CreateRenderTargetView(m_SsaoTarget.Get(), nullptr,
                                   m_SsaoRtvHeap->GetCPUDescriptorHandleForHeapStart());

    // --- SRVs in the SHARED heap (only one shader-visible heap can be bound at a time) ---
    auto* rm = m_Core->GetRenderer2D()->GetResourceManager();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    if (!rm->AllocateSrvSlot(cpu, m_SsaoDepthSrvGpu)) {
        OutputDebugStringA("[Renderer3D] SRV heap full -- SSAO disabled.\n");
        m_SsaoDepth.Reset(); m_SsaoTarget.Reset(); return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                  = DXGI_FORMAT_R32_FLOAT;   // readable view of the TYPELESS depth
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(m_SsaoDepth.Get(), &srv, cpu);

    if (!rm->AllocateSrvSlot(cpu, m_SsaoSrvGpu)) {
        OutputDebugStringA("[Renderer3D] SRV heap full -- SSAO disabled.\n");
        m_SsaoDepth.Reset(); m_SsaoTarget.Reset(); m_SsaoSrvGpu = {}; return;
    }
    srv.Format = DXGI_FORMAT_R8_UNORM;
    device->CreateShaderResourceView(m_SsaoTarget.Get(), &srv, cpu);

    // --- root signature: b0 params + t0 depth + a POINT sampler (bilinear across a depth
    //     discontinuity blends two unrelated surfaces into a depth that exists nowhere) ---
    CD3DX12_DESCRIPTOR_RANGE rDepth;
    rDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_ROOT_PARAMETER sp[2] = {};
    sp[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    sp[1].InitAsDescriptorTable(1, &rDepth, D3D12_SHADER_VISIBILITY_PIXEL);
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD           = D3D12_FLOAT32_MAX;
    ss.ShaderRegister   = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    CD3DX12_ROOT_SIGNATURE_DESC sd;
    sd.Init(2, sp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE);   // no input layout: vertex ID only
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&sd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); ThrowIfFailed(hr); }
    ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&m_SsaoRootSig)));

    auto vs = ReadFile(ExeRelative(L"shaders\\SSAO_VS.cso"));
    auto ps = ReadFile(ExeRelative(L"shaders\\SSAO_PS.cso"));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_SsaoRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { nullptr, 0 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;   // fullscreen pass: nothing to test or write against
    pso.SampleMask            = UINT_MAX;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = DXGI_FORMAT_R8_UNORM;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count      = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_SsaoPso)));

    // --- per-frame CBs (SSAO params + the prepass camera matrix) + command lists ---
    const UINT ssaoCbSize    = (UINT)((sizeof(SsaoCBData) + 255) & ~255u);
    const UINT prepassCbSize = (UINT)((sizeof(DirectX::XMFLOAT4X4) + 255) & ~255u);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        CD3DX12_RESOURCE_DESC b1 = CD3DX12_RESOURCE_DESC::Buffer(ssaoCbSize);
        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &b1,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_SsaoCB[i])));
        ThrowIfFailed(m_SsaoCB[i]->Map(0, nullptr, &m_SsaoCBMapped[i]));

        CD3DX12_RESOURCE_DESC b2 = CD3DX12_RESOURCE_DESC::Buffer(prepassCbSize);
        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &b2,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_PrepassCB[i])));
        ThrowIfFailed(m_PrepassCB[i]->Map(0, nullptr, &m_PrepassCBMapped[i]));

        ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&m_SsaoAlloc[i])));
        ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_SsaoAlloc[i].Get(), nullptr, IID_PPV_ARGS(&m_SsaoList[i])));
        ThrowIfFailed(m_SsaoList[i]->Close());
    }
}

void Renderer3D::RecordSsaoPass(int frame) {
    auto* cmd = m_SsaoList[frame].Get();
    ThrowIfFailed(m_SsaoAlloc[frame]->Reset());
    ThrowIfFailed(cmd->Reset(m_SsaoAlloc[frame].Get(), m_ShadowPso.Get()));

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)m_SsaoWidth, (float)m_SsaoHeight, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)m_SsaoWidth, (LONG)m_SsaoHeight };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    // ---- pass 1: camera depth prepass ----
    memcpy(m_PrepassCBMapped[frame], &m_ViewProj, sizeof(m_ViewProj));

    CD3DX12_RESOURCE_BARRIER toDepth = CD3DX12_RESOURCE_BARRIER::Transition(
        m_SsaoDepth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &toDepth);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_SsaoDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);

    cmd->SetGraphicsRootSignature(m_ShadowRootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_PrepassCB[frame]->GetGPUVirtualAddress());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW instVBV{};
    instVBV.BufferLocation = m_InstanceBuffer[frame]->GetGPUVirtualAddress();
    instVBV.StrideInBytes  = sizeof(DirectX::XMFLOAT4X4);
    instVBV.SizeInBytes    = kMaxInstances * (UINT)sizeof(DirectX::XMFLOAT4X4);
    cmd->IASetVertexBuffers(1, 1, &instVBV);

    for (const InstanceBatch& b : m_Batches) {
        if (!b.mesh || b.count == 0) continue;
        cmd->IASetVertexBuffers(0, 1, &b.mesh->vertexBufferView);
        cmd->IASetIndexBuffer(&b.mesh->indexBufferView);
        cmd->DrawIndexedInstanced(b.mesh->indexCount, b.count, 0, 0, b.firstInstance);
    }

    CD3DX12_RESOURCE_BARRIER depthToRead = CD3DX12_RESOURCE_BARRIER::Transition(
        m_SsaoDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &depthToRead);

    // ---- pass 2: occlusion (skipped when only SSGI wants the prepass) ----
    if (m_SsaoEnabled && m_SsaoTarget) {
    SsaoCBData cb{};
    cb.viewProj = m_ViewProj;
    DirectX::XMStoreFloat4x4(&cb.invViewProj,
        DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&m_ViewProj)));
    cb.eyePos    = m_CamPos;
    cb.radius    = m_SsaoRadius;
    cb.bias      = m_SsaoBias;
    cb.intensity = m_SsaoIntensity;
    memcpy(m_SsaoCBMapped[frame], &cb, sizeof(cb));

    CD3DX12_RESOURCE_BARRIER toRt = CD3DX12_RESOURCE_BARRIER::Transition(
        m_SsaoTarget.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &toRt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_SsaoRtvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_Core->GetRenderer2D()->GetSrvHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetPipelineState(m_SsaoPso.Get());
    cmd->SetGraphicsRootSignature(m_SsaoRootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_SsaoCB[frame]->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(1, m_SsaoDepthSrvGpu);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);   // the fullscreen triangle SSAO_VS builds from SV_VertexID

    CD3DX12_RESOURCE_BARRIER aoToRead = CD3DX12_RESOURCE_BARRIER::Transition(
        m_SsaoTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &aoToRead);
    }

    // ---- pass 3: SSGI gather. Same depth prepass, same kernel shape as the occlusion pass above --
    // it just reads colour from the history buffer instead of counting hits. Runs here, before the
    // geometry pass, because Basic3D_PS consumes the result as an indirect-light term. ----
    if (m_SsgiEnabled && m_SsgiTarget && m_HdrHistory) {
        SsgiCBData gcb{};
        gcb.viewProj = m_ViewProj;
        DirectX::XMStoreFloat4x4(&gcb.invViewProj,
            DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&m_ViewProj)));
        gcb.eyePos    = m_CamPos;
        gcb.maxDist   = m_SsgiMaxDist;
        gcb.intensity = m_SsgiIntensity;
        gcb.maxLuma   = m_SsgiMaxLuma;
        gcb.thickness = m_SsgiThickness;
        gcb.steps     = (float)m_SsgiSteps;
        gcb.rays      = (float)m_SsgiRays;
        // Advancing every frame is what stops the sampling noise being a fixed screen-space pattern.
        // A static pattern reads as a texture printed on the world and the eye locks onto it; one that
        // changes reads as grain, which is far less objectionable and is what a later temporal filter
        // would average away.
        gcb.frame     = (float)(m_SsgiFrame++ & 1023u);
        memcpy(m_SsgiCBMapped[frame], &gcb, sizeof(gcb));

        CD3DX12_RESOURCE_BARRIER giToRt = CD3DX12_RESOURCE_BARRIER::Transition(
            m_SsgiTarget.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &giToRt);

        D3D12_CPU_DESCRIPTOR_HANDLE girtv = m_SsgiRtvHeap->GetCPUDescriptorHandleForHeapStart();
        cmd->OMSetRenderTargets(1, &girtv, FALSE, nullptr);

        ID3D12DescriptorHeap* gheaps[] = { m_Core->GetRenderer2D()->GetSrvHeap() };
        cmd->SetDescriptorHeaps(1, gheaps);
        cmd->SetPipelineState(m_SsgiPso.Get());
        cmd->SetGraphicsRootSignature(m_SsgiRootSig.Get());
        cmd->SetGraphicsRootConstantBufferView(0, m_SsgiCB[frame]->GetGPUVirtualAddress());
        cmd->SetGraphicsRootDescriptorTable(1, m_SsaoDepthSrvGpu);   // this frame's depth
        cmd->SetGraphicsRootDescriptorTable(2, m_HistSrvGpu);        // LAST frame's colour
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetVertexBuffers(0, 0, nullptr);
        cmd->IASetIndexBuffer(nullptr);
        cmd->DrawInstanced(3, 1, 0, 0);

        CD3DX12_RESOURCE_BARRIER giToRead = CD3DX12_RESOURCE_BARRIER::Transition(
            m_SsgiTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &giToRead);

        // ---- denoise: edge-aware blur of the raw gather into its own target ----
        // A separate target rather than filtering in place, because a pass cannot sample the surface
        // it is writing. The geometry pass then reads THIS, which is also why the 25-tap box that
        // used to live in Basic3D_PS is gone: filtering once per screen pixel beats once per lit
        // pixel, and having the depth buffer here is what lets it stop at silhouettes.
        if (m_SsgiBlurTarget[0]) {
            // Flip FIRST: the index that was "current" last frame becomes the history this frame.
            m_SsgiBlurIndex ^= 1;
            const int cur = m_SsgiBlurIndex, hist = cur ^ 1;

            SsgiBlurCBData bcb{};
            DirectX::XMStoreFloat4x4(&bcb.invViewProj,
                DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&m_ViewProj)));
            bcb.prevViewProj = m_PrevViewProj;
            bcb.texelX = 1.0f / (float)m_SsgiWidth;
            bcb.texelY = 1.0f / (float)m_SsgiHeight;
            bcb.stride = m_SsgiBlurStride;
            bcb.depthSigma = m_SsgiBlurSigma;
            bcb.alpha = m_SsgiAlpha;
            bcb.historyValid = m_SsgiHistoryValid ? 1.0f : 0.0f;
            memcpy(m_SsgiBlurCBMapped[frame], &bcb, sizeof(bcb));

            CD3DX12_RESOURCE_BARRIER bToRt = CD3DX12_RESOURCE_BARRIER::Transition(
                m_SsgiBlurTarget[cur].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->ResourceBarrier(1, &bToRt);

            const UINT bs = m_Core->GetDevice()->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            CD3DX12_CPU_DESCRIPTOR_HANDLE brtv(
                m_SsgiBlurRtvHeap->GetCPUDescriptorHandleForHeapStart(), cur, bs);
            cmd->OMSetRenderTargets(1, &brtv, FALSE, nullptr);
            cmd->SetPipelineState(m_SsgiBlurPso.Get());
            cmd->SetGraphicsRootSignature(m_SsgiBlurRootSig.Get());
            cmd->SetGraphicsRootConstantBufferView(0, m_SsgiBlurCB[frame]->GetGPUVirtualAddress());
            cmd->SetGraphicsRootDescriptorTable(1, m_SsgiSrvGpu);            // raw gather, this frame
            cmd->SetGraphicsRootDescriptorTable(2, m_SsaoDepthSrvGpu);       // depth, for edge stopping
            cmd->SetGraphicsRootDescriptorTable(3, m_SsgiBlurSrvGpu[hist]);  // LAST frame's result
            cmd->DrawInstanced(3, 1, 0, 0);

            CD3DX12_RESOURCE_BARRIER bToRead = CD3DX12_RESOURCE_BARRIER::Transition(
                m_SsgiBlurTarget[cur].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1, &bToRead);

            // Only now is there something worth reprojecting into next frame.
            m_SsgiHistoryValid = true;
        }
    }

    ThrowIfFailed(cmd->Close());
}


// ============================ HDR intermediate + tonemapping ============================
// Everything the 3D renderer draws goes into an FP16 texture instead of the back buffer, and one
// fullscreen pass maps that linear HDR range down to what a display can show.
//
// Why this is worth a whole extra target: an 8-bit UNORM back buffer cannot hold a value above 1.0,
// so before this the pixel shader had to squash its lighting result inline (a Reinhard divide) the
// instant it was computed. Every highlight above white was destroyed at that point -- which is both
// why bright scenes read flat and why bloom could not be built (bloom needs something to bloom
// FROM, and there was nothing left over 1.0 to find). Keeping the range and deciding how to map it
// ONCE, at the end, is the entire structural change; the curve itself is a handful of ALU ops.
//
// The pass reuses SSAO_VS -- one oversized triangle from SV_VertexID, no vertex buffer -- and gets
// its ordering from resource barriers, which is the established idiom in this renderer.

// Root signature + PSO + command lists: everything that does NOT depend on the window size, built
// once from Initialize. Keeping the shader read here is the point -- see the header comment.
void Renderer3D::CreateTonemapPipeline() {
    auto* device = m_Core->GetDevice();

    // ---- root signature: b0 = 4 root constants (exposure + operator), t0 = the HDR texture ----
    // Root constants rather than a per-frame CB: two live floats, changed at most once per frame,
    // so a CB resource per frame slot would be pure bookkeeping.
    CD3DX12_DESCRIPTOR_RANGE rHdr, rBloom, rGiDbg;
    rHdr.Init  (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);   // t0: the FP16 scene
    rBloom.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);   // t1: the finished bloom chain
    rGiDbg.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);   // t2: SSGI bounce, for the debug view
    // t0 and t1 need SEPARATE ranges rather than one range of 2, because the HDR target and
    // bloom[0] are allocated independently in the shared SRV heap and are not adjacent -- a
    // 2-descriptor range would require them to be.
    CD3DX12_ROOT_PARAMETER tp[4] = {};
    tp[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);            // b0
    tp[1].InitAsDescriptorTable(1, &rHdr,   D3D12_SHADER_VISIBILITY_PIXEL);   // t0
    tp[2].InitAsDescriptorTable(1, &rBloom, D3D12_SHADER_VISIBILITY_PIXEL);   // t1
    tp[3].InitAsDescriptorTable(1, &rGiDbg, D3D12_SHADER_VISIBILITY_PIXEL);   // t2
    // POINT: the pass is 1:1 with the back buffer, so there is nothing to interpolate. A linear
    // sampler here would only soften the image for no reason.
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD           = D3D12_FLOAT32_MAX;
    ss.ShaderRegister   = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    CD3DX12_ROOT_SIGNATURE_DESC sd;
    sd.Init(4, tp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE);   // no input layout: vertex ID only
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&sd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); ThrowIfFailed(hr); }
    ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&m_TonemapRootSig)));

    auto vs = ReadFile(ExeRelative(L"shaders\\SSAO_VS.cso"));       // shared fullscreen triangle
    auto ps = ReadFile(ExeRelative(L"shaders\\Tonemap_PS.cso"));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_TonemapRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { nullptr, 0 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);   // opaque: it REPLACES the frame
    pso.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask            = UINT_MAX;
    pso.NumRenderTargets      = 1;
    // THE encode point. This one PSO writes through the _SRGB view, which is what gamma-encodes the
    // frame; every other 3D PSO writes linear into HdrFormat and must NOT encode.
    pso.RTVFormats[0]         = RendererCore::BackBufferRTVFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count      = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_TonemapPso)));
    m_TonemapPso->SetName(L"Tonemap PSO");

    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&m_HdrBeginAlloc[i])));
        ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_HdrBeginAlloc[i].Get(), nullptr, IID_PPV_ARGS(&m_HdrBeginList[i])));
        ThrowIfFailed(m_HdrBeginList[i]->Close());
        m_HdrBeginList[i]->SetName(L"HDR target clear");

        ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&m_TonemapAlloc[i])));
        ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_TonemapAlloc[i].Get(), nullptr, IID_PPV_ARGS(&m_TonemapList[i])));
        ThrowIfFailed(m_TonemapList[i]->Close());
        m_TonemapList[i]->SetName(L"Tonemap pass");
    }
}

// The size-dependent half: the FP16 texture plus its RTV and SRV. Called on first record and again
// whenever the window size changes.
bool Renderer3D::CreateHdrTarget(UINT width, UINT height) {
    auto* device = m_Core->GetDevice();
    if (!device || width == 0 || height == 0) return false;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = width;
    td.Height           = height;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = RendererCore::HdrFormat;
    td.SampleDesc.Count = 1;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // The optimized clear value MUST match what RecordHdrBegin actually clears to, or the driver
    // takes a slow path (and the debug layer complains).
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = RendererCore::HdrFormat;
    memcpy(clear.Color, &m_HdrClearColor, sizeof(clear.Color));

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    // Created in RENDER_TARGET and it RESTS there: the tonemap list is the only thing that moves it
    // (to PIXEL_SHADER_RESOURCE and straight back), so no other pass needs a barrier.
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&m_HdrTarget)));
    m_HdrTarget->SetName(L"HDR scene target (FP16)");

    if (!m_HdrRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC rh = {};
        rh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = 1;
        ThrowIfFailed(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m_HdrRtvHeap)));
    }
    device->CreateRenderTargetView(m_HdrTarget.Get(), nullptr,
                                   m_HdrRtvHeap->GetCPUDescriptorHandleForHeapStart());

    // SRV in the SHARED heap (only one shader-visible heap can be bound at a time). On a RESIZE the
    // slot is REUSED -- the view is rewritten at the CPU handle we kept -- rather than allocating a
    // fresh one. AllocateSrvSlot has no free list, so re-allocating here would leak a descriptor on
    // every window resize; the SSAO path does exactly that and should be given the same treatment.
    auto* rm = m_Core->GetRenderer2D()->GetResourceManager();
    if (!m_HdrSrvGpu.ptr) {
        if (!rm->AllocateSrvSlot(m_HdrSrvCpu, m_HdrSrvGpu)) {
            OutputDebugStringA("[Renderer3D] SRV heap full -- cannot create the HDR target; "
                               "the 3D pass will be skipped.\n");
            m_HdrTarget.Reset();
            return false;
        }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                  = RendererCore::HdrFormat;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(m_HdrTarget.Get(), &srv, m_HdrSrvCpu);

    m_HdrWidth = width; m_HdrHeight = height;
    return true;
}

// Clears the FP16 target. Pushed FIRST each frame. RendererCore's BeginFrame clears the BACK BUFFER,
// which the 3D passes no longer touch -- without this list the scene target keeps whatever the
// previous frame left in it wherever nothing draws over it this frame.
void Renderer3D::RecordHdrBegin(int frame) {
    auto* cmd = m_HdrBeginList[frame].Get();
    ThrowIfFailed(m_HdrBeginAlloc[frame]->Reset());
    ThrowIfFailed(cmd->Reset(m_HdrBeginAlloc[frame].Get(), nullptr));
    // No barrier: the target rests in RENDER_TARGET (the tonemap list restores it before it ends).
    cmd->ClearRenderTargetView(m_HdrRtvHeap->GetCPUDescriptorHandleForHeapStart(),
                               reinterpret_cast<const FLOAT*>(&m_HdrClearColor), 0, nullptr);
    ThrowIfFailed(cmd->Close());
}

// Resolves the FP16 scene onto the back buffer. Pushed LAST of the 3D lists, so RendererCore's 2D
// layers -- submitted after them -- composite on top of the finished image.
void Renderer3D::RecordTonemapPass(int frame, D3D12_CPU_DESCRIPTOR_HANDLE backRtv,
                                   UINT width, UINT height) {
    auto* cmd = m_TonemapList[frame].Get();
    ThrowIfFailed(m_TonemapAlloc[frame]->Reset());
    ThrowIfFailed(cmd->Reset(m_TonemapAlloc[frame].Get(), m_TonemapPso.Get()));

    CD3DX12_RESOURCE_BARRIER toRead = CD3DX12_RESOURCE_BARRIER::Transition(
        m_HdrTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &toRead);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)width, (LONG)height };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    // The back buffer is already in RENDER_TARGET (RendererCore's PRE list transitioned it) and no
    // depth is bound -- a fullscreen resolve has nothing to test against.
    cmd->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_Core->GetRenderer2D()->GetSrvHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_TonemapRootSig.Get());
    // Bloom is live only if it was asked for AND the chain actually exists this frame. When it
    // doesn't, t1 is still bound (D3D12 requires every declared table populated even where the
    // shader branches past it) -- the HDR target itself is the placeholder, and intensity 0 is what
    // switches it off.
    const bool  bloomLive = m_BloomEnabled && m_BloomLevels > 0 && m_BloomTex[0];
    // SSGI debug is a DISPLAY mode: it shows the bounce target instead of the scene, and deliberately
    // does NOT alter what the geometry pass wrote. That matters because the HDR target is copied to
    // the history buffer SSGI reads next frame -- putting the debug image there made the view feed
    // itself a black scene and extinguish after one frame. The scale is generous because the bounce
    // is a small fraction of a lit surface and would be near-invisible shown at 1:1.
    const bool  giDbgLive = m_SsgiEnabled && m_SsgiDebug && m_SsgiTarget;
    const float consts[4] = { m_Exposure, (float)(uint32_t)m_Tonemapper,
                              bloomLive ? m_BloomIntensity : 0.0f,
                              giDbgLive ? 6.0f : 0.0f };
    cmd->SetGraphicsRoot32BitConstants(0, 4, consts, 0);
    cmd->SetGraphicsRootDescriptorTable(1, m_HdrSrvGpu);
    cmd->SetGraphicsRootDescriptorTable(2, bloomLive ? m_BloomSrvGpu[0] : m_HdrSrvGpu);
    // t2 must be populated whether or not the debug view is on -- D3D12 requires every declared
    // table bound even where the shader branches past it.
    cmd->SetGraphicsRootDescriptorTable(3, m_SsgiBlurSrvGpu[m_SsgiBlurIndex].ptr ? m_SsgiBlurSrvGpu[m_SsgiBlurIndex] : m_HdrSrvGpu);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);   // the fullscreen triangle SSAO_VS builds from SV_VertexID

    // SNAPSHOT THIS FRAME'S HDR FOR SSGI. Done here, at the end of the tonemap list, because that is
    // the last point the finished scene exists and the HDR target is already in a readable state --
    // and because next frame's SSGI gather runs BEFORE any geometry, so it can only ever read a
    // previous frame. One full-res FP16 copy per frame is the price of a screen-space bounce that
    // needs lit colour it cannot otherwise have.
    // The size guard is deliberate belt-and-braces. CopyResource requires EXACTLY matching
    // dimensions and a mismatch is a debug-layer ERROR that terminates the process, not a no-op --
    // which is exactly what a resize did before the SSGI targets tracked their own size. The
    // creation path fixes the cause; this makes the failure mode a skipped frame of GI instead of
    // a crash if any future path ever gets them out of step again.
    if (m_SsgiEnabled && m_HdrHistory &&
        m_SsgiWidth == m_HdrWidth && m_SsgiHeight == m_HdrHeight) {
        CD3DX12_RESOURCE_BARRIER toCopy[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_HdrTarget.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_HdrHistory.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cmd->ResourceBarrier(2, toCopy);
        cmd->CopyResource(m_HdrHistory.Get(), m_HdrTarget.Get());
        CD3DX12_RESOURCE_BARRIER afterCopy[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_HdrTarget.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(m_HdrHistory.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        cmd->ResourceBarrier(2, afterCopy);
    } else {
        CD3DX12_RESOURCE_BARRIER backToRt = CD3DX12_RESOURCE_BARRIER::Transition(
            m_HdrTarget.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &backToRt);
    }

    ThrowIfFailed(cmd->Close());
}


// ============================ SSGI ============================
// One bounce of indirect light. Records onto the SSAO list (it needs that pass's camera depth
// prepass), reading the PREVIOUS frame's HDR colour and writing a gathered-bounce target that
// Basic3D_PS adds to its ambient term. See SSGI_PS.hlsl for the method and its limits.

void Renderer3D::CreateSsgiPipeline() {
    auto* device = m_Core->GetDevice();

    // b0 params, t0 depth, t1 history. Two SEPARATE ranges, not one range of 2: the depth SRV comes
    // from the SSAO allocation and the history from this one, so they are not adjacent in the heap.
    CD3DX12_DESCRIPTOR_RANGE rDepth, rHist;
    rDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    rHist.Init (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    CD3DX12_ROOT_PARAMETER sp[3] = {};
    sp[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    sp[1].InitAsDescriptorTable(1, &rDepth, D3D12_SHADER_VISIBILITY_PIXEL);
    sp[2].InitAsDescriptorTable(1, &rHist,  D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_STATIC_SAMPLER_DESC ss[2] = {};
    // s0 POINT for depth -- bilinear across a depth discontinuity blends two unrelated surfaces into
    // a depth that exists nowhere, which is the same reason SSAO uses point.
    ss[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss[0].AddressU = ss[0].AddressV = ss[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss[0].MaxLOD = D3D12_FLOAT32_MAX; ss[0].ShaderRegister = 0;
    ss[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s1 LINEAR for colour -- here the smoothing is wanted: it is the cheapest denoise available on
    // a 16-sample gather, and colour has no discontinuity problem the way depth does.
    ss[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss[1].AddressU = ss[1].AddressV = ss[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss[1].MaxLOD = D3D12_FLOAT32_MAX; ss[1].ShaderRegister = 1;
    ss[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC sd;
    sd.Init(3, sp, 2, ss, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&sd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); ThrowIfFailed(hr); }
    ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&m_SsgiRootSig)));

    auto vs = ReadFile(ExeRelative(L"shaders\\SSAO_VS.cso"));   // shared fullscreen triangle
    auto ps = ReadFile(ExeRelative(L"shaders\\SSGI_PS.cso"));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_SsgiRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { nullptr, 0 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask            = UINT_MAX;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = DXGI_FORMAT_R11G11B10_FLOAT;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count      = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_SsgiPso)));
    m_SsgiPso->SetName(L"SSGI PSO");

    // ---- denoise pipeline: b0 params, t0 raw gather, t1 depth ----
    {
        CD3DX12_DESCRIPTOR_RANGE rSrc, rDep, rHist;
        rSrc.Init (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        rDep.Init (D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
        rHist.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
        CD3DX12_ROOT_PARAMETER bp[4] = {};
        // A CBV, not root constants: two 4x4 matrices alone are 32 DWORDs of the 64-DWORD budget.
        bp[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
        bp[1].InitAsDescriptorTable(1, &rSrc,  D3D12_SHADER_VISIBILITY_PIXEL);
        bp[2].InitAsDescriptorTable(1, &rDep,  D3D12_SHADER_VISIBILITY_PIXEL);
        bp[3].InitAsDescriptorTable(1, &rHist, D3D12_SHADER_VISIBILITY_PIXEL);
        D3D12_STATIC_SAMPLER_DESC bs[2] = {};
        bs[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;   // s0: colour, smoothing is wanted
        bs[0].AddressU = bs[0].AddressV = bs[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        bs[0].MaxLOD = D3D12_FLOAT32_MAX; bs[0].ShaderRegister = 0;
        bs[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        bs[1] = bs[0];
        bs[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;    // s1: depth, never interpolate across an edge
        bs[1].ShaderRegister = 1;
        CD3DX12_ROOT_SIGNATURE_DESC bd;
        bd.Init(4, bp, 2, bs, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> bblob, berr;
        HRESULT bhr = D3D12SerializeRootSignature(&bd, D3D_ROOT_SIGNATURE_VERSION_1, &bblob, &berr);
        if (FAILED(bhr)) { if (berr) OutputDebugStringA((const char*)berr->GetBufferPointer()); ThrowIfFailed(bhr); }
        ThrowIfFailed(device->CreateRootSignature(0, bblob->GetBufferPointer(), bblob->GetBufferSize(),
                                                  IID_PPV_ARGS(&m_SsgiBlurRootSig)));
        auto bps = ReadFile(ExeRelative(L"shaders\\SSGIBlur_PS.cso"));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC bpso = pso;   // same fullscreen-triangle setup as the gather
        bpso.pRootSignature = m_SsgiBlurRootSig.Get();
        bpso.PS = { bps.data(), bps.size() };
        ThrowIfFailed(device->CreateGraphicsPipelineState(&bpso, IID_PPV_ARGS(&m_SsgiBlurPso)));
        m_SsgiBlurPso->SetName(L"SSGI denoise PSO");
    }

    const UINT cbSize = (UINT)((sizeof(SsgiCBData) + 255) & ~255u);
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        CD3DX12_RESOURCE_DESC b = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &b,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_SsgiCB[i])));
        ThrowIfFailed(m_SsgiCB[i]->Map(0, nullptr, &m_SsgiCBMapped[i]));
    }
}

bool Renderer3D::CreateSsgiTargets(UINT width, UINT height) {
    auto* device = m_Core->GetDevice();
    if (!device || width == 0 || height == 0) return false;
    auto* rm = m_Core->GetRenderer2D()->GetResourceManager();
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    D3D12_RESOURCE_DESC td = {};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = width;
    td.Height           = height;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.SampleDesc.Count = 1;

    // ---- gathered-bounce target ----
    td.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    td.Flags  = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_R11G11B10_FLOAT;   // black = no bounce, the safe default
    m_SsgiTarget.Reset();
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_SsgiTarget)));
    m_SsgiTarget->SetName(L"SSGI bounce");

    if (!m_SsgiRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC rh = {};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 1;
        ThrowIfFailed(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m_SsgiRtvHeap)));
    }
    device->CreateRenderTargetView(m_SsgiTarget.Get(), nullptr,
                                   m_SsgiRtvHeap->GetCPUDescriptorHandleForHeapStart());

    // ---- history: a copy of last frame's HDR. COPY_DEST-capable, no RTV needed. ----
    td.Format = RendererCore::HdrFormat;
    td.Flags  = D3D12_RESOURCE_FLAG_NONE;
    m_HdrHistory.Reset();
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_HdrHistory)));
    m_HdrHistory->SetName(L"HDR history (SSGI gather source)");

    // SRV slots REUSED across resizes -- AllocateSrvSlot has no free list.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;

    if (!m_SsgiSrvGpu.ptr && !rm->AllocateSrvSlot(m_SsgiSrvCpu, m_SsgiSrvGpu)) {
        OutputDebugStringA("[Renderer3D] SRV heap full -- SSGI disabled.\n");
        m_SsgiTarget.Reset(); m_HdrHistory.Reset(); return false;
    }
    srv.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    device->CreateShaderResourceView(m_SsgiTarget.Get(), &srv, m_SsgiSrvCpu);

    if (!m_HistSrvGpu.ptr && !rm->AllocateSrvSlot(m_HistSrvCpu, m_HistSrvGpu)) {
        OutputDebugStringA("[Renderer3D] SRV heap full -- SSGI disabled.\n");
        m_SsgiTarget.Reset(); m_HdrHistory.Reset(); return false;
    }
    srv.Format = RendererCore::HdrFormat;
    device->CreateShaderResourceView(m_HdrHistory.Get(), &srv, m_HistSrvCpu);
    // ---- denoise targets: a PAIR, ping-ponged so one holds history while the other is written ----
    td.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    td.Flags  = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!m_SsgiBlurRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC brh = {};
        brh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; brh.NumDescriptors = 2;
        ThrowIfFailed(device->CreateDescriptorHeap(&brh, IID_PPV_ARGS(&m_SsgiBlurRtvHeap)));
    }
    const UINT brtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    for (int i = 0; i < 2; ++i) {
        m_SsgiBlurTarget[i].Reset();
        ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_SsgiBlurTarget[i])));
        m_SsgiBlurTarget[i]->SetName(L"SSGI denoised (ping-pong)");
        CD3DX12_CPU_DESCRIPTOR_HANDLE brtv(m_SsgiBlurRtvHeap->GetCPUDescriptorHandleForHeapStart(),
                                           i, brtvStride);
        device->CreateRenderTargetView(m_SsgiBlurTarget[i].Get(), nullptr, brtv);

        if (!m_SsgiBlurSrvGpu[i].ptr &&
            !rm->AllocateSrvSlot(m_SsgiBlurSrvCpu[i], m_SsgiBlurSrvGpu[i])) {
            OutputDebugStringA("[Renderer3D] SRV heap full -- SSGI disabled.\n");
            m_SsgiTarget.Reset(); m_HdrHistory.Reset();
            m_SsgiBlurTarget[0].Reset(); m_SsgiBlurTarget[1].Reset();
            return false;
        }
        device->CreateShaderResourceView(m_SsgiBlurTarget[i].Get(), &srv, m_SsgiBlurSrvCpu[i]);
    }
    // Both targets hold garbage at a new size, so the first frame after this must not blend against
    // them -- reprojecting into a history that describes a different resolution is pure smear.
    m_SsgiHistoryValid = false;

    if (!m_SsgiBlurCB[0]) {
        const UINT cbSize = (UINT)((sizeof(SsgiBlurCBData) + 255) & ~255u);
        CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        for (int i = 0; i < RendererCore::NumFrames; ++i) {
            CD3DX12_RESOURCE_DESC b = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
            ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &b,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_SsgiBlurCB[i])));
            ThrowIfFailed(m_SsgiBlurCB[i]->Map(0, nullptr, &m_SsgiBlurCBMapped[i]));
        }
    }

    m_SsgiWidth = width; m_SsgiHeight = height;
    return true;
}


// ============================ FXAA ============================
// The final pass. Reads the tonemapped LDR intermediate and resolves it to the back buffer.
// See FXAA_PS.hlsl for why this must run on gamma-encoded data, and the m_LdrTarget declaration for
// why one resource carries an _SRGB render-target view and a plain _UNORM shader view.

void Renderer3D::CreateFxaaPipeline() {
    auto* device = m_Core->GetDevice();

    CD3DX12_DESCRIPTOR_RANGE rScene;
    rScene.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_ROOT_PARAMETER fp[2] = {};
    fp[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);            // b0: texel size + thresholds
    fp[1].InitAsDescriptorTable(1, &rScene, D3D12_SHADER_VISIBILITY_PIXEL);   // t0
    // LINEAR, not point: the edge-blend taps land BETWEEN texels by design -- that sub-texel
    // interpolation is where the smoothing actually comes from. A point sampler here reduces FXAA
    // to picking whole neighbours and it stops working.
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD           = D3D12_FLOAT32_MAX;
    ss.ShaderRegister   = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    CD3DX12_ROOT_SIGNATURE_DESC sd;
    sd.Init(2, fp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&sd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); ThrowIfFailed(hr); }
    ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&m_FxaaRootSig)));

    auto vs = ReadFile(ExeRelative(L"shaders\\SSAO_VS.cso"));   // shared fullscreen triangle
    auto ps = ReadFile(ExeRelative(L"shaders\\FXAA_PS.cso"));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_FxaaRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { ps.data(), ps.size() };
    pso.InputLayout           = { nullptr, 0 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask            = UINT_MAX;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = RendererCore::BackBufferRTVFormat;
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count      = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_FxaaPso)));
    m_FxaaPso->SetName(L"FXAA PSO");

    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&m_FxaaAlloc[i])));
        ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_FxaaAlloc[i].Get(), nullptr, IID_PPV_ARGS(&m_FxaaList[i])));
        ThrowIfFailed(m_FxaaList[i]->Close());
        m_FxaaList[i]->SetName(L"FXAA pass");
    }
}

bool Renderer3D::CreateLdrTarget(UINT width, UINT height) {
    auto* device = m_Core->GetDevice();
    if (!device || width == 0 || height == 0) return false;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = width;
    td.Height           = height;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    // The RESOURCE is plain UNORM; the two VIEWS below differ. Same trick as the back buffer -- a
    // resource created _SRGB could not also be read raw.
    td.Format           = RendererCore::BackBufferFormat;
    td.SampleDesc.Count = 1;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = RendererCore::BackBufferRTVFormat;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    m_LdrTarget.Reset();
    ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&m_LdrTarget)));
    m_LdrTarget->SetName(L"LDR target (tonemap output, FXAA input)");

    if (!m_LdrRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC rh = {};
        rh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = 1;
        ThrowIfFailed(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m_LdrRtvHeap)));
    }
    // EXPLICIT desc: passing nullptr would inherit the resource's plain UNORM format and silently
    // skip the sRGB encode -- the same trap documented on RendererCore::BackBufferRTVFormat.
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format        = RendererCore::BackBufferRTVFormat;   // _UNORM_SRGB: encode on write
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_LdrTarget.Get(), &rtvDesc,
                                   m_LdrRtvHeap->GetCPUDescriptorHandleForHeapStart());

    auto* rm = m_Core->GetRenderer2D()->GetResourceManager();
    if (!m_LdrSrvGpu.ptr) {
        if (!rm->AllocateSrvSlot(m_LdrSrvCpu, m_LdrSrvGpu)) {
            OutputDebugStringA("[Renderer3D] SRV heap full -- FXAA disabled.\n");
            m_LdrTarget.Reset();
            return false;
        }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                  = RendererCore::BackBufferFormat;   // plain UNORM: NO decode on read
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(m_LdrTarget.Get(), &srv, m_LdrSrvCpu);

    m_LdrWidth = width; m_LdrHeight = height;
    return true;
}

void Renderer3D::RecordFxaaPass(int frame, D3D12_CPU_DESCRIPTOR_HANDLE backRtv,
                                UINT width, UINT height) {
    auto* cmd = m_FxaaList[frame].Get();
    ThrowIfFailed(m_FxaaAlloc[frame]->Reset());
    ThrowIfFailed(cmd->Reset(m_FxaaAlloc[frame].Get(), m_FxaaPso.Get()));

    // The LDR target rests in RENDER_TARGET (the tonemap pass just wrote it) and is restored before
    // this list ends, so the tonemap pass needs no knowledge of this one.
    auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(m_LdrTarget.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &toRead);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)width, (LONG)height };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = { m_Core->GetRenderer2D()->GetSrvHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_FxaaRootSig.Get());
    const float consts[4] = { 1.0f / (float)width, 1.0f / (float)height,
                              m_FxaaThreshold, m_FxaaThresholdMin };
    cmd->SetGraphicsRoot32BitConstants(0, 4, consts, 0);
    cmd->SetGraphicsRootDescriptorTable(1, m_LdrSrvGpu);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);

    auto backToRt = CD3DX12_RESOURCE_BARRIER::Transition(m_LdrTarget.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &backToRt);

    ThrowIfFailed(cmd->Close());
}


// ============================ Bloom ============================
// Progressive downsample/upsample over the FP16 scene target (Jimenez, "Next Generation Post
// Processing in Call of Duty"), composited back in Tonemap_PS BEFORE the curve.
//
// The chain runs HDR -> bloom[0] (prefilter + threshold) -> bloom[1] .. bloom[N-1] (downsample),
// then back up bloom[N-1] -> ... -> bloom[0] with ADDITIVE blending. Summing every level is what
// gives a response that looks like light: the small levels are a tight core, the large ones a wide
// faint halo, and real glare is that whole stack rather than any one blur radius.
//
// Every target rests in PIXEL_SHADER_RESOURCE and is flipped to RENDER_TARGET only for the step
// that writes it, so each step is self-contained -- the same idiom as the shadow and SSAO passes.

void Renderer3D::CreateBloomPipeline() {
    auto* device = m_Core->GetDevice();

    // b0 = 8 root constants (BloomConsts), t0 = the source level. LINEAR CLAMP sampler: the whole
    // filter design assumes bilinear taps between texels, and clamp keeps the edge of the screen
    // from wrapping a bright pixel around to the opposite side as the chain shrinks.
    CD3DX12_DESCRIPTOR_RANGE rSrc;
    rSrc.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_ROOT_PARAMETER bp[2] = {};
    bp[0].InitAsConstants(8, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    bp[1].InitAsDescriptorTable(1, &rSrc, D3D12_SHADER_VISIBILITY_PIXEL);
    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD           = D3D12_FLOAT32_MAX;
    ss.ShaderRegister   = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    CD3DX12_ROOT_SIGNATURE_DESC sd;
    sd.Init(2, bp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE);   // no input layout: vertex ID only
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&sd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) { if (err) OutputDebugStringA((const char*)err->GetBufferPointer()); ThrowIfFailed(hr); }
    ThrowIfFailed(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(&m_BloomRootSig)));

    auto vs   = ReadFile(ExeRelative(L"shaders\\SSAO_VS.cso"));   // shared fullscreen triangle
    auto down = ReadFile(ExeRelative(L"shaders\\BloomDownsample_PS.cso"));
    auto up   = ReadFile(ExeRelative(L"shaders\\BloomUpsample_PS.cso"));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature        = m_BloomRootSig.Get();
    pso.VS                    = { vs.data(), vs.size() };
    pso.PS                    = { down.data(), down.size() };
    pso.InputLayout           = { nullptr, 0 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);   // downsample REPLACES
    pso.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask            = UINT_MAX;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = RendererCore::HdrFormat;   // the chain stays FP16 end to end
    pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count      = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_BloomDownPso)));
    m_BloomDownPso->SetName(L"Bloom downsample PSO");

    // The upsample PSO differs from the downsample one ONLY in the shader and the blend state.
    // ONE/ONE additive IS the accumulation step -- that is why the upsample shader never reads its
    // destination and the chain needs no ping-pong target.
    pso.PS = { up.data(), up.size() };
    CD3DX12_BLEND_DESC addBlend(D3D12_DEFAULT);
    addBlend.RenderTarget[0].BlendEnable    = TRUE;
    addBlend.RenderTarget[0].SrcBlend       = D3D12_BLEND_ONE;
    addBlend.RenderTarget[0].DestBlend      = D3D12_BLEND_ONE;
    addBlend.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
    addBlend.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
    addBlend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    addBlend.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
    pso.BlendState = addBlend;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_BloomUpPso)));
    m_BloomUpPso->SetName(L"Bloom upsample PSO");

    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&m_BloomAlloc[i])));
        ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_BloomAlloc[i].Get(), nullptr, IID_PPV_ARGS(&m_BloomList[i])));
        ThrowIfFailed(m_BloomList[i]->Close());
        m_BloomList[i]->SetName(L"Bloom chain");
    }
}

bool Renderer3D::CreateBloomTargets(UINT width, UINT height) {
    auto* device = m_Core->GetDevice();
    if (!device || width < 4 || height < 4) return false;

    if (!m_BloomRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC rh = {};
        rh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = kBloomMips;
        ThrowIfFailed(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m_BloomRtvHeap)));
    }
    const UINT rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto* rm = m_Core->GetRenderer2D()->GetResourceManager();

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = RendererCore::HdrFormat;   // black; must match what the passes clear to
    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    m_BloomLevels = 0;
    for (UINT i = 0; i < kBloomMips; ++i) {
        const UINT w = width  >> (i + 1);
        const UINT h = height >> (i + 1);
        // Stop before the level gets degenerate. A 4x4 target has no meaningful filter footprint
        // left, and on a small window that can happen well before kBloomMips levels exist.
        if (w < 4 || h < 4) break;

        D3D12_RESOURCE_DESC td = {};
        td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width            = w;
        td.Height           = h;
        td.DepthOrArraySize = 1;
        td.MipLevels        = 1;
        td.Format           = RendererCore::HdrFormat;
        td.SampleDesc.Count = 1;
        td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        m_BloomTex[i].Reset();
        ThrowIfFailed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_BloomTex[i])));
        m_BloomTex[i]->SetName(L"Bloom level");

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(m_BloomRtvHeap->GetCPUDescriptorHandleForHeapStart(),
                                          (INT)i, rtvStride);
        device->CreateRenderTargetView(m_BloomTex[i].Get(), nullptr, rtv);

        // Slot REUSED across resizes (AllocateSrvSlot has no free list) -- allocate only the first time.
        if (!m_BloomSrvGpu[i].ptr) {
            if (!rm->AllocateSrvSlot(m_BloomSrvCpu[i], m_BloomSrvGpu[i])) {
                OutputDebugStringA("[Renderer3D] SRV heap full -- bloom disabled.\n");
                for (UINT k = 0; k <= i; ++k) m_BloomTex[k].Reset();
                m_BloomLevels = 0;
                return false;
            }
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format                  = RendererCore::HdrFormat;
        srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels     = 1;
        device->CreateShaderResourceView(m_BloomTex[i].Get(), &srv, m_BloomSrvCpu[i]);

        m_BloomW[i] = w; m_BloomH[i] = h;
        ++m_BloomLevels;
    }
    return m_BloomLevels > 0;
}

void Renderer3D::RecordBloomPass(int frame) {
    auto* cmd = m_BloomList[frame].Get();
    ThrowIfFailed(m_BloomAlloc[frame]->Reset());
    ThrowIfFailed(cmd->Reset(m_BloomAlloc[frame].Get(), m_BloomDownPso.Get()));

    ID3D12DescriptorHeap* heaps[] = { m_Core->GetRenderer2D()->GetSrvHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_BloomRootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);

    const UINT rtvStride =
        m_Core->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto levelRtv = [&](UINT i) {
        return CD3DX12_CPU_DESCRIPTOR_HANDLE(
            m_BloomRtvHeap->GetCPUDescriptorHandleForHeapStart(), (INT)i, rtvStride);
    };
    auto setViewport = [&](UINT w, UINT h) {
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
        D3D12_RECT     sc = { 0, 0, (LONG)w, (LONG)h };
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
    };
    auto toTarget = [&](ID3D12Resource* r) {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(r,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &b);
    };
    auto toRead = [&](ID3D12Resource* r) {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(r,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &b);
    };

    // The HDR target rests in RENDER_TARGET and is put BACK before this list ends, so the tonemap
    // pass's own barriers stay valid without either pass knowing about the other.
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_HdrTarget.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &b);
    }

    // ---- prefilter: full-res HDR -> bloom[0], applying the threshold ----
    {
        toTarget(m_BloomTex[0].Get());
        auto rtv = levelRtv(0);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        setViewport(m_BloomW[0], m_BloomH[0]);
        cmd->SetPipelineState(m_BloomDownPso.Get());
        // Texel size is the SOURCE's (the full-res HDR target), not the destination's.
        BloomConsts c{ 1.0f / (float)m_HdrWidth, 1.0f / (float)m_HdrHeight,
                       m_BloomThreshold, m_BloomKnee, 1.0f, m_BloomScatter, 0.0f, 0.0f };
        cmd->SetGraphicsRoot32BitConstants(0, 8, &c, 0);
        cmd->SetGraphicsRootDescriptorTable(1, m_HdrSrvGpu);
        cmd->DrawInstanced(3, 1, 0, 0);
        toRead(m_BloomTex[0].Get());
    }

    // ---- downsample: bloom[i] -> bloom[i+1] ----
    for (UINT i = 0; i + 1 < m_BloomLevels; ++i) {
        toTarget(m_BloomTex[i + 1].Get());
        auto rtv = levelRtv(i + 1);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        setViewport(m_BloomW[i + 1], m_BloomH[i + 1]);
        cmd->SetPipelineState(m_BloomDownPso.Get());
        BloomConsts c{ 1.0f / (float)m_BloomW[i], 1.0f / (float)m_BloomH[i],
                       m_BloomThreshold, m_BloomKnee, 0.0f, m_BloomScatter, 0.0f, 0.0f };
        cmd->SetGraphicsRoot32BitConstants(0, 8, &c, 0);
        cmd->SetGraphicsRootDescriptorTable(1, m_BloomSrvGpu[i]);
        cmd->DrawInstanced(3, 1, 0, 0);
        toRead(m_BloomTex[i + 1].Get());
    }

    // ---- upsample + ACCUMULATE: bloom[i+1] added into bloom[i], smallest first ----
    // Additive blend, so each level is summed into the one above rather than replacing it. Walking
    // downward (i from the second-smallest to 0) means bloom[0] ends up holding the whole stack.
    for (int i = (int)m_BloomLevels - 2; i >= 0; --i) {
        toTarget(m_BloomTex[i].Get());
        auto rtv = levelRtv((UINT)i);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        setViewport(m_BloomW[i], m_BloomH[i]);
        cmd->SetPipelineState(m_BloomUpPso.Get());
        BloomConsts c{ 1.0f / (float)m_BloomW[i + 1], 1.0f / (float)m_BloomH[i + 1],
                       m_BloomThreshold, m_BloomKnee, 0.0f, m_BloomScatter, 0.0f, 0.0f };
        cmd->SetGraphicsRoot32BitConstants(0, 8, &c, 0);
        cmd->SetGraphicsRootDescriptorTable(1, m_BloomSrvGpu[i + 1]);
        cmd->DrawInstanced(3, 1, 0, 0);
        toRead(m_BloomTex[i].Get());
    }

    // Hand the HDR target back in the state the tonemap pass expects to find it.
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(m_HdrTarget.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &b);
    }

    ThrowIfFailed(cmd->Close());
}


// ============================ Point-light shadows (depth cube map) ============================
// Six 90-degree perspective renders from the light, one per axis direction, into the six faces of a
// cube map. Six 90-degree frustums exactly tile a sphere, so together they capture everything the
// light can see. The pixel shader then samples the cube by DIRECTION (light -> surface) instead of
// by face, and the hardware picks the right face for it.
//
// This is Approach A -- six passes -- rather than a geometry-shader/layered single pass. The reason
// is architectural: the six passes are completely independent, so they record in PARALLEL on the
// scheduler, which turns the "6x draw call submission" cost into ~1x wall time. The GS route trades
// that CPU cost for vertex amplification on the GPU, which is a worse trade on hardware where
// geometry shaders have poor throughput.

void Renderer3D::CreateCubeShadowResources() {
    ID3D12Device2* device = m_Core->GetDevice();

    // TYPELESS again: written as 6 depth views, read as one cube SRV.
    D3D12_RESOURCE_DESC td = {};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = kCubeShadowSize;
    td.Height           = kCubeShadowSize;
    td.DepthOrArraySize = 6;                     // the six faces
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format             = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;

    CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_CubeShadowMap)));

    // One DSV per face -- a depth pass can only target a single 2D slice at a time.
    D3D12_DESCRIPTOR_HEAP_DESC dh = {};
    dh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dh.NumDescriptors = 6;
    ThrowIfFailed(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&m_CubeDsvHeap)));

    const UINT dsvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (UINT f = 0; f < 6; ++f) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format                         = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = f;
        dsv.Texture2DArray.ArraySize       = 1;
        CD3DX12_CPU_DESCRIPTOR_HANDLE h(m_CubeDsvHeap->GetCPUDescriptorHandleForHeapStart(), f, dsvStride);
        device->CreateDepthStencilView(m_CubeShadowMap.Get(), &dsv, h);
    }

    // ONE SRV over the whole thing, viewed as a cube -- that's what lets the shader sample by direction.
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    if (!m_Core->GetRenderer2D()->GetResourceManager()->AllocateSrvSlot(srvCpu, m_CubeSrvGpu)) {
        OutputDebugStringA("[Renderer3D] SRV heap full -- point shadows disabled.\n");
        m_CubeShadowMap.Reset();
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format                      = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension               = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping     = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MipLevels       = 1;
    device->CreateShaderResourceView(m_CubeShadowMap.Get(), &srv, srvCpu);

    // Per-face constant buffers: six matrices per frame, each 256-byte aligned so a face can bind
    // its own region as a CBV.
    const UINT stride = 256;
    CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC   cbDesc = CD3DX12_RESOURCE_DESC::Buffer(stride * 6);
    for (int i = 0; i < RendererCore::NumFrames; ++i) {
        ThrowIfFailed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_CubeCB[i])));
        ThrowIfFailed(m_CubeCB[i]->Map(0, nullptr, &m_CubeCBMapped[i]));
        for (int f = 0; f < 6; ++f) {
            ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&m_CubeAlloc[i][f])));
            ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_CubeAlloc[i][f].Get(), nullptr, IID_PPV_ARGS(&m_CubeList[i][f])));
            ThrowIfFailed(m_CubeList[i][f]->Close());
        }
    }
}

// Records ONE face. Split out so the six can run as independent scheduler tasks -- they share only
// read-only state (m_Batches, the instance buffer) and each writes its own command list.
void Renderer3D::RecordCubeFace(int frame, int face) {
    auto* cmd = m_CubeList[frame][face].Get();
    ThrowIfFailed(m_CubeAlloc[frame][face]->Reset());
    ThrowIfFailed(cmd->Reset(m_CubeAlloc[frame][face].Get(), m_ShadowPso.Get()));

    // The cube's resource state must be flipped ONCE around the whole six-face group, not per face.
    // The lists execute in submission order, so face 0 opens the window and face 5 closes it --
    // putting the transition in every list would double-transition and trip the debug layer.
    if (face == 0) {
        CD3DX12_RESOURCE_BARRIER toDepth = CD3DX12_RESOURCE_BARRIER::Transition(
            m_CubeShadowMap.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmd->ResourceBarrier(1, &toDepth);
    }

    const UINT dsvStride = m_Core->GetDevice()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(m_CubeDsvHeap->GetCPUDescriptorHandleForHeapStart(),
                                      face, dsvStride);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)kCubeShadowSize, (float)kCubeShadowSize, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)kCubeShadowSize, (LONG)kCubeShadowSize };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    cmd->SetGraphicsRootSignature(m_ShadowRootSig.Get());
    cmd->SetGraphicsRootConstantBufferView(0,
        m_CubeCB[frame]->GetGPUVirtualAddress() + (UINT64)face * 256);   // this face's matrix
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VERTEX_BUFFER_VIEW instVBV{};
    instVBV.BufferLocation = m_InstanceBuffer[frame]->GetGPUVirtualAddress();
    instVBV.StrideInBytes  = sizeof(DirectX::XMFLOAT4X4);
    instVBV.SizeInBytes    = kMaxInstances * (UINT)sizeof(DirectX::XMFLOAT4X4);
    cmd->IASetVertexBuffers(1, 1, &instVBV);

    for (const InstanceBatch& b : m_Batches) {
        if (!b.mesh || b.count == 0) continue;
        cmd->IASetVertexBuffers(0, 1, &b.mesh->vertexBufferView);
        cmd->IASetIndexBuffer(&b.mesh->indexBufferView);
        cmd->DrawIndexedInstanced(b.mesh->indexCount, b.count, 0, 0, b.firstInstance);
    }

    if (face == 5) {   // last list of the group: hand the cube back to the pixel shader
        CD3DX12_RESOURCE_BARRIER toRead = CD3DX12_RESOURCE_BARRIER::Transition(
            m_CubeShadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd->ResourceBarrier(1, &toRead);
    }
    ThrowIfFailed(cmd->Close());
}

void Renderer3D::RecordCubeFaceTask(void* data) {
    CubeFaceCtx* c = static_cast<CubeFaceCtx*>(data);
    c->self->RecordCubeFace(c->frame, c->face);
}

void Renderer3D::RecordCubeShadowPass(int frame) {
    const GpuLight& L = m_Lights[m_ShadowLightIndex];
    DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&L.position);

    // The six face directions in cube-map order (+X,-X,+Y,-Y,+Z,-Z) with their conventional up
    // vectors. These MUST match D3D's cube face ordering or the shader samples the wrong face.
    static const DirectX::XMFLOAT3 dirs[6] = {
        {  1,  0,  0 }, { -1,  0,  0 }, {  0,  1,  0 }, {  0, -1,  0 }, {  0,  0,  1 }, {  0,  0, -1 }
    };
    static const DirectX::XMFLOAT3 ups[6] = {
        {  0,  1,  0 }, {  0,  1,  0 }, {  0,  0, -1 }, {  0,  0,  1 }, {  0,  1,  0 }, {  0,  1,  0 }
    };

    // 90 degrees exactly: any wider and faces overlap, any narrower and gaps appear at the seams.
    // Near plane pushed well off 0.1 for the same precision reason as the spot path -- a perspective
    // depth buffer spends most of its resolution just past the near plane, so a tiny near plane
    // starves the far end and produces acne at distance.
    m_ShadowFar  = L.range > 1.0f ? L.range : 1.0f;
    m_ShadowNear = (m_ShadowFar * 0.02f > 0.5f) ? m_ShadowFar * 0.02f : 0.5f;
    DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XM_PIDIV2, 1.0f, m_ShadowNear, m_ShadowFar);

    uint8_t* dst = reinterpret_cast<uint8_t*>(m_CubeCBMapped[frame]);
    for (int f = 0; f < 6; ++f) {
        DirectX::XMVECTOR d  = DirectX::XMLoadFloat3(&dirs[f]);
        DirectX::XMVECTOR up = DirectX::XMLoadFloat3(&ups[f]);
        DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, DirectX::XMVectorAdd(eye, d), up);
        DirectX::XMFLOAT4X4 vp;
        DirectX::XMStoreFloat4x4(&vp, view * proj);
        memcpy(dst + (size_t)f * 256, &vp, sizeof(vp));
    }

    // Record the six faces. They touch only read-only shared state, so they go out as tasks and the
    // wall time is one face rather than six -- the reason the "expensive" approach is affordable here.
    JLib::TaskScheduler& sched = JLib::TaskScheduler::Instance();
    JLib::WaitGroup wg;
    for (int f = 0; f < 6; ++f) {
        m_CubeFaceCtx[f] = { this, frame, f };
        auto* t = sched.CreateTask(&Renderer3D::RecordCubeFaceTask, &m_CubeFaceCtx[f], true);
        t->waitGroup = &wg;
        wg.n.fetch_add(1, std::memory_order_release);
        sched.Push(t);
    }
    sched.WaitFor(wg);
}

}

