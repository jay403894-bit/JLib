// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// ============================================================================================
// Basic3D_PS.hlsl -- the PIXEL (fragment) shader. Runs ONCE PER PIXEL the triangle covers. Its
// job: output the final color for that pixel (SV_TARGET).
// ============================================================================================

// This input struct MUST match Basic3D_VS.hlsl's VSOutput field-for-field (that's the VS->PS
// contract). SV_POSITION arrives as the interpolated screen position; `normal` arrives smoothly
// interpolated across the triangle from the three vertices' normals.
struct PSInput {
    float4 clip   : SV_POSITION;
    float3 normal : NORMAL;
};

// SV_TARGET = the color written to the render target (the back buffer).
float4 PSMain(PSInput input) : SV_TARGET {
    float3 n = normalize(input.normal);      // interpolation can shorten it -- renormalize

    // No lighting yet. Each cube face points a different direction, and a direction's components
    // run -1..1; mapping them to 0..1 turns "which way the surface faces" into a distinct color
    // per face, so you can SEE the geometry with zero light setup. Real lighting (a dot product
    // against a light direction) is a later step -- this is a debug view of the normals.
    float3 color = n * 0.5f + 0.5f;
    return float4(color, 1.0f);
}
