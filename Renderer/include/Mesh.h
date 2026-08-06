#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <DirectXMath.h>     // bounding sphere (frustum culling)
#include "TextureHandle.h"   // Material references a texture by handle (no ResourceManager dependency)
namespace JLib {
    // What a Mesh is drawn WITH -- its surface appearance, distinct from its geometry. Kept minimal
    // (just an albedo texture) on purpose; add roughness/metallic/normal-map handles here as the
    // shading model grows. A loaded asset fills this in so the mesh is self-describing: the renderer
    // reads mesh.material to bind, instead of the caller hand-pairing a texture at every Submit.
    // glTF metallic-roughness material. Each channel is factor x texture (the texture is optional -- when
    // absent, the factor alone applies, which is exactly how procedural meshes like MakeCubeMesh work).
    // Textures bind at t0..t4 in the 3D pixel shader; factors + a "which maps are present" bitmask ride in
    // the b4 root constants per draw. The glTF loader (ModelLoader) fills these from the asset; callers can
    // still hand-set albedo + metallic/roughness for untextured meshes.
    struct Material {
        TextureHandle albedo;      // t0: base color
        TextureHandle metalRough;  // t1: glTF packing -- G = roughness, B = metalness (optional)
        TextureHandle emissive;    // t2: emissive color (optional)
        TextureHandle occlusion;   // t3: ambient occlusion in R, applied to ambient only (optional; often the
                                   //     SAME image as metalRough -- the loader dedups by image, so it's one texture)
        TextureHandle normal;      // t4: tangent-space normal map (optional). Needs Vertex3D tangents to build
                                   //     the TBN; absent -> the shader uses the interpolated vertex normal alone.
        DirectX::XMFLOAT4 baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };  // multiplies the base-color texture
        DirectX::XMFLOAT3 emissiveFactor  = { 0.0f, 0.0f, 0.0f };        // multiplies the emissive texture (0 = none)
        // metallic/roughness FACTORS (multiply metalRough.B / .G). Defaults = neutral dielectric so untextured
        // procedural meshes look sane; the glTF loader overwrites with the asset's real factors.
        float metallic  = 0.0f;
        float roughness = 0.5f;
    };
    struct Mesh {
        Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
        uint32_t vertexCount;
        Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
        D3D12_INDEX_BUFFER_VIEW indexBufferView;
        uint32_t indexCount;
        uint32_t meshID;
        Material material;   // self-describing surface -- Renderer3D binds this instead of a per-Submit tex
        // Object-space bounding sphere, computed from the vertices at creation (see CreateMeshRaw).
        // Renderer3D transforms it to world space per Submit and frustum-culls against it. radius 0 =
        // "no bounds" (e.g. a failed/empty mesh) -> never culled.
        DirectX::XMFLOAT3 boundsCenter = { 0.0f, 0.0f, 0.0f };
        float             boundsRadius = 0.0f;
    };
};