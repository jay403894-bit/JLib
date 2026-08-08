// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>
#include "Mesh.h"
#include "ResourceManager.h"

namespace JLib {

    // One bone of a skeleton. All matrices are ROW-major / row-vector (this engine's convention) --
    // glTF stores them column-major and they're converted on load (see ModelLoader.cpp / GltfMat).
    struct Joint {
        int parent;                        // index into Skeleton::joints, or -1 if this joint is a root
        DirectX::XMFLOAT4X4 inverseBind;   // mesh space -> this joint's bind-local space
        // Bind-pose local transform kept as separate T/R/S: animation channels override individual
        // components (a joint animated only in rotation keeps its bind T and S), so these can't be
        // pre-collapsed into one matrix. bindR is a quaternion (x,y,z,w).
        DirectX::XMFLOAT3 bindT;
        DirectX::XMFLOAT4 bindR;
        DirectX::XMFLOAT3 bindS;
    };
    // The skeleton a SkinnedVertex3D's boneIDs index into. Flat array; hierarchy is via Joint::parent.
    struct Skeleton {
        std::vector<Joint> joints;         // JOINTS_0 values are indices into this vector
    };

    // One animation track: keyframes for ONE property (translation/rotation/scale) of ONE joint.
    struct AnimChannel {
        int joint;                              // index into Skeleton::joints
        int path;                               // 0 = translation, 1 = rotation (quat), 2 = scale
        std::vector<float>              times;  // keyframe times, seconds (ascending)
        std::vector<DirectX::XMFLOAT4>  values; // xyz for T/S (w unused); xyzw quat for R
    };
    // A named-less animation clip: all its channels + total duration. LINEAR interpolation assumed.
    struct AnimationClip {
        float duration = 0.0f;
        std::vector<AnimChannel> channels;
    };

    // What LoadGlbSkinnedMesh returns: skinned geometry + the skeleton that drives it + any animation
    // clips. On failure, mesh.vertexBuffer is null AND skeleton.joints is empty -- check before using.
    struct SkinnedModel {
        Mesh     mesh;
        Skeleton skeleton;
        std::vector<AnimationClip> animations;
    };

    // Loads a Wavefront .obj into ONE Mesh (all shapes merged), de-indexed into Vertex3D and uploaded
    // via CreateMesh. tinyobjloader triangulates; if the file has no vertex normals, smooth ones are
    // computed. Materials/textures are ignored for now (geometry only -- the normal-color shader needs
    // nothing else). Returns a default-constructed Mesh on failure (its vertexBuffer is null -- CHECK
    // before submitting). Path is narrow (use JLib::ExeRelativeA("models\\x.obj")).
    //
    // normalize (default true): recenter on the AABB midpoint and uniformly scale so the largest
    // dimension is 1 unit. .obj files carry arbitrary units/offsets (the Utah teapot is ~150 wide and
    // sits on y=0), so without this every model needs a hand-tuned per-file scale. Normalized, any model
    // arrives unit-sized at the origin and the scene transform alone controls final size/placement.
    // Pass false to keep authored coordinates (e.g. a level/scene exported in world space).
    Mesh LoadObjMesh(const std::string& path, ResourceManager& rm, bool normalize = true);

    // Loads a glTF / GLB (.gltf or .glb) into ONE Mesh, STATIC geometry only -- all mesh primitives
    // merged, skinning/animation ignored (a skinned mesh loads in its bind pose). Same normalize
    // behavior and same failure contract (null vertexBuffer) as LoadObjMesh. The material's EMBEDDED
    // base-color texture (BIN-chunk PNG/JPEG) IS loaded into mesh.material.albedo via the async texture
    // path -- it's Loading until PumpAsyncUploads() runs, so gate draws on IsTextureReady(albedo). A
    // model with no embedded base-color texture leaves albedo invalid; assign one yourself.
    Mesh LoadGlbMesh(const std::string& path, ResourceManager& rm, bool normalize = true);

    // MULTI-MATERIAL load: one Mesh per material, instead of LoadGlbMesh's single merged Mesh with a
    // single material. Real level assets are always multi-material -- Sponza has ~25 across 103
    // primitives -- so merging them means the whole level renders with whichever texture happened to
    // come first. Primitives sharing a material are merged together, so the result is one Mesh per
    // distinct material (not per primitive), which keeps the draw count at the material count.
    //
    // Submit each returned Mesh with the same world transform to draw the model:
    //     for (auto& m : model) r3d.Submit(m, world);
    //
    // Returns an empty vector on failure. Textures load through the async path as usual, including
    // EXTERNAL uri images (a .gltf beside a folder of .jpg/.png), resolved relative to the .gltf.
    std::vector<Mesh> LoadGlbModel(const std::string& path, ResourceManager& rm, bool normalize = false);

    // CPU-ONLY geometry read: positions + triangle indices, no GPU upload, no materials, no textures.
    // Exists because a loaded Mesh keeps its vertex data on the GPU and nothing else -- so anything
    // that needs to REASON about the geometry (collision meshes, navmesh baking, bounds tools) has
    // no way to get it back. Merges all primitives into one buffer, exactly like LoadGlbMesh, so the
    // triangles line up with what that function draws.
    //
    // The obvious consumer is JLib::Physics3D::AddStaticMesh for level collision. Prefer a separate
    // low-poly collision mesh where you have one: collision cost scales with triangle count, and the
    // detail that matters visually almost never matters for a capsule sliding along a wall.
    //
    // Returns false (and leaves the outputs empty) if the file can't be read or has no triangles.
    bool LoadGlbGeometry(const std::string& path,
                         std::vector<DirectX::XMFLOAT3>& outPositions,
                         std::vector<uint32_t>& outIndices,
                         bool normalize = false);

    // .obj counterpart of LoadGlbGeometry, same contract. Faces are triangulated on load, so n-gons
    // in the source file come back as triangles either way.
    bool LoadObjGeometry(const std::string& path,
                         std::vector<DirectX::XMFLOAT3>& outPositions,
                         std::vector<uint32_t>& outIndices,
                         bool normalize = false);

    // Dispatches on the file extension (.glb/.gltf -> LoadGlbGeometry, .obj -> LoadObjGeometry), so
    // collision loading doesn't care what the artist exported. Unknown extensions return false.
    bool LoadMeshGeometry(const std::string& path,
                          std::vector<DirectX::XMFLOAT3>& outPositions,
                          std::vector<uint32_t>& outIndices,
                          bool normalize = false);

    // Loads a SKINNED glTF / GLB into a SkinnedModel (SkinnedVertex3D geometry + its Skeleton). Reads
    // POSITION/NORMAL/TEXCOORD_0/JOINTS_0/WEIGHTS_0 plus the first skin's joints + inverse-bind matrices.
    // Assumes a single skin (true for typical character models). Does NOT normalize geometry -- that
    // would desync the inverse-bind matrices from the vertices; scale via the model matrix at submit.
    // Animation is NOT applied here: uploading an IDENTITY bone palette reproduces the exact bind pose
    // (a built-in verification checkpoint -- it must match the static LoadGlbMesh result). Fails (null
    // mesh.vertexBuffer / empty skeleton) if the file has no skin or no geometry. The embedded base-color
    // texture loads into mesh.material.albedo (async -- gate draws on IsTextureReady(albedo)), same as
    // LoadGlbMesh; CesiumMan ships its skin texture this way.
    SkinnedModel LoadGlbSkinnedMesh(const std::string& path, ResourceManager& rm);

    // Sample an animation clip at `timeSeconds` into a bone palette ready to upload to the skinning
    // shader's b2 (one row-major float4x4 per joint). For each joint: start from its bind T/R/S, let
    // the clip's channels override, compose local = S*R*T, walk parents for the world transform, then
    // palette[j] = inverseBind[j] * world[j] (row-vector order -- see GltfMat in ModelLoader.cpp).
    // Result size == skeleton.joints.size(). ASSUMES joints are ordered parent-before-child (true for
    // typical exporters incl. CesiumMan) and the skinned mesh node is at identity.
    std::vector<DirectX::XMFLOAT4X4> SamplePose(const Skeleton& skeleton, const AnimationClip& clip,
                                                float timeSeconds);
}
