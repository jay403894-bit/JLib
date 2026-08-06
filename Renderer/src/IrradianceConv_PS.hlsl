// Step 2 of the IBL bake: environment cubemap -> DIFFUSE IRRADIANCE cubemap.
//
// This is the term that replaces flat/hemisphere ambient. For a given surface normal, it answers
// "summing every direction in the hemisphere above me, weighted by cosine, how much light arrives?"
// Because that answer depends only on the normal, it can be baked into a cubemap once and then read
// with a single sample per pixel at runtime -- which is the entire reason IBL is affordable.
//
// The output is TINY (32x32 per face is the usual choice, and what this is built at): cosine-weighted
// irradiance is extremely low-frequency, so there is nothing high-resolution left to preserve. A big
// irradiance map is wasted memory, not extra quality.
cbuffer CubeFaceCB : register(b0) {
    float3 gFwd;    float gRoughness;   // unused here
    float3 gRight;  float gMipCount;    // unused here
    float3 gUp;     float gSourceSize;
}

TextureCube  gEnv    : register(t0);
SamplerState gLinear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

static const float PI = 3.14159265359f;

float4 PSMain(VSOut input) : SV_Target {
    float2 t = input.uv * 2.0f - 1.0f;
    float3 N = normalize(gFwd + gRight * t.x - gUp * t.y);

    // Tangent basis around N. The up-vector pick has to avoid being parallel to N, or the cross
    // product collapses and the basis degenerates -- that shows up as a corrupted pole on two faces.
    float3 up    = abs(N.y) > 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    float3 right = normalize(cross(up, N));
    up           = cross(N, right);

    // Riemann sum over the hemisphere in spherical coordinates. 0.025 rad steps is ~1800 samples --
    // heavy, but this runs ONCE at load for a 32x32x6 target, so it is a few milliseconds total.
    float3 irradiance = 0.0f;
    float  samples    = 0.0f;
    const float dPhi   = 0.025f;
    const float dTheta = 0.025f;

    // [loop] is NOT optional here: left to itself dxc tries to fully unroll ~1800 iterations of
    // sincos plus a cube sample, which takes the shader compiler minutes and emits an enormous
    // shader. This is a real loop -- the trip count is fixed but large, and unrolling buys nothing.
    [loop]
    for (float phi = 0.0f; phi < 2.0f * PI; phi += dPhi) {
        float sinPhi, cosPhi;
        sincos(phi, sinPhi, cosPhi);
        [loop]
        for (float theta = 0.0f; theta < 0.5f * PI; theta += dTheta) {
            float sinTheta, cosTheta;
            sincos(theta, sinTheta, cosTheta);
            // Tangent-space direction -> world.
            float3 tangentDir = float3(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
            float3 worldDir   = tangentDir.x * right + tangentDir.y * up + tangentDir.z * N;
            // cos(theta) is the Lambert term; sin(theta) is the solid-angle measure of this ring.
            // Dropping the sin is the classic bug -- it over-weights the pole and washes everything out.
            irradiance += gEnv.SampleLevel(gLinear, worldDir, 0).rgb * cosTheta * sinTheta;
            samples    += 1.0f;
        }
    }

    // PI cancels the Lambert 1/PI applied at shading time, so the shader can use this directly.
    irradiance = PI * irradiance / max(samples, 1.0f);
    return float4(irradiance, 1.0f);
}
