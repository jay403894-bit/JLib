#pragma once
#include <vector>
#include <functional>
#include "Vec2.h"
#include "CollisionLayer.h"

// Forward declarations only! DO NOT include PhysicsWorld.h/EntityTable.h/PlatformTable.h here.
namespace PlatformerPhysics2D {
struct PhysicsWorld;
struct SpatialGrid;
struct TileMap;
class EntityTable;
class PlatformTable;
}
namespace JLib {
class TaskScheduler;
}

namespace PlatformerPhysics2D {

struct CollisionConstraint {
    bool resolve = false;
    float deltaY = 0.0f;
    float deltaX = 0.0f;
    bool killVelY = false;
    bool killVelX = false;
};

// Bundles the four pieces every non-trivial physics function needs, so call
// sites don't have to thread world/grid/entities/platforms through separately.
struct PhysicsContext {
    PhysicsWorld& world;
    SpatialGrid& grid;
    EntityTable& entities;
    PlatformTable& platforms;
    // Stage-4 static collision geometry (see TileMap.h). A defaulted POINTER, not a
    // reference, so every existing 4-member aggregate init keeps compiling -- and so the
    // smoke tests / anything tile-free can simply not have one. Sensors migrate off the
    // SpatialGrid onto this one at a time; until a sensor checks it, a null tiles is
    // simply "no implicit tilemap, keep using grid tiles."
    TileMap* tiles = nullptr;
};

class PhysicsSystem {
public:
    // The four directional probes. When a TileMap is provided (ctx.tiles), the SEARCH
    // goes through it -- O(cells under the probe) direct lookups, no broadphase -- but the
    // RETURN is still a world row index (via TileMap::rowHandles) so every caller keeps
    // reading tile data through world columns unchanged. tiles == nullptr -> the original
    // SpatialGrid path, bit-identical behavior (this is the Stage-4 migration seam).
    static int TileAboveSensor(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid, const TileMap* tiles = nullptr);
    static int LeftTileSensor(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid, const TileMap* tiles = nullptr);
    static int RightTileSensor(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid, const TileMap* tiles = nullptr);
    static int TileBelowSensor(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid, const TileMap* tiles = nullptr);
    // Wider, slope-robust alternative to TileBelowSensor -- see its comment in the .cpp for why.
    // Used by CheckGrounding; TileBelowSensor itself is left as-is (and still used elsewhere,
    // e.g. CheckHorizontalCollision's adjacentToSlope check) since that usage doesn't need it.
    static int FindFloorMultiTile(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid);
    static int PlatformSensor(int entityIdx, const PhysicsWorld& world, SpatialGrid& grid);

    static void ApplyPhysics(int i, PhysicsContext& ctx, float dt);
    static void ResolveParticle(int i, PhysicsContext& ctx);
    static void SyncPrevPositions(PhysicsWorld& world);

    // Single-threaded chunk update (was UpdateParticleChunk in the raylib source).
    // Now a thin composition of the stages below: IntegrateChunk then ResolveChunk. Kept with
    // its old signature so every existing caller is unchanged and behavior is byte-identical.
    static void UpdateChunk(int start, int end, float dt, PhysicsContext& ctx, bool gridMovement = false);

    // UpdateChunk decomposed into stages, so specialized particle BEHAVIORS whose motion can't
    // be expressed as data through the uniform integrator (sine-wave, boids, homing bullets --
    // anything needing its own code, even something as simple as posY = base + amp*sin(age))
    // can be inserted as their own pass BETWEEN these without bloating the shared hot loops.
    // Intended pipeline per chunk: [behavior passes] -> IntegrateChunk -> ResolveChunk. A
    // behavior pass runs first and writes velX/velY (or position directly), exactly the way
    // UpdateEnemyAI already feeds the shared physics -- integration/collision never change.
    //   IntegrateChunk: uniform gravity/velocity/position + rotation/spin-drag (masked SIMD).
    //   ResolveChunk:   life countdown, grid-movement seek, age, and ResolveParticle (scalar).
    static void IntegrateChunk(int start, int end, float dt, PhysicsContext& ctx, bool gridMovement = false);
    static void ResolveChunk(int start, int end, float dt, PhysicsContext& ctx, bool gridMovement = false);
    // Dispatches UpdateChunk across [0, ctx.world.activeCount) via JLib::TaskScheduler::ParallelFor.
    static void UpdateWorldParallel(PhysicsContext& ctx, float dt, JLib::TaskScheduler& sched,
                                     int chunkSize, bool gridMovement = false);

    static void UpdateGroups(PhysicsContext& ctx, float dt);
    static void LinkGroups(PhysicsContext& ctx);
    static void UpdatePlatforms(PhysicsContext& ctx, float dt);

private:
    static float Lerp(float a, float b, float t);
    static void UpdateEntityRotation(int index, PhysicsWorld& world, Vec2 floorNormal);
    static void WallSlideCheck(int i, PhysicsContext& ctx);
    static void PlatformPhysics(int i, PhysicsContext& ctx, float dt);
    static Vec2 GetSlopeNormal(SlopeType type);
    static void HandleSlopeSnap(int i, PhysicsContext& ctx, float dx);
    static CollisionConstraint CheckSlopeSnap(int i, PhysicsContext& ctx, float dx);
    static CollisionConstraint CheckHorizontalCollision(int i, PhysicsContext& ctx, float dx);
    static CollisionConstraint CheckGrounding(int i, PhysicsContext& ctx);
    static CollisionConstraint CheckCeilingCollision(int i, PhysicsContext& ctx);
    static int GetFloorCandidate(int entityIdx, float testX, float testY, const PhysicsWorld& world, SpatialGrid& grid);
    static int GetHorizontalSensor(int i, PhysicsWorld& world, SpatialGrid& grid, float dx);
    static bool IsBlocked(int i, PhysicsContext& ctx, float dir);
};

} // namespace PlatformerPhysics2D
