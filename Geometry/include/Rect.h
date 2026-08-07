#pragma once
#include "Vec2.h"

namespace Physics2D {

// Vec2 lives in namespace PlatformerPhysics2D (see PlatformerPhysics2D/include/Vec2.h), so it has
// to be named. Without this the header only compiled when a consumer happened to have already
// pulled Vec2 into scope -- it was NOT self-contained, and including it first produced "unknown
// override specifier" rather than an honest missing-type error. Fallout from the
// Physics2D -> PlatformerPhysics2D rename.
//
// Worth revisiting the layering rather than the symptom: Vec2 is geometry, so it arguably belongs
// in THIS library with PlatformerPhysics2D depending on Geometry, instead of Geometry reaching
// into the physics library for its own vector type.
using PlatformerPhysics2D::Vec2;

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
