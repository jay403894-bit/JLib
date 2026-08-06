#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace PlatformerPhysics2D {

// Stable-slot pool for particle-only data -- what KIND of particle this is (a plain
// game-defined int tag, PlatformerPhysics2D attaches no meaning to it) and any payload a collision
// callback needs to act on, e.g. damage. PlatformerPhysics2D itself never reads these columns --
// ResolveParticle's non-tile branch just fires onCollision and lets the game-side callback
// look up world.particles.damage[world.particleSlot[i]] (etc.) to decide what happened.
// Same freelist mechanics as EntityTable/PlatformTable -- see EntityTable.h for the rationale.
class ParticleTable {
public:
    explicit ParticleTable(size_t capacity) : cap(capacity) {
        particleType.resize(cap, 0);
        damage.resize(cap, 0.0f);
        sourceEntity.resize(cap, -1);

        freelist.resize(cap);
        for (size_t i = 0; i < cap; ++i) freelist[i] = static_cast<int>(cap - 1 - i);
        freeTop = static_cast<int>(cap);
    }

    int Alloc() {
        if (freeTop <= 0) throw std::runtime_error("PlatformerPhysics2D: ParticleTable exhausted");
        int slot = freelist[--freeTop];
        ResetRow(slot);
        return slot;
    }

    void Free(int slot) {
        ResetRow(slot);
        freelist[freeTop++] = slot;
    }

    bool IsValid(int slot) const { return slot >= 0 && slot < static_cast<int>(cap); }
    size_t Capacity() const { return cap; }

    std::vector<int>   particleType;  // game-defined tag (cast from the game's own enum)
    std::vector<float> damage;
    std::vector<int>   sourceEntity;  // who fired/spawned this particle, -1 if none

private:
    void ResetRow(int slot) {
        particleType[slot] = 0;
        damage[slot] = 0.0f;
        sourceEntity[slot] = -1;
    }

    std::vector<int> freelist;
    int freeTop = 0;
    size_t cap = 0;
};

} // namespace PlatformerPhysics2D
