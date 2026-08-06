// Particle3D_VS.hlsl -- world-space, CAMERA-FACING billboards for the 3D GPU particles. No vertex buffer:
// each particle is one instance (SV_InstanceID), expanded to a quad (6 verts, SV_VertexID). The corners are
// offset from the particle center along the camera's WORLD-space right/up axes, so the quad always faces the
// camera and scales correctly with perspective (mul by ViewProj after the world-space offset). Drawn in the
// 3D pass against the scene depth buffer, so opaque geometry occludes particles.

cbuffer ParticleCam : register(b0) {
    row_major float4x4 ViewProj;
    float3 gCamRight; float _p0;    // camera world-space right (RendererCore derives it from InvViewProj)
    float3 gCamUp;    float _p1;    // camera world-space up
}

// Per-pool draw params: colorEnd (particles lerp spawn-color -> this by age) + an atlas flipbook grid.
cbuffer ParticleDraw : register(b1) {
    float4 gColorEnd;
    uint   gAtlasX; uint gAtlasY; uint2 _pd;
}

struct Particle3D {
    float3 position; float size;
    float3 velocity; float lifetime;
    float  age; uint isActive; float2 _pad;
    float4 color;
};
StructuredBuffer<Particle3D> Particles : register(t0);   // read-only view of the compute-written buffer

// Same corner/UV tables as the 2D particle VS (two triangles of a quad; UV 0,0 = top-left).
static const float2 kCorner[6] = {
    float2(-0.5f, 0.5f), float2(0.5f, 0.5f), float2(-0.5f, -0.5f),
    float2(-0.5f, -0.5f), float2(0.5f, 0.5f), float2(0.5f, -0.5f),
};
static const float2 kUV[6] = {
    float2(0, 0), float2(1, 0), float2(0, 1),
    float2(0, 1), float2(1, 0), float2(1, 1),
};

struct VSOutput {
    float4 clip  : SV_POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

VSOutput VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    Particle3D p = Particles[iid];
    float2 c = kCorner[vid] * p.size;
    float3 world = p.position + gCamRight * c.x + gCamUp * c.y;   // camera-facing world-space corner

    VSOutput o;
    o.clip = mul(float4(world, 1.0f), ViewProj);

    // Life progress 0 (spawned) -> 1 (dying); drives the color lerp and the atlas frame (same trick as 2D).
    float progress = saturate(p.age / max(p.age + p.lifetime, 1e-4f));
    uint frames = max(gAtlasX * gAtlasY, 1u);
    uint fi     = min((uint)(progress * frames), frames - 1);
    float2 fsz  = float2(1.0f / gAtlasX, 1.0f / gAtlasY);
    float2 forg = float2(fi % gAtlasX, fi / gAtlasX) * fsz;
    o.uv = forg + kUV[vid] * fsz;

    float4 col = lerp(p.color, gColorEnd, progress);
    o.color = float4(col.rgb, (p.isActive == 1u) ? col.a : 0.0f);   // dead/inactive slots draw invisible
    return o;
}
