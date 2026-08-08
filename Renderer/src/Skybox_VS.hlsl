// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// ============================================================================================
// Skybox_VS.hlsl -- the VISUAL-ONLY procedural sky, vertex stage. No vertex buffer: 3 verts from
// SV_VertexID make a FULLSCREEN TRIANGLE at the far plane (z=1). For each pixel it reconstructs a
// world-space VIEW RAY by unprojecting the clip position through InvViewProj; the PS turns that ray
// into a sky color. Drawn BEFORE the 3D geometry with depth OFF, so the world draws on top and the
// sky only shows through the background (it feeds NOTHING into lighting -- purely a backdrop).
// ============================================================================================

// b0 = inverse of the camera's ViewProj (to go clip -> world) + the camera position. Same row-major /
// row-vector convention as Basic3D_VS (mul(vector, matrix)); the CPU stores InvViewProj that way.
cbuffer SkyCB : register(b0) {
    row_major float4x4 InvViewProj;
    float3 gCamPos; float _pad;
}

struct VSOutput {
    float4 clip : SV_POSITION;
    float3 ray  : TEXCOORD0;   // world-space camera->pixel direction (un-normalized; PS normalizes)
};

VSOutput VSMain(uint id : SV_VertexID) {
    // Fullscreen triangle: uv (0,0)/(2,0)/(0,2) -> ndc (-1,-1)/(3,-1)/(-1,3), which covers the whole
    // [-1,1] screen with a single over-sized triangle (cheaper than a quad, no vertex buffer needed).
    float2 uv  = float2((id << 1) & 2, id & 2);
    float2 ndc = uv * 2.0f - 1.0f;

    VSOutput o;
    o.clip = float4(ndc, 1.0f, 1.0f);                        // z = 1: sit on the far plane
    float4 world = mul(float4(ndc, 1.0f, 1.0f), InvViewProj); // clip -> world
    world /= world.w;                                        // perspective divide
    o.ray = world.xyz - gCamPos;                             // camera -> far point == view direction
    return o;
}
