#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace PlatformerPhysics2D {

// Stable-slot pool for actor-only physics state (player/enemy movement state).
// Fixed capacity, freelist-based -- like JLib::TaskAllocator, slots never move,
// so back-pointers held elsewhere (PhysicsWorld::entitySlot) never go stale.
class EntityTable {
public:
    explicit EntityTable(size_t capacity) : cap(capacity) {
        isJumping.resize(cap, 0);
        isGrounded.resize(cap, 0);
        isFacingRight.resize(cap, 0);
        isPlayerControlled.resize(cap, 0);
        isSliding.resize(cap, 0);
        jumpRequested.resize(cap, 0);
        gridMovement.resize(cap, 0);
        isMoving.resize(cap, 0);
        jumpsRemaining.resize(cap, 0);
        jumpForce.resize(cap, 0.0f);
        hitTimer.resize(cap, 0.0f);
        targetX.resize(cap, 0.0f);
        targetY.resize(cap, 0.0f);
        moveSpeed.resize(cap, 0.0f);
        parentEntity.resize(cap, -1);
        moveIntent.resize(cap, 0.0f);
        health.resize(cap, 0);

        freelist.resize(cap);
        for (size_t i = 0; i < cap; ++i) freelist[i] = static_cast<int>(cap - 1 - i);
        freeTop = static_cast<int>(cap);
    }

    int Alloc() {
        if (freeTop <= 0) throw std::runtime_error("PlatformerPhysics2D: EntityTable exhausted");
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

    std::vector<uint8_t> isJumping, isGrounded, isFacingRight, isPlayerControlled,
                          isSliding, jumpRequested, gridMovement, isMoving, health;
    std::vector<uint8_t> jumpsRemaining;
    std::vector<float>   jumpForce, hitTimer, targetX, targetY, moveSpeed;
    std::vector<int>     parentEntity;
    std::vector<float>   moveIntent; // -1..+1 horizontal intent, written by a game-side controller

private:
    void ResetRow(int slot) {
        isJumping[slot] = 0;
        isGrounded[slot] = 0;
        isFacingRight[slot] = 0;
        isPlayerControlled[slot] = 0;
        isSliding[slot] = 0;
        jumpRequested[slot] = 0;
        gridMovement[slot] = 0;
        isMoving[slot] = 0;
        jumpsRemaining[slot] = 0;
        jumpForce[slot] = 0.0f;
        hitTimer[slot] = 0.0f;
        targetX[slot] = 0.0f;
        targetY[slot] = 0.0f;
        moveSpeed[slot] = 0.0f;
        parentEntity[slot] = -1;
        moveIntent[slot] = 0.0f;
        health[slot] = 0;
    }

    std::vector<int> freelist;
    int freeTop = 0;
    size_t cap = 0;
};

} // namespace PlatformerPhysics2D
