// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <DirectXMath.h>
#include <cstdlib>
namespace JLib {
    // Ported from raylib's Color palette (raylib.h) -- same names, values normalized from
    // raylib's 0-255 uint8_t channels to DirectX::XMFLOAT4's 0.0-1.0 floats (channel / 255.0f).
    namespace Colors {
        const DirectX::XMFLOAT4 CornflowerBlue = { 0.4f, 0.6f, 0.9f, 1.0f };

        const DirectX::XMFLOAT4 LightGray  = { 0.7843f, 0.7843f, 0.7843f, 1.0f };
        const DirectX::XMFLOAT4 Gray       = { 0.5098f, 0.5098f, 0.5098f, 1.0f };
        const DirectX::XMFLOAT4 DarkGray   = { 0.3137f, 0.3137f, 0.3137f, 1.0f };
        const DirectX::XMFLOAT4 Yellow     = { 0.9922f, 0.9765f, 0.0f,    1.0f };
        const DirectX::XMFLOAT4 Gold       = { 1.0f,    0.7961f, 0.0f,    1.0f };
        const DirectX::XMFLOAT4 Orange     = { 1.0f,    0.6314f, 0.0f,    1.0f };
        const DirectX::XMFLOAT4 Pink       = { 1.0f,    0.4275f, 0.7608f, 1.0f };
        const DirectX::XMFLOAT4 Red        = { 0.9020f, 0.1608f, 0.2157f, 1.0f };
        const DirectX::XMFLOAT4 Maroon     = { 0.7451f, 0.1294f, 0.2157f, 1.0f };
        const DirectX::XMFLOAT4 Green      = { 0.0f,    0.8941f, 0.1882f, 1.0f };
        const DirectX::XMFLOAT4 Lime       = { 0.0f,    0.6196f, 0.1843f, 1.0f };
        const DirectX::XMFLOAT4 DarkGreen  = { 0.0f,    0.4588f, 0.1725f, 1.0f };
        const DirectX::XMFLOAT4 SkyBlue    = { 0.4f,    0.7490f, 1.0f,    1.0f };
        const DirectX::XMFLOAT4 Blue       = { 0.0f,    0.4745f, 0.9451f, 1.0f };
        const DirectX::XMFLOAT4 DarkBlue   = { 0.0f,    0.3216f, 0.6745f, 1.0f };
        const DirectX::XMFLOAT4 Purple     = { 0.7843f, 0.4784f, 1.0f,    1.0f };
        const DirectX::XMFLOAT4 Violet     = { 0.5294f, 0.2353f, 0.7451f, 1.0f };
        const DirectX::XMFLOAT4 DarkPurple = { 0.4392f, 0.1216f, 0.4941f, 1.0f };
        const DirectX::XMFLOAT4 Beige      = { 0.8275f, 0.6902f, 0.5137f, 1.0f };
        const DirectX::XMFLOAT4 Brown      = { 0.4980f, 0.4157f, 0.3098f, 1.0f };
        const DirectX::XMFLOAT4 DarkBrown  = { 0.2980f, 0.2471f, 0.1843f, 1.0f };
		const DirectX::XMFLOAT4 Cyan = { 0.0823f, 0.8f, 0.8196f, 1.0f };

		const DirectX::XMFLOAT4 Transparent = { 0.0f,    0.0f,    0.0f,    0.0f };
        const DirectX::XMFLOAT4 White    = { 1.0f,    1.0f,    1.0f,    1.0f };
        // Slightly off-white (245,245,245) -- the standard neutral background for 2D demos, and
        // the equivalent of the one raylib preset this palette doesn't mirror by name. Pure White
        // is the wrong substitute: white sprites and UI vanish against it.
        const DirectX::XMFLOAT4 OffWhite = { 0.9608f, 0.9608f, 0.9608f, 1.0f };
        const DirectX::XMFLOAT4 Black    = { 0.0f,    0.0f,    0.0f,    1.0f };
        const DirectX::XMFLOAT4 Blank    = { 0.0f,    0.0f,    0.0f,    0.0f };
        const DirectX::XMFLOAT4 Magenta  = { 1.0f,    0.0f,    1.0f,    1.0f };

		// inline -- these are defined in a header included by many .cpp files across the project.
		// Without inline, each translation unit emits its own external-linkage copy of the same
		// symbol, and the linker fails with "already defined"/"multiply defined symbols" the
		// moment more than one of them gets linked together (same pattern as GetTime.h's inline
		// statics -- a header-only definition needs inline or a single .cpp home, one or the other).
		inline DirectX::XMFLOAT4 GetRandomColor() {
			float r = static_cast<float>(rand()) / RAND_MAX;
			float g = static_cast<float>(rand()) / RAND_MAX;
			float b = static_cast<float>(rand()) / RAND_MAX;
			return { r, g, b, 1.0f };
		}
		inline DirectX::XMFLOAT4 ColorLerp(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b, float t) {
			return {
				a.x + (b.x - a.x) * t,
				a.y + (b.y - a.y) * t,
				a.z + (b.z - a.z) * t,
				a.w + (b.w - a.w) * t
			};
		}
		inline DirectX::XMFLOAT4 ColorFromHex(uint32_t hex) {
			float r = ((hex >> 16) & 0xFF) / 255.0f;
			float g = ((hex >> 8) & 0xFF) / 255.0f;
			float b = (hex & 0xFF) / 255.0f;
			return { r, g, b, 1.0f };
		}
    }
}
