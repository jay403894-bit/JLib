// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// Tonemapping: the pass that turns the FP16 scene into something a display can show.
//
// Everything before this writes LINEAR HDR into an R16G16B16A16_FLOAT intermediate -- values well
// past 1.0 are legal there, which is the whole point (a sunlit wall and a light bulb are genuinely
// different brightnesses, and 8-bit UNORM cannot hold both). This pass maps that unbounded range
// into 0..1 and writes the back buffer, whose RTV is _UNORM_SRGB so the hardware does the sRGB
// encode on write. Nothing here applies gamma by hand -- doing both is the classic double-encode.
//
// Drawn as ONE fullscreen triangle from SV_VertexID (SSAO_VS, shared), no vertex buffer.

Texture2D<float4> gHdr     : register(t0);
// The finished bloom chain (bloom[0], half-res). Bound to a valid texture even when bloom is off --
// D3D12 requires every declared descriptor table populated -- so gBloomIntensity, not the binding,
// is what turns it off.
Texture2D<float4> gBloom   : register(t1);
SamplerState      gSampler : register(s0);

// Root CONSTANTS, not a CB: a handful of live floats that change at most once per frame.
cbuffer TonemapParams : register(b0) {
    float  gExposure;       // linear multiplier applied BEFORE the curve (see SetExposure)
    float  gOperator;       // 0 = none (clamp), 1 = Reinhard, 2 = ACES, 3 = Uchimura, 4 = ACESFitted
    float  gBloomIntensity; // 0 = bloom off
    float  _pad;
};

struct PSInput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Reinhard: c / (c+1). Never clips, but desaturates and washes out the midtones -- everything
// crowds toward 0.5. Kept because it is what this renderer used inline in Basic3D_PS before the
// FP16 intermediate existed, so it is the A/B reference for "did the new curve actually help".
float3 TonemapReinhard(float3 c) { return c / (c + 1.0f); }

// ACES, Narkowicz's analytic fit to the full RRT+ODT. Filmic shoulder (highlights roll off instead
// of clipping) and a toe that keeps shadows from going flat grey. The real ACES transform is a pair
// of matrices plus a spline; this fit is a few ALU ops and is what most engines actually ship.
float3 TonemapACES(float3 x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Stephen Hill's ACES fit: the same RRT+ODT approximation, but applied in ACEScg with sRGB<->ACEScg
// matrices either side. Costs two 3x3 multiplies more than the fit above and is worth it whenever
// the scene has saturated colour -- the per-channel version desaturates as channels climb the
// shoulder at different rates, which is exactly why strong greens and reds go milky under it.
static const float3x3 kACESInput = {
    0.59719f, 0.35458f, 0.04823f,
    0.07600f, 0.90834f, 0.01566f,
    0.02840f, 0.13383f, 0.83777f
};
static const float3x3 kACESOutput = {
     1.60475f, -0.53108f, -0.07367f,
    -0.10208f,  1.10813f, -0.00605f,
    -0.00327f, -0.07276f,  1.07602f
};
float3 RRTAndODTFit(float3 v) {
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}
float3 TonemapACESFitted(float3 c) {
    c = mul(kACESInput, c);
    c = RRTAndODTFit(c);
    c = mul(kACESOutput, c);
    return saturate(c);
}

// Uchimura (Gran Turismo). Three explicit segments -- a power-curve toe, a straight LINEAR midtone
// section, and an exponential shoulder -- so unlike ACES the middle of the range is left alone.
// That matters when a scene was authored by eye: ACES quietly adds contrast everywhere, Uchimura
// only bends the ends. Defaults are the paper's.
float3 TonemapUchimura(float3 x) {
    const float P = 1.0f;    // max display brightness
    const float a = 1.0f;    // contrast of the linear section
    const float m = 0.22f;   // where the linear section starts
    const float l = 0.4f;    // how much of the range is linear
    const float c = 1.33f;   // toe curvature
    const float b = 0.0f;    // black level

    float l0 = ((P - m) * l) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;

    float3 w0 = 1.0f - smoothstep(0.0f, m, x);
    float3 w2 = step(m + l0, x);
    float3 w1 = 1.0f - w0 - w2;

    float3 T = m * pow(max(x, 1e-6f) / m, c) + b;      // toe -- max() keeps pow() off exactly 0
    float3 S = P - (P - S1) * exp(CP * (x - S0));      // shoulder
    float3 L = m + a * (x - m);                        // linear midtones

    return T * w0 + L * w1 + S * w2;
}

float4 PSMain(PSInput input) : SV_TARGET {
    // SampleLevel, not Sample: 1:1 with the back buffer, so there is no mip chain and no derivative
    // to compute -- and an implicit-LOD sample on a non-mipped target is just wasted work.
    float3 hdr = gHdr.SampleLevel(gSampler, input.uv, 0.0f).rgb;

    // A NaN or a negative survives every curve below and shows up as a black or white firefly that
    // no amount of tuning removes. Clamp the input, once, here.
    hdr = max(hdr, 0.0f);

    // BLOOM IS ADDED HERE -- in linear HDR, BEFORE exposure and the curve. That ordering is the
    // whole difference between bloom that reads as light and bloom that reads as a screen-space
    // smear: real glare is extra radiance arriving at the sensor, so it must be exposed and
    // tonemapped along with everything else. Compositing it onto the tonemapped image instead would
    // let it survive the shoulder untouched and sit on top of the frame like fog. Same class of
    // ordering mistake as encoding sRGB in the wrong pass.
    if (gBloomIntensity > 0.0f)
        hdr += max(gBloom.SampleLevel(gSampler, input.uv, 0.0f).rgb, 0.0f) * gBloomIntensity;

    hdr *= gExposure;

    float3 color;
    if      (gOperator < 0.5f) color = saturate(hdr);          // none: straight clip (debug/A-B)
    else if (gOperator < 1.5f) color = TonemapReinhard(hdr);   // DEFAULT == the pre-FP16 look
    else if (gOperator < 2.5f) color = TonemapACES(hdr);
    else if (gOperator < 3.5f) color = saturate(TonemapUchimura(hdr));
    else                       color = TonemapACESFitted(hdr);

    // Linear out. The back buffer's RTV is _UNORM_SRGB, so the hardware encodes -- see
    // RendererCore::BackBufferRTVFormat.
    return float4(color, 1.0f);
}
