// ShadowDepth_VS.hlsl -- the shadow map pass. Renders the scene from the LIGHT's point of view into
// a depth-only target; the resulting depth buffer is "how far the light can see in each direction",
// and the main pixel shader later asks "is this surface further from the light than that?" to decide
// whether it's shadowed.
//
// Depth-only means there is NO pixel shader bound at all -- the rasterizer writes depth and nothing
// else, which is roughly twice as fast as a full pass and is why shadow maps are affordable.
//
// The vertex input layout is IDENTICAL to Basic3D_VS (same kVertex3DLayout, same per-instance matrix
// rows in slot 1) so this pass can replay the exact same vertex/instance buffers the geometry pass
// uses -- no separate geometry, no separate instance data.

cbuffer ShadowCamera : register(b0) { row_major float4x4 LightViewProj; }

struct VSInput {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;     // unused here, but the layout must match the bound vertex buffer
    float2 uv      : TEXCOORD;   // unused here
    float4 tangent : TANGENT;    // unused here
    float4 m0      : INSTMAT0;   // per-instance model matrix, row 0
    float4 m1      : INSTMAT1;
    float4 m2      : INSTMAT2;
    float4 m3      : INSTMAT3;
};

float4 VSMain(VSInput input) : SV_POSITION {
    // Same row-major reconstruction as Basic3D_VS -- see its GOTCHA #1 comment for why this matters.
    float4x4 ModelMat = float4x4(input.m0, input.m1, input.m2, input.m3);
    float4 worldPos = mul(float4(input.pos, 1.0f), ModelMat);
    return mul(worldPos, LightViewProj);
}
