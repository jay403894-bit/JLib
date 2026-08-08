// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// Step 1 of the IBL bake: equirectangular HDR -> cubemap face.
//
// HDRIs ship as equirectangular (latitude/longitude) images because that is a convenient way to store
// a sphere in one rectangle, but it is a terrible thing to SAMPLE: the poles are wildly oversampled
// and a direction lookup costs an atan2. A cubemap costs one hardware sample by direction, so the
// whole environment gets converted once at load and never touched again.
//
// Run once per face (six draws), each with gFwd/gRight/gUp set to that face's basis. Shares SSAO_VS's
// fullscreen triangle -- this pass only needs a UV, and that shader already provides one.
cbuffer CubeFaceCB : register(b0) {
    float3 gFwd;    float gRoughness;   // gRoughness unused here; the three bake shaders share this CB
    float3 gRight;  float gMipCount;
    float3 gUp;     float gSourceSize;
}

Texture2D    gEquirect : register(t0);
SamplerState gLinear   : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

static const float PI = 3.14159265359f;

float4 PSMain(VSOut input) : SV_Target {
    // UV across the face (0..1, y down) -> a direction through that texel. The face basis does the
    // rest, so one shader covers all six faces.
    float2 t = input.uv * 2.0f - 1.0f;
    float3 dir = normalize(gFwd + gRight * t.x - gUp * t.y);

    // Direction -> equirectangular UV. atan2 gives longitude, asin gives latitude.
    float2 e = float2(atan2(dir.z, dir.x), asin(clamp(dir.y, -1.0f, 1.0f)));
    e.x = e.x / (2.0f * PI) + 0.5f;
    e.y = 0.5f - e.y / PI;

    // SampleLevel, not Sample: there are no derivatives to speak of across a face boundary, and an
    // implicit-LOD sample would pick garbage mips along the seam.
    return float4(gEquirect.SampleLevel(gLinear, e, 0).rgb, 1.0f);
}
