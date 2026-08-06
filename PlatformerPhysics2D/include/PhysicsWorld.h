#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include <functional>
#include "Vec2.h"
#include "Column.h"
#include "CollisionLayer.h"
#include "EntityTable.h"
#include "PlatformTable.h"
#include "ParticleTable.h"

namespace PlatformerPhysics2D {

struct PhysicsWorld;
struct SpatialGrid;
using CollisionCallback = std::function<void(int, int, PhysicsWorld&, float, float)>;

// Stable reference to a world row. Raw row indices are only valid until the next
// commit(&grid) -- the cell-sort scatter physically reorders every live row -- so
// anything holding a reference across frames (the player, a boss, a camera target)
// must hold a Handle and resolve() it each frame. The generation counter catches
// stale handles: once the row dies, its id is recycled with a bumped generation and
// every old handle resolves to -1 instead of silently reading the recycled row.
struct Handle {
    uint32_t id = UINT32_MAX;
    uint32_t gen = 0;
};

// A deferred spawn, recorded via PhysicsWorld::requestSpawn() and applied by commit().
// Carries everything spawn() takes PLUS the post-spawn stamps a runtime spawner would
// otherwise do through spawn()'s returned index (ParticleTable payload, collision
// callback) -- a deferred spawn has no index to return, so those must ride along.
// Anything genuinely needing the index at spawn time (e.g. wiring an emitterSlot)
// remains a direct-spawn use case, done outside the parallel phase.
struct SpawnRequest {
    ShapeType shape = ShapeType::RECT;
    Vec2 size{ 0.0f, 0.0f };
    float x = 0.0f, y = 0.0f;
    float vx = 0.0f, vy = 0.0f;
    float lifetime = 0.0f;
    float gravity = 0.0f;
    uint8_t layer = 0, mask = 0;
    bool isEntity = false, isPlatform = false, isParticle = false;
    // Applied iff isParticle (ParticleTable payload -- see Emitter-style spawners):
    int particleType = 0;
    float damage = 0.0f;
    int sourceEntity = -1;
    // Optional collision callback (owner-first convention -- see ResolveParticle):
    CollisionCallback onCollision = nullptr;
};

// Dense, swap-compacted SoA for every physical cell (tiles, particles, and entities
// alike). Actor-only and platform-only behavior state lives in the small stable-slot
// `entities`/`platforms` pools instead, reached via the `entitySlot`/`platformSlot`
// back-pointers below -- see EntityTable.h/PlatformTable.h for why those pools use a
// freelist rather than dense compaction.
// Columns are Column<T> (Column.h): fixed-capacity, 64-byte-aligned, double-buffered,
// raw-pointer-indexed. Sized once by reset() to maxEntities and NEVER reallocated after --
// indices and .data() pointers stay stable for the world's whole lifetime. operator[] is
// unchanged from the vector days, so call sites don't care. The ONE exception is
// onCollision: std::function is not trivially copyable, so it stays a std::vector -- it's
// cold data (only touched when a collision event actually fires), so it doesn't need the
// alignment/double-buffer treatment anyway.
struct PhysicsWorld {
    Column<ShapeType> shapeType;
    Column<float> gravity;
    Column<float> posX, posY;
    Column<float> velX, velY;
    Column<float> life;
    Column<Vec2> size;
    Column<size_t> age;
    Column<uint8_t> mask;
    Column<uint8_t> layer;
    Column<float> rotation;
    Column<float> angularVel;
    Column<uint8_t> isEntity;
    Column<uint8_t> isTile;
    Column<uint8_t> isParticle;
    Column<uint8_t> isPlatform;
    Column<uint8_t> isGhosted;
    Column<uint8_t> isSlope;
    Column<SlopeType> slopeType;
    Column<uint8_t> health;     // Durability (so particles can chip away at tiles)
    Column<float> friction;
    Column<float> restitution;
    std::vector<CollisionCallback> onCollision;
    Column<uint8_t> hasCollisionEvent;
    Column<Vec2> prevPos;
    Column<uint32_t> tileID;
    Column<int32_t> entitySlot;   // -1 if this cell is not an actor; else index into `entities`
    Column<int32_t> platformSlot; // -1 if this cell is not a behavior-platform; else index into `platforms`
    Column<int32_t> particleSlot; // -1 if this cell is not a particle; else index into `particles`
    // -1 if this cell doesn't own a particle emitter; else an index into a pool the GAME owns,
    // not PlatformerPhysics2D -- unlike entities/platforms/particles above, an emitter (RendererCore
    // effectID, Renderer2D&, ...) is a rendering concern and can't live in this renderer-agnostic
    // lib. PlatformerPhysics2D only carries the slot number and relocates/clears it like any other column;
    // the game must check for it on a dying row (life[i] <= 0) BEFORE calling compressArrays()
    // and free its own pool slot then -- compressArrays() clears this to -1 but never frees it.
    Column<int32_t> emitterSlot;

    size_t activeCount = 0;
    size_t maxEntities = 0;

    EntityTable entities;
    PlatformTable platforms;
    ParticleTable particles;

    PhysicsWorld() : entities(0), platforms(0), particles(0) {}
    PhysicsWorld(size_t maxCells, size_t maxActors, size_t maxPlatforms, size_t maxParticles)
        : maxEntities(maxCells), entities(maxActors), platforms(maxPlatforms), particles(maxParticles) {
        reset();
    }

    void reset();

    // Direct spawn -- writes the row and publishes it by bumping activeCount. NOT
    // thread-safe by design (the old particle_mutex_ is gone): activeCount is a
    // publication boundary, and no lock-free scheme can make "bump count, then fill
    // fields" safe against a concurrent reader iterating 0..activeCount. Call this only
    // when no parallel physics pass is in flight: level load, scene setup, main-thread
    // gameplay code outside UpdateWorldParallel. Anything running DURING the parallel
    // phase (collision callbacks, emitters ticked from worker tasks) must use
    // requestSpawn() instead.
    int spawn(ShapeType shape, Vec2 size, float x, float y, float vx, float vy, float lifetime,
              float gravity = 0.0f, uint8_t layer = 0, uint8_t mask = 0,
              bool isEntity = false, bool isPlatform = false, bool isParticle = false);

    // Thread-safe deferred spawn: records the request in the calling thread's own lane
    // (no shared writes, no atomics on the hot path -- one thread_local lane assignment
    // per thread, ever). Nothing becomes visible until commit() applies it.
    void requestSpawn(SpawnRequest req);

    // Single-threaded frame-commit. Call from ONE thread at a point where no parallel
    // physics pass is in flight -- this replaces both the old compressArrays() call AND
    // the game-side per-substep grid rebuild loop.
    //
    // With a grid: ONE fused O(n) pass does kill-compaction, cell-sort (counting-sort
    // scatter into the Columns' back buffers, then a pointer swap -- no in-place
    // shuffle), and the grid build (fills grid.cellStart/cellCount; bin b's rows are
    // the contiguous index range [cellStart[b], cellStart[b]+cellCount[b])).
    // Without a grid (nullptr): falls back to plain compressArrays() compaction.
    // Either way, deferred SpawnRequests are applied afterward -- fresh spawns land in
    // contiguous rows at the end, integrate immediately, and get binned by the NEXT
    // commit (spawn-this-frame / broadphase-next-frame; invisible at substep rates).
    //
    // EVERY live row's index changes on a sorted commit -- hold a Handle (handleOf/
    // resolve) for anything referenced across frames. Same emitterSlot contract as
    // compressArrays(): the game frees its own emitter-pool slots for dying rows BEFORE
    // this runs. Requests that don't fit are dropped, matching spawn()'s -1 contract.
    void commit(SpatialGrid* grid = nullptr);

    void compressArrays();

    // Stable-reference API (see Handle above). handleOf(index) is valid for any live
    // row; resolve(h) returns the row's CURRENT index, or -1 if the row died (or the
    // handle is stale -- generation mismatch).
    Handle handleOf(int index) const {
        uint32_t rowId = id[(size_t)index];
        return Handle{ rowId, idGen[rowId] };
    }
    int resolve(Handle h) const {
        if (h.id >= idGen.size() || idGen[h.id] != h.gen) return -1;
        return idToIndex[h.id];
    }

    // Per-thread deferred-spawn lanes. A thread claims a lane index once (thread_local)
    // and only ever appends to its own lane, so the parallel phase writes these with no
    // synchronization. alignas(64) keeps lanes off each other's cache lines. 64 lanes
    // covers any realistic worker count; >64 distinct threads would share via modulo
    // (documented, not expected on this project's hardware).
    static constexpr size_t kSpawnLanes = 64;
    struct alignas(64) SpawnLane { std::vector<SpawnRequest> pending; };
    std::array<SpawnLane, kSpawnLanes> spawnLanes;

    // ---- stable-id plumbing (see Handle) ----
    Column<uint32_t> id;              // per-row stable id, relocated with the row
    std::vector<int32_t> idToIndex;   // id -> current row index; -1 while the id is dead
    std::vector<uint32_t> idGen;      // bumped when an id is freed; stale handles miss
    std::vector<uint32_t> idFreelist; // LIFO stack of dead ids
    int idFreeTop = 0;

private:
    void scatterCommit(SpatialGrid& grid);  // the fused compact+sort+grid-build pass
    void applySpawnLanes();
    // Fix up the WORLD-ROW-INDEX fields stored inside the stable-slot tables
    // (EntityTable::parentEntity, PlatformTable::masterID/parentID/childEntity) after
    // rows move. scatterDst must hold the old-row -> new-row mapping (-1 = row died);
    // any stored reference to a dead row becomes -1. This was a LATENT bug even in the
    // pre-sort compressArrays: a rider's parentEntity went stale whenever any earlier
    // row died and shifted the platform down -- masked in practice only by Game01's
    // ghost-don't-kill discipline.
    void remapTableRowRefs(size_t liveCount, size_t oldCount);

    // Scratch for scatterCommit, sized lazily, reused every commit (no per-frame allocs
    // after warmup).
    std::vector<int32_t> binOfRow;
    std::vector<int32_t> scatterDst;
    std::vector<CollisionCallback> onCollisionScratch;
};

} // namespace PlatformerPhysics2D
