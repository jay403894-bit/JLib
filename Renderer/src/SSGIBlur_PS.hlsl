// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// SSGI denoise -- edge-aware spatial blur PLUS temporal accumulation, in one pass.
//
// WHY BOTH, AND WHY TOGETHER. A 12-ray Monte Carlo gather is far too noisy to use raw. Spatial
// filtering alone can only trade noise for blur: widen it enough to kill the speckle and the
// indirect light turns to mush. Temporal accumulation is the half that actually adds INFORMATION --
// reusing last frame's estimate is effectively free extra rays, and because each frame draws a
// different random pattern the average converges instead of just smearing. Every shipping SSGI does
// both; this had neither, then only the first.
//
// They live in one pass because the spatial loop already samples the neighbourhood, and that
// neighbourhood's min/max is exactly what the temporal step needs for its ghost rejection. Splitting
// them would mean sampling the same taps twice.
//
// WHAT THIS STILL DOES NOT DO: there are no per-object motion vectors. Reprojection uses the previous
// view-projection matrix and the current depth, which is exact for STATIC geometry and wrong for
// anything that moved on its own. The neighbourhood clamp below is what keeps that wrongness from
// becoming a visible trail -- it is the standard mitigation, not a full solution.

cbuffer BlurCB : register(b0) {
    row_major float4x4 gInvViewProj;   // clip -> world, to rebuild this pixel's world position
    row_major float4x4 gPrevViewProj;  // world -> LAST frame's clip, to find where this pixel was
    float2 gTexelSize;                 // 1 / render target dimensions
    float  gStride;                    // tap spacing in texels
    float  gDepthSigma;                // relative depth tolerance before a tap is rejected
    float  gAlpha;                     // history weight; 0.9 ~= a 10-frame running average
    float  gHistoryValid;              // 0 on the first frame and after a resize
    float2 gPad;
};

Texture2D    gSsgi        : register(t0);   // the raw gathered bounce, this frame
Texture2D    gDepth       : register(t1);   // camera depth prepass, for edge stopping
Texture2D    gHistory     : register(t2);   // LAST frame's accumulated result
SamplerState gLinearClamp : register(s0);
SamplerState gPointClamp  : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float3 WorldPosFromUV(float2 uv, float depth) {
    float4 clip = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 w    = mul(clip, gInvViewProj);   // row-vector convention, as everywhere else here
    return w.xyz / w.w;
}

float4 PSMain(VSOut input) : SV_Target {
    float centreDepth = gDepth.SampleLevel(gPointClamp, input.uv, 0).r;
    if (centreDepth >= 0.999999f) return float4(0.0f, 0.0f, 0.0f, 1.0f);   // sky

    // ---- spatial: bilateral 5x5 ----
    float3 sum = 0.0f, wsum = 0.0f;
    float3 mn = 1e30f, mx = -1e30f;   // neighbourhood bounds, reused by the temporal step below

    [unroll]
    for (int y = -2; y <= 2; ++y) {
        [unroll]
        for (int x = -2; x <= 2; ++x) {
            float2 uv = input.uv + float2(x, y) * gTexelSize * gStride;

            float d = gDepth.SampleLevel(gPointClamp, uv, 0).r;
            if (d >= 0.999999f) continue;

            // RELATIVE depth difference. Depth is non-linear, so a fixed epsilon that works up close
            // rejects everything in the distance; dividing by the centre depth makes the tolerance
            // behave the same at any range.
            float dd = abs(d - centreDepth) / max(centreDepth, 1e-6f);
            float wd = exp(-dd * dd / max(gDepthSigma * gDepthSigma, 1e-12f));
            float ws = exp(-(float)(x * x + y * y) / 8.0f);
            float w  = ws * wd;

            float3 c = gSsgi.SampleLevel(gLinearClamp, uv, 0).rgb;
            sum  += c * w;
            wsum += w;
            // Bounds are taken over taps that PASSED the depth test, so the clamp below is built from
            // samples on this surface only -- otherwise a background pixel would widen the box and
            // let a ghost straight through it.
            if (wd > 0.5f) { mn = min(mn, c); mx = max(mx, c); }
        }
    }
    // The centre tap always has depth difference 0, so wsum >= 1 and this cannot divide by zero.
    float3 cur = sum / max(wsum, 1e-6f);

    // ---- temporal: reproject into last frame and blend ----
    if (gHistoryValid > 0.5f) {
        float3 world = WorldPosFromUV(input.uv, centreDepth);
        float4 pclip = mul(float4(world, 1.0f), gPrevViewProj);
        if (pclip.w > 0.0f) {
            float3 pndc = pclip.xyz / pclip.w;
            float2 puv  = float2(pndc.x * 0.5f + 0.5f, 0.5f - pndc.y * 0.5f);
            // Off-screen last frame means there is genuinely no history for this pixel -- newly
            // disoccluded geometry, or the camera turned. Using it anyway is where smearing comes from.
            if (all(puv >= 0.0f) && all(puv <= 1.0f)) {
                float3 hist = gHistory.SampleLevel(gLinearClamp, puv, 0).rgb;
                // NEIGHBOURHOOD CLAMP -- the whole ghost-rejection mechanism. If the history value
                // lies outside the range of plausible values for THIS surface right now, the scene
                // changed there (something moved, a shadow swept across) and that history is stale.
                // Clamping rather than discarding keeps the variance reduction while bounding how
                // wrong a stale sample can be, which is what stops moving objects trailing.
                hist = clamp(hist, mn, mx);
                cur  = lerp(cur, hist, gAlpha);
            }
        }
    }

    return float4(max(cur, 0.0f), 1.0f);
}
