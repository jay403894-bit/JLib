// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// FXAA -- Fast Approximate Anti-Aliasing (Lottes, NVIDIA).
//
// The LAST pass in the chain. It runs on the tonemapped, gamma-encoded LDR image, which is not an
// implementation detail: FXAA finds edges by comparing LUMA, and luma differences only correspond to
// perceived contrast in a perceptual space. Run on linear HDR it over-weights bright regions and
// under-detects edges in shadow, so it smooths the wrong things.
//
// This is the COMPACT variant (the "FXAA II / console" formulation), not the full 3.11 quality
// preset with its edge-search loop. It is a handful of taps and catches the large majority of
// visible aliasing; the full version buys sharper handling of near-horizontal/vertical edges for
// considerably more work. Worth upgrading only if long shallow edges (railings, wires) still crawl.
//
// Being a post-process, it also anti-aliases things MSAA never could -- specular aliasing, alpha
// cutouts, the bloom-lit edge of an emitter -- because it works on the finished image rather than on
// geometry coverage. The trade is that it cannot distinguish a real edge from a textured one, so it
// slightly softens high-frequency texture detail. That is the whole bargain.

Texture2D<float4> gScene   : register(t0);   // tonemapped LDR, sRGB-ENCODED (sampled as UNORM, no decode)
SamplerState      gLinear  : register(s0);

cbuffer FxaaParams : register(b0) {
    float2 gTexelSize;      // 1 / render target dimensions
    float  gEdgeThreshold;  // relative luma delta needed to treat a pixel as an edge
    float  gEdgeThresholdMin; // absolute floor -- stops FXAA chewing on near-flat gradients/noise
};

struct PSInput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Perceptual luma weights. Green dominates because the eye does; using an unweighted average here
// makes FXAA miss edges that are almost entirely a red/blue transition.
float Luma(float3 c) { return dot(c, float3(0.299f, 0.587f, 0.114f)); }

// THE OUTPUT HAS TO BE DECODED, and this is the subtle part of wiring FXAA in.
// The chain is: tonemap writes the LDR target through an _UNORM_SRGB RTV (hardware ENCODES) -> we
// sample that target through a plain _UNORM SRV (no decode, deliberately, because FXAA needs
// perceptual values) -> so everything above is in sRGB space -> but the BACK BUFFER's RTV is also
// _UNORM_SRGB, so the hardware will encode our result a SECOND time on write.
// Decoding here makes that hardware encode a no-op round trip. It cancels exactly in float, so it
// costs nothing beyond the 8-bit quantisation that already happened at the LDR store. Skip it and
// the frame is double-encoded and washed out -- the exact failure mode the sRGB pipeline work fixed,
// reintroduced at the very last pass.
float3 SrgbToLinear(float3 c) {
    // step()/lerp() rather than a ternary: dxc rejects a VECTOR condition in `?:` (it wants the
    // HLSL 2021 `select` intrinsic), and this form needs no language-version assumption at all.
    float3 lo = c / 12.92f;
    float3 hi = pow(abs(c + 0.055f) / 1.055f, 2.4f);
    return lerp(hi, lo, step(c, 0.04045f));
}

float4 PSMain(PSInput input) : SV_TARGET {
    const float2 t  = gTexelSize;
    const float2 uv = input.uv;

    float3 rgbM  = gScene.SampleLevel(gLinear, uv, 0).rgb;
    float3 rgbNW = gScene.SampleLevel(gLinear, uv + float2(-t.x, -t.y), 0).rgb;
    float3 rgbNE = gScene.SampleLevel(gLinear, uv + float2( t.x, -t.y), 0).rgb;
    float3 rgbSW = gScene.SampleLevel(gLinear, uv + float2(-t.x,  t.y), 0).rgb;
    float3 rgbSE = gScene.SampleLevel(gLinear, uv + float2( t.x,  t.y), 0).rgb;

    float lumaM  = Luma(rgbM);
    float lumaNW = Luma(rgbNW);
    float lumaNE = Luma(rgbNE);
    float lumaSW = Luma(rgbSW);
    float lumaSE = Luma(rgbSE);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    float range   = lumaMax - lumaMin;

    // EARLY OUT, and it is most of the performance story: the overwhelming majority of pixels are
    // not on an edge, and this rejects them after five taps. The threshold is RELATIVE to local
    // brightness (a 0.05 step is an edge in shadow and invisible in a highlight), with an absolute
    // floor so flat gradients and film-grain-scale noise are left alone.
    if (range < max(gEdgeThresholdMin, lumaMax * gEdgeThreshold))
        return float4(SrgbToLinear(rgbM), 1.0f);

    // Edge direction from the diagonal luma gradients. This is PERPENDICULAR to the edge: the blur
    // has to run ALONG the edge to soften the staircase without destroying the edge itself.
    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    // Where the gradient is tiny the direction is mostly noise, and normalizing it would amplify
    // that noise into visible wobble. dirReduce biases the reciprocal so weak gradients produce
    // short offsets instead of wild ones.
    const float kDirReduceMul = 1.0f / 8.0f;
    const float kDirReduceMin = 1.0f / 128.0f;
    const float kSpanMax      = 8.0f;   // cap in texels; longer smears across genuine detail
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25f * kDirReduceMul, kDirReduceMin);
    float rcpDirMin = 1.0f / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -kSpanMax, kSpanMax) * t;

    // Two tap pairs along the edge: rgbA is the narrow average, rgbB widens it with the two extreme
    // samples for a smoother result on strong edges.
    float3 rgbA = 0.5f * (gScene.SampleLevel(gLinear, uv + dir * (1.0f / 3.0f - 0.5f), 0).rgb +
                          gScene.SampleLevel(gLinear, uv + dir * (2.0f / 3.0f - 0.5f), 0).rgb);
    float3 rgbB = rgbA * 0.5f +
                  0.25f * (gScene.SampleLevel(gLinear, uv + dir * -0.5f, 0).rgb +
                           gScene.SampleLevel(gLinear, uv + dir *  0.5f, 0).rgb);

    // If the wide average strayed outside the original 3x3 luma range it reached past the edge and
    // pulled in an unrelated surface -- fall back to the narrow one. This guard is what stops FXAA
    // bleeding a bright object across a silhouette onto whatever is behind it.
    float lumaB = Luma(rgbB);
    float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return float4(SrgbToLinear(result), 1.0f);
}
