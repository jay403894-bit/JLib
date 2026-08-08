// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include "Vec2.h"

namespace PlatformerPhysics2D {

// Stable-slot pool for moving/falling-platform-only physics state.
// Same freelist mechanics as EntityTable -- see that header for the rationale.
class PlatformTable {
public:
    explicit PlatformTable(size_t capacity) : cap(capacity) {
        groupID.resize(cap, -1);
        masterID.resize(cap, -1);
        parentID.resize(cap, -1);
        relativeOffsetX.resize(cap, 0.0f);
        relativeOffsetY.resize(cap, 0.0f);
        targetA.resize(cap, Vec2{ 0.0f, 0.0f });
        targetB.resize(cap, Vec2{ 0.0f, 0.0f });
        movingForward.resize(cap, 0);
        canFall.resize(cap, 0);
        platformState.resize(cap, 0);
        timerStarted.resize(cap, 0);
        isTriggered.resize(cap, 0);
        trigger.resize(cap, 0);
        touched.resize(cap, 0);
        moveSpeed.resize(cap, 0.0f);
        platformTimer.resize(cap, 0.0f);
        fallDelay.resize(cap, 0.0f);
        shakeTimer.resize(cap, 0.0f);
        childEntity.resize(cap, -1);

        freelist.resize(cap);
        for (size_t i = 0; i < cap; ++i) freelist[i] = static_cast<int>(cap - 1 - i);
        freeTop = static_cast<int>(cap);
    }

    int Alloc() {
        if (freeTop <= 0) throw std::runtime_error("PlatformerPhysics2D: PlatformTable exhausted");
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

    std::vector<int>     groupID, masterID, parentID;
    std::vector<float>   relativeOffsetX, relativeOffsetY;
    std::vector<Vec2>    targetA, targetB;
    std::vector<uint8_t> movingForward, canFall, platformState, timerStarted,
                          isTriggered, trigger, touched;
    std::vector<float>   moveSpeed, platformTimer, fallDelay, shakeTimer;
    std::vector<int>     childEntity;

private:
    void ResetRow(int slot) {
        groupID[slot] = -1;
        masterID[slot] = -1;
        parentID[slot] = -1;
        relativeOffsetX[slot] = 0.0f;
        relativeOffsetY[slot] = 0.0f;
        targetA[slot] = Vec2{ 0.0f, 0.0f };
        targetB[slot] = Vec2{ 0.0f, 0.0f };
        movingForward[slot] = 0;
        canFall[slot] = 0;
        platformState[slot] = 0;
        timerStarted[slot] = 0;
        isTriggered[slot] = 0;
        trigger[slot] = 0;
        touched[slot] = 0;
        moveSpeed[slot] = 0.0f;
        platformTimer[slot] = 0.0f;
        fallDelay[slot] = 0.0f;
        shakeTimer[slot] = 0.0f;
        childEntity[slot] = -1;
    }

    std::vector<int> freelist;
    int freeTop = 0;
    size_t cap = 0;
};

} // namespace PlatformerPhysics2D
