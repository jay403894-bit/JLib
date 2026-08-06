// ============================================================================================
// Basic3D_VS.hlsl -- the VERTEX shader. Runs ONCE PER VERTEX. Its job: output where this vertex
// lands on the screen (SV_POSITION), plus anything the pixel shader will need (here, the normal).
// ============================================================================================

// These two cbuffers MUST line up with Renderer3D's root signature:
//   b0 = the camera constant buffer          (root parameter 0)
//   b1 = the 16 root-constant floats = a Model matrix (root parameter 1)
//
// GOTCHA #1 (the classic invisible-cube bug): `row_major`. DirectXMath stores matrices row-major
// and does row-vector math (v * M). HLSL constant buffers default to COLUMN-major, which silently
// transposes the matrix -> your cube renders as garbage or vanishes. Declaring the matrices
// row_major makes HLSL read them the same way the CPU wrote them, so we do NOT transpose on upload.
// (The other valid recipe is: transpose on the CPU and drop `row_major`. Pick one convention and
// stick to it everywhere -- mixing them is what causes the bug.)
cbuffer Camera : register(b0) { row_major float4x4 ViewProj; }  // View * Projection, shared per frame
cbuffer Model  : register(b1) { row_major float4x4 ModelMat;  } // this object's world transform

// The input semantics (POSITION/NORMAL/TEXCOORD) bind to kVertex3DLayout in Renderer3D.cpp.
struct VSInput {
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD;
};

// GOTCHA #2 (the VS->PS contract): this output struct MUST match the pixel shader's INPUT struct
// field-for-field (same types, same semantics). The GPU interpolates every field except
// SV_POSITION across the triangle before handing it to the pixel shader.
struct VSOutput {
    float4 clip   : SV_POSITION; // REQUIRED: the on-screen (clip-space) position of this vertex
    float3 normal : NORMAL;      // passed through to the PS, interpolated across the face
};

VSOutput VSMain(VSInput input) {
    VSOutput o;
    float4 worldPos = mul(float4(input.pos, 1.0f), ModelMat); // object space -> world space
    o.clip   = mul(worldPos, ViewProj);                       // world -> clip space  <-- THE 3D step
    o.normal = mul(input.normal, (float3x3)ModelMat);         // rotate the normal into world space
    return o;
}
