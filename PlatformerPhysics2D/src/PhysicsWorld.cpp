// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#include "../include/PhysicsWorld.h"
#include "../include/SpatialGrid.h"
#include <atomic>

namespace PlatformerPhysics2D {

// Lane assignment for requestSpawn(): each thread pays one relaxed fetch_add the FIRST
// time it ever requests a spawn, then reuses its lane forever (thread_local). Shared
// across PhysicsWorld instances by design -- the lane id is a property of the thread,
// each world has its own buffers under that id.
static size_t SpawnLaneIndex() {
    static std::atomic<size_t> nextLane{ 0 };
    static thread_local size_t lane = SIZE_MAX;
    if (lane == SIZE_MAX)
        lane = nextLane.fetch_add(1, std::memory_order_relaxed) % PhysicsWorld::kSpawnLanes;
    return lane;
}

void PhysicsWorld::reset() {
    // Column::allocate sizes to maxEntities (padded to a multiple of 16, 64B-aligned,
    // both buffers) and fills every slot -- padding included -- with the default value.
    // Re-calling reset() at the same capacity is just a refill, no reallocation.
    gravity.allocate(maxEntities, 0.0f);
    posX.allocate(maxEntities, 0.0f);
    posY.allocate(maxEntities, 0.0f);
    velX.allocate(maxEntities, 0.0f);
    velY.allocate(maxEntities, 0.0f);
    life.allocate(maxEntities, 0.0f);
    size.allocate(maxEntities, Vec2{ 0.0f, 0.0f });
    age.allocate(maxEntities, 0);
    shapeType.allocate(maxEntities, ShapeType::RECT);
    rotation.allocate(maxEntities, 0.0f);
    mask.allocate(maxEntities, 0);
    layer.allocate(maxEntities, 0);
    isTile.allocate(maxEntities, 0);
    health.allocate(maxEntities, 0);
    isEntity.allocate(maxEntities, 0);
    friction.allocate(maxEntities, 0.0f);
    isParticle.allocate(maxEntities, 0);
    restitution.allocate(maxEntities, 0.0f);
    angularVel.allocate(maxEntities, 0.0f);
    hasCollisionEvent.allocate(maxEntities, 0);
    isPlatform.allocate(maxEntities, 0);
    prevPos.allocate(maxEntities, Vec2{ 0.0f, 0.0f });
    isGhosted.allocate(maxEntities, 0);
    isSlope.allocate(maxEntities, 0);
    slopeType.allocate(maxEntities, SlopeType::NONE);
    tileID.allocate(maxEntities, 0);
    entitySlot.allocate(maxEntities, -1);
    platformSlot.allocate(maxEntities, -1);
    particleSlot.allocate(maxEntities, -1);
    emitterSlot.allocate(maxEntities, -1);

    // std::function isn't trivially copyable, so onCollision stays a std::vector (cold
    // data -- see the member's comment in PhysicsWorld.h).
    onCollision.clear();
    onCollision.resize(maxEntities, nullptr);

    // Stable-id plumbing. On a RE-reset at the same capacity, every generation is
    // bumped instead of zeroed -- handles taken before the reset must resolve to -1
    // afterward, not accidentally match a fresh row that reused their id.
    id.allocate(maxEntities, 0u);
    idToIndex.assign(maxEntities, -1);
    if (idGen.size() == maxEntities) {
        for (auto& g : idGen) ++g;
    } else {
        idGen.assign(maxEntities, 0u);
    }
    idFreelist.resize(maxEntities);
    for (size_t i = 0; i < maxEntities; ++i)
        idFreelist[i] = static_cast<uint32_t>(maxEntities - 1 - i);
    idFreeTop = static_cast<int>(maxEntities);

    activeCount = 0;
}

int PhysicsWorld::spawn(ShapeType shape, Vec2 _size, float x, float y, float vx, float vy, float lifetime,
                         float _gravity, uint8_t _layer, uint8_t _mask, bool _isEntity, bool _isPlatform, bool _isParticle) {
    // Unlocked by design -- see the declaration's comment. Single-writer discipline
    // (spawn/commit only ever run where no parallel pass is in flight) replaces the
    // old particle_mutex_; worker-side spawning goes through requestSpawn().
    if (activeCount >= maxEntities) return -1; // Database is full!

    size_t idx = activeCount;
    velX[idx] = vx;
    velY[idx] = vy;
    life[idx] = lifetime;
    size[idx] = _size;
    shapeType[idx] = shape;
    age[idx] = 0;
    posX[idx] = x;
    posY[idx] = y;
    // Stamped here, not left for the next SyncPrevPositions call -- prevPos otherwise stays
    // whatever a previous occupant of this slot left it at (or zero-initialized {0,0} for a
    // brand-new slot) until the NEXT substep's SyncPrevPositions runs. Any caller that
    // interpolates render position between prevPos and pos (to smooth out a fixed-timestep
    // accumulator's uneven substep cadence) would see this row visibly lerp in from the wrong
    // point -- world origin for a first-ever spawn, or the previous occupant's last position for
    // a reused slot -- for one full frame every single time anything spawns.
    prevPos[idx] = Vec2{ x, y };
    gravity[idx] = _gravity;
    layer[idx] = _layer;
    mask[idx] = _mask;
    isEntity[idx] = _isEntity ? 1 : 0;
    isPlatform[idx] = _isPlatform ? 1 : 0;
    isParticle[idx] = _isParticle ? 1 : 0;

    // Always stamp -1 first: this index may be a reused slot from a previous
    // occupant, and its old back-pointer must never leak into the new cell.
    entitySlot[idx] = -1;
    platformSlot[idx] = -1;
    particleSlot[idx] = -1;
    // Same reasoning for the callback pair -- these were never stamped on spawn, so a
    // reused slot silently inherited its previous occupant's onCollision (latent since
    // the original port; the sorted-commit's buffer swaps would have made it fire).
    hasCollisionEvent[idx] = 0;
    onCollision[idx] = nullptr;
    // And the remaining columns a fresh row must not inherit through a swapped-in
    // back buffer (pre-scatter these were guaranteed by reset()'s fill; post-scatter
    // the slot may hold garbage from two commits ago):
    rotation[idx] = 0.0f;
    angularVel[idx] = 0.0f;
    isTile[idx] = 0;
    isGhosted[idx] = 0;
    isSlope[idx] = 0;
    slopeType[idx] = SlopeType::NONE;
    health[idx] = 0;
    friction[idx] = 0.0f;
    restitution[idx] = 0.0f;
    tileID[idx] = 0;
    emitterSlot[idx] = -1; // caller opts an entity into an emitter explicitly after spawn() returns
    if (_isEntity)   entitySlot[idx] = entities.Alloc();
    if (_isPlatform) platformSlot[idx] = platforms.Alloc();
    if (_isParticle) particleSlot[idx] = particles.Alloc();

    // Stable id: exhaustion is impossible (ids == maxEntities >= live rows, and the
    // activeCount check above already rejected a full world).
    uint32_t newId = idFreelist[--idFreeTop];
    id[idx] = newId;
    idToIndex[newId] = static_cast<int32_t>(idx);

    activeCount++; // Slide the boundary forward
    return static_cast<int>(idx);
}

void PhysicsWorld::requestSpawn(SpawnRequest req) {
    spawnLanes[SpawnLaneIndex()].pending.push_back(std::move(req));
}

void PhysicsWorld::commit(SpatialGrid* grid) {
    // Compact/sort first, then apply spawns: freshly spawned rows land in contiguous
    // slots at the end and are never shuffled by the pass they just missed. They get
    // binned by the NEXT commit (see the header comment).
    if (grid) scatterCommit(*grid);
    else      compressArrays();
    applySpawnLanes();
}

// The fused pass: kill-compaction + cell-sort + grid build in one O(n) sweep.
// Counting sort by bin: classify every live row, prefix-sum the bin counts into run
// starts, then stream every column front->back at its final sorted position and flip
// the buffers. Dead rows simply never get a destination -- compaction is free.
void PhysicsWorld::scatterCommit(SpatialGrid& grid) {
    const size_t oldActive = activeCount;
    const int bins = grid.binCountTotal();

    grid.cellCount.assign(bins, 0);
    grid.cellStart.assign(bins, 0);
    binOfRow.resize(oldActive);
    scatterDst.resize(oldActive);

    // Pass 1: classify live rows into bins; retire dead rows (identical bookkeeping to
    // compressArrays' dead branch -- slots freed BEFORE any relocation can overwrite
    // them, emitterSlot cleared but never freed here, id generation bumped).
    size_t live = 0;
    for (size_t i = 0; i < oldActive; ++i) {
        if (life[i] > 0.0f) {
            int b = grid.binOf(posX[i], posY[i], size[i].x, size[i].y);
            binOfRow[i] = b;
            grid.cellCount[b]++;
            live++;
        } else {
            binOfRow[i] = -1;
            if (entitySlot[i] != -1)   { entities.Free(entitySlot[i]);   entitySlot[i] = -1; }
            if (platformSlot[i] != -1) { platforms.Free(platformSlot[i]); platformSlot[i] = -1; }
            if (particleSlot[i] != -1) { particles.Free(particleSlot[i]); particleSlot[i] = -1; }
            emitterSlot[i] = -1;
            uint32_t deadId = id[i];
            idGen[deadId]++;
            idToIndex[deadId] = -1;
            idFreelist[idFreeTop++] = deadId;
        }
    }

    // Exclusive prefix sum -> run starts.
    int running = 0;
    for (int b = 0; b < bins; ++b) {
        grid.cellStart[b] = running;
        running += grid.cellCount[b];
    }

    // Destination assignment. cellStart doubles as the per-bin cursor here (classic
    // counting-sort trick), then gets restored by subtracting the counts -- no separate
    // cursor array, no per-commit allocation.
    for (size_t i = 0; i < oldActive; ++i) {
        int b = binOfRow[i];
        scatterDst[i] = (b >= 0) ? grid.cellStart[b]++ : -1;
    }
    for (int b = 0; b < bins; ++b)
        grid.cellStart[b] -= grid.cellCount[b];

    // Repoint the id map at every survivor's new home (front id buffer still valid --
    // nothing has swapped yet).
    for (size_t i = 0; i < oldActive; ++i)
        if (scatterDst[i] >= 0) idToIndex[id[i]] = scatterDst[i];

    // Pass 2: column-wise scatter into the back buffers, then flip. Column-by-column
    // (not row-by-row) so each column streams sequentially on the read side.
    auto scatter = [&](auto& col) {
        auto* src = col.data();
        auto* dst = col.backBuffer();
        for (size_t i = 0; i < oldActive; ++i) {
            int32_t d = scatterDst[i];
            if (d >= 0) dst[d] = src[i];
        }
        col.swapBuffers();
    };
    scatter(shapeType); scatter(gravity);
    scatter(posX); scatter(posY); scatter(velX); scatter(velY);
    scatter(life); scatter(size); scatter(age);
    scatter(mask); scatter(layer);
    scatter(rotation); scatter(angularVel);
    scatter(isEntity); scatter(isTile); scatter(isParticle); scatter(isPlatform);
    scatter(isGhosted); scatter(isSlope); scatter(slopeType);
    scatter(health); scatter(friction); scatter(restitution);
    scatter(hasCollisionEvent); scatter(prevPos); scatter(tileID);
    scatter(entitySlot); scatter(platformSlot); scatter(particleSlot); scatter(emitterSlot);
    scatter(id);

    // onCollision is a std::vector<std::function> (non-trivial), so it gets its own
    // move-scatter through a persistent scratch vector instead of the Column path.
    if (onCollisionScratch.size() != maxEntities) onCollisionScratch.resize(maxEntities);
    for (size_t i = 0; i < oldActive; ++i) {
        int32_t d = scatterDst[i];
        if (d >= 0) onCollisionScratch[d] = std::move(onCollision[i]);
    }
    std::swap(onCollision, onCollisionScratch);
    // Release stale callbacks past the live range (leftovers from an older commit in
    // the swapped-in vector) so dead captures don't stay alive invisibly.
    for (size_t j = live; j < maxEntities; ++j)
        onCollision[j] = nullptr;

    // Defensive tail stamp, same as compressArrays: rows past the live range must read
    // as dead even though nothing should iterate them.
    for (size_t j = live; j < oldActive; ++j) {
        life[j] = -1.0f;
        age[j] = 0;
    }

    activeCount = live;

    // Post-swap: entitySlot/platformSlot are in sorted order, scatterDst still maps
    // old rows to new -- fix up every table-stored row reference (see the decl comment).
    remapTableRowRefs(live, oldActive);
}

void PhysicsWorld::remapTableRowRefs(size_t liveCount, size_t oldCount) {
    auto remap = [&](int v) -> int {
        return (v >= 0 && v < (int)oldCount) ? scatterDst[v] : -1;
    };
    for (size_t r = 0; r < liveCount; ++r) {
        int es = entitySlot[r];
        if (es != -1) entities.parentEntity[es] = remap(entities.parentEntity[es]);
        int ps = platformSlot[r];
        if (ps != -1) {
            platforms.masterID[ps]    = remap(platforms.masterID[ps]);
            platforms.parentID[ps]    = remap(platforms.parentID[ps]);
            platforms.childEntity[ps] = remap(platforms.childEntity[ps]);
        }
    }
}

void PhysicsWorld::applySpawnLanes() {
    for (auto& lane : spawnLanes) {
        for (SpawnRequest& req : lane.pending) {
            int idx = spawn(req.shape, req.size, req.x, req.y, req.vx, req.vy, req.lifetime,
                            req.gravity, req.layer, req.mask,
                            req.isEntity, req.isPlatform, req.isParticle);
            if (idx == -1) continue; // world full -- drop, same contract as direct spawn()

            if (req.isParticle) {
                int slot = particleSlot[idx];
                if (slot != -1) {
                    particles.particleType[slot] = req.particleType;
                    particles.damage[slot] = req.damage;
                    particles.sourceEntity[slot] = req.sourceEntity;
                }
            }
            if (req.onCollision) {
                onCollision[idx] = std::move(req.onCollision);
                hasCollisionEvent[idx] = 1;
            }
        }
        lane.pending.clear();
    }
}

void PhysicsWorld::compressArrays() {
    const size_t oldActive = activeCount;
    scatterDst.resize(oldActive); // old-row -> new-row map, for remapTableRowRefs below
    size_t writeIdx = 0;
    for (size_t readIdx = 0; readIdx < activeCount; ++readIdx) {
        if (life[readIdx] > 0.0f) {
            scatterDst[readIdx] = static_cast<int32_t>(writeIdx);
            if (readIdx != writeIdx) {
                posX[writeIdx] = posX[readIdx];
                posY[writeIdx] = posY[readIdx];
                velX[writeIdx] = velX[readIdx];
                velY[writeIdx] = velY[readIdx];
                gravity[writeIdx] = gravity[readIdx];
                life[writeIdx] = life[readIdx];
                shapeType[writeIdx] = shapeType[readIdx];
                size[writeIdx] = size[readIdx];
                rotation[writeIdx] = rotation[readIdx];
                age[writeIdx] = age[readIdx];
                layer[writeIdx] = layer[readIdx];
                mask[writeIdx] = mask[readIdx];
                isTile[writeIdx] = isTile[readIdx];
                health[writeIdx] = health[readIdx];
                isEntity[writeIdx] = isEntity[readIdx];
                friction[writeIdx] = friction[readIdx];
                isParticle[writeIdx] = isParticle[readIdx];
                restitution[writeIdx] = restitution[readIdx];
                angularVel[writeIdx] = angularVel[readIdx];
                onCollision[writeIdx] = onCollision[readIdx];
                hasCollisionEvent[writeIdx] = hasCollisionEvent[readIdx];
                isPlatform[writeIdx] = isPlatform[readIdx];
                prevPos[writeIdx] = prevPos[readIdx];
                isGhosted[writeIdx] = isGhosted[readIdx];
                isSlope[writeIdx] = isSlope[readIdx];
                slopeType[writeIdx] = slopeType[readIdx];
                tileID[writeIdx] = tileID[readIdx];
                entitySlot[writeIdx] = entitySlot[readIdx];
                platformSlot[writeIdx] = platformSlot[readIdx];
                particleSlot[writeIdx] = particleSlot[readIdx];
                emitterSlot[writeIdx] = emitterSlot[readIdx];

                // Relocate the stable id with the row and repoint the id map at the
                // row's new home -- this is what keeps Handles valid across compaction.
                id[writeIdx] = id[readIdx];
                idToIndex[id[readIdx]] = static_cast<int32_t>(writeIdx);

                life[readIdx] = -1.0f;
                age[readIdx] = 0;
            }
            writeIdx++;
        } else {
            scatterDst[readIdx] = -1;
            // This cell died this pass -- free its actor/platform slot now, before
            // its storage is potentially overwritten by a later surviving cell.
            // Rows that were merely relocated (the `readIdx != writeIdx` branch
            // above) are NOT touched here; their slot is still owned by the
            // relocated cell at its new position.
            if (entitySlot[readIdx] != -1) {
                entities.Free(entitySlot[readIdx]);
                entitySlot[readIdx] = -1;
            }
            if (platformSlot[readIdx] != -1) {
                platforms.Free(platformSlot[readIdx]);
                platformSlot[readIdx] = -1;
            }
            if (particleSlot[readIdx] != -1) {
                particles.Free(particleSlot[readIdx]);
                particleSlot[readIdx] = -1;
            }
            // emitterSlot is NOT freed here -- PlatformerPhysics2D doesn't own that pool (see the field's
            // comment in PhysicsWorld.h). The game must free its own emitter slot for this row
            // BEFORE calling compressArrays(), by checking (life[i] <= 0 && emitterSlot[i] != -1)
            // for every row first. Just clear the stale index so a future occupant of this slot
            // never inherits it.
            emitterSlot[readIdx] = -1;

            // Retire the stable id: bump the generation FIRST so every outstanding
            // Handle to this row goes stale, then recycle the id.
            uint32_t deadId = id[readIdx];
            idGen[deadId]++;
            idToIndex[deadId] = -1;
            idFreelist[idFreeTop++] = deadId;
        }
    }

    for (size_t i = writeIdx; i < activeCount; ++i) {
        life[i] = -1.0f;
        age[i] = 0;
    }

    activeCount = writeIdx;

    // Rows moved -- fix up table-stored row references (parentEntity/masterID/...).
    remapTableRowRefs(writeIdx, oldActive);
}

} // namespace PlatformerPhysics2D
