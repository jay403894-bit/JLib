// Bloom, stage 1 of 2: PREFILTER + DOWNSAMPLE.
//
// This is the progressive-downsample half of the Call of Duty: Advanced Warfare scheme (Jimenez,
// "Next Generation Post Processing in Call of Duty"). A single wide gaussian is the naive approach
// and looks it -- boxy, and it either misses the wide halo or costs an enormous kernel. Repeatedly
// halving with a good filter gets a very wide, very smooth response for almost nothing, because
// each level does the same small amount of work on a quarter of the pixels.
//
// Runs on the FP16 scene target BEFORE tonemapping, so it operates on real radiance -- values above
// 1.0 are exactly what bloom is supposed to find, and they only exist at all because the pipeline
// stopped crushing to 8-bit mid-frame.
//
// gMode == 1 on the first invocation only (HDR -> bloom[0]): that pass additionally applies the
// brightness threshold and the Karis average. Every later level is a plain downsample.

Texture2D<float4> gSource  : register(t0);
SamplerState      gLinear  : register(s0);

cbuffer BloomParams : register(b0) {
    float2 gTexelSize;   // 1 / SOURCE dimensions -- taps are in source texels, not destination
    float  gThreshold;   // luminance where bloom starts contributing
    float  gKnee;        // width of the SOFT shoulder below the threshold (0 = hard cutoff)
    float  gMode;        // 1 = prefilter (threshold + Karis), 0 = plain downsample
    float  gRadius;      // upsample only; unused here
    float2 _pad;
};

struct PSInput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// A HARD threshold makes bloom pop on and off as a surface crosses it -- extremely visible when the
// camera moves. This is the standard quadratic soft-knee: below (threshold - knee) nothing passes,
// above (threshold + knee) everything does, and between them it eases in.
float3 SoftThreshold(float3 c) {
    const float knee = max(gKnee, 1e-4f);
    const float br   = max(c.r, max(c.g, c.b));   // max, not luma: a saturated red light should bloom
    float soft = clamp(br - gThreshold + knee, 0.0f, 2.0f * knee);
    soft = (soft * soft) / (4.0f * knee);
    return c * (max(soft, br - gThreshold) / max(br, 1e-4f));
}

// Karis average: weight each group by 1/(1+luma) before averaging. Without it a single very bright
// pixel (a specular glint, one sun texel) dominates its whole neighbourhood, and because that pixel
// flickers between frames as geometry moves sub-pixel, the entire bloom halo flickers with it. This
// is the standard fix and it is only needed on the FIRST downsample, where the fireflies still exist.
float KarisWeight(float3 c) {
    return 1.0f / (1.0f + dot(c, float3(0.2126f, 0.7152f, 0.0722f)));
}

float4 PSMain(PSInput input) : SV_TARGET {
    const float2 t = gTexelSize;
    const float2 uv = input.uv;

    // 13-tap pattern: a centre quad, four corner-adjacent groups, and the four diagonals. It is
    // deliberately overlapping -- that overlap is what makes the chain smooth rather than boxy, and
    // it costs 13 bilinear taps once per level instead of a huge kernel once at full resolution.
    float3 a = gSource.SampleLevel(gLinear, uv + t * float2(-2.0f,  2.0f), 0).rgb;
    float3 b = gSource.SampleLevel(gLinear, uv + t * float2( 0.0f,  2.0f), 0).rgb;
    float3 c = gSource.SampleLevel(gLinear, uv + t * float2( 2.0f,  2.0f), 0).rgb;
    float3 d = gSource.SampleLevel(gLinear, uv + t * float2(-2.0f,  0.0f), 0).rgb;
    float3 e = gSource.SampleLevel(gLinear, uv,                            0).rgb;
    float3 f = gSource.SampleLevel(gLinear, uv + t * float2( 2.0f,  0.0f), 0).rgb;
    float3 g = gSource.SampleLevel(gLinear, uv + t * float2(-2.0f, -2.0f), 0).rgb;
    float3 h = gSource.SampleLevel(gLinear, uv + t * float2( 0.0f, -2.0f), 0).rgb;
    float3 i = gSource.SampleLevel(gLinear, uv + t * float2( 2.0f, -2.0f), 0).rgb;
    float3 j = gSource.SampleLevel(gLinear, uv + t * float2(-1.0f,  1.0f), 0).rgb;
    float3 k = gSource.SampleLevel(gLinear, uv + t * float2( 1.0f,  1.0f), 0).rgb;
    float3 l = gSource.SampleLevel(gLinear, uv + t * float2(-1.0f, -1.0f), 0).rgb;
    float3 m = gSource.SampleLevel(gLinear, uv + t * float2( 1.0f, -1.0f), 0).rgb;

    float3 result;
    if (gMode > 0.5f) {
        // Prefilter: average the five 2x2 groups SEPARATELY with Karis weights, then combine. The
        // weighting has to happen per group -- averaging first and weighting after would already
        // have let the firefly contaminate the average, which is the whole thing being prevented.
        float3 g0 = (j + k + l + m) * 0.25f;
        float3 g1 = (a + b + d + e) * 0.25f;
        float3 g2 = (b + c + e + f) * 0.25f;
        float3 g3 = (d + e + g + h) * 0.25f;
        float3 g4 = (e + f + h + i) * 0.25f;

        float w0 = KarisWeight(g0) * 0.5f;      // same group weights as the plain path below
        float w1 = KarisWeight(g1) * 0.125f;
        float w2 = KarisWeight(g2) * 0.125f;
        float w3 = KarisWeight(g3) * 0.125f;
        float w4 = KarisWeight(g4) * 0.125f;
        float wsum = w0 + w1 + w2 + w3 + w4;

        result = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / max(wsum, 1e-4f);
        result = SoftThreshold(result);
    } else {
        // Plain downsample. Weights: the centre quad carries half, the four outer quads an eighth
        // each -- that distribution is what keeps the chain stable instead of aliasing as it shrinks.
        result  = (j + k + l + m) * 0.5f  * 0.25f;
        result += (a + b + d + e) * 0.125f * 0.25f;
        result += (b + c + e + f) * 0.125f * 0.25f;
        result += (d + e + g + h) * 0.125f * 0.25f;
        result += (e + f + h + i) * 0.125f * 0.25f;
    }

    return float4(max(result, 0.0f), 1.0f);
}
