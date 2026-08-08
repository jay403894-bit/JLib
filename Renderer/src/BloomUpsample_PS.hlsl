// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// Bloom, stage 2 of 2: UPSAMPLE + ACCUMULATE.
//
// Walks back UP the chain built by BloomDownsample_PS, adding each smaller level into the next
// larger one. The PSO for this pass uses ADDITIVE blending (ONE/ONE), so the "accumulate" half is
// the blend state rather than anything in this shader -- which is why there is no destination read
// here and no ping-pong target.
//
// Summing every level is what produces bloom that looks like light: the small levels contribute a
// tight core and the large ones a wide, faint halo, and a real bloom response is that whole stack at
// once rather than any single blur radius.

Texture2D<float4> gSource : register(t0);
SamplerState      gLinear : register(s0);

cbuffer BloomParams : register(b0) {
    float2 gTexelSize;   // 1 / SOURCE dimensions (the SMALLER level being read)
    float  gThreshold;   // downsample only; unused here
    float  gKnee;        // downsample only; unused here
    float  gMode;        // downsample only; unused here
    float  gRadius;      // tent radius in source texels -- the "scatter" knob
    float2 _pad;
};

struct PSInput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET {
    // 3x3 TENT filter (1 2 1 / 2 4 2 / 1 2 1, /16). A tent on upsample rather than a box is what
    // hides the fact that the source is a quarter of the resolution -- a box leaves visible blocky
    // steps at each level, and those steps survive all the way up the chain into the final image.
    const float2 o = gTexelSize * gRadius;
    const float2 uv = input.uv;

    float3 s;
    s  = gSource.SampleLevel(gLinear, uv + float2(-o.x,  o.y), 0).rgb * 1.0f;
    s += gSource.SampleLevel(gLinear, uv + float2( 0.0f, o.y), 0).rgb * 2.0f;
    s += gSource.SampleLevel(gLinear, uv + float2( o.x,  o.y), 0).rgb * 1.0f;

    s += gSource.SampleLevel(gLinear, uv + float2(-o.x, 0.0f), 0).rgb * 2.0f;
    s += gSource.SampleLevel(gLinear, uv,                      0).rgb * 4.0f;
    s += gSource.SampleLevel(gLinear, uv + float2( o.x, 0.0f), 0).rgb * 2.0f;

    s += gSource.SampleLevel(gLinear, uv + float2(-o.x, -o.y), 0).rgb * 1.0f;
    s += gSource.SampleLevel(gLinear, uv + float2( 0.0f, -o.y), 0).rgb * 2.0f;
    s += gSource.SampleLevel(gLinear, uv + float2( o.x, -o.y), 0).rgb * 1.0f;

    return float4(max(s * (1.0f / 16.0f), 0.0f), 1.0f);
}
