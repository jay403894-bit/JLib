#pragma once
#include <cstdint>

namespace PlatformerPhysics2D {

enum CollisionLayer : uint8_t
{
    LAYER_BG = 0,              // 00000000 (Cosmetic sparks, background stars)
    LAYER_PLAYER = 1 << 0,     // 00000001
    LAYER_PLAYER_BULLET = 1 << 1, // 00000010
    LAYER_ENEMY = 1 << 2,      // 00000100
    LAYER_ENEMY_BULLET = 1 << 3,  // 00001000
    LAYER_ITEM = 1 << 4        // 00010000
};

enum TileType {
    Background,
    Solid,
    Platform,
    Slope
};

enum SlopeType
{
    NONE,
    UP_RIGHT,
    UP_LEFT,
    DOWN_LEFT,
    DOWN_RIGHT
};

enum class ShapeType { RECT, CIRCLE, TRIANGLE };

} // namespace PlatformerPhysics2D
