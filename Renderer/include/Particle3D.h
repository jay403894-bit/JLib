// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <DirectXMath.h>
#include <cstdint>

namespace JLib {
    // One 3D GPU particle. Simulated entirely on the GPU (compute) and drawn as a camera-facing billboard;
    // the CPU only seeds the buffer once. Layout MUST match the `Particle3D` struct in the particle shaders
    // (UpdateParticles3D.hlsl / Particle3D_VS.hlsl) EXACTLY -- it's a StructuredBuffer element shared between
    // the compute UAV and the vertex SRV. Packed into four 16-byte rows (float3+float pairs) so the float4
    // color lands on a 16-byte boundary with no compiler-inserted padding disagreeing between C++ and HLSL:
    //   [ position.xyz | size ]  [ velocity.xyz | lifetime ]  [ age | isActive | pad | pad ]  [ color.rgba ]
    // NOTE (future SoA): this is AoS for a simple first cut. If/when we want the data-oriented layout or
    // CPU-readable gameplay particles, split these into parallel arrays (posX[], posY[], ...) -- the compute
    // shader and the draw SRV would then bind several tight buffers instead of one interleaved one.
    struct Particle3D {
        DirectX::XMFLOAT3 position;   float size;       // size = world-space billboard half-extent
        DirectX::XMFLOAT3 velocity;   float lifetime;   // lifetime = seconds remaining (dead at <= 0)
        float             age;                          // seconds since (re)spawn -- drives color/atlas progress
        uint32_t          isActive;                     // 1 = alive/drawn, 0 = invisible
        float             pad0, pad1;
        DirectX::XMFLOAT4 color;                         // spawn color; lerps toward the pool's colorEnd by age
    };
    static_assert(sizeof(Particle3D) == 64, "Particle3D must match the 64-byte HLSL struct");
}
