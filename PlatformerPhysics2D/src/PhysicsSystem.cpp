// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#include "../include/PhysicsSystem.h"
#include "../include/PhysicsWorld.h"
#include "../include/SpatialGrid.h"
#include "../include/EntityTable.h"
#include "../include/PlatformTable.h"
#include "../include/PhysicsTuning.h"
#include "../include/TileMap.h"
#include <TaskScheduler.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <numbers>
#include <cstdio>
#include <cfloat>
#include <immintrin.h>

namespace PlatformerPhysics2D {

namespace {
constexpr float kPi = std::numbers::pi_v<float>;

// Free helper, ported verbatim from the source project (not currently called
// anywhere in PhysicsSystem, kept for parity).
bool IsPlayerOnPlatform(float playerX, float centerX) {
    // 80.0f + 2.0f buffer to prevent edge-snapping jitter
    return std::abs(playerX - centerX) <= (80.0f + 2.0f);
}

// The TileMap fast path shared by all four directional sensors: which cells does the
// probe rect touch (usually 1-2), does the map say a tile lives there, and if so resolve
// its row and apply the EXACT same guards + AABB test the grid path applies -- so the
// two paths return the same rows for the same world. CellX/CellY are unclamped, so a
// probe hanging off the map edge just iterates cells whose BehaviorAtCell says None.
int ProbeTileMapRows(const PlatformerPhysics2D::PhysicsWorld& world, const PlatformerPhysics2D::TileMap& tiles,
                     float sensorX, float sensorY, float sensorHW, float sensorHH) {
    using namespace PlatformerPhysics2D;
    float sMinX = sensorX - sensorHW, sMaxX = sensorX + sensorHW;
    float sMinY = sensorY - sensorHH, sMaxY = sensorY + sensorHH;
    int cx0 = tiles.CellX(sMinX), cx1 = tiles.CellX(sMaxX);
    int cy0 = tiles.CellY(sMinY), cy1 = tiles.CellY(sMaxY);

    for (int cy = cy0; cy <= cy1; ++cy) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            if (tiles.BehaviorAtCell(cx, cy) == TileBehavior::None) continue; // also covers out-of-bounds
            int row = world.resolve(tiles.rowHandles[(size_t)cy * tiles.width + cx]);
            if (row < 0) continue; // tile row destroyed (e.g. destructible) -- map not yet cleared

            if (!world.isTile[row] || world.layer[row] == 0 || world.life[row] <= 0.0f) continue;

            float tMinX = world.posX[row] - (world.size[row].x * 0.5f);
            float tMaxX = world.posX[row] + (world.size[row].x * 0.5f);
            float tMinY = world.posY[row] - (world.size[row].y * 0.5f);
            float tMaxY = world.posY[row] + (world.size[row].y * 0.5f);

            if (sMaxX > tMinX && sMinX < tMaxX && sMaxY > tMinY && sMinY < tMaxY) {
                return row;
            }
        }
    }
    return -1;
}

float GetSlopeSurfaceY(int slopeID, float playerX, const PhysicsWorld& world) {
    float left = world.posX[slopeID] - world.size[slopeID].x * 0.5f;
    float right = world.posX[slopeID] + world.size[slopeID].x * 0.5f;
    float top = world.posY[slopeID] - world.size[slopeID].y * 0.5f;
    float bottom = world.posY[slopeID] + world.size[slopeID].y * 0.5f;
    float height = world.size[slopeID].y;

    float clampedX = std::clamp(playerX, left, right);
    float t = (clampedX - left) / world.size[slopeID].x;

    if (world.slopeType[slopeID] == SlopeType::UP_RIGHT)
        return top + (1.0f - t) * height;
    if (world.slopeType[slopeID] == SlopeType::UP_LEFT)
        return top + t * height;
    return bottom - t * height; // DOWN_LEFT / DOWN_RIGHT
}

// True when the entity's feet are already resting on this slope's own surface (grounded on
// it, or within the same snap-tolerance CheckSlopeSnap/HandleSlopeSnap use) -- as opposed to
// approaching the slope's tile fresh from off to the side. An entity already standing on top
// of a slope must be able to walk/slide either direction along its own surface; the
// direction-of-approach gate next to every use of this only exists to stop tunneling through
// the slope's vertical back face when approaching from ground level at its base, and was
// wrongly also blocking downhill movement while already standing on top of the slope.
bool IsStandingOnSlope(int i, int slopeID, const PhysicsWorld& world) {
    if (slopeID == -1 || world.slopeType[slopeID] == SlopeType::NONE) return false;
    float slopeSurfaceY = GetSlopeSurfaceY(slopeID, world.posX[i], world);
    float feetY = world.posY[i] + world.size[i].y * 0.5f;
    return feetY <= slopeSurfaceY + Tuning::kSlopeSnapReach;
}
} // namespace

int PhysicsSystem::TileAboveSensor(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid, const TileMap* tiles) {
    float hw = world.size[entityIdx].x * 0.5f;
    float hh = world.size[entityIdx].y * 0.5f;

    float sensorX = world.posX[entityIdx];
    float sensorY = world.posY[entityIdx] - hh - Tuning::kSensorProbeOffset;
    float sensorHW = Tuning::kSensorProbeHalfWidth;
    float sensorHH = hh * Tuning::kSensorShrink;

    if (tiles && !tiles->rowHandles.empty())
        return ProbeTileMapRows(world, *tiles, sensorX, sensorY, sensorHW, sensorHH);

    // Callback iteration -- no neighbor list, no per-call heap allocation. Returns
    // false from the lambda on the first hit to stop the scan (bool protocol).
    int hit = -1;
    grid.forEachCandidate(entityIdx, sensorX, sensorY, sensorHW, sensorHH, [&](int otherID) -> bool {
        if (!world.isTile[otherID] || world.layer[otherID] == 0 || world.life[otherID] <= 0.0f)
            return true;

        float tileMinX = world.posX[otherID] - (world.size[otherID].x * 0.5f);
        float tileMaxX = world.posX[otherID] + (world.size[otherID].x * 0.5f);
        float tileMinY = world.posY[otherID] - (world.size[otherID].y * 0.5f);
        float tileMaxY = world.posY[otherID] + (world.size[otherID].y * 0.5f);

        float sMinX = sensorX - sensorHW;
        float sMaxX = sensorX + sensorHW;
        float sMinY = sensorY - sensorHH;
        float sMaxY = sensorY + sensorHH;

        if (sMaxX > tileMinX && sMinX < tileMaxX && sMaxY > tileMinY && sMinY < tileMaxY) {
            hit = otherID;
            return false;
        }
        return true;
    });
    return hit;
}

int PhysicsSystem::LeftTileSensor(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid, const TileMap* tiles) {
    float hw = world.size[entityIdx].x * 0.5f;
    float hh = world.size[entityIdx].y * 0.5f;

    float sensorX = world.posX[entityIdx] - hw - Tuning::kSensorProbeOffset;
    float sensorY = world.posY[entityIdx];
    float sensorHW = Tuning::kSensorProbeHalfWidth;
    float sensorHH = hh * Tuning::kSensorShrink;

    if (tiles && !tiles->rowHandles.empty())
        return ProbeTileMapRows(world, *tiles, sensorX, sensorY, sensorHW, sensorHH);

    // Callback iteration -- no neighbor list, no per-call heap allocation. Returns
    // false from the lambda on the first hit to stop the scan (bool protocol).
    int hit = -1;
    grid.forEachCandidate(entityIdx, sensorX, sensorY, sensorHW, sensorHH, [&](int otherID) -> bool {
        if (!world.isTile[otherID] || world.layer[otherID] == 0 || world.life[otherID] <= 0.0f)
            return true;

        float tileMinX = world.posX[otherID] - (world.size[otherID].x * 0.5f);
        float tileMaxX = world.posX[otherID] + (world.size[otherID].x * 0.5f);
        float tileMinY = world.posY[otherID] - (world.size[otherID].y * 0.5f);
        float tileMaxY = world.posY[otherID] + (world.size[otherID].y * 0.5f);

        float sMinX = sensorX - sensorHW;
        float sMaxX = sensorX + sensorHW;
        float sMinY = sensorY - sensorHH;
        float sMaxY = sensorY + sensorHH;

        if (sMaxX > tileMinX && sMinX < tileMaxX && sMaxY > tileMinY && sMinY < tileMaxY) {
            hit = otherID;
            return false;
        }
        return true;
    });
    return hit;
}

int PhysicsSystem::RightTileSensor(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid, const TileMap* tiles) {
    float hw = world.size[entityIdx].x * 0.5f;
    float hh = world.size[entityIdx].y * 0.5f;

    float sensorX = world.posX[entityIdx] + hw + Tuning::kSensorProbeOffset;
    float sensorY = world.posY[entityIdx];
    float sensorHW = Tuning::kSensorProbeHalfWidth;
    float sensorHH = hh * Tuning::kSensorShrink;

    if (tiles && !tiles->rowHandles.empty())
        return ProbeTileMapRows(world, *tiles, sensorX, sensorY, sensorHW, sensorHH);

    // Callback iteration -- no neighbor list, no per-call heap allocation. Returns
    // false from the lambda on the first hit to stop the scan (bool protocol).
    int hit = -1;
    grid.forEachCandidate(entityIdx, sensorX, sensorY, sensorHW, sensorHH, [&](int otherID) -> bool {
        if (!world.isTile[otherID] || world.layer[otherID] == 0 || world.life[otherID] <= 0.0f)
            return true;

        float tileMinX = world.posX[otherID] - (world.size[otherID].x * 0.5f);
        float tileMaxX = world.posX[otherID] + (world.size[otherID].x * 0.5f);
        float tileMinY = world.posY[otherID] - (world.size[otherID].y * 0.5f);
        float tileMaxY = world.posY[otherID] + (world.size[otherID].y * 0.5f);

        float sMinX = sensorX - sensorHW;
        float sMaxX = sensorX + sensorHW;
        float sMinY = sensorY - sensorHH;
        float sMaxY = sensorY + sensorHH;

        if (sMaxX > tileMinX && sMinX < tileMaxX && sMaxY > tileMinY && sMinY < tileMaxY) {
            hit = otherID;
            return false;
        }
        return true;
    });
    return hit;
}

int PhysicsSystem::TileBelowSensor(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid, const TileMap* tiles) {
    float hw = world.size[entityIdx].x * 0.5f;
    float hh = world.size[entityIdx].y * 0.5f;

    float sensorX = world.posX[entityIdx];
    float sensorY = world.posY[entityIdx] + hh + Tuning::kSensorProbeOffset;
    float sensorHW = Tuning::kSensorProbeHalfWidth;
    float sensorHH = hh * Tuning::kSensorShrink;

    if (tiles && !tiles->rowHandles.empty())
        return ProbeTileMapRows(world, *tiles, sensorX, sensorY, sensorHW, sensorHH);

    // Callback iteration -- no neighbor list, no per-call heap allocation. Returns
    // false from the lambda on the first hit to stop the scan (bool protocol).
    int hit = -1;
    grid.forEachCandidate(entityIdx, sensorX, sensorY, sensorHW, sensorHH, [&](int otherID) -> bool {
        if (!world.isTile[otherID] || world.layer[otherID] == 0 || world.life[otherID] <= 0.0f)
            return true;

        float tileMinX = world.posX[otherID] - (world.size[otherID].x * 0.5f);
        float tileMaxX = world.posX[otherID] + (world.size[otherID].x * 0.5f);
        float tileMinY = world.posY[otherID] - (world.size[otherID].y * 0.5f);
        float tileMaxY = world.posY[otherID] + (world.size[otherID].y * 0.5f);

        float sMinX = sensorX - sensorHW;
        float sMaxX = sensorX + sensorHW;
        float sMinY = sensorY - sensorHH;
        float sMaxY = sensorY + sensorHH;

        if (sMaxX > tileMinX && sMinX < tileMaxX && sMaxY > tileMinY && sMinY < tileMaxY) {
            hit = otherID;
            return false;
        }
        return true;
    });
    return hit;
}

// Same job as TileBelowSensor (find what the entity is standing on) but considers every tile
// touching the entity's footprint, not just whichever one a single center probe happens to land
// on -- TileBelowSensor's narrow probe assumed the entity is small relative to a tile, which
// breaks for slopes (surface Y varies across the tile's width, so a wide/off-center entity
// straddling a slope and its flat neighbor could miss the one actually underneath).
//
// The naive way to do this -- widen the probe/query box -- was tried twice and broke worse both
// times: any box (or even several narrow point-samples) that reaches close to the entity's true
// edge will find a same-row wall or a passing platform whenever the entity is flush against it,
// because a grid-aligned wall's top surface sits at the EXACT same height as the floor beside it
// -- there is no height difference for the epsilon check to reject. Walking into a wall, or being
// grazed by a moving platform, produces a few px of real overlap with that neighbor (the sensor
// system doesn't prevent one frame of penetration before a horizontal collision resolves), and
// any fixed-width probe placed close enough to the edge to catch multi-tile straddling is also
// close enough to catch that graze.
//
// The actual discriminator between "resting on top of this" and "brushing its side" isn't
// distance from the edge, it's how MUCH of the entity's width the tile actually covers: standing
// on a tile means it's under most/all of the entity's footprint; brushing a wall while walking
// into it, or a platform sweeping past, only ever covers a small sliver. Requiring a real
// majority overlap (not just any touch) rejects graze contact by a wide margin (a few px of
// accidental penetration is nowhere near 50% of a 16px-wide entity) while still finding whichever
// tile genuinely underlies most of a slope-straddling entity.
int PhysicsSystem::FindFloorMultiTile(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid) {
    float hw = world.size[entityIdx].x * 0.5f;
    float hh = world.size[entityIdx].y * 0.5f;
    float entityWidth = hw * 2.0f;

    float sensorX = world.posX[entityIdx];
    float sensorY = world.posY[entityIdx] + hh + Tuning::kSensorProbeOffset;
    float sensorHW = hw; // gather every touching candidate -- overlap fraction below rejects grazes
    float sensorHH = hh * Tuning::kSensorShrink;

    float entityMinX = sensorX - hw;
    float entityMaxX = sensorX + hw;
    float sMinY = sensorY - sensorHH;
    float sMaxY = sensorY + sensorHH;

    int bestID = -1;
    float bestSurfaceY = FLT_MAX;

    grid.forEachCandidate(entityIdx, sensorX, sensorY, sensorHW, sensorHH, [&](int otherID) {
        if (!world.isTile[otherID] || world.layer[otherID] == 0 || world.life[otherID] <= 0.0f)
            return;

        float tileMinX = world.posX[otherID] - (world.size[otherID].x * 0.5f);
        float tileMaxX = world.posX[otherID] + (world.size[otherID].x * 0.5f);
        float tileMinY = world.posY[otherID] - (world.size[otherID].y * 0.5f);
        float tileMaxY = world.posY[otherID] + (world.size[otherID].y * 0.5f);

        if (!(sMaxY > tileMinY && sMinY < tileMaxY)) return;

        float overlapX = std::min(entityMaxX, tileMaxX) - std::max(entityMinX, tileMinX);
        if (overlapX <= 0.0f) return;
        // Require covering close to half the entity's width -- comfortably above what a few px
        // of walk-into-a-wall/platform-graze penetration produces, comfortably below the ~50%
        // each side gets when genuinely straddling two equal-width tiles at their shared boundary.
        if (overlapX < entityWidth * 0.4f) return;

        // Slope tiles: surface height at the entity's own X. Flat tiles: their top edge.
        float surfaceY = (world.slopeType[otherID] != SlopeType::NONE)
            ? GetSlopeSurfaceY(otherID, sensorX, world)
            : tileMinY;

        if (surfaceY < bestSurfaceY) {
            bestSurfaceY = surfaceY;
            bestID = otherID;
        }
    });
    return bestID;
}

int PhysicsSystem::PlatformSensor(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid) {
    float sensorHW = world.size[entityIdx].x * 0.3f;
    float sensorHH = 6.0f;
    float sensorX = world.posX[entityIdx];
    float sensorY = world.posY[entityIdx] + (world.size[entityIdx].y * 0.5f) + Tuning::kSensorProbeOffset;

    int bestTile = -1;
    float closestDist = 9999.0f;

    grid.forEachCandidate(entityIdx, sensorX, sensorY, sensorHW, sensorHH, [&](int otherID) {
        if (world.isPlatform[otherID]) {
            float platformHW = world.size[otherID].x * 0.5f;
            float platformMinX = world.posX[otherID] - platformHW;
            float platformMaxX = world.posX[otherID] + platformHW;
            float sensorMinX = sensorX - sensorHW;
            float sensorMaxX = sensorX + sensorHW;

            if (platformMaxX > sensorMinX && platformMinX < sensorMaxX) {
                float dist = std::abs(world.posX[otherID] - sensorX);
                if (dist < closestDist) {
                    closestDist = dist;
                    bestTile = otherID;
                }
            }
        }
    });
    return bestTile;
}

float PhysicsSystem::Lerp(float a, float b, float t) {
    return a + t * (b - a);
}

void PhysicsSystem::UpdateEntityRotation(int index, PhysicsWorld& world, Vec2 floorNormal) {
    // atan2(y, x) gives the angle; adjust by PI/2 because the sprite's "up" is usually at angle 0.
    float targetRotation = atan2(floorNormal.y, floorNormal.x) + (kPi / 2.0f);
    world.rotation[index] = Lerp(world.rotation[index], targetRotation, 0.1f);
}

int PhysicsSystem::GetHorizontalSensor(int i, PhysicsWorld& world, SpatialGrid& grid, float dx) {
    float dir = (dx > 0) ? 1.0f : -1.0f;
    float checkX = world.posX[i] + (world.size[i].x * 0.5f) * dir;
    float stripHW = 0.1f;
    float stripHH = world.size[i].y * 0.4f;

    float sMinX = checkX - stripHW;
    float sMaxX = checkX + stripHW;
    float sMinY = world.posY[i] - stripHH;
    float sMaxY = world.posY[i] + stripHH;

    int hit = -1;
    grid.forEachCandidate(i, checkX, world.posY[i], stripHW, stripHH, [&](int otherID) -> bool {
        if (i == otherID || !world.isTile[otherID]) return true;
        if (world.layer[otherID] == 0 || world.life[otherID] <= 0.0f) return true;
        if (world.isGhosted[otherID]) return true;
        if (world.slopeType[otherID] != SlopeType::NONE) {
            // Only skip (let CheckSlopeSnap/CheckHorizontalCollision's slope-climb path handle
            // it) when approaching from the slope's LOW side in its "uphill" direction -- e.g.
            // moving right into an UP_RIGHT slope. Approaching from the HIGH/back side (moving
            // left into an UP_RIGHT slope) must still register as a normal wall hit here, or the
            // entity walks straight through the vertical edge with no horizontal resistance and
            // gets vacuumed up onto the surface by the grounding sensor instead of being blocked.
            bool climbableFromThisDirection =
                (world.slopeType[otherID] == SlopeType::UP_RIGHT && dir > 0.0f) ||
                (world.slopeType[otherID] == SlopeType::UP_LEFT && dir < 0.0f) ||
                IsStandingOnSlope(i, otherID, world);
            if (climbableFromThisDirection) return true;
        }

        float tMinX = world.posX[otherID] - world.size[otherID].x * 0.5f;
        float tMaxX = world.posX[otherID] + world.size[otherID].x * 0.5f;
        float tMinY = world.posY[otherID] - world.size[otherID].y * 0.5f;
        float tMaxY = world.posY[otherID] + world.size[otherID].y * 0.5f;

        if (sMaxX > tMinX && sMinX < tMaxX && sMaxY > tMinY && sMinY < tMaxY) {
            hit = otherID;
            return false;
        }
        return true;
    });
    return hit;
}

bool PhysicsSystem::IsBlocked(int i, PhysicsContext& ctx, float dir) {
    PhysicsWorld& world = ctx.world;
    int slot = world.entitySlot[i];
    int parent = (slot != -1) ? ctx.entities.parentEntity[slot] : -1;

    float halfWidth = world.size[i].x * 0.5f;
    float halfHeight = world.size[i].y * 0.5f;
    float edgeX = (dir > 0) ? (world.posX[i] + halfWidth) : (world.posX[i] - halfWidth);

    float probeX = edgeX;
    float probeWidth = 1.0f;
    float probeHeight = halfHeight * Tuning::kSensorShrink;

    bool blocked = false;
    ctx.grid.forEachCandidate(i, probeX, world.posY[i], probeWidth, probeHeight, [&](int otherID) -> bool {
        if (i == otherID || otherID == parent ||
            !world.isTile[otherID] || world.isGhosted[otherID] ||
            world.isPlatform[otherID] || world.layer[otherID] == 0 ||
            world.life[otherID] <= 0.0f) return true;

        float otherHW = world.size[otherID].x * 0.5f;
        float otherHH = world.size[otherID].y * 0.5f;

        float probeMinX = probeX - probeWidth * 0.5f;
        float probeMaxX = probeX + probeWidth * 0.5f;
        float probeMinY = world.posY[i] - probeHeight;
        float probeMaxY = world.posY[i] + probeHeight;

        float tileMinX = world.posX[otherID] - otherHW;
        float tileMaxX = world.posX[otherID] + otherHW;
        float tileMinY = world.posY[otherID] - otherHH;
        float tileMaxY = world.posY[otherID] + otherHH;

        if (probeMaxX > tileMinX && probeMinX < tileMaxX &&
            probeMaxY > tileMinY && probeMinY < tileMaxY) {
            float overlapX = (dir > 0) ? (probeMaxX - tileMinX) : (tileMaxX - probeMinX);
            world.posX[i] -= (dir > 0) ? overlapX : -overlapX;
            blocked = true;
            return false;
        }
        return true;
    });
    return blocked;
}

int PhysicsSystem::GetFloorCandidate(int entityIdx, float testX, float testY, const PhysicsWorld& world, SpatialGrid& grid) {
    return TileBelowSensor(entityIdx, world, grid);
}

void PhysicsSystem::WallSlideCheck(int i, PhysicsContext& ctx) {
    PhysicsWorld& world = ctx.world;
    int slot = world.entitySlot[i];
    float intent = (slot != -1) ? ctx.entities.moveIntent[slot] : 0.0f;

    int leftTile = LeftTileSensor(i, world, ctx.grid, ctx.tiles);
    int rightTile = RightTileSensor(i, world, ctx.grid, ctx.tiles);
    bool onWall = (leftTile >= 0 && world.velY[i] > 0.0f && intent < 0.0f && world.slopeType[leftTile] == SlopeType::NONE)
        || (rightTile >= 0 && world.velY[i] > 0.0f && intent > 0.0f && world.slopeType[rightTile] == SlopeType::NONE);

    if (onWall) {
        float maxSlideSpeed = world.gravity[i] / 15.0f;
        if (world.velY[i] > maxSlideSpeed) world.velY[i] = maxSlideSpeed;
        if (slot != -1) ctx.entities.isSliding[slot] = true;
    } else if (slot != -1) {
        ctx.entities.isSliding[slot] = false;
    }
}

void PhysicsSystem::PlatformPhysics(int i, PhysicsContext& ctx, float dt) {
    PhysicsWorld& world = ctx.world;
    int slot = world.entitySlot[i];
    if (slot == -1) return; // not an actor, nothing to ride with

    if (world.velY[i] < 0.0f) {
        int p = ctx.entities.parentEntity[slot];
        if (p != -1 && world.platformSlot[p] != -1) {
            ctx.platforms.childEntity[world.platformSlot[p]] = -1;
        }
        ctx.entities.parentEntity[slot] = -1;
    }

    if (ctx.entities.parentEntity[slot] != -1) {
        int p = ctx.entities.parentEntity[slot];
        if (PlatformSensor(i, world, ctx.grid) == -1) {
            ctx.entities.parentEntity[slot] = -1;
            if (world.platformSlot[p] != -1) ctx.platforms.childEntity[world.platformSlot[p]] = -1;
        } else {
            // Carries BOTH the rider's own intended movement AND the platform's own horizontal
            // motion in one shot -- this is now the ONLY horizontal carry mechanism (the caller's
            // per-frame Update no longer does a second position-delta carry on top of this; that
            // was true double-counting for a velocity-driven master. This term alone was
            // insufficient on its own for real-time horizontal riding when tried in isolation, so
            // it's back, with the external carry removed instead of both mechanisms coexisting.
            float moveX = world.velX[i] * dt + world.velX[p] * dt;
            if (!IsBlocked(i, ctx, (moveX > 0) ? 1.0f : -1.0f)) {
                world.posX[i] += moveX;
            }

            if (ctx.entities.parentEntity[slot] != -1) {
                int p2 = ctx.entities.parentEntity[slot];
                int pSlot = world.platformSlot[p2];
                bool platformFalling = (pSlot != -1) && (ctx.platforms.platformState[pSlot] == 2);

                if (!ctx.entities.isJumping[slot] && !platformFalling) {
                    world.posY[i] = world.posY[p2] - (world.size[p2].y * 0.5f) - (world.size[i].y * 0.5f);
                    world.velY[i] = 0.0f;
                } else {
                    if (pSlot != -1) ctx.platforms.childEntity[pSlot] = -1;
                    ctx.entities.parentEntity[slot] = -1;
                }
            }
        }
    }
}

Vec2 PhysicsSystem::GetSlopeNormal(SlopeType type) {
    switch (type) {
    case SlopeType::NONE:       return { 0.0f, -1.0f };
    case SlopeType::UP_RIGHT:   return { -0.707f, -0.707f };
    case SlopeType::UP_LEFT:    return { 0.707f, -0.707f };
    case SlopeType::DOWN_LEFT:  return { 0.707f, 0.707f };
    case SlopeType::DOWN_RIGHT: return { -0.707f, 0.707f };
    default:                    return { 0.0f, -1.0f };
    }
}

void PhysicsSystem::HandleSlopeSnap(int i, PhysicsContext& ctx, float dx) {
    PhysicsWorld& world = ctx.world;
    int slot = world.entitySlot[i];
    bool jumping = (slot != -1) && ctx.entities.isJumping[slot];

    int potentialSlope = (world.velX[i] > 0.0f) ? RightTileSensor(i, world, ctx.grid, ctx.tiles) : LeftTileSensor(i, world, ctx.grid, ctx.tiles);
    // Only snap up when approaching the slope's LOW side in its uphill direction -- same
    // direction check as GetHorizontalSensor. Without this, approaching from the tall/back
    // side (e.g. moving left into an UP_RIGHT slope) still snaps you onto the surface even
    // though the horizontal sensor now correctly walls you.
    bool climbableFromThisDirection = potentialSlope != -1 &&
        ((world.velX[i] > 0.0f && world.slopeType[potentialSlope] == SlopeType::UP_RIGHT) ||
         (world.velX[i] < 0.0f && world.slopeType[potentialSlope] == SlopeType::UP_LEFT) ||
         IsStandingOnSlope(i, potentialSlope, world));
    if (climbableFromThisDirection) {
        if (!jumping && world.velY[i] >= 0.0f) {
            float slopeSurfaceY = GetSlopeSurfaceY(potentialSlope, world.posX[i] + dx, world);
            float feetY = world.posY[i] + (world.size[i].y * 0.5f);

            if (slopeSurfaceY < feetY + Tuning::kSlopeSnapReach) {
                world.posY[i] = slopeSurfaceY - (world.size[i].y * 0.5f);
            }
        }
    }
}

CollisionConstraint PhysicsSystem::CheckSlopeSnap(int i, PhysicsContext& ctx, float dx) {
    CollisionConstraint c;
    PhysicsWorld& world = ctx.world;
    int slot = world.entitySlot[i];
    bool jumping = (slot != -1) && ctx.entities.isJumping[slot];

    int potentialSlope = (world.velX[i] > 0.0f) ? RightTileSensor(i, world, ctx.grid, ctx.tiles) : LeftTileSensor(i, world, ctx.grid, ctx.tiles);

    // Same uphill-direction check as HandleSlopeSnap/GetHorizontalSensor -- otherwise this
    // still snaps the entity onto the slope's surface even when approaching from its
    // tall/back side, undoing the wall block GetHorizontalSensor now correctly applies there.
    bool climbableFromThisDirection = potentialSlope != -1 &&
        ((world.velX[i] > 0.0f && world.slopeType[potentialSlope] == SlopeType::UP_RIGHT) ||
         (world.velX[i] < 0.0f && world.slopeType[potentialSlope] == SlopeType::UP_LEFT) ||
         IsStandingOnSlope(i, potentialSlope, world));
    if (climbableFromThisDirection) {
        if (!jumping && world.velY[i] >= 0.0f) {
            float slopeSurfaceY = GetSlopeSurfaceY(potentialSlope, world.posX[i] + dx, world);
            float feetY = world.posY[i] + (world.size[i].y * 0.5f);

            if (slopeSurfaceY < feetY + Tuning::kSlopeSnapReach) {
                c.resolve = true;
                c.deltaY = (slopeSurfaceY - (world.size[i].y * 0.5f)) - world.posY[i];
            }
        }
    }
    return c;
}

CollisionConstraint PhysicsSystem::CheckHorizontalCollision(int i, PhysicsContext& ctx, float dx) {
    CollisionConstraint c;
    PhysicsWorld& world = ctx.world;
    int hitID = GetHorizontalSensor(i, world, ctx.grid, dx);

    if (hitID == -1) return c;

    // FindFloorMultiTile, not TileBelowSensor -- right at a slope-to-flat-ground seam,
    // TileBelowSensor's narrow center probe can already be sitting over the flat tile even
    // though most of the entity's footprint (and its actual support) is still the slope, so
    // adjacentToSlope below came out false exactly at the boundary and the flat tile's edge got
    // treated as a wall -- a visible snag climbing over the top of a slope onto flat ground.
    int floorID = FindFloorMultiTile(i, world, ctx.grid);
    // Direction-gated, same as GetHorizontalSensor/CheckSlopeSnap -- otherwise this branch
    // still lets the entity climb through a slope beneath its feet regardless of which way
    // it's moving, overriding the wall block GetHorizontalSensor already correctly applied
    // to `hitID` when approaching from the slope's tall/back side.
    bool adjacentToSlope = floorID != -1 &&
        ((dx > 0.0f && world.slopeType[floorID] == SlopeType::UP_RIGHT) ||
         (dx < 0.0f && world.slopeType[floorID] == SlopeType::UP_LEFT) ||
         IsStandingOnSlope(i, floorID, world));

    if (world.isTile[hitID] && !adjacentToSlope) {
        bool movingInto = (dx > 0 && world.posX[i] < world.posX[hitID]) ||
            (dx < 0 && world.posX[i] > world.posX[hitID]);

        if (movingInto) {
            c.resolve = true;
            c.killVelX = true;
        }
    } else if (adjacentToSlope) {
        float floorY = GetSlopeSurfaceY(floorID, world.posX[i] + dx, world);
        float feetY = world.posY[i] + world.size[i].y * 0.5f;

        if (feetY >= floorY) {
            c.resolve = true;
            c.deltaY = (floorY - world.size[i].y * 0.5f) - world.posY[i];
            c.deltaX = dx;
        } else {
            c.resolve = true;
            c.killVelX = true;
        }
        return c;
    } else if (world.isEntity[hitID]) {
        // Fire the hit entity's callback, matching ResolveParticle's convention exactly
        // (see its non-tile branch): owner-first args, and the two floats are position
        // deltas from the OWNER's perspective (ownerPos - otherPos). The old form here
        // predated that convention: it guarded on `i` but indexed `hitID`, passed the
        // owner as the SECOND arg (a callback reading particleSlot[self] off its first
        // arg -- like the smoke test's -- would read the wrong cell), and passed a
        // movement direction + penetration depth where every other site passes deltas.
        if (hitID < (int)world.maxEntities && world.hasCollisionEvent[hitID]) {
            world.onCollision[hitID](hitID, i, world,
                world.posX[hitID] - world.posX[i],
                world.posY[hitID] - world.posY[i]);
        }
    }
    return c;
}

CollisionConstraint PhysicsSystem::CheckGrounding(int i, PhysicsContext& ctx) {
    CollisionConstraint c;
    PhysicsWorld& world = ctx.world;
    int slot = world.entitySlot[i];

    int floorID = FindFloorMultiTile(i, world, ctx.grid);
    float feetY = world.posY[i] + (world.size[i].y * 0.5f);
    float epsilon = Tuning::kGroundingEpsilon;

    if (floorID != -1) {
        bool isSlope = world.slopeType[floorID] != SlopeType::NONE;
        float floorY = isSlope
            ? GetSlopeSurfaceY(floorID, world.posX[i], world)
            : (world.posY[floorID] - world.size[floorID].y * 0.5f);

        // Slopes re-snap unconditionally while already grounded on one -- the flat-ground
        // epsilon below assumes slow relative movement vs. tile width, which doesn't hold for a
        // slope's surface height changing every substep as the entity crosses it.
        bool alreadyOnThisSlope = isSlope && slot != -1 && ctx.entities.isGrounded[slot];
        bool withinEpsilon = feetY >= (floorY - epsilon);

        // Reject an implausibly large snap-UP. `withinEpsilon` alone has no upper bound on how
        // far above feetY the candidate's surface can be, because that's needed for legitimate
        // fast-fall tunneling (feet overshoot deep past a static floor's top in one substep --
        // still want to snap back). But a tile TALLER than a few px descending onto the entity
        // from above (a platform the same height as the player, moving down) satisfies the exact
        // same inequality: by the time its bottom edge reaches this feet-level sensor band, its
        // top edge (what we compute floorY from) is already up near the entity's own head, and
        // the "correction" would be nearly a full entity-height snap upward -- which is what
        // looked like "touch the platform's underside with your head, teleport onto its top."
        // Legitimate landings only ever need a small correction (a few px per substep); capping
        // the snap distance rejects the platform-swallowing case without touching normal landing.
        float maxSnapUp = world.size[i].y * 0.5f;
        bool plausibleSnap = (feetY - floorY) <= maxSnapUp;

        if ((alreadyOnThisSlope || (withinEpsilon && plausibleSnap)) && world.velY[i] >= 0.0f) {
            c.resolve = true;
            c.deltaY = (floorY - (world.size[i].y * 0.5f)) - world.posY[i];
            c.killVelY = true;
            world.velY[i] = 0.0f;

            if (slot != -1) {
                ctx.entities.isGrounded[slot] = true;
                ctx.entities.isJumping[slot] = false;
                ctx.entities.jumpsRemaining[slot] = 1;
            }

            if (world.isPlatform[floorID]) {
                if (slot != -1) ctx.entities.parentEntity[slot] = floorID;
                int pSlot = world.platformSlot[floorID];
                if (pSlot != -1) ctx.platforms.childEntity[pSlot] = i;
            }
        }
        return c;
    } else {
        if (slot != -1) {
            ctx.entities.isGrounded[slot] = false;
            if (ctx.entities.parentEntity[slot] != -1) {
                int oldParent = ctx.entities.parentEntity[slot];
                ctx.entities.parentEntity[slot] = -1;
                if (oldParent >= 0 && oldParent < (int)world.activeCount) {
                    int pSlot = world.platformSlot[oldParent];
                    if (pSlot != -1) ctx.platforms.childEntity[pSlot] = -1;
                }
            }
        }
    }
    return c;
}

CollisionConstraint PhysicsSystem::CheckCeilingCollision(int i, PhysicsContext& ctx) {
    PhysicsWorld& world = ctx.world;
    int ceilingID = TileAboveSensor(i, world, ctx.grid, ctx.tiles);
    CollisionConstraint result;
    // A SLOPE is never a flat ceiling: its AABB's low half is empty air (the surface is the
    // diagonal), but this constraint resolves against the full AABB's bottom edge. Rising past
    // a slope's toe put the head inside that empty triangle, and the push-below-the-AABB
    // "correction" was a full body-height shove DOWNWARD -- at a toe adjacent to same-row
    // ground, that planted the entity inside the ground row (jump -> instantly buried). The
    // slope's solid back side is already walled off by the direction-gated horizontal checks.
    if (ceilingID != -1 && world.slopeType[ceilingID] != SlopeType::NONE) ceilingID = -1;
    if (ceilingID != -1 && world.velY[i] < 0.0f) {
        float ceilingY = world.posY[ceilingID] + (world.size[ceilingID].y * 0.5f);
        result.deltaY = (ceilingY + (world.size[i].y * 0.5f)) - world.posY[i];
        result.killVelY = true;
        result.resolve = true; // the apply site gates on this -- without it the hit is computed then dropped
    }
    return result;
}

void PhysicsSystem::ApplyPhysics(int i, PhysicsContext& ctx, float dt) {
    PhysicsWorld& world = ctx.world;
    float dx = world.velX[i] * dt;

    CollisionConstraint horizontal = CheckHorizontalCollision(i, ctx, dx);
    CollisionConstraint slope = CheckSlopeSnap(i, ctx, dx);
    CollisionConstraint ground = CheckGrounding(i, ctx);
    CollisionConstraint ceiling = CheckCeilingCollision(i, ctx);

    int slot = world.entitySlot[i];
    int p = (slot != -1) ? ctx.entities.parentEntity[slot] : -1;
    bool onPlatform = (p != -1) && world.isPlatform[p];

    WallSlideCheck(i, ctx);
    if (onPlatform) {
        PlatformPhysics(i, ctx, dt);
    }

    if (horizontal.resolve) {
        int hitID = GetHorizontalSensor(i, world, ctx.grid, dx);
        bool jumping = (slot != -1) && ctx.entities.isJumping[slot];

        // Fourth spot that needed the same uphill-direction gate: this ran the "deflect along
        // the slope's normal" treatment for ANY slope hitID regardless of approach direction,
        // which takes priority over (and never falls through to) horizontal.killVelX below --
        // so even with GetHorizontalSensor/CheckHorizontalCollision correctly reporting a wall
        // hit for a wrong-direction approach, this still let the entity slide along the slope's
        // surface instead of actually stopping. Only take the slope-deflection path when the
        // hit is genuinely climbable from this direction; otherwise fall through to killVelX.
        bool climbableFromThisDirection = hitID != -1 && world.slopeType[hitID] != SlopeType::NONE &&
            ((dx > 0.0f && world.slopeType[hitID] == SlopeType::UP_RIGHT) ||
             (dx < 0.0f && world.slopeType[hitID] == SlopeType::UP_LEFT) ||
             IsStandingOnSlope(i, hitID, world));

        if (climbableFromThisDirection && !jumping) {
            Vec2 normal = GetSlopeNormal(world.slopeType[hitID]);
            float dot = (world.velX[i] * normal.x) + (world.velY[i] * normal.y);

            if (dot < 0) {
                world.velX[i] -= dot * normal.x;
                world.velY[i] -= dot * normal.y;
            }
        } else if (horizontal.killVelX) {
            world.velX[i] = 0.0f;
        }
    }

    // PlatformPhysics already advanced posX by velX*dt itself (plus its own IsBlocked check)
    // when riding a platform -- applying the generic velX*dt integration again here as well
    // was double-moving the rider horizontally every frame, exactly why platform movement felt
    // much faster than walking on the ground.
    if (!onPlatform) {
        world.posX[i] += world.velX[i] * dt;
    }

    world.velY[i] += world.gravity[i] * dt;
    world.posY[i] += world.velY[i] * dt;

    if (slope.resolve) {
        world.posY[i] += slope.deltaY;
    }

    if (ground.resolve) {
        world.posY[i] += ground.deltaY;
        world.velY[i] = 0.0f;
        if (slot != -1) {
            ctx.entities.isGrounded[slot] = true;
            ctx.entities.isJumping[slot] = false;
        }

        // Ground friction -- decelerates horizontal velocity whenever nothing is actively
        // driving it this frame (moveIntent == 0): a player releasing A/D, an idle AI entity,
        // a resting particle (entitySlot == -1, so this is always "not driven"). GameplayScene
        // now accelerates the player's velX toward a target instead of hard-setting it, and
        // leaves velX alone entirely while moveIntent == 0 -- exactly the gap this fills.
        // world.friction[floorID] is a small per-tile coefficient (MapReader: .15 for solid
        // ground, .01 for a slippery moving platform), scaled up here into an actual px/s^2
        // deceleration -- using it directly as the decel rate would be imperceptible.
        bool activelyDriven = slot != -1 && ctx.entities.moveIntent[slot] != 0.0f;
        if (!activelyDriven) {
            int floorID = TileBelowSensor(i, world, ctx.grid, ctx.tiles);
            // TileBelowSensor's TileMap fast path only sees STATIC cells (rowHandles) -- a
            // moving platform is a world row, never a TileMap cell, so standing on one returns
            // -1 here and friction silently never applied (entities slid frictionlessly on
            // every platform regardless of its authored friction). The platform we're standing
            // on is already known via the parent link, so use its row's friction directly.
            if (floorID == -1 && onPlatform) floorID = p;
            if (floorID != -1) {
                constexpr float kFrictionDecelScale = 2000.0f;
                float decel = world.friction[floorID] * kFrictionDecelScale * dt;
                if (std::fabs(world.velX[i]) <= decel) world.velX[i] = 0.0f;
                else world.velX[i] -= decel * (world.velX[i] > 0.0f ? 1.0f : -1.0f);
            }
        }
    }

    if (ceiling.resolve) {
        world.posY[i] += ceiling.deltaY;
        world.velY[i] = 0.0f;
    }

    if (slot != -1 && ctx.entities.isGrounded[slot]) {
        int floorID = TileBelowSensor(i, world, ctx.grid);
        if (floorID != -1 && world.slopeType[floorID] != SlopeType::NONE) {
            Vec2 normal = GetSlopeNormal(world.slopeType[floorID]);
            UpdateEntityRotation(i, world, normal);
        } else {
            UpdateEntityRotation(i, world, Vec2{ 0.0f, -1.0f });
        }
    }
}

void PhysicsSystem::ResolveParticle(int i, PhysicsContext& ctx) {
    PhysicsWorld& world = ctx.world;
    int slot = world.entitySlot[i];

    // The full scalar narrow phase + response for ONE candidate -- every filter intact.
    // The SIMD prefilter below re-verifies nothing; it only decides which candidates
    // are WORTH calling this on. Because this re-checks everything (self, layer/mask,
    // ghost, overlap), the prefilter may safely over-approve (e.g. a lane ghosted by an
    // earlier hit's callback in the same 8-block) and can never wrongly drop a hit.
    auto resolveOne = [&](int otherID) {
        if (i == otherID) return;
        if ((world.layer[otherID] & world.mask[i]) == 0) return;
        if (world.layer[otherID] == 0) return;
        if (world.isGhosted[otherID]) return;

        float dx = world.posX[i] - world.posX[otherID];
        float dy = world.posY[i] - world.posY[otherID];
        float combinedHW = (world.size[i].x + world.size[otherID].x) * 0.5f;
        float combinedHH = (world.size[i].y + world.size[otherID].y) * 0.5f;

        float overlapX = combinedHW - std::abs(dx);
        float overlapY = combinedHH - std::abs(dy);

        if (overlapX <= 0 || overlapY <= 0) return;

        if (!world.isTile[otherID]) {
            // Non-tile hit (e.g. a bullet vs. an entity) -- no physical bounce/landing here,
            // PhysicsSystem doesn't know or care what a "hit" means gameplay-wise. Just fire
            // whichever side(s) registered a callback and let THEM sort it out (damage,
            // despawn, pickup, etc.) -- the original layer/mask + onCollision pattern.
            if (i < (int)world.maxEntities && world.hasCollisionEvent[i]) {
                world.onCollision[i](i, otherID, world, dx, dy);
            }
            if (otherID < (int)world.maxEntities && world.hasCollisionEvent[otherID]) {
                world.onCollision[otherID](otherID, i, world, -dx, -dy);
            }
            return;
        }

        if (world.slopeType[otherID] != SlopeType::NONE) {
            // Slope-aware resolution -- the generic flat-rect branch below has no notion of a
            // diagonal surface, so any entity relying on it would just wall against the slope
            // tile's bounding box. This reuses the EXACT same direction gate proven correct
            // elsewhere in this file (GetHorizontalSensor/CheckHorizontalCollision/ApplyPhysics) --
            // NOT a feetY-vs-surface comparison (an earlier version of this branch tried that and
            // got the sense backwards, letting almost every wrong-direction approach snap up onto
            // the ramp instead of walling). "Climbable from this direction" means: moving uphill
            // into the slope, OR already standing on its own surface (IsStandingOnSlope, so
            // downhill sliding while already on top is never blocked -- see PhysicsSystem.cpp's
            // history for why that second condition exists). Anything else is a wall.
            // isEntity[i] gate -- climbing a ramp is something only an actual actor does (walking/
            // sliding up it). A generic particle (a bullet, a spark, anything from an Emitter) has
            // no legs: without this, a bullet moving in the slope's uphill direction got treated as
            // "climbing" too, snapping it up onto the ramp surface instead of just being blocked by
            // it like any other wall -- particles always take the wall branch below now, regardless
            // of direction.
            bool climbableFromThisDirection = world.isEntity[i] && (
                (world.slopeType[otherID] == SlopeType::UP_RIGHT && world.velX[i] > 0.0f) ||
                (world.slopeType[otherID] == SlopeType::UP_LEFT  && world.velX[i] < 0.0f) ||
                IsStandingOnSlope(i, otherID, world));

            if (!climbableFromThisDirection) {
                world.posX[i] += (dx > 0) ? overlapX : -overlapX;
                world.velX[i] = 0.0f;
                // Same onCollision firing the flat-wall branch below already does -- without this,
                // nothing ever learned a particle (e.g. a bullet with its own self-destruct
                // callback) hit a slope tile's wall side; it would just silently bounce/stop here
                // forever instead of being able to react to the hit at all.
                if (i < (int)world.maxEntities && world.hasCollisionEvent[i]) {
                    world.onCollision[i](i, otherID, world, dx, dy);
                }
                return;
            }

            float slopeSurfaceY = GetSlopeSurfaceY(otherID, world.posX[i], world);
            float feetY = world.posY[i] + world.size[i].y * 0.5f;
            if (feetY >= slopeSurfaceY - Tuning::kSlopeLandingEpsilon && world.velY[i] >= 0.0f) {
                world.posY[i] = slopeSurfaceY - (world.size[i].y * 0.5f);
                world.velY[i] = 0.0f;
                if (slot != -1) ctx.entities.isGrounded[slot] = true;
            }
            return;
        }

        if (overlapX < overlapY) {
            // At a slope-to-flat seam: if this entity is grounded on an adjacent slope,
            // and this flat tile is the slope's horizontal neighbor at the same surface
            // height, skip the collision -- the slope already owns the landing, and this
            // would create a double-collision wall at the internal edge.
            if (slot != -1 && ctx.entities.isGrounded[slot]) {
                int currentFloor = FindFloorMultiTile(i, world, ctx.grid);
                if (currentFloor != -1 && world.slopeType[currentFloor] != SlopeType::NONE) {
                    float slopeY = world.posY[currentFloor] - world.size[currentFloor].y * 0.5f;
                    float flatY = world.posY[otherID] - world.size[otherID].y * 0.5f;
                    if (std::abs(slopeY - flatY) < 1.0f) { // same surface height (within 1px)
                        return; // skip this internal edge
                    }
                }
            }

            world.posX[i] += (dx > 0) ? overlapX : -overlapX;

            if ((dx < 0 && world.velX[i] > 0.0f) || (dx > 0 && world.velX[i] < 0.0f)) {
                world.velX[i] *= -world.restitution[otherID];
                if (i < (int)world.maxEntities && world.hasCollisionEvent[i]) {
                    world.onCollision[i](i, otherID, world, dx, dy);
                }
            }
        } else {
            // ONE-WAY LOGIC: only resolve if we are above the object and moving down.
            bool onTop = (world.posY[i] < world.posY[otherID]);
            int otherPlatformSlot = world.platformSlot[otherID];

            // Ledge/gap guard (entities only): an actor exactly as wide as a tile -- or wider --
            // corner-clips the TOP of the tiles flanking a single-tile hole, so this one-way
            // landing snaps it up onto each in turn every substep and it BRIDGES the gap, never
            // falling. A 2+ tile hole or a platform edge leaves a real fall window, which is why
            // those already work. Require the tile to genuinely be UNDERFOOT -- overlapping >=40%
            // of the entity's width -- the exact rule FindFloorMultiTile uses for the player's
            // foot sensor (why the player falls into 1-tile holes and enemies didn't). overlapX
            // here is the true AABB horizontal overlap (combinedHW - |dx|), same quantity that
            // sensor measures. A grazing corner is no longer a floor. Particles keep the old
            // any-overlap landing -- this bridging only matters for walking actors.
            bool grounding = onTop && world.velY[i] >= 0.0f;
            if (grounding && world.isEntity[i] && overlapX < world.size[i].x * 0.4f) {
                return; // tile is beside the entity, not under it -- let gravity carry it into the hole
            }

            if (grounding) {
                world.posY[i] = (world.posY[otherID] - (world.size[otherID].y * 0.5f)) - (world.size[i].y * 0.5f);
                world.velY[i] = 0.0f;
                if (slot != -1) {
                    ctx.entities.isGrounded[slot] = true;
                    ctx.entities.parentEntity[slot] = otherID;
                }
                // Ported behavior change (see plan): the source project wrote
                // childEntity on ANY tile landed on, but every reader
                // (UpdateGroups/LinkGroups/UpdatePlatforms/PlatformPhysics) only
                // ever consulted it in an isPlatform-gated context -- so only
                // platforms need to track their riding child now.
                if (otherPlatformSlot != -1) {
                    ctx.platforms.childEntity[otherPlatformSlot] = i;
                }
            } else if (!onTop && world.velY[i] < 0.0f) {
                world.posY[i] += overlapY;
                world.velY[i] *= -world.restitution[otherID];
                if (slot != -1) ctx.entities.parentEntity[slot] = -1;
                if (otherPlatformSlot != -1) ctx.platforms.childEntity[otherPlatformSlot] = -1;
            }
        }
    };

    // 8-wide narrow-phase prefilter over the broadphase's contiguous runs. Per block of
    // 8 candidates: layer/mask + ghost eligibility and the AABB overlap test, all in
    // registers; only lanes that pass reach resolveOne (usually none -- one movemask
    // branch and on to the next 8). The layer!=0 check is implied by (layer&mask)!=0.
    // Run tails (<8) go scalar, so no sentinel padding is needed.
    const __m256  vMyX  = _mm256_set1_ps(world.posX[i]);
    const __m256  vMyY  = _mm256_set1_ps(world.posY[i]);
    const __m256  vMyHW = _mm256_set1_ps(world.size[i].x * 0.5f);
    const __m256  vMyHH = _mm256_set1_ps(world.size[i].y * 0.5f);
    const __m256  vHalf = _mm256_set1_ps(0.5f);
    const __m256  vSign = _mm256_set1_ps(-0.0f);
    const __m256  vZero = _mm256_setzero_ps();
    const __m256i vMask = _mm256_set1_epi32((int)world.mask[i]);
    const __m256i vZeroI = _mm256_setzero_si256();

    ctx.grid.forEachRun(world.posX[i], world.posY[i], world.size[i].x, world.size[i].y,
        [&](int begin, int end) {
        int j = begin;
        for (; j + 8 <= end; j += 8) {
            // eligibility: (layer & myMask) != 0 && !ghosted  (byte columns -> i32 lanes)
            __m256i layer = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)(world.layer.data() + j)));
            __m256i ghost = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)(world.isGhosted.data() + j)));
            __m256i elig = _mm256_andnot_si256(
                _mm256_cmpeq_epi32(_mm256_and_si256(layer, vMask), vZeroI),
                _mm256_cmpeq_epi32(ghost, vZeroI));

            // overlap test: (myHW + theirHW) - |myX - theirX| > 0, both axes.
            // size is a Column<Vec2> (x,y interleaved) -- deinterleave 8 pairs into
            // per-axis registers with the standard permute2f128 + shuffle_ps pattern.
            __m256 ox = _mm256_loadu_ps(world.posX.data() + j);
            __m256 oy = _mm256_loadu_ps(world.posY.data() + j);
            __m256 s0 = _mm256_loadu_ps((const float*)(world.size.data() + j));     // x0 y0 .. x3 y3
            __m256 s1 = _mm256_loadu_ps((const float*)(world.size.data() + j + 4)); // x4 y4 .. x7 y7
            __m256 t0 = _mm256_permute2f128_ps(s0, s1, 0x20);
            __m256 t1 = _mm256_permute2f128_ps(s0, s1, 0x31);
            __m256 oW = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(2, 0, 2, 0));
            __m256 oH = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(3, 1, 3, 1));

            __m256 dx = _mm256_sub_ps(vMyX, ox);
            __m256 dy = _mm256_sub_ps(vMyY, oy);
            __m256 ovX = _mm256_sub_ps(_mm256_add_ps(vMyHW, _mm256_mul_ps(oW, vHalf)),
                                       _mm256_andnot_ps(vSign, dx));
            __m256 ovY = _mm256_sub_ps(_mm256_add_ps(vMyHH, _mm256_mul_ps(oH, vHalf)),
                                       _mm256_andnot_ps(vSign, dy));

            __m256 hit = _mm256_and_ps(
                _mm256_and_ps(_mm256_cmp_ps(ovX, vZero, _CMP_GT_OQ),
                              _mm256_cmp_ps(ovY, vZero, _CMP_GT_OQ)),
                _mm256_castsi256_ps(elig));

            int bits = _mm256_movemask_ps(hit);
            while (bits) {
                unsigned lane = _tzcnt_u32((unsigned)bits);
                bits &= bits - 1;
                resolveOne(j + (int)lane);
            }
        }
        for (; j < end; ++j) resolveOne(j); // scalar run tail
    });
}

void PhysicsSystem::SyncPrevPositions(PhysicsWorld& world) {
    for (size_t i = 0; i < world.activeCount; ++i) {
        world.prevPos[i] = Vec2{ world.posX[i], world.posY[i] };
    }
}

// Split into two passes so the pure column math can go 8-wide:
//   Pass A (AVX2): gravity/velocity/position integration + rotation/spin-drag, masked per
//   lane on (alive && !ghosted && !(playerControlled && !gridMovement)), with platforms
//   additionally excluded from position integration (UpdatePlatforms owns their movement --
//   see the double-move bug note below). Inactive lanes are written back with their
//   ORIGINAL values via blendv, so a full 8-lane store never corrupts a dead/skipped row;
//   this is safe against other chunks because a block never straddles [start, end).
//   Pass B (scalar): everything branchy -- life countdown, grid-movement target-seek,
//   age, and ResolveParticle -- in the original per-row order.
// Two deliberate ordering changes vs. the old fused loop, both flagged and accepted:
//   1. EVERY row integrates before ANY row resolves (Jacobi sweep). The old loop resolved
//      row i against neighbors j<i already-integrated and j>i not-yet -- and parallel
//      chunks always had that inconsistency ACROSS chunks anyway. Now in-chunk and
//      cross-chunk semantics match.
//   2. Rotation/spin-drag now update before resolve instead of after -- nothing in
//      resolve reads rotation, so this is unobservable.
// isGhosted meaning (from the old loop, still applies): ghosted must be fully inert on
// BOTH sides of a collision check -- not just invisible to other rows' queries, but not
// integrating or resolving as itself either, or its onCollision fires every frame forever.
// Thin composition -- the two stages below, in order. Existing callers unchanged.
void PhysicsSystem::UpdateChunk(int start, int end, float dt, PhysicsContext& ctx, bool gridMovement) {
    IntegrateChunk(start, end, dt, ctx, gridMovement);
    ResolveChunk(start, end, dt, ctx, gridMovement);
    // Future: specialized behavior passes (sine-wave/boids/homing) run BEFORE IntegrateChunk,
    // writing velX/velY for their tagged rows; the two shared stages then move+resolve them.
}

void PhysicsSystem::IntegrateChunk(int start, int end, float dt, PhysicsContext& ctx, bool gridMovement) {
    PhysicsWorld& world = ctx.world;
    if (end > (int)world.maxEntities) end = (int)world.maxEntities;

    // ---- masked SIMD integration (gravity/velocity/position + rotation/spin-drag) ----
    const __m256  vdt    = _mm256_set1_ps(dt);
    const __m256  vdrag  = _mm256_set1_ps(0.99f); // "air resistance" so spin doesn't last forever
    const __m256  vzero  = _mm256_setzero_ps();
    const __m256i vzeroi = _mm256_setzero_si256();
    const __m256i vminus1 = _mm256_set1_epi32(-1);

    int i = start;
    for (; i + 8 <= end; i += 8) {
        // active = life > 0 && !isGhosted
        __m256 life = _mm256_loadu_ps(world.life.data() + i);
        __m256 active = _mm256_cmp_ps(life, vzero, _CMP_GT_OQ);
        __m256i ghost = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)(world.isGhosted.data() + i)));
        active = _mm256_and_ps(active, _mm256_castsi256_ps(_mm256_cmpeq_epi32(ghost, vzeroi)));

        // Player-controlled exclusion. The fast path stays pure SIMD: only blocks that
        // actually CONTAIN entity rows (entitySlot != -1, checked 8-wide) take the scalar
        // lane patch, and those blocks are rare -- almost every block is tiles/particles.
        if (!gridMovement) {
            __m256i slots = _mm256_loadu_si256((const __m256i*)(world.entitySlot.data() + i));
            int entLanes = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(slots, vminus1)));
            if (entLanes) {
                alignas(32) float lanes[8];
                _mm256_store_ps(lanes, active);
                unsigned m = (unsigned)entLanes;
                while (m) {
                    unsigned lane = _tzcnt_u32(m); m &= m - 1;
                    int slot = world.entitySlot[i + (int)lane];
                    if (ctx.entities.isPlayerControlled[slot]) lanes[lane] = 0.0f;
                }
                active = _mm256_load_ps(lanes);
            }
        }

        // Rotation + spin drag -- every active lane, PLATFORMS INCLUDED (matches the old
        // loop, where the rotation lines sat outside the !isPlatform guard).
        __m256 rot = _mm256_loadu_ps(world.rotation.data() + i);
        __m256 av  = _mm256_loadu_ps(world.angularVel.data() + i);
        _mm256_storeu_ps(world.rotation.data() + i,
            _mm256_blendv_ps(rot, _mm256_add_ps(rot, _mm256_mul_ps(av, vdt)), active));
        _mm256_storeu_ps(world.angularVel.data() + i,
            _mm256_blendv_ps(av, _mm256_mul_ps(av, vdrag), active));

        // Position/velocity integration -- active lanes that are NOT platforms. Platforms
        // are entirely moved by UpdatePlatforms (its own fixed step, toward targetA/B);
        // integrating velX/velY*dt here too silently double-moved every platform every
        // frame -- the original "platform outruns the rider" bug.
        __m256i plat = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)(world.isPlatform.data() + i)));
        __m256 integ = _mm256_and_ps(active, _mm256_castsi256_ps(_mm256_cmpeq_epi32(plat, vzeroi)));

        __m256 vy = _mm256_loadu_ps(world.velY.data() + i);
        __m256 g  = _mm256_loadu_ps(world.gravity.data() + i);
        vy = _mm256_blendv_ps(vy, _mm256_add_ps(vy, _mm256_mul_ps(g, vdt)), integ);
        _mm256_storeu_ps(world.velY.data() + i, vy);

        __m256 px = _mm256_loadu_ps(world.posX.data() + i);
        __m256 vx = _mm256_loadu_ps(world.velX.data() + i);
        _mm256_storeu_ps(world.posX.data() + i,
            _mm256_blendv_ps(px, _mm256_add_ps(px, _mm256_mul_ps(vx, vdt)), integ));

        // vy here is already post-gravity -- same order as the scalar code (velY += g*dt
        // happens before posY += velY*dt).
        __m256 py = _mm256_loadu_ps(world.posY.data() + i);
        _mm256_storeu_ps(world.posY.data() + i,
            _mm256_blendv_ps(py, _mm256_add_ps(py, _mm256_mul_ps(vy, vdt)), integ));
    }

    // Scalar tail: the last <8 rows of the chunk, identical math.
    for (; i < end; ++i) {
        if (world.life[i] <= 0.0f || world.isGhosted[i]) continue;
        int slot = world.entitySlot[i];
        if (slot != -1 && ctx.entities.isPlayerControlled[slot] && !gridMovement) continue;

        world.rotation[i] += world.angularVel[i] * dt;
        world.angularVel[i] *= 0.99f;

        if (!world.isPlatform[i]) {
            world.velY[i] += world.gravity[i] * dt;
            world.posX[i] += world.velX[i] * dt;
            world.posY[i] += world.velY[i] * dt;
        }
    }
}

// ---- scalar per-row finalize: life countdown, grid movement, age, collision resolve ----
void PhysicsSystem::ResolveChunk(int start, int end, float dt, PhysicsContext& ctx, bool gridMovement) {
    PhysicsWorld& world = ctx.world;
    if (end > (int)world.maxEntities) end = (int)world.maxEntities;

    for (int j = start; j < end; ++j) {
        if (world.life[j] <= 0.0f) continue;
        if (world.isGhosted[j]) continue;
        // Immovable/kinematic rows NEVER self-resolve. ResolveParticle resolves by moving
        // row i ITSELF out of an overlap -- correct for entities/particles, catastrophic for
        // tiles: a kinematic platform intruding into a static tile made the TILE wall-resolve
        // itself out of the platform's way (slopes shoved upward by a falling platform, solid
        // tiles pushed along by a rising one -- and since Draw recovers a static tile's sprite
        // by POSITION, the displaced row then rendered a different cell's sprite). Platforms
        // (also isTile=1) are excluded for the same reason: UpdatePlatforms owns their motion.
        // Entity-vs-tile interaction is unaffected -- the ENTITY side of every pair still
        // resolves and still fires both sides' collision callbacks. Also a perf win: static
        // tiles no longer run a neighbor query every substep.
        if (world.isTile[j]) continue;

        int slot = world.entitySlot[j];
        bool isPlayerControlled = (slot != -1) && ctx.entities.isPlayerControlled[slot];
        if (isPlayerControlled && !gridMovement) continue;

        // Life counts down AFTER integration (Pass A ran first), same net semantics as the
        // old loop: a particle whose life hits 0 this step still got its final integration,
        // and still resolves below (the life>0 guard above read the PRE-decrement value).
        if (!world.isEntity[j] && !world.isTile[j]) {
            world.life[j] -= dt;
            if (world.life[j] <= 0.0f) world.life[j] = 0.0f;
        }

        if (slot != -1 && ctx.entities.gridMovement[slot]) {
            float tdx = ctx.entities.targetX[slot] - world.posX[j];
            float tdy = ctx.entities.targetY[slot] - world.posY[j];

            float speed = (ctx.entities.moveSpeed[slot] > 0.0f) ? ctx.entities.moveSpeed[slot] : 128.0f;
            float step = speed * dt;

            if (fabsf(tdx) <= step) world.posX[j] = ctx.entities.targetX[slot];
            else world.posX[j] += (tdx > 0 ? 1 : -1) * step;

            if (fabsf(tdy) <= step) world.posY[j] = ctx.entities.targetY[slot];
            else world.posY[j] += (tdy > 0 ? 1 : -1) * step;

            if (world.posX[j] == ctx.entities.targetX[slot] && world.posY[j] == ctx.entities.targetY[slot]) {
                ctx.entities.isMoving[slot] = false;
            }
        }

        world.age[j] += 1;
        PhysicsSystem::ResolveParticle(j, ctx);
    }
}

void PhysicsSystem::UpdateWorldParallel(PhysicsContext& ctx, float dt, JLib::TaskScheduler& sched,
                                        int chunkSize, bool gridMovement) {
    sched.ParallelFor(0, (int)ctx.world.activeCount, chunkSize,
        [&](int start, int end) {
            // Each chunk runs on a worker thread/fiber with no handler of its own -- an
            // uncaught exception there terminates the ENTIRE process (std::terminate),
            // not just this chunk. Catch, report, and drop the chunk instead.
            try {
                UpdateChunk(start, end, dt, ctx, gridMovement);
            } catch (const std::exception& e) {
                FILE* f = nullptr;
                fopen_s(&f, "physics2d_crash.log", "a");
                if (f) { fprintf(f, "[PlatformerPhysics2D] UpdateChunk(%d,%d) threw: %s\n", start, end, e.what()); fclose(f); }
            } catch (...) {
                FILE* f = nullptr;
                fopen_s(&f, "physics2d_crash.log", "a");
                if (f) { fprintf(f, "[PlatformerPhysics2D] UpdateChunk(%d,%d) threw a non-std exception\n", start, end); fclose(f); }
            }
        });
}

void PhysicsSystem::UpdateGroups(PhysicsContext& ctx, float dt) {
    PhysicsWorld& world = ctx.world;
    PlatformTable& plat = ctx.platforms;
    int active = (int)world.activeCount;

    for (int i = 0; i < active; ++i) {
        if (!world.isPlatform[i] || world.platformSlot[i] == -1) continue;
        int slotI = world.platformSlot[i];
        if (plat.parentID[slotI] != -1) continue;

        bool hasCollision = (plat.childEntity[slotI] != -1);
        for (int j = 0; j < active; ++j) {
            if (!world.isPlatform[j] || world.platformSlot[j] == -1) continue;
            int slotJ = world.platformSlot[j];
            if (plat.masterID[slotJ] >= 0 && plat.masterID[slotJ] < active && plat.masterID[slotJ] == i) {
                if (plat.childEntity[slotJ] != -1) {
                    hasCollision = true;
                    break;
                }
            }
        }

        if (hasCollision) {
            plat.timerStarted[slotI] = true;
            plat.trigger[slotI] = true;
        }

        for (int j = 0; j < active; ++j) {
            if (!world.isPlatform[j] || world.platformSlot[j] == -1) continue;
            int slotJ = world.platformSlot[j];
            if (plat.masterID[slotJ] >= 0 && plat.masterID[slotJ] < active && plat.masterID[slotJ] == i) {
                plat.timerStarted[slotJ] = plat.timerStarted[slotI];
                plat.trigger[slotJ] = plat.trigger[slotI];
            }
        }
    }
}

void PhysicsSystem::LinkGroups(PhysicsContext& ctx) {
    PhysicsWorld& world = ctx.world;
    PlatformTable& plat = ctx.platforms;

    // Master = the member closest to the group's own centroid, not just whichever one happens
    // to be spawned/iterated first (for a straight row that would always be one end-piece).
    // A centered master gives a group's own targetA/targetB (defined relative to whichever
    // tile is master -- see MapReader) a path centered on the platform's visual middle, and
    // keeps the master/slave relativeOffset split more even instead of all-on-one-side.
    std::unordered_map<int, std::vector<int>> groupMembers;
    std::unordered_map<int, Vec2> groupPosSum;
    for (int i = 0; i < (int)world.activeCount; ++i) {
        if (!world.isPlatform[i] || world.platformSlot[i] == -1) continue;
        int gID = plat.groupID[world.platformSlot[i]];
        groupMembers[gID].push_back(i);
        groupPosSum[gID].x += world.posX[i];
        groupPosSum[gID].y += world.posY[i];
    }

    std::unordered_map<int, int> groupToMaster;
    for (auto& entry : groupMembers) {
        int gID = entry.first;
        std::vector<int>& members = entry.second;
        Vec2 centroid = groupPosSum[gID];
        centroid.x /= (float)members.size();
        centroid.y /= (float)members.size();

        int best = members[0];
        float bestDistSq = -1.0f;
        for (int m : members) {
            float dx = world.posX[m] - centroid.x;
            float dy = world.posY[m] - centroid.y;
            float distSq = dx * dx + dy * dy;
            if (bestDistSq < 0.0f || distSq < bestDistSq) {
                bestDistSq = distSq;
                best = m;
            }
        }
        groupToMaster[gID] = best;
        plat.parentID[world.platformSlot[best]] = -1;
    }

    for (int i = 0; i < (int)world.activeCount; ++i) {
        if (!world.isPlatform[i] || world.platformSlot[i] == -1) continue;
        int slotI = world.platformSlot[i];
        int gID = plat.groupID[slotI];
        int masterIdx = groupToMaster[gID];

        if (i != masterIdx) {
            plat.parentID[slotI] = masterIdx;
            plat.masterID[slotI] = masterIdx;
            plat.relativeOffsetX[slotI] = world.posX[i] - world.posX[masterIdx];
            plat.relativeOffsetY[slotI] = world.posY[i] - world.posY[masterIdx];
        } else {
            plat.masterID[slotI] = i;
        }
    }
}

void PhysicsSystem::UpdatePlatforms(PhysicsContext& ctx, float dt) {
    PhysicsWorld& world = ctx.world;
    PlatformTable& plat = ctx.platforms;

    for (int i = 0; i < (int)world.activeCount; ++i) {
        if (!world.isPlatform[i] || world.platformSlot[i] == -1) continue;
        int slotI = world.platformSlot[i];
        if (plat.parentID[slotI] != -1) continue; // Skip Slaves (they stay dormant)

        // --- MASTER BRAIN ---
        bool groupTouched = (plat.childEntity[slotI] != -1);
        for (int j = 0; j < (int)world.activeCount; ++j) {
            if (!world.isPlatform[j] || world.platformSlot[j] == -1) continue;
            int slotJ = world.platformSlot[j];
            if (plat.masterID[slotJ] == i && plat.childEntity[slotJ] != -1) {
                groupTouched = true;
                break;
            }
        }

        int originalChild = plat.childEntity[slotI];
        if (groupTouched) plat.childEntity[slotI] = 1; // Lie to the code so it thinks it's touched

        if (plat.canFall[slotI]) {
            // fallDelay, NOT platformTimer -- platformTimer also drives the ordinary back-and-forth
            // reversal pause (see the trigger/shouldMove logic below), so a platform that both
            // patrols AND can fall used to have ONE shared countdown controlling two unrelated
            // things: how long it pauses before reversing, and how long after being stepped on
            // before it starts shaking/falling. fallDelay was already a PlatformTable field --
            // declared and reset, but never actually read anywhere -- this is what wires it up.
            if (plat.childEntity[slotI] != -1 || plat.timerStarted[slotI]) {
                plat.timerStarted[slotI] = true;
                plat.fallDelay[slotI] -= dt;
                if (plat.fallDelay[slotI] < 1.0f && plat.platformState[slotI] == 0) {
                    plat.platformState[slotI] = 1;
                    plat.shakeTimer[slotI] = 1.0f;
                }
                if (plat.platformState[slotI] == 1) { // SHAKING
                    plat.shakeTimer[slotI] -= dt;
                    float offset = (std::sin(plat.shakeTimer[slotI] * 50.0f)) * 2.0f;
                    world.posX[i] += offset;

                    if (plat.shakeTimer[slotI] <= 0.0f) {
                        plat.platformState[slotI] = 2; // FALLING
                    }
                }
            }

            if (plat.platformState[slotI] == 2) {
                world.posY[i] += 500.0f * dt;

                // FORCE DETACH: if a platform falls, nothing can stay on it
                if (plat.childEntity[slotI] != -1) {
                    int child = plat.childEntity[slotI];
                    int childSlot = world.entitySlot[child];
                    if (childSlot != -1) ctx.entities.parentEntity[childSlot] = -1;
                    plat.childEntity[slotI] = -1;
                }
            }
        }

        bool shouldMove = false;
        if (plat.isTriggered[slotI]) {
            shouldMove = (plat.trigger[slotI] || plat.childEntity[slotI] != -1);
        } else {
            shouldMove = true;  // Not triggered: always moving
        }

        if (shouldMove) {
            Vec2 target = plat.movingForward[slotI] ? plat.targetB[slotI] : plat.targetA[slotI];
            float dx = target.x - world.posX[i];
            float dy = target.y - world.posY[i];
            float dist = sqrtf(dx * dx + dy * dy);
            float step = plat.moveSpeed[slotI] * dt;

            if (dist <= step) {
                world.posX[i] = target.x;
                world.posY[i] = target.y;
                world.velX[i] = 0.0f;
                world.velY[i] = 0.0f;
                plat.movingForward[slotI] = !plat.movingForward[slotI];
            } else {
                world.velX[i] = (dx / dist) * plat.moveSpeed[slotI];
                world.velY[i] = (dy / dist) * plat.moveSpeed[slotI];
                world.posX[i] += world.velX[i] * dt;
                world.posY[i] += world.velY[i] * dt;
            }
        }

        // RESTORE: put the original value back so we don't mess up other systems
        plat.childEntity[slotI] = originalChild;

        // BROADCAST: sync the slaves
        for (int j = 0; j < (int)world.activeCount; ++j) {
            if (!world.isPlatform[j] || world.platformSlot[j] == -1 || j == i) continue;
            int slotJ = world.platformSlot[j];
            if (plat.masterID[slotJ] == i) {
                world.posX[j] = world.posX[i] + plat.relativeOffsetX[slotJ];
                world.posY[j] = world.posY[i] + plat.relativeOffsetY[slotJ];
                // velX/velY too, not just position -- PlatformPhysics's rider-carry formula
                // (moveX = velX[rider]*dt + velX[parent]*dt) reads whichever specific segment the
                // rider is currently parented to. Without this, only the master ever had a nonzero
                // velX (only it runs the real movement math above; followers were purely
                // teleported via position broadcast), so a rider standing on any follower segment
                // got ZERO carry from a platform that was actually moving -- the ground visibly
                // slid out from under them, which direction depending on which segment they
                // happened to be parented to. Confirmed via a multi-segment platform group.
                world.velX[j] = world.velX[i];
                world.velY[j] = world.velY[i];
                plat.platformState[slotJ] = plat.platformState[slotI];
                plat.timerStarted[slotJ] = plat.timerStarted[slotI];
            }
        }
    }
}

} // namespace PlatformerPhysics2D
