// ============================================================================================
// Skybox_PS.hlsl -- procedural gradient sky from the view ray (no texture, no HDR asset). A horizon->
// zenith blend for the upper hemisphere, a dim ground tone below, plus a soft sun disc + glow. Tone-
// mapped with the SAME Reinhard curve as Basic3D_PS so the sky and the lit geometry share one range and
// the horizon meets the world cleanly. PURELY VISUAL -- this color never contributes to lighting (the
// single-global-environment/IBL approach was deliberately dropped; local lighting would supersede it).
// Swap ProceduralSky() for a TextureCube sample later if a sky HDR/cubemap is ever added.
// ============================================================================================

struct PSInput {
    float4 clip : SV_POSITION;
    float3 ray  : TEXCOORD0;
};

// The HDR environment, when one is loaded (Renderer3D::LoadEnvironment). Sampled as the ORIGINAL
// equirectangular image rather than the baked IBL cube: those cubes are deliberately tiny (512 for the
// environment, 128 for prefiltered) because convolution destroys detail anyway, and blowing a 128-texel
// face up across the screen would look like a smeared mess. The source is full resolution and costs one
// extra sample, so the visible sky stays sharp while IBL keeps its small bakes.
Texture2D    gSkyEquirect : register(t0);
SamplerState gSkySampler  : register(s0);

// gSkyHasEnv > 0.5 -> sample the HDRI; otherwise fall back to the procedural gradient below, so a
// scene without the asset still gets a sky.
cbuffer SkyExtra : register(b1) { float gSkyHasEnv; float gSkyIntensity; float2 _skypad; }

static const float kPI = 3.14159265359f;

// dir = normalized world-space view direction. Returns a linear (pre-tonemap) sky color.
float3 ProceduralSky(float3 dir) {
    const float3 zenith  = float3(0.18f, 0.38f, 0.78f);   // straight up
    const float3 horizon = float3(0.72f, 0.78f, 0.86f);   // pale band at eye level
    const float3 ground  = float3(0.25f, 0.24f, 0.22f);   // dim tone below the horizon

    float  up  = dir.y;
    float3 sky = (up >= 0.0f)
        ? lerp(horizon, zenith, pow(saturate(up), 0.45f))     // horizon -> zenith
        : lerp(horizon, ground, saturate(-up * 3.0f));        // horizon -> ground (quick falloff)

    // Soft sun: a tight bright disc (pow 1500) plus a wider, faint glow (pow 20) around it.
    const float3 sunDir = normalize(float3(0.4f, 0.7f, 0.35f));   // matches the scene's key-light direction
    float d = saturate(dot(dir, sunDir));
    sky += float3(1.0f, 0.95f, 0.85f) * (pow(d, 1500.0f) * 4.0f + pow(d, 20.0f) * 0.15f);
    return sky;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 dir = normalize(input.ray);

    float3 sky;
    if (gSkyHasEnv > 0.5f) {
        // Direction -> equirectangular UV, the same mapping EquirectToCube_PS uses for the bake. Doing
        // it identically here is what makes the visible sky and the lighting agree: both are reading
        // the same image through the same projection.
        float2 uv = float2(atan2(dir.z, dir.x), asin(clamp(dir.y, -1.0f, 1.0f)));
        uv.x = uv.x / (2.0f * kPI) + 0.5f;
        uv.y = 0.5f - uv.y / kPI;
        // SampleLevel, not Sample: the wrap seam at uv.x 1->0 puts a full-width derivative on one pixel
        // column, and implicit LOD would pick the smallest mip there -- a visible vertical stripe.
        sky = gSkyEquirect.SampleLevel(gSkySampler, uv, 0).rgb * gSkyIntensity;
    } else {
        sky = ProceduralSky(dir);
    }

    sky = sky / (sky + 1.0f);        // Reinhard -- same as Basic3D_PS so sky + geometry are consistent
    return float4(sky, 1.0f);
}
