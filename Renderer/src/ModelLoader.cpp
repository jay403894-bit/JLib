// MUST be first, before any include pulls in the CRT: cgltf (and tinyobj) use fopen/strncpy, which
// MSVC flags as C4996 "deprecated". This silences that for THIS TU only, so a pristine re-downloaded
// cgltf.h compiles unmodified -- no need to hand-patch it to fopen_s/strncpy_s each update.
#define _CRT_SECURE_NO_WARNINGS

// The ONE translation unit that compiles tinyobjloader's implementation (define goes here only).
#define TINYOBJLOADER_IMPLEMENTATION
#include "../include/tiny_obj_loader.h"

// Same rule for cgltf: this is the single TU that compiles its implementation.
#define CGLTF_IMPLEMENTATION
#include "../include/cgltf.h"

#include "../include/ModelLoader.h"
#include "../include/Vertex.h"

#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <cstring>   // memcpy (glTF matrix -> XMFLOAT4X4)

namespace JLib {

// --- shared post-processing, used by BOTH the .obj and .glb loaders ---

// Compute smooth vertex normals from the triangle list: accumulate each face's normal into its 3
// verts, then normalize. Shared verts => smooth shading. Call only when the file had no normals.
// Templated on vertex type so both Vertex3D and SkinnedVertex3D (which share .pos/.normal) can use it.
template <class V>
static void ComputeSmoothNormals(std::vector<V>& verts, const std::vector<uint32_t>& indices) {
    for (auto& v : verts) v.normal = { 0.0f, 0.0f, 0.0f };
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        auto& a = verts[indices[i]];
        auto& b = verts[indices[i + 1]];
        auto& c = verts[indices[i + 2]];
        float ux = b.pos.x - a.pos.x, uy = b.pos.y - a.pos.y, uz = b.pos.z - a.pos.z;
        float wx = c.pos.x - a.pos.x, wy = c.pos.y - a.pos.y, wz = c.pos.z - a.pos.z;
        float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        a.normal.x += nx; a.normal.y += ny; a.normal.z += nz;
        b.normal.x += nx; b.normal.y += ny; b.normal.z += nz;
        c.normal.x += nx; c.normal.y += ny; c.normal.z += nz;
    }
    for (auto& v : verts) {
        float l = std::sqrt(v.normal.x * v.normal.x + v.normal.y * v.normal.y + v.normal.z * v.normal.z);
        if (l > 1e-8f) { v.normal.x /= l; v.normal.y /= l; v.normal.z /= l; }
    }
}

// Compute per-vertex tangents from positions + UVs (Lengyel's method) when the glTF supplied no TANGENT
// accessor -- REQUIRED for normal mapping, since the pixel shader builds its TBN from Vertex3D.tangent.
// (DamagedHelmet.glb, our main test asset, ships WITHOUT tangents.) Mirrors ComputeSmoothNormals:
// accumulate each triangle's tangent/bitangent into its 3 verts, then Gram-Schmidt-orthogonalize against
// the (already-finalized) normal and pack the bitangent-handedness sign into w. Call AFTER normals exist.
static void ComputeTangents(std::vector<Vertex3D>& verts, const std::vector<uint32_t>& indices) {
    std::vector<DirectX::XMFLOAT3> tan(verts.size(), { 0.0f, 0.0f, 0.0f });
    std::vector<DirectX::XMFLOAT3> bit(verts.size(), { 0.0f, 0.0f, 0.0f });
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        const Vertex3D& a = verts[i0]; const Vertex3D& b = verts[i1]; const Vertex3D& c = verts[i2];
        const float e1x = b.pos.x - a.pos.x, e1y = b.pos.y - a.pos.y, e1z = b.pos.z - a.pos.z;
        const float e2x = c.pos.x - a.pos.x, e2y = c.pos.y - a.pos.y, e2z = c.pos.z - a.pos.z;
        const float du1 = b.uv.x - a.uv.x, dv1 = b.uv.y - a.uv.y;
        const float du2 = c.uv.x - a.uv.x, dv2 = c.uv.y - a.uv.y;
        const float det = du1 * dv2 - du2 * dv1;
        const float r = (std::fabs(det) > 1e-12f) ? 1.0f / det : 0.0f;   // degenerate UVs -> no contribution
        const float tx = (e1x * dv2 - e2x * dv1) * r, ty = (e1y * dv2 - e2y * dv1) * r, tz = (e1z * dv2 - e2z * dv1) * r;
        const float bx = (e2x * du1 - e1x * du2) * r, by = (e2y * du1 - e1y * du2) * r, bz = (e2z * du1 - e1z * du2) * r;
        for (uint32_t k : { i0, i1, i2 }) {
            tan[k].x += tx; tan[k].y += ty; tan[k].z += tz;
            bit[k].x += bx; bit[k].y += by; bit[k].z += bz;
        }
    }
    for (size_t v = 0; v < verts.size(); ++v) {
        const DirectX::XMFLOAT3& n = verts[v].normal;
        const DirectX::XMFLOAT3& t = tan[v];
        const float nt = n.x * t.x + n.y * t.y + n.z * t.z;          // Gram-Schmidt: t' = t - n*(n.t)
        float ox = t.x - n.x * nt, oy = t.y - n.y * nt, oz = t.z - n.z * nt;
        const float len = std::sqrt(ox * ox + oy * oy + oz * oz);
        if (len > 1e-8f) { ox /= len; oy /= len; oz /= len; }
        else { ox = 1.0f; oy = 0.0f; oz = 0.0f; }                    // degenerate -> arbitrary valid tangent
        const float cx = n.y * oz - n.z * oy, cy = n.z * ox - n.x * oz, cz = n.x * oy - n.y * ox;  // cross(n,t')
        const float hand = (cx * bit[v].x + cy * bit[v].y + cz * bit[v].z) < 0.0f ? -1.0f : 1.0f;  // bitangent sign
        verts[v].tangent = { ox, oy, oz, hand };
    }
}

// Recenter on the AABB midpoint and uniformly scale so the LARGEST axis is 1.0 (translation + uniform
// scale only, so normals are untouched). Makes any model arrive unit-sized at the origin regardless of
// the file's authored units -- see ModelLoader.h for why this matters.
static void NormalizeMesh(std::vector<Vertex3D>& verts) {
    if (verts.empty()) return;
    float mnx = verts[0].pos.x, mny = verts[0].pos.y, mnz = verts[0].pos.z;
    float mxx = mnx, mxy = mny, mxz = mnz;
    for (const auto& v : verts) {
        mnx = std::min(mnx, v.pos.x); mxx = std::max(mxx, v.pos.x);
        mny = std::min(mny, v.pos.y); mxy = std::max(mxy, v.pos.y);
        mnz = std::min(mnz, v.pos.z); mxz = std::max(mxz, v.pos.z);
    }
    float cx = 0.5f * (mnx + mxx), cy = 0.5f * (mny + mxy), cz = 0.5f * (mnz + mxz);
    float ext = std::max(mxx - mnx, std::max(mxy - mny, mxz - mnz));
    float inv = (ext > 1e-8f) ? 1.0f / ext : 1.0f;   // degenerate (single point / flat) -> just recenter
    for (auto& v : verts) {
        v.pos.x = (v.pos.x - cx) * inv;
        v.pos.y = (v.pos.y - cy) * inv;
        v.pos.z = (v.pos.z - cz) * inv;
    }
}

// Convert a glTF mat4 (16 floats, COLUMN-major) into this engine's ROW-major XMFLOAT4X4. This is a
// straight byte copy, NOT a transpose call -- and that's deliberate: the same 16 floats that encode
// matrix M under glTF's (column-major, column-vector) convention encode M^T under our (row-major,
// row-vector) convention, and M*v == v*M^T. So the copied matrix is already the correct row-vector
// transform (same reason the renderer never transposes matrices on upload). CONSEQUENCE for the
// runtime palette: multiply in row-vector order -> palette = inverseBind * jointWorld (which is the
// reverse of the glTF spec's jointWorld * inverseBind).
static DirectX::XMFLOAT4X4 GltfMat(const float* m16) {
    DirectX::XMFLOAT4X4 o;
    std::memcpy(&o, m16, 16 * sizeof(float));
    return o;
}

// A glTF's meshes are positioned by the NODE HIERARCHY, not by their vertex data: a mesh sits at the
// origin and its node supplies translation/rotation/scale, with parent nodes compounding. Reading
// data->meshes directly therefore loads every model unposed -- which for Sponza means ignoring a root
// node scale of 0.008 and getting geometry 125x too large.
//
// This walks the nodes instead and hands back each mesh primitive together with its WORLD matrix.
// cgltf_node_transform_world does the parent chain for us.
struct GltfPrimRef {
    const cgltf_primitive* prim;
    DirectX::XMFLOAT4X4    world;
    bool                   uniformScale;   // false => normals need the inverse-transpose, not the matrix
};

static void CollectGltfPrims(cgltf_data* data, std::vector<GltfPrimRef>& out) {
    out.clear();
    for (size_t ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;

        float m[16];
        cgltf_node_transform_world(&node, m);   // includes every parent's transform
        DirectX::XMFLOAT4X4 world;
        std::memcpy(&world, m, sizeof(world));

        // Detect non-uniform scale so normals can be handled correctly. Row lengths of the upper 3x3
        // are the axis scales; if they differ, transforming normals by the same matrix skews them.
        const float sx = std::sqrt(world._11 * world._11 + world._12 * world._12 + world._13 * world._13);
        const float sy = std::sqrt(world._21 * world._21 + world._22 * world._22 + world._23 * world._23);
        const float sz = std::sqrt(world._31 * world._31 + world._32 * world._32 + world._33 * world._33);
        const float mx = std::max(sx, std::max(sy, sz)), mn = std::min(sx, std::min(sy, sz));
        const bool uniform = (mx - mn) <= 1e-4f * (mx > 1e-6f ? mx : 1.0f);

        for (size_t pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            out.push_back({ &prim, world, uniform });
        }
    }

    // Fallback: a file whose meshes aren't referenced by any node (rare, but valid). Load them at
    // identity rather than dropping the geometry entirely.
    if (out.empty()) {
        DirectX::XMFLOAT4X4 ident;
        DirectX::XMStoreFloat4x4(&ident, DirectX::XMMatrixIdentity());
        for (size_t mi = 0; mi < data->meshes_count; ++mi)
            for (size_t pi = 0; pi < data->meshes[mi].primitives_count; ++pi) {
                const cgltf_primitive& prim = data->meshes[mi].primitives[pi];
                if (prim.type != cgltf_primitive_type_triangles) continue;
                out.push_back({ &prim, ident, true });
            }
    }
}

// Applies a node's world matrix to a position.
static inline DirectX::XMFLOAT3 XformPos(const DirectX::XMFLOAT4X4& m, const DirectX::XMFLOAT3& p) {
    DirectX::XMVECTOR v = DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&p),
                                                      DirectX::XMLoadFloat4x4(&m));
    DirectX::XMFLOAT3 o; DirectX::XMStoreFloat3(&o, v);
    return o;
}

// Applies a node's world matrix to a DIRECTION (normal/tangent): no translation, and re-normalized
// because scale changes length. Non-uniform scale would strictly need the inverse-transpose; the
// caller flags that case and this stays the common-path approximation.
static inline DirectX::XMFLOAT3 XformDir(const DirectX::XMFLOAT4X4& m, const DirectX::XMFLOAT3& d) {
    DirectX::XMVECTOR v = DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&d),
                                                            DirectX::XMLoadFloat4x4(&m));
    v = DirectX::XMVector3Normalize(v);
    DirectX::XMFLOAT3 o; DirectX::XMStoreFloat3(&o, v);
    return o;
}

Mesh LoadObjMesh(const std::string& path, ResourceManager& rm, bool normalize) {
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string err;   // tinyobjloader v1.0.6 combines warnings + errors into one string

    // tinyobjloader resolves the `mtllib` (and thus its map_Kd paths) relative to mtl_basedir, NOT next
    // to the .obj -- a null basedir sends it looking in the process CWD, which under the VS debugger is
    // the project dir, not where the .obj lives, so a default.mtl sitting right beside the .obj isn't
    // found. Pass the .obj's own directory (path is already exe-anchored by the caller).
    std::string baseDir;
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) baseDir = path.substr(0, slash + 1);

    // triangulate = true -> every face comes back as triangles (no polygon handling needed downstream).
    bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &err,
                               path.c_str(), baseDir.c_str(), /*triangulate*/ true);
    if (!err.empty()) OutputDebugStringA(("[LoadObjMesh] " + path + ": " + err + "\n").c_str());
    if (!ok) {
        OutputDebugStringA(("[LoadObjMesh] FAILED to load " + path + "\n").c_str());
        return Mesh{};                       // vertexBuffer == null -> caller must check
    }

    // De-index. .obj indexes position/normal/uv SEPARATELY (f p/t/n), but Vertex3D interleaves them,
    // so each unique (p,n,t) triple becomes one Vertex3D; the map dedups verts shared between faces.
    struct Key { int p, n, t; bool operator==(const Key& o) const { return p == o.p && n == o.n && t == o.t; } };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return (size_t)(k.p * 73856093) ^ (size_t)(k.n * 19349663) ^ (size_t)(k.t * 83492791);
        }
    };

    std::vector<Vertex3D> verts;
    std::vector<uint32_t>  indices;
    std::unordered_map<Key, uint32_t, KeyHash> uniq;
    const bool fileHasNormals = !attrib.normals.empty();

    for (const auto& shape : shapes) {
        for (const auto& idx : shape.mesh.indices) {
            Key key{ idx.vertex_index, idx.normal_index, idx.texcoord_index };
            auto found = uniq.find(key);
            if (found != uniq.end()) { indices.push_back(found->second); continue; }

            Vertex3D v{};                    // value-init: pos/normal/uv all zero
            v.pos = { attrib.vertices[3 * idx.vertex_index + 0],
                      attrib.vertices[3 * idx.vertex_index + 1],
                      attrib.vertices[3 * idx.vertex_index + 2] };
            if (idx.normal_index >= 0)
                v.normal = { attrib.normals[3 * idx.normal_index + 0],
                             attrib.normals[3 * idx.normal_index + 1],
                             attrib.normals[3 * idx.normal_index + 2] };
            if (idx.texcoord_index >= 0)
                v.uv = { attrib.texcoords[2 * idx.texcoord_index + 0],
                         attrib.texcoords[2 * idx.texcoord_index + 1] };

            uint32_t vi = (uint32_t)verts.size();
            verts.push_back(v);
            uniq.emplace(key, vi);
            indices.push_back(vi);
        }
    }

    if (!fileHasNormals) ComputeSmoothNormals(verts, indices);

    if (verts.empty() || indices.empty()) {
        OutputDebugStringA(("[LoadObjMesh] " + path + " produced no geometry\n").c_str());
        return Mesh{};
    }

    if (normalize) NormalizeMesh(verts);
    Mesh mesh = rm.CreateMesh<Vertex3D>(verts.data(), (uint32_t)verts.size(),
                                        indices.data(), (uint32_t)indices.size());

    // Make the .obj self-describing like glTF: if the .mtl gave a diffuse (base-color) map, load it as the
    // albedo so the caller needn't hand-assign one. map_Kd is relative to the .mtl's directory (== baseDir).
    // Use the first material that actually has a texture (geometry is merged into one mesh, so we can only
    // carry one albedo -- same "first material" simplification as the glTF loader). Async load: the handle
    // is valid immediately and the texture pops in once PumpAsyncUploads completes it, exactly like the glTF
    // embedded textures (Submit gates on IsTextureReady). If no material has a map_Kd, albedo stays invalid
    // and the caller can assign one, so existing scenes that set mesh.material.albedo by hand still work.
    for (const auto& m : materials) {
        if (!m.diffuse_texname.empty()) {
            std::string  texPath = baseDir + m.diffuse_texname;
            std::wstring wpath(texPath.begin(), texPath.end());   // ASCII asset paths -> widen directly
            mesh.material.albedo = rm.LoadTextureAsync(wpath);
            break;
        }
    }
    return mesh;
}

// Loads ONE embedded glTF texture (given its cgltf_texture*) from the BIN chunk and returns the async
// handle -- invalid if there's no texture or it's a uri/external image we don't fetch yet. cgltf never
// decodes images; it just points at the raw PNG/JPEG bytes and DirectXTex (via LoadTextureFromMemoryAsync)
// decodes. Keyed by IMAGE index, so two material slots pointing at the SAME image (e.g. occlusion sharing
// the metallic-roughness "ORM" texture) resolve to ONE cached upload, not two.
static TextureHandle LoadGlbEmbeddedTexture(cgltf_data* data, const std::string& path,
                                            ResourceManager& rm, const cgltf_texture* tex,
                                            bool srgb) {
    if (!tex || !tex->image) return {};
    const cgltf_image* img = tex->image;

    // EXTERNAL image (a .gltf next to a folder of .jpg/.png, which is how most real assets ship --
    // Sponza has 70 of them). The uri is relative TO THE .gltf, not to the exe or the working
    // directory, so it has to be resolved against that file's own folder. Percent-escapes are
    // decoded because glTF uris are URL-encoded ("%20" for spaces in a texture name).
    if (!img->buffer_view) {
        if (!img->uri || img->uri[0] == '\0') return {};
        if (strncmp(img->uri, "data:", 5) == 0) return {};    // base64 data uri -- cgltf puts these in buffer_view
        std::string uri = img->uri;
        cgltf_decode_uri(&uri[0]);                            // decodes in place; result is <= original length
        uri.resize(strlen(uri.c_str()));

        std::string dir;
        const size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) dir = path.substr(0, slash + 1);
        const std::string full = dir + uri;
        // Keyed by resolved path, so the same image referenced by several materials uploads ONCE.
        return rm.LoadTextureAsync(std::wstring(full.begin(), full.end()), srgb);
    }

    const cgltf_buffer_view* bv = img->buffer_view;
    const uint8_t* bytes = cgltf_buffer_view_data(bv);       // handles the extension data-override case
    if (!bytes) return {};
    const size_t imgIndex = static_cast<size_t>(img - data->images);
    const std::string key = path + "#img" + std::to_string(imgIndex);
    return rm.LoadTextureFromMemoryAsync(key, bytes, bv->size, srgb);
}

// Fills a Material from the FIRST primitive material in the glTF (all primitives merge into one Mesh, so we
// carry one material -- correct for single-material assets like CesiumMan / DamagedHelmet). Reads every map
// we support: base color, metallic-roughness, emissive, occlusion, plus the scalar factors. NORMAL maps are
// deliberately not read yet (Phase 2 -- needs vertex tangents). Missing maps stay invalid, so the shader
// falls back to the factor alone -- exactly how an untextured procedural mesh already behaves.
// Fills a Material from ONE specific cgltf_material. Split out of LoadGlbMaterial so multi-material
// loading (LoadGlbModel) can call it per material group instead of only ever reading the first.
static void LoadGlbMaterialFrom(cgltf_data* data, const std::string& path, ResourceManager& rm,
                                const cgltf_material* mat, Material& out) {
    if (!mat) return;

    if (mat->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = mat->pbr_metallic_roughness;
        out.albedo     = LoadGlbEmbeddedTexture(data, path, rm, pbr.base_color_texture.texture, true);
        out.metalRough = LoadGlbEmbeddedTexture(data, path, rm, pbr.metallic_roughness_texture.texture, false);
        out.metallic   = pbr.metallic_factor;
        out.roughness  = pbr.roughness_factor;
        out.baseColorFactor = { pbr.base_color_factor[0], pbr.base_color_factor[1],
                                pbr.base_color_factor[2], pbr.base_color_factor[3] };
    }
    out.emissive       = LoadGlbEmbeddedTexture(data, path, rm, mat->emissive_texture.texture, true);   // COLOUR
    out.emissiveFactor = { mat->emissive_factor[0], mat->emissive_factor[1], mat->emissive_factor[2] };
    out.occlusion      = LoadGlbEmbeddedTexture(data, path, rm, mat->occlusion_texture.texture, false);
    out.normal         = LoadGlbEmbeddedTexture(data, path, rm, mat->normal_texture.texture, false);   // tangent-space normals

}

// Single-material convenience: finds the first primitive material and reads it. Correct for
// single-material assets (CesiumMan, DamagedHelmet); LoadGlbMesh's whole model is "one merged mesh,
// one material", so this is what it wants. Multi-material assets need LoadGlbModel instead.
static void LoadGlbMaterial(cgltf_data* data, const std::string& path, ResourceManager& rm, Material& out) {
    const cgltf_material* mat = nullptr;
    for (size_t mi = 0; mi < data->meshes_count && !mat; ++mi)
        for (size_t pi = 0; pi < data->meshes[mi].primitives_count; ++pi)
            if (data->meshes[mi].primitives[pi].material) { mat = data->meshes[mi].primitives[pi].material; break; }
    LoadGlbMaterialFrom(data, path, rm, mat, out);

    OutputDebugStringA(("[LoadGlbMaterial] " + path + ": albedo=" + std::to_string(out.albedo.IsValid()) +
        " metalRough=" + std::to_string(out.metalRough.IsValid()) +
        " emissive=" + std::to_string(out.emissive.IsValid()) +
        " occlusion=" + std::to_string(out.occlusion.IsValid()) +
        " normal=" + std::to_string(out.normal.IsValid()) + "\n").c_str());
}

// Loads a glTF / GLB into ONE Mesh (all mesh primitives merged), STATIC geometry only. Unlike .obj,
// glTF stores one index per vertex with attributes already aligned, so there's no de-index step --
// POSITION/NORMAL/TEXCOORD_0 are read straight across [0, count). Skinning & animation (JOINTS_0/
// WEIGHTS_0, skins, animations) are DELIBERATELY IGNORED: a skinned mesh's vertices are stored in
// bind pose, so it loads as a static figure standing in that pose -- perfect for a loader test, and
// the base for real skeletal animation later. The material's EMBEDDED base-color texture IS now loaded
// (mesh.material.albedo) via LoadGlbAlbedo; if the model has none, albedo stays invalid -- assign one.
//
// NOTE (handedness): glTF is right-handed; this renderer is left-handed (XMMatrix*LH) with CULL_NONE,
// so the model shows but may appear mirrored along Z. A proper RH->LH conversion (negate z on
// pos/normal + reverse index winding) is a later correctness pass, not needed to see geometry.
Mesh LoadGlbMesh(const std::string& path, ResourceManager& rm, bool normalize) {
    cgltf_options options = {};
    cgltf_data*   data    = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        OutputDebugStringA(("[LoadGlbMesh] FAILED to parse " + path + "\n").c_str());
        return Mesh{};
    }
    // Resolves external .bin files and decodes the GLB binary chunk / base64 data URIs into data->buffers.
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        OutputDebugStringA(("[LoadGlbMesh] FAILED to load buffers for " + path + "\n").c_str());
        cgltf_free(data);
        return Mesh{};
    }

    std::vector<Vertex3D> verts;
    std::vector<uint32_t>  indices;
    bool anyNormals  = false;
    bool anyTangents = false;

    // NODE HIERARCHY, not data->meshes -- a glTF positions its meshes via nodes, and reading meshes
    // directly drops every translation/rotation/scale in the file (Sponza's root node scale of 0.008
    // is what made it load 125x too big). CollectGltfPrims hands back each primitive with the world
    // matrix its node chain implies; it's baked into the vertices because a Mesh carries only one
    // model matrix at submit time.
    std::vector<GltfPrimRef> glbPrims;
    CollectGltfPrims(data, glbPrims);
    for (const GltfPrimRef& pr : glbPrims) {
        {
            const cgltf_primitive& prim = *pr.prim;

            const cgltf_accessor* posA = nullptr;
            const cgltf_accessor* nrmA = nullptr;
            const cgltf_accessor* uvA  = nullptr;
            const cgltf_accessor* tanA = nullptr;   // TANGENT (vec4: xyz dir + w handedness) for normal mapping
            for (size_t ai = 0; ai < prim.attributes_count; ++ai) {
                const cgltf_attribute& at = prim.attributes[ai];
                if      (at.type == cgltf_attribute_type_position)                  posA = at.data;
                else if (at.type == cgltf_attribute_type_normal)                    nrmA = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uvA  = at.data;
                else if (at.type == cgltf_attribute_type_tangent)                   tanA = at.data;
            }
            if (!posA) continue;   // no positions -> nothing to draw from this primitive

            uint32_t base   = (uint32_t)verts.size();
            size_t   vcount = posA->count;
            for (size_t v = 0; v < vcount; ++v) {
                Vertex3D out{};
                float p[3] = { 0, 0, 0 };
                cgltf_accessor_read_float(posA, v, p, 3);   // handles component type + stride for us
                out.pos = { p[0], p[1], p[2] };
                if (nrmA) {
                    float n[3] = { 0, 0, 0 };
                    cgltf_accessor_read_float(nrmA, v, n, 3);
                    out.normal = { n[0], n[1], n[2] };
                    anyNormals = true;
                }
                if (uvA) {
                    float t[2] = { 0, 0 };
                    cgltf_accessor_read_float(uvA, v, t, 2);
                    out.uv = { t[0], t[1] };
                }
                if (tanA) {   // glTF packs the vec4 as (tangent.xyz, handedness.w); read all 4 straight across
                    float tg[4] = { 0, 0, 0, 0 };
                    cgltf_accessor_read_float(tanA, v, tg, 4);
                    out.tangent = { tg[0], tg[1], tg[2], tg[3] };
                    anyTangents = true;
                }   // else: tangent stays {0,0,0,0}; ComputeTangents() fills it from UVs below (DamagedHelmet path).
                verts.push_back(out);
            }

            if (prim.indices) {
                for (size_t k = 0; k < prim.indices->count; ++k)
                    indices.push_back(base + (uint32_t)cgltf_accessor_read_index(prim.indices, k));
            } else {   // non-indexed: vertices are already in draw order
                for (size_t k = 0; k < vcount; ++k) indices.push_back(base + (uint32_t)k);
            }
        }
    }

    // Read the full material (all maps + factors) BEFORE cgltf_free -- it copies the texture bytes out.
    Material mat; LoadGlbMaterial(data, path, rm, mat);
    cgltf_free(data);   // geometry is copied into our vectors now; the parser data can go

    if (verts.empty() || indices.empty()) {
        OutputDebugStringA(("[LoadGlbMesh] " + path + " produced no geometry\n").c_str());
        return Mesh{};
    }
    if (!anyNormals)  ComputeSmoothNormals(verts, indices);   // normals first -- tangents Gram-Schmidt against them
    if (!anyTangents) ComputeTangents(verts, indices);        // glTF had no TANGENT (e.g. DamagedHelmet) -> derive
    if (normalize)    NormalizeMesh(verts);

    Mesh mesh = rm.CreateMesh<Vertex3D>(verts.data(), (uint32_t)verts.size(),
                                        indices.data(), (uint32_t)indices.size());
    mesh.material = mat;   // full PBR material (albedo/metalRough/emissive/occlusion + factors)
    return mesh;
}

SkinnedModel LoadGlbSkinnedMesh(const std::string& path, ResourceManager& rm) {
    SkinnedModel result;   // failure sentinel: mesh.vertexBuffer null + skeleton.joints empty

    cgltf_options options = {};
    cgltf_data*   data    = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        OutputDebugStringA(("[LoadGlbSkinnedMesh] FAILED to parse " + path + "\n").c_str());
        return result;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        OutputDebugStringA(("[LoadGlbSkinnedMesh] FAILED to load buffers for " + path + "\n").c_str());
        cgltf_free(data);
        return result;
    }
    if (data->skins_count == 0) {   // no skin -> not a skinned mesh; caller should use LoadGlbMesh
        OutputDebugStringA(("[LoadGlbSkinnedMesh] " + path + " has no skin\n").c_str());
        cgltf_free(data);
        return result;
    }
    const cgltf_skin& skin = data->skins[0];   // single-skin assumption (holds for typical characters)

    std::vector<SkinnedVertex3D> verts;
    std::vector<uint32_t>        indices;
    bool anyNormals = false;

    for (size_t mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];
        for (size_t pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor *posA = nullptr, *nrmA = nullptr, *uvA = nullptr,
                                 *jntA = nullptr, *wgtA = nullptr;
            for (size_t ai = 0; ai < prim.attributes_count; ++ai) {
                const cgltf_attribute& at = prim.attributes[ai];
                switch (at.type) {
                    case cgltf_attribute_type_position:                        posA = at.data; break;
                    case cgltf_attribute_type_normal:                          nrmA = at.data; break;
                    case cgltf_attribute_type_texcoord: if (at.index == 0)     uvA  = at.data; break;
                    case cgltf_attribute_type_joints:   if (at.index == 0)     jntA = at.data; break;
                    case cgltf_attribute_type_weights:  if (at.index == 0)     wgtA = at.data; break;
                    default: break;
                }
            }
            if (!posA) continue;

            uint32_t base   = (uint32_t)verts.size();
            size_t   vcount = posA->count;
            for (size_t v = 0; v < vcount; ++v) {
                SkinnedVertex3D out{};   // defaults: boneIDs {0,0,0,0}, weights {1,0,0,0}
                float p[3] = { 0, 0, 0 };
                cgltf_accessor_read_float(posA, v, p, 3);
                out.pos = { p[0], p[1], p[2] };
                if (nrmA) {
                    float n[3] = { 0, 0, 0 };
                    cgltf_accessor_read_float(nrmA, v, n, 3);
                    out.normal = { n[0], n[1], n[2] };
                    anyNormals = true;
                }
                if (uvA) {
                    float t[2] = { 0, 0 };
                    cgltf_accessor_read_float(uvA, v, t, 2);
                    out.uv = { t[0], t[1] };
                }
                if (jntA) {   // JOINTS_0 are indices into skin.joints[] -- read as uints regardless of u8/u16
                    cgltf_uint j[4] = { 0, 0, 0, 0 };
                    cgltf_accessor_read_uint(jntA, v, j, 4);
                    out.boneIDs[0] = j[0]; out.boneIDs[1] = j[1];
                    out.boneIDs[2] = j[2]; out.boneIDs[3] = j[3];
                }
                if (wgtA) {
                    float w[4] = { 0, 0, 0, 0 };
                    cgltf_accessor_read_float(wgtA, v, w, 4);
                    out.weights[0] = w[0]; out.weights[1] = w[1];
                    out.weights[2] = w[2]; out.weights[3] = w[3];
                }   // else: keep the {1,0,0,0} default (rigidly follow joint 0, never all-zero)
                verts.push_back(out);
            }

            if (prim.indices) {
                for (size_t k = 0; k < prim.indices->count; ++k)
                    indices.push_back(base + (uint32_t)cgltf_accessor_read_index(prim.indices, k));
            } else {
                for (size_t k = 0; k < vcount; ++k) indices.push_back(base + (uint32_t)k);
            }
        }
    }

    if (verts.empty() || indices.empty()) {
        OutputDebugStringA(("[LoadGlbSkinnedMesh] " + path + " produced no geometry\n").c_str());
        cgltf_free(data);
        return result;
    }
    if (!anyNormals) ComputeSmoothNormals(verts, indices);   // template picks SkinnedVertex3D
    // NOTE: intentionally NO NormalizeMesh -- scaling the verts would desync them from the
    // inverse-bind matrices. Scale the whole model via its model matrix at submit instead.

    result.mesh = rm.CreateMesh<SkinnedVertex3D>(verts.data(), (uint32_t)verts.size(),
                                                 indices.data(), (uint32_t)indices.size());
    LoadGlbMaterial(data, path, rm, result.mesh.material);   // full PBR material (maps + factors) into the mesh

    // Build the skeleton: one Joint per skin joint. A vertex's boneID k refers to joints[k].
    const size_t jc = skin.joints_count;
    result.skeleton.joints.resize(jc);
    auto jointIndexOf = [&](const cgltf_node* n) -> int {
        for (size_t i = 0; i < jc; ++i) if (skin.joints[i] == n) return (int)i;
        return -1;   // parent lies outside the skin's joint set (e.g. above the skeleton root)
    };
    for (size_t j = 0; j < jc; ++j) {
        const cgltf_node* node = skin.joints[j];
        Joint& out = result.skeleton.joints[j];
        out.parent = node->parent ? jointIndexOf(node->parent) : -1;

        // Bind-pose local T/R/S (glTF node defaults when a component is absent: T=0, R=identity, S=1).
        out.bindT = node->has_translation
            ? DirectX::XMFLOAT3(node->translation[0], node->translation[1], node->translation[2])
            : DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        out.bindR = node->has_rotation
            ? DirectX::XMFLOAT4(node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3])
            : DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        out.bindS = node->has_scale
            ? DirectX::XMFLOAT3(node->scale[0], node->scale[1], node->scale[2])
            : DirectX::XMFLOAT3(1.0f, 1.0f, 1.0f);

        if (skin.inverse_bind_matrices) {
            float ibm[16] = { 0 };
            cgltf_accessor_read_float(skin.inverse_bind_matrices, j, ibm, 16);   // column-major
            out.inverseBind = GltfMat(ibm);
        } else {
            DirectX::XMStoreFloat4x4(&out.inverseBind, DirectX::XMMatrixIdentity()); // spec: absent = identity
        }
    }

    // Read animation clips. Each channel holds keyframes for ONE property (T/R/S) of ONE joint.
    for (size_t a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& anim = data->animations[a];
        AnimationClip clip;
        for (size_t c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& ch = anim.channels[c];
            if (!ch.target_node || !ch.sampler) continue;
            int jidx = jointIndexOf(ch.target_node);
            if (jidx < 0) continue;   // targets a non-joint node (or a morph-weights channel)
            int path;
            switch (ch.target_path) {
                case cgltf_animation_path_type_translation: path = 0; break;
                case cgltf_animation_path_type_rotation:    path = 1; break;
                case cgltf_animation_path_type_scale:       path = 2; break;
                default: continue;    // morph-target weights etc. not supported
            }
            const cgltf_accessor* in = ch.sampler->input;    // keyframe times
            const cgltf_accessor* ov = ch.sampler->output;   // keyframe values
            if (!in || !ov) continue;

            AnimChannel out;
            out.joint = jidx;
            out.path  = path;
            const size_t nk    = in->count;
            const int    comps = (path == 1) ? 4 : 3;   // rotation = quat (4); translation/scale = vec3 (3)
            out.times.resize(nk);
            out.values.resize(nk);
            for (size_t k = 0; k < nk; ++k) {
                float tt = 0.0f;
                cgltf_accessor_read_float(in, k, &tt, 1);
                out.times[k] = tt;
                if (tt > clip.duration) clip.duration = tt;
                float v[4] = { 0, 0, 0, 0 };
                cgltf_accessor_read_float(ov, k, v, comps);
                out.values[k] = { v[0], v[1], v[2], v[3] };
            }
            clip.channels.push_back(std::move(out));
        }
        result.animations.push_back(std::move(clip));
    }

    cgltf_free(data);
    OutputDebugStringA(("[LoadGlbSkinnedMesh] " + path + ": " + std::to_string(verts.size()) +
                        " verts, " + std::to_string(jc) + " joints, " +
                        std::to_string(result.animations.size()) + " clip(s)\n").c_str());
    return result;
}

std::vector<DirectX::XMFLOAT4X4> SamplePose(const Skeleton& skel, const AnimationClip& clip, float time) {
    using namespace DirectX;
    const size_t jc = skel.joints.size();

    // 1) Start every joint at its bind T/R/S. Stored as XMFLOAT4 (not XMVECTOR) so the std::vectors
    //    don't need over-aligned allocation; loaded to SIMD only where used.
    std::vector<XMFLOAT4> T(jc), R(jc), S(jc);
    for (size_t j = 0; j < jc; ++j) {
        const Joint& jt = skel.joints[j];
        T[j] = XMFLOAT4(jt.bindT.x, jt.bindT.y, jt.bindT.z, 0.0f);
        R[j] = jt.bindR;   // quaternion
        S[j] = XMFLOAT4(jt.bindS.x, jt.bindS.y, jt.bindS.z, 0.0f);
    }

    // 2) Apply each channel at `time` -- LINEAR interp, clamped to the clip's ends (caller loops time).
    for (const auto& ch : clip.channels) {
        if (ch.joint < 0 || (size_t)ch.joint >= jc || ch.times.empty()) continue;
        const size_t n = ch.times.size();
        size_t k = 0; float f = 0.0f;
        if (time <= ch.times.front())     { k = 0;     f = 0.0f; }
        else if (time >= ch.times.back()) { k = n - 1; f = 0.0f; }
        else {
            while (k + 1 < n && ch.times[k + 1] <= time) ++k;
            const float t0 = ch.times[k], t1 = ch.times[k + 1];
            f = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;
        }
        XMVECTOR v0 = XMLoadFloat4(&ch.values[k]);
        XMVECTOR v1 = XMLoadFloat4(&ch.values[(k + 1 < n) ? (k + 1) : k]);
        if      (ch.path == 1) XMStoreFloat4(&R[ch.joint], XMQuaternionSlerp(v0, v1, f)); // rotation
        else if (ch.path == 0) XMStoreFloat4(&T[ch.joint], XMVectorLerp(v0, v1, f));       // translation
        else                   XMStoreFloat4(&S[ch.joint], XMVectorLerp(v0, v1, f));       // scale
    }

    // 3) Compose local = S*R*T, walk parents for world, then palette = inverseBind * world (row-vector
    //    order -- see GltfMat). Parent-before-child order means world[parent] is already stored.
    std::vector<XMFLOAT4X4> world(jc), palette(jc);
    for (size_t j = 0; j < jc; ++j) {
        XMMATRIX local = XMMatrixScalingFromVector(XMLoadFloat4(&S[j])) *
                         XMMatrixRotationQuaternion(XMLoadFloat4(&R[j])) *
                         XMMatrixTranslationFromVector(XMLoadFloat4(&T[j]));
        const int p = skel.joints[j].parent;
        XMMATRIX w = (p >= 0) ? (local * XMLoadFloat4x4(&world[p])) : local;
        XMStoreFloat4x4(&world[j], w);
        XMStoreFloat4x4(&palette[j], XMLoadFloat4x4(&skel.joints[j].inverseBind) * w);
    }
    return palette;
}


// CPU-only geometry read -- see ModelLoader.h for why this exists separately from LoadGlbMesh.
// Deliberately mirrors that function's primitive walk and index handling, so the triangles it
// produces are the same triangles that get drawn. It just skips everything GPU-side: no normals,
// no tangents, no UVs, no material, no upload.
bool LoadGlbGeometry(const std::string& path,
                     std::vector<DirectX::XMFLOAT3>& outPositions,
                     std::vector<uint32_t>& outIndices,
                     bool normalize) {
    outPositions.clear();
    outIndices.clear();

    cgltf_options options = {};
    cgltf_data*   data    = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        OutputDebugStringA(("[LoadGlbGeometry] FAILED to parse " + path + "\n").c_str());
        return false;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        OutputDebugStringA(("[LoadGlbGeometry] FAILED to load buffers for " + path + "\n").c_str());
        cgltf_free(data);
        return false;
    }

    // Node hierarchy, same as the render path -- collision geometry has to sit exactly where the
    // rendered geometry does, so it must apply the identical node transforms.
    std::vector<GltfPrimRef> prims;
    CollectGltfPrims(data, prims);

    for (const GltfPrimRef& pr : prims) {
        const cgltf_primitive& prim = *pr.prim;
        const cgltf_accessor* posA = nullptr;
        for (size_t ai = 0; ai < prim.attributes_count; ++ai)
            if (prim.attributes[ai].type == cgltf_attribute_type_position)
                posA = prim.attributes[ai].data;
        if (!posA) continue;

        // Every primitive appends to ONE buffer, so its indices shift by however many vertices
        // came before it -- same merge LoadGlbMesh performs.
        const uint32_t base   = (uint32_t)outPositions.size();
        const size_t   vcount = posA->count;
        for (size_t v = 0; v < vcount; ++v) {
            float p[3] = { 0, 0, 0 };
            cgltf_accessor_read_float(posA, v, p, 3);
            outPositions.push_back(XformPos(pr.world, { p[0], p[1], p[2] }));
        }

        if (prim.indices) {
            for (size_t k = 0; k < prim.indices->count; ++k)
                outIndices.push_back(base + (uint32_t)cgltf_accessor_read_index(prim.indices, k));
        } else {
            // Non-indexed primitive: vertices are already in triangle order.
            for (size_t k = 0; k < vcount; ++k) outIndices.push_back(base + (uint32_t)k);
        }
    }
    cgltf_free(data);

    if (outPositions.empty() || outIndices.size() < 3) {
        OutputDebugStringA(("[LoadGlbGeometry] " + path + " produced no triangles\n").c_str());
        outPositions.clear();
        outIndices.clear();
        return false;
    }

    // Optional unit-cube normalize, matching LoadGlbMesh's `normalize` so a collision mesh can be
    // made to line up with a render mesh loaded the same way. Defaults OFF here: level geometry
    // wants its authored scale, and silently resizing a collision world is a nasty surprise.
    if (normalize) {
        DirectX::XMFLOAT3 lo = outPositions[0], hi = outPositions[0];
        for (const auto& p : outPositions) {
            lo.x = p.x < lo.x ? p.x : lo.x;  lo.y = p.y < lo.y ? p.y : lo.y;  lo.z = p.z < lo.z ? p.z : lo.z;
            hi.x = p.x > hi.x ? p.x : hi.x;  hi.y = p.y > hi.y ? p.y : hi.y;  hi.z = p.z > hi.z ? p.z : hi.z;
        }
        const DirectX::XMFLOAT3 c{ (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
        float ext = hi.x - lo.x;
        ext = (hi.y - lo.y) > ext ? (hi.y - lo.y) : ext;
        ext = (hi.z - lo.z) > ext ? (hi.z - lo.z) : ext;
        const float s = (ext > 1e-6f) ? (1.0f / ext) : 1.0f;
        for (auto& p : outPositions) {
            p.x = (p.x - c.x) * s;  p.y = (p.y - c.y) * s;  p.z = (p.z - c.z) * s;
        }
    }
    return true;
}


// .obj counterpart of LoadGlbGeometry. Note this does NOT de-index the way LoadObjMesh does: that
// function builds unique (position, normal, uv) triples because a render vertex interleaves all
// three, but collision only cares about positions -- so the raw position array IS the vertex list,
// and face indices can reference it directly. That also makes the collision mesh smaller than the
// render mesh for the same file, since positions shared between faces stay shared.
bool LoadObjGeometry(const std::string& path,
                     std::vector<DirectX::XMFLOAT3>& outPositions,
                     std::vector<uint32_t>& outIndices,
                     bool normalize) {
    outPositions.clear();
    outIndices.clear();

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string err;

    std::string baseDir;
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) baseDir = path.substr(0, slash + 1);

    // triangulate = true, so n-gon faces arrive as triangles and the index stream is uniform.
    bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &err,
                               path.c_str(), baseDir.c_str(), /*triangulate*/ true);
    if (!err.empty()) OutputDebugStringA(("[LoadObjGeometry] " + path + ": " + err + "\n").c_str());
    if (!ok) {
        OutputDebugStringA(("[LoadObjGeometry] FAILED to load " + path + "\n").c_str());
        return false;
    }

    const size_t vcount = attrib.vertices.size() / 3;
    outPositions.reserve(vcount);
    for (size_t i = 0; i < vcount; ++i)
        outPositions.push_back({ attrib.vertices[3 * i + 0],
                                 attrib.vertices[3 * i + 1],
                                 attrib.vertices[3 * i + 2] });

    for (const auto& shape : shapes)
        for (const auto& idx : shape.mesh.indices)
            if (idx.vertex_index >= 0 && (size_t)idx.vertex_index < vcount)
                outIndices.push_back((uint32_t)idx.vertex_index);

    if (outPositions.empty() || outIndices.size() < 3) {
        OutputDebugStringA(("[LoadObjGeometry] " + path + " produced no triangles\n").c_str());
        outPositions.clear();
        outIndices.clear();
        return false;
    }

    if (normalize) {
        DirectX::XMFLOAT3 lo = outPositions[0], hi = outPositions[0];
        for (const auto& p : outPositions) {
            lo.x = p.x < lo.x ? p.x : lo.x;  lo.y = p.y < lo.y ? p.y : lo.y;  lo.z = p.z < lo.z ? p.z : lo.z;
            hi.x = p.x > hi.x ? p.x : hi.x;  hi.y = p.y > hi.y ? p.y : hi.y;  hi.z = p.z > hi.z ? p.z : hi.z;
        }
        const DirectX::XMFLOAT3 c{ (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
        float ext = hi.x - lo.x;
        ext = (hi.y - lo.y) > ext ? (hi.y - lo.y) : ext;
        ext = (hi.z - lo.z) > ext ? (hi.z - lo.z) : ext;
        const float s = (ext > 1e-6f) ? (1.0f / ext) : 1.0f;
        for (auto& p : outPositions) {
            p.x = (p.x - c.x) * s;  p.y = (p.y - c.y) * s;  p.z = (p.z - c.z) * s;
        }
    }
    return true;
}

bool LoadMeshGeometry(const std::string& path,
                      std::vector<DirectX::XMFLOAT3>& outPositions,
                      std::vector<uint32_t>& outIndices,
                      bool normalize) {
    // Lowercase the extension so ".OBJ" and ".Glb" work -- downloaded assets are inconsistent.
    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        for (char& c : ext) c = (char)tolower((unsigned char)c);
    }
    if (ext == ".glb" || ext == ".gltf")
        return LoadGlbGeometry(path, outPositions, outIndices, normalize);
    if (ext == ".obj")
        return LoadObjGeometry(path, outPositions, outIndices, normalize);

    OutputDebugStringA(("[LoadMeshGeometry] unsupported extension for " + path +
                        " (expected .glb, .gltf or .obj)\n").c_str());
    outPositions.clear();
    outIndices.clear();
    return false;
}


// Multi-material load -- see ModelLoader.h. Same primitive walk as LoadGlbMesh, but primitives are
// bucketed BY MATERIAL instead of all merged into one buffer, and each bucket becomes its own Mesh
// carrying that material. Grouping (rather than one Mesh per primitive) matters: Sponza is 103
// primitives across ~25 materials, so grouping turns 103 draws into 25.
std::vector<Mesh> LoadGlbModel(const std::string& path, ResourceManager& rm, bool normalize) {
    std::vector<Mesh> out;

    cgltf_options options = {};
    cgltf_data*   data    = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        OutputDebugStringA(("[LoadGlbModel] FAILED to parse " + path + "\n").c_str());
        return out;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        OutputDebugStringA(("[LoadGlbModel] FAILED to load buffers for " + path + "\n").c_str());
        cgltf_free(data);
        return out;
    }

    // One bucket per material. Index materials_count is the catch-all for primitives with no
    // material assigned, so untextured geometry still loads instead of being dropped.
    const size_t matCount = data->materials_count;
    struct Bucket {
        std::vector<Vertex3D> verts;
        std::vector<uint32_t> indices;
        bool anyNormals = false, anyTangents = false;
    };
    std::vector<Bucket> buckets(matCount + 1);

    // Walk the NODE hierarchy, not data->meshes -- see CollectGltfPrims. Each primitive arrives with
    // the world matrix its node chain implies, and that transform is BAKED into the vertices here,
    // because the renderer submits a Mesh with one model matrix and has nowhere to carry per-node
    // transforms of its own.
    std::vector<GltfPrimRef> prims;
    CollectGltfPrims(data, prims);

    for (const GltfPrimRef& pr : prims) {
        const cgltf_primitive& prim = *pr.prim;

        const cgltf_accessor* posA = nullptr; const cgltf_accessor* nrmA = nullptr;
        const cgltf_accessor* uvA  = nullptr; const cgltf_accessor* tanA = nullptr;
        for (size_t ai = 0; ai < prim.attributes_count; ++ai) {
            const cgltf_attribute& at = prim.attributes[ai];
            if      (at.type == cgltf_attribute_type_position)                  posA = at.data;
            else if (at.type == cgltf_attribute_type_normal)                    nrmA = at.data;
            else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uvA  = at.data;
            else if (at.type == cgltf_attribute_type_tangent)                   tanA = at.data;
        }
        if (!posA) continue;

        const size_t bi = prim.material ? (size_t)(prim.material - data->materials) : matCount;
        Bucket& b = buckets[bi];

        const uint32_t base = (uint32_t)b.verts.size();
        const size_t vcount = posA->count;
        for (size_t v = 0; v < vcount; ++v) {
            Vertex3D out3{};
            float p[3] = { 0, 0, 0 };
            cgltf_accessor_read_float(posA, v, p, 3);
            out3.pos = XformPos(pr.world, { p[0], p[1], p[2] });
            if (nrmA) { float n[3] = { 0,0,0 }; cgltf_accessor_read_float(nrmA, v, n, 3);
                        out3.normal = XformDir(pr.world, { n[0], n[1], n[2] }); b.anyNormals = true; }
            if (uvA)  { float t[2] = { 0,0 };   cgltf_accessor_read_float(uvA,  v, t, 2);
                        out3.uv = { t[0], t[1] }; }
            if (tanA) { float g[4] = { 0,0,0,0 }; cgltf_accessor_read_float(tanA, v, g, 4);
                        DirectX::XMFLOAT3 td = XformDir(pr.world, { g[0], g[1], g[2] });
                        out3.tangent = { td.x, td.y, td.z, g[3] };   // w is handedness, not a direction
                        b.anyTangents = true; }
            b.verts.push_back(out3);
        }
        if (prim.indices) {
            for (size_t k = 0; k < prim.indices->count; ++k)
                b.indices.push_back(base + (uint32_t)cgltf_accessor_read_index(prim.indices, k));
        } else {
            for (size_t k = 0; k < vcount; ++k) b.indices.push_back(base + (uint32_t)k);
        }
    }

    // Materials must be read BEFORE cgltf_free -- the texture loader copies bytes / resolves uris here.
    std::vector<Material> mats(matCount + 1);
    for (size_t i = 0; i < matCount; ++i)
        LoadGlbMaterialFrom(data, path, rm, &data->materials[i], mats[i]);
    cgltf_free(data);

    for (size_t i = 0; i < buckets.size(); ++i) {
        Bucket& b = buckets[i];
        if (b.verts.empty() || b.indices.empty()) continue;
        if (!b.anyNormals)  ComputeSmoothNormals(b.verts, b.indices);
        if (!b.anyTangents) ComputeTangents(b.verts, b.indices);
        // NOTE: `normalize` is deliberately NOT applied per bucket -- each mesh would be recentred
        // and rescaled independently, blowing the model apart. A whole-model normalize would have to
        // compute one shared transform across every bucket; level assets want their authored scale
        // anyway, so this defaults off and is left unimplemented rather than implemented wrongly.
        Mesh m = rm.CreateMesh<Vertex3D>(b.verts.data(), (uint32_t)b.verts.size(),
                                         b.indices.data(), (uint32_t)b.indices.size());
        m.material = mats[i];
        out.push_back(m);
    }

    char buf[192];
    snprintf(buf, sizeof(buf), "[LoadGlbModel] %s: %zu materials -> %zu meshes\n",
             path.c_str(), matCount, out.size());
    OutputDebugStringA(buf);
    return out;
}

}


