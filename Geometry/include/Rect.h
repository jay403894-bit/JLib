#pragma once
#include "Vec2.h"

namespace Physics2D {

// Center + full size, matching PhysicsWorld's own convention (posX/posY is a cell's center,
// size.x/size.y is full width/height, half-extents computed on the fly) -- not min-corner, so a
// Rect built from a PhysicsWorld row is just { {world.posX[i], world.posY[i]}, world.size[i] }.
// Renderer/dimension-agnostic on purpose (Vec2, no DirectX types) so it works for 2D HUD hit-
// testing now and stays usable if a 3D renderer's HUD ever needs the same screen-space checks.
struct Rect {
    Vec2 center;
    Vec2 size;

    Rect() = default;
    Rect(Vec2 center_, Vec2 size_) : center(center_), size(size_) {}

    bool Contains(Vec2 point) const {
        return std::fabs(point.x - center.x) <= size.x * 0.5f &&
               std::fabs(point.y - center.y) <= size.y * 0.5f;
    }

    bool Intersects(const Rect& other) const {
        return std::fabs(center.x - other.center.x) <= (size.x + other.size.x) * 0.5f &&
               std::fabs(center.y - other.center.y) <= (size.y + other.size.y) * 0.5f;
    }
};

} // namespace Physics2D
