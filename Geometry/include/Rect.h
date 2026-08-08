// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <DirectXMath.h>
#include <cmath>
namespace JLib {

// Center + full size, matching PhysicsWorld's own convention (posX/posY is a cell's center,
// size.x/size.y is full width/height, half-extents computed on the fly) -- not min-corner, so a
// Rect built from a PhysicsWorld row is just { {world.posX[i], world.posY[i]}, world.size[i] }.
// Uses XMFLOAT2 rather than the physics library's Vec2 ON PURPOSE: Geometry reaching into
// PlatformerPhysics2D for its own vector type would invert the dependency, and XMFLOAT2 is what
// callers already hold (renderer positions, HUD coordinates) so it needs no conversion at the
// boundary. Rect is geometry, not physics - a rectangle with containment and overlap tests, as
// useful for HUD hit-testing as for anything simulated. Hence namespace JLib, like the rest of the
// suite.
struct Rect {
    DirectX::XMFLOAT2 center;
    DirectX::XMFLOAT2 size;

    Rect() = default;
    Rect(DirectX::XMFLOAT2 center_, DirectX::XMFLOAT2 size_) : center(center_), size(size_) {}

    bool Contains(DirectX::XMFLOAT2 point) const {
        return std::fabs(point.x - center.x) <= size.x * 0.5f &&
               std::fabs(point.y - center.y) <= size.y * 0.5f;
    }

    bool Intersects(const Rect& other) const {
        return std::fabs(center.x - other.center.x) <= (size.x + other.size.x) * 0.5f &&
               std::fabs(center.y - other.center.y) <= (size.y + other.size.y) * 0.5f;
    }
};

} // namespace JLib
