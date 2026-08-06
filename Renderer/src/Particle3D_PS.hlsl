// Particle3D_PS.hlsl -- phase 1 keeps it texture-free: a SOFT RADIAL falloff computed from the quad UVs
// turns each hard billboard into a round, glowing dot (no texture bind, no descriptor heap). The PSO uses
// alpha blending with depth TEST on but depth WRITE off, so particles are occluded by opaque geometry yet
// don't occlude each other. Phase 2 can swap this for a textured sample (the VS already carries atlas UVs).
struct PSInput {
    float4 clip  : SV_POSITION;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

float4 PSMain(PSInput input) : SV_TARGET {
    float2 d = input.uv - 0.5f;             // uv spans the quad 0..1 -> center the coords
    float  r = saturate(length(d) * 2.0f);  // 0 at center, 1 at the quad edge
    float  soft = 1.0f - r;
    soft = soft * soft;                      // smooth radial falloff -> round particle
    float4 c = input.color;
    c.a *= soft;
    if (c.a < 0.003f) discard;
    return c;
}
