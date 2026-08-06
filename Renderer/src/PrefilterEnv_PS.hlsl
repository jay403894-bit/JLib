// Step 3 of the IBL bake: environment cubemap -> PREFILTERED SPECULAR cubemap (one mip per roughness).
//
// The specular half of the split-sum approximation. A rough surface reflects a wide cone of the
// environment rather than a single direction, so instead of integrating that cone per pixel at
// runtime, the cone is pre-integrated into a mip chain: mip 0 = mirror (roughness 0), the last mip =
// fully rough. At shading time one SampleLevel with LOD = roughness * (mipCount-1) picks the right
// blur, which is why this costs a single texture read no matter how rough the material is.
//
// Run once per (mip, face). gRoughness is the roughness that mip represents.
cbuffer CubeFaceCB : register(b0) {
    float3 gFwd;    float gRoughness;
    float3 gRight;  float gMipCount;
    float3 gUp;     float gSourceSize;   // source cube face size, for the mip-selection heuristic
}

TextureCube  gEnv    : register(t0);
SamplerState gLinear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

static const float PI = 3.14159265359f;
static const uint  kSamples = 256u;

// Van der Corput / Hammersley low-discrepancy sequence. Random sampling would need far more samples
// for the same smoothness; a stratified sequence converges much faster for the same count.
float RadicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}
float2 Hammersley(uint i, uint n) { return float2(float(i) / float(n), RadicalInverse_VdC(i)); }

// Draw a half-vector from the GGX distribution for this roughness -- concentrating samples where the
// BRDF actually has energy instead of spreading them uniformly over the hemisphere.
float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness) {
    float a = roughness * roughness;
    float phi      = 2.0f * PI * Xi.x;
    float cosTheta = sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 H = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    float3 up    = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent   = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float DistributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-7f);
}

float4 PSMain(VSOut input) : SV_Target {
    float2 t = input.uv * 2.0f - 1.0f;
    float3 N = normalize(gFwd + gRight * t.x - gUp * t.y);
    // The standard approximation: assume the viewer is looking straight down the normal. It costs the
    // long grazing-angle reflection streaks, and buys being able to bake this at all -- otherwise the
    // result would depend on view direction and could not be a static cubemap.
    float3 V = N;

    float3 color      = 0.0f;
    float  totalWeight = 0.0f;

    // [loop] for the same reason as the irradiance pass: 256 iterations of importance sampling is a
    // real loop, and unrolling it makes the compile pathological for no runtime gain.
    [loop]
    for (uint i = 0u; i < kSamples; ++i) {
        float2 Xi = Hammersley(i, kSamples);
        float3 H  = ImportanceSampleGGX(Xi, N, gRoughness);
        float3 L  = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = saturate(dot(N, L));
        if (NdotL <= 0.0f) continue;

        // Sample a MIP of the source rather than mip 0. With a finite sample count, a high-frequency
        // environment (an HDRI's sun is a handful of very bright texels) aliases into sparkling
        // fireflies; picking a mip whose texel footprint matches this sample's solid angle
        // pre-averages that away. Same trick Karis describes for UE4.
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));
        float D     = DistributionGGX(NdotH, gRoughness);
        float pdf   = (D * NdotH / (4.0f * max(VdotH, 1e-4f))) + 1e-4f;

        float saTexel  = 4.0f * PI / (6.0f * gSourceSize * gSourceSize);
        float saSample = 1.0f / (float(kSamples) * pdf + 1e-4f);
        float mip      = (gRoughness == 0.0f) ? 0.0f : 0.5f * log2(saSample / saTexel);

        color       += gEnv.SampleLevel(gLinear, L, mip).rgb * NdotL;
        totalWeight += NdotL;
    }

    return float4(color / max(totalWeight, 1e-4f), 1.0f);
}
