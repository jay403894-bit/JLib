// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <DirectXMath.h>
#include <cstdint>
namespace JLib {
    struct Vertex {
        float x, y, z;    // Position
        float r, g, b, a; // Color
        float u, v;       // Texture coordinates
    };
    struct Vertex3D {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 uv;
        // Tangent for normal mapping. xyz = the surface tangent (points along +U), w = handedness
        // (+/-1) that orients the bitangent as cross(normal, tangent.xyz) * w -- glTF's TANGENT
        // accessor already packs it this way. Unused (left {0,0,0,0}) on procedural/untextured meshes;
        // the pixel shader only reads it when a normal map is bound (mapFlags bit3), so a zero tangent
        // on those meshes is harmless. Adding this grows the stride 32 -> 48 bytes (see kVertex3DLayout).
        DirectX::XMFLOAT4 tangent;
    };
    // Skinned variant: adds up to 4 bone influences per vertex. boneIDs index into a Skeleton's
    // joints[] (see ModelLoader.h), weights blend those bones' palette matrices in the vertex shader.
    // Exactly 64 bytes, all members 4-byte aligned -> maps 1:1 to the skinned input layout
    // (POSITION/NORMAL/TEXCOORD at 0/12/24, BLENDINDICES uint4 at 32, BLENDWEIGHT float4 at 48).
    struct SkinnedVertex3D {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 uv;
        uint32_t          boneIDs[4] = { 0, 0, 0, 0 };
        float             weights[4] = { 1.0f, 0.0f, 0.0f, 0.0f };  // default: follow bone 0 (never all-zero -> no collapse)
    };
};