// Same GlobalUniforms layout as VertexShader.hlsl, kept identical so both shaders can share the
// SAME constant buffer (m_ConstantBuffers[frame]) without a second upload. cameraPos/cameraZoom
// are what this shader actually needs them for (see VSMain): particles are simulated entirely in
// WORLD space on the GPU (UpdateParticles.hlsl/WaveParticles.hlsl just do position += velocity*dt,
// which doesn't care what space it's in), and nothing on the CPU ever reprojects a live
// particle's position through the camera the way Renderer2D::Submit does for sprites -- this is
// the one place camera-awareness has to live for particles.
cbuffer GlobalUniforms : register(b0)
{
    float time;
    float2 cameraPos;
    float padding1;
    float2 screenSize;
    float aspectRatio;
    float cameraZoom;
};

// Packed per-pool via RendererCore::RecordParticleDraw's SetGraphicsRoot32BitConstants(3, 8, ...) --
// atlasFramesX/Y describe a flipbook grid on the bound texture (1x1 = no animation, just a static
// sprite), colorEnd is the color/alpha this pool's particles lerp toward over their lifetime.
// screenSpace (see RendererCore::ParticleEffectPool::screenSpace): 0 = normal, world-space
// particles that scroll/rescale with the camera (the common case). 1 = this pool's
// Particle2D::position is ALREADY in screen pixels -- VSMain skips the camera transform
// entirely, for effects that decorate the view itself (lens grime, damage vignette, menu
// background) rather than existing at a location in the scene.
// screenSpace/_pad0 are placed BEFORE colorEnd, not after: HLSL cbuffers pack into 16-byte
// registers and never let a member straddle a register boundary. atlasFramesX+atlasFramesY only
// fill 8 of the first register's 16 bytes, so a float4 placed right after them would get silently
// padded to the NEXT register by the compiler -- while the C++-side struct packs everything
// tightly with no such gap, so the two layouts silently disagree on where screenSpace lives.
// Filling the first register's remaining 8 bytes with screenSpace/_pad0 keeps every offset
// exactly matching the plain C++ struct with no compiler-inserted padding on either side.
cbuffer AnimParams : register(b1)
{
    uint atlasFramesX;
    uint atlasFramesY;
    uint screenSpace;
    uint _pad0;
    float4 colorEnd;
};

struct Particle2D
{
    float2 position;
    float2 velocity;
    float lifetime;
    float age; // seconds since spawn -- unused here, kept for layout parity with the compute shaders
    uint isActive;
    float size;    // quad side length in pixels -- see the corner-offset table below
    float4 color; // set per-spawn via RequestSpawn -- must stay in sync with the CS structs
};

// Read-only view of the SAME buffer the update shaders write as a UAV -- RendererCore::UpdateParticles()
// transitions it UNORDERED_ACCESS -> NON_PIXEL_SHADER_RESOURCE after the compute dispatch, every frame,
// before this draw runs.
StructuredBuffer<Particle2D> ParticleBuffer : register(t0);

struct VSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

// No vertex/index buffer bound -- each particle is a procedurally-generated quad (2 triangles,
// 6 vertices), SV_VertexID picks which corner. This is what actually gives particles a
// controllable SIZE: D3D12's POINTLIST topology (the previous approach) draws a fixed ~1px dot
// with no size or UV support at all, so there was no way to make a point-list particle bigger.
// Corner order matches a triangle list: (0,1,2) then (2,1,3) of a quad laid out
// TL(0) TR(1)
// BL(2) BR(3)
static const float2 kCornerOffsets[6] = {
    float2(-0.5f,  0.5f), float2(0.5f,  0.5f), float2(-0.5f, -0.5f), // triangle 1: TL, TR, BL
    float2(-0.5f, -0.5f), float2(0.5f,  0.5f), float2(0.5f, -0.5f), // triangle 2: BL, TR, BR
};
// Same winding/order as kCornerOffsets above -- standard texture-space UVs (0,0)=top-left.
static const float2 kCornerUVs[6] = {
    float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(0.0f, 1.0f),
    float2(0.0f, 1.0f), float2(1.0f, 0.0f), float2(1.0f, 1.0f),
};

VSOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    VSOutput output;
    Particle2D p = ParticleBuffer[instanceID];

    // screenSpace pools (see AnimParams comment above) skip the camera transform entirely --
    // p.position is already in screen pixels for those, same as the old pre-camera behavior.
    // Otherwise p.position/p.size are WORLD-space: offset by cameraPos, scale by cameraZoom (so
    // particles get visibly bigger/smaller as you zoom, not just further apart -- matches a real
    // camera, unlike Renderer2D's current sprite path, which only offsets position).
    float2 localPos = p.position + kCornerOffsets[vertexID] * p.size;
    float2 screenPos = screenSpace != 0
        ? localPos
        : (localPos - cameraPos) * cameraZoom + screenSize * 0.5f;

    float2 ndc;
    ndc.x = (screenPos.x / screenSize.x) * 2.0f - 1.0f;
    ndc.y = 1.0f - (screenPos.y / screenSize.y) * 2.0f;

    output.pos = float4(ndc, 0.0f, 1.0f);

    // age+lifetime is invariant (both tick by the same DeltaTime in opposite directions), so this
    // gives a clean 0 (just spawned) -> 1 (about to die) progress value without needing to store
    // the particle's original spawn lifetime anywhere.
    float progress = saturate(p.age / max(p.age + p.lifetime, 0.0001f));

    uint totalFrames = max(atlasFramesX * atlasFramesY, 1u);
    uint frameIndex = min((uint)(progress * totalFrames), totalFrames - 1);
    uint frameX = frameIndex % atlasFramesX;
    uint frameY = frameIndex / atlasFramesX;
    float2 frameSize = float2(1.0f / atlasFramesX, 1.0f / atlasFramesY);
    float2 frameOrigin = float2(frameX, frameY) * frameSize;
    output.uv = frameOrigin + kCornerUVs[vertexID] * frameSize;

    float4 animColor = lerp(p.color, colorEnd, progress);
    // Inactive particles (never spawned, or dead and awaiting reuse) must be invisible, not just
    // faded: they still occupy a real slot in the buffer with a real seeded lifetime, so without
    // this check every unspawned particle would draw at full alpha, burying any actually-active
    // ones in a permanent field of static quads.
    float alpha = (p.isActive == 1) ? animColor.a : 0.0f;
    output.color = float4(animColor.rgb, alpha);
    return output;
}
