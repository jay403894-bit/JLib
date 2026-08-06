// ============================================================================================
// Skinned3D_VS.hlsl -- the SKINNED vertex shader. Same job as Basic3D_VS (output clip pos + the
// normal/uv the pixel shader needs), but first DEFORMS the vertex by its bones. Pairs with the
// UNCHANGED Basic3D_PS (identical VSOutput/PSInput contract), so only the vertex stage differs.
// ============================================================================================

// Must line up with Renderer3D's SKINNED root signature:
//   b0 = camera View*Projection        (root param 0)
//   b1 = 16 root constants = ModelMat   (root param 1)
//   b2 = the bone palette               (root param 2)   <-- the skinning-only addition
// row_major everywhere: DirectXMath is row-major/row-vector and the palette matrices were stored in
// that same convention on load (see ModelLoader.cpp/GltfMat), so we do NOT transpose. mul(vector, matrix).
cbuffer Camera : register(b0) { row_major float4x4 ViewProj; }
cbuffer Model  : register(b1) { row_major float4x4 ModelMat; }

#define MAX_BONES 128   // MUST match Renderer3D::kMaxBones
cbuffer Bones  : register(b2) { row_major float4x4 gBones[MAX_BONES]; }  // per-joint palette matrices

struct VSInput {
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float2 uv      : TEXCOORD;
    uint4  boneIDs : BLENDINDICES;   // indices into gBones (== the Skeleton's joint indices)
    float4 weights : BLENDWEIGHT;    // how much each of the 4 bones influences this vertex
};

// MUST match Basic3D_PS.hlsl's PSInput field-for-field (worldPos added for the PBR lighting). TANGENT is
// present ONLY to satisfy the shared PS's input signature -- skinned vertices carry no tangent yet, so it's
// a placeholder and the skinned draw path never sets the normal-map flag (bit3), so the PS never reads it.
struct VSOutput {
    float4 clip     : SV_POSITION;
    float3 worldPos : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
    float4 tangent  : TANGENT;
};

VSOutput VSMain(VSInput input) {
    // Defensive re-normalize: glTF guarantees weights sum to 1, but interpolation/precision can drift,
    // and an all-zero weight vector (an unrigged vertex) would otherwise collapse to the origin.
    float4 w    = input.weights;
    float  wsum = dot(w, float4(1, 1, 1, 1));
    w = (wsum > 1e-5f) ? (w / wsum) : float4(1, 0, 0, 0);

    // Blend the 4 influencing bones' palette matrices. At the bind pose the palette is identity, so
    // skin == identity and this reproduces the static (un-skinned) mesh exactly -- the verification step.
    float4x4 skin =
        w.x * gBones[input.boneIDs.x] +
        w.y * gBones[input.boneIDs.y] +
        w.z * gBones[input.boneIDs.z] +
        w.w * gBones[input.boneIDs.w];

    float4 skinnedPos = mul(float4(input.pos, 1.0f), skin);   // object-space bone deform (row-vector)
    float4 worldPos   = mul(skinnedPos, ModelMat);            // -> world

    VSOutput o;
    o.worldPos = worldPos.xyz;                                // hand the world position to the PS
    o.clip   = mul(worldPos, ViewProj);                       // -> clip
    float3 sn = mul(input.normal, (float3x3)skin);            // deform the normal too
    o.normal = mul(sn, (float3x3)ModelMat);
    o.uv     = input.uv;
    o.tangent = float4(1.0f, 0.0f, 0.0f, 1.0f);               // placeholder (see VSOutput note; unused by the PS)
    return o;
}
