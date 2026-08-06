#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include "CollisionLayer.h"
#include "PhysicsWorld.h" // for Handle -- see rowHandles (transitional)

namespace PlatformerPhysics2D {

// Static collision geometry -- the Stage-4 "implicit tilemap." Deliberately the opposite
// of PhysicsWorld in every design decision: no rows, no spawning, no compaction, no
// sorting, no handles. A tile's position IS its index, so "what's at (x, y)" is two
// integer divides and a load -- no broadphase, no grid, no per-substep rebuild. This is
// what the tile sensors will consult directly once they migrate off the SpatialGrid
// (tiles then stop being PhysicsWorld rows entirely).
//
// Renderer-agnostic on purpose (same rule as PhysicsWorld::emitterSlot): the per-cell
// sprite index stays in the game's loaded BinaryLevel -- this struct holds ONLY what
// collision needs.
//
// Values deliberately match Game01's BinaryBehaviorType 1:1 (minus BACKGROUND, which
// never enters collision -- it loads as None here). Never renumber: saved levels store
// these by value.
enum class TileBehavior : uint8_t {
    None           = 0,  // empty / background-only cell
    Solid          = 1,
    OneWay         = 2,  // stand on top, pass through from below
    SlopeUpRight   = 3,
    SlopeUpLeft    = 4,
    SlopeDownLeft  = 5,
    SlopeDownRight = 6,
};

struct TileMap {
    int width = 0, height = 0;
    float tileSize = 16.0f;
    // World position of cell (0,0)'s top-left corner -- lets a map sit anywhere without
    // baking an offset into every lookup's caller.
    float originX = 0.0f, originY = 0.0f;

    // One byte per cell, row-major. Sized once by Init(); never reallocated after.
    std::vector<TileBehavior> behavior;
    // Per-cell friction (full column -- 4 bytes/cell is nothing at map scale). Init fills
    // with the map's default; the level loader stamps per-instance overrides on top.
    std::vector<float> friction;
    // TRANSITIONAL (dies with the dual-write): while static tiles are still ALSO
    // PhysicsWorld rows, this maps each cell to its row's Handle so the sensors can use
    // the tilemap for the SEARCH (O(cells-under-probe) instead of a broadphase query)
    // while still RETURNING a row index -- which is what every sensor caller consumes
    // (posY[hitID], slopeType[hitID], restitution[hitID]...). Handles, not raw indices,
    // because commit()'s sort moves every row every frame; resolve() is O(1) and stays
    // correct. When tiles stop being rows entirely, sensors return tile references
    // instead and this table is deleted along with LevelInstantiator's row block.
    std::vector<Handle> rowHandles;

    void Init(int w, int h, float tile, float ox = 0.0f, float oy = 0.0f,
              float defaultFriction = 0.0f) {
        width = w; height = h; tileSize = tile; originX = ox; originY = oy;
        behavior.assign((size_t)w * h, TileBehavior::None);
        friction.assign((size_t)w * h, defaultFriction);
        rowHandles.assign((size_t)w * h, Handle{}); // default Handle resolves to -1
    }

    bool InBounds(int cx, int cy) const {
        return cx >= 0 && cy >= 0 && cx < width && cy < height;
    }

    // World position -> cell coordinates. NOT clamped: out-of-map queries must read as
    // empty (see BehaviorAt), not alias onto a boundary tile -- the SpatialGrid's clamp
    // convention exists because ITS bins must always be indexable; a tilemap has a true
    // "nothing here" answer instead.
    int CellX(float worldX) const { return (int)std::floor((worldX - originX) / tileSize); }
    int CellY(float worldY) const { return (int)std::floor((worldY - originY) / tileSize); }

    TileBehavior BehaviorAt(float worldX, float worldY) const {
        int cx = CellX(worldX), cy = CellY(worldY);
        if (!InBounds(cx, cy)) return TileBehavior::None;
        return behavior[(size_t)cy * width + cx];
    }
    TileBehavior BehaviorAtCell(int cx, int cy) const {
        if (!InBounds(cx, cy)) return TileBehavior::None;
        return behavior[(size_t)cy * width + cx];
    }
    float FrictionAt(float worldX, float worldY) const {
        int cx = CellX(worldX), cy = CellY(worldY);
        if (!InBounds(cx, cy)) return 0.0f;
        return friction[(size_t)cy * width + cx];
    }

    bool SolidAt(float worldX, float worldY) const {
        TileBehavior b = BehaviorAt(worldX, worldY);
        return b != TileBehavior::None; // one-ways and slopes count -- callers that care
                                        // about direction check the behavior themselves
    }

    // Bridge to the existing slope machinery (GetSlopeSurfaceY etc. speak SlopeType).
    SlopeType SlopeAt(float worldX, float worldY) const {
        switch (BehaviorAt(worldX, worldY)) {
        case TileBehavior::SlopeUpRight:   return SlopeType::UP_RIGHT;
        case TileBehavior::SlopeUpLeft:    return SlopeType::UP_LEFT;
        case TileBehavior::SlopeDownLeft:  return SlopeType::DOWN_LEFT;
        case TileBehavior::SlopeDownRight: return SlopeType::DOWN_RIGHT;
        default:                           return SlopeType::NONE;
        }
    }
};

} // namespace PlatformerPhysics2D
