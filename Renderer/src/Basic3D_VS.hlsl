// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// ============================================================================================
// Basic3D_VS.hlsl -- the VERTEX shader. Runs ONCE PER VERTEX. Its job: output where this vertex
// lands on the screen (SV_POSITION), plus anything the pixel shader will need (here, the normal).
// ============================================================================================

// b0 = the camera constant buffer (root parameter 0), shared per frame.
//   The Model matrix is NO LONGER a cbuffer (b1) -- it now arrives PER INSTANCE via the vertex stream
//   (INSTMAT0..3, slot 1 of kVertex3DLayout), so one instanced draw handles many objects.
//
// GOTCHA #1 (the classic invisible-cube bug): `row_major`. DirectXMath stores matrices row-major and
// does row-vector math (v * M). HLSL cbuffers default to COLUMN-major, which silently transposes the
// matrix -> garbage/vanished geometry. ViewProj is declared row_major so HLSL reads it the way the CPU
// wrote it. The per-instance matrix needs no such keyword: we ASSEMBLE it from 4 explicit rows below,
// so it's row-major by construction.
cbuffer Camera : register(b0) { row_major float4x4 ViewProj; }  // View * Projection, shared per frame

// POSITION/NORMAL/TEXCOORD bind to slot 0 (per-vertex); INSTMAT0..3 bind to slot 1 (per-instance).
struct VSInput {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float2 uv      : TEXCOORD;
    float4 tangent : TANGENT;   // xyz = surface tangent, w = bitangent handedness (see Vertex.h)
    float4 m0      : INSTMAT0;   // this instance's Model matrix, row 0
    float4 m1      : INSTMAT1;   // row 1
    float4 m2      : INSTMAT2;   // row 2
    float4 m3      : INSTMAT3;   // row 3
};

// GOTCHA #2 (the VS->PS contract): this output struct MUST match the pixel shader's INPUT struct
// field-for-field (same types, same semantics). The GPU interpolates every field except
// SV_POSITION across the triangle before handing it to the pixel shader.
struct VSOutput {
    float4 clip     : SV_POSITION; // REQUIRED: the on-screen (clip-space) position of this vertex
    float3 worldPos : POSITION;    // world-space position -- the PS needs it for point/spot lights + view vec
    float3 normal   : NORMAL;      // passed through to the PS, interpolated across the face
    float2 uv       : TEXCOORD;    // passed through for the albedo sample in the PS
    float4 tangent  : TANGENT;     // world-space tangent (xyz) + handedness (w) -- the PS builds the TBN
};

VSOutput VSMain(VSInput input) {
    VSOutput o;
    // Rebuild this instance's world matrix from its 4 rows. float4x4(r0,r1,r2,r3) fills ROW-major, so
    // rows == what the CPU wrote (XMFLOAT4X4), and mul(v, M) stays the same row-vector math as before.
    float4x4 ModelMat = float4x4(input.m0, input.m1, input.m2, input.m3);
    float4 worldPos = mul(float4(input.pos, 1.0f), ModelMat); // object space -> world space
    o.worldPos = worldPos.xyz;                                // hand the world position to the PS
    o.clip   = mul(worldPos, ViewProj);                       // world -> clip space  <-- THE 3D step
    o.normal = mul(input.normal, (float3x3)ModelMat);         // rotate the normal into world space
    o.uv     = input.uv;                                      // texture coords ride straight through
    // Rotate the tangent into world by the SAME 3x3 (no translate) as the normal; the PS re-orthogonalizes
    // it. Handedness (w) is a sign, not a direction -- it rides through untouched so B = cross(N,T)*w stays valid.
    o.tangent = float4(mul(input.tangent.xyz, (float3x3)ModelMat), input.tangent.w);
    return o;
}
