// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <cstddef>
#include <malloc.h>
#include <type_traits>
#include <utility>

namespace PlatformerPhysics2D {

// Fixed-capacity, 64-byte-aligned, double-buffered SoA column. Replaces std::vector for
// PhysicsWorld's hot columns, for four reasons:
//  1. Raw-pointer operator[] -- no MSVC iterator-debug overhead in Debug builds (vector's
//     checked operator[] dominates Debug-build physics time at particle counts).
//  2. 64-byte-aligned allocation -- aligned SIMD loads, no cache-line-split accesses.
//  3. Capacity padded up to a multiple of 16 elements -- an 8-wide SIMD loop may read past
//     the logical count without faulting; padding is filled with the column's fill value
//     (a later broadphase pass can overwrite padding with never-matching sentinels).
//  4. Two buffers -- the planned commit-phase scatter (kill-compaction + cell-sort + grid
//     build in one O(n) pass) writes into back(), then swapBuffers() flips. No in-place
//     shuffle, no per-swap element copies.
// Trivially-copyable types only: buffers are raw aligned storage; constructors never run.
// Non-trivial columns (std::function callbacks) must stay in std::vector.
template <typename T>
class Column {
    static_assert(std::is_trivially_copyable_v<T>,
        "Column<T> is raw aligned storage -- keep non-trivial types in std::vector");
public:
    Column() = default;
    Column(const Column&) = delete;
    Column& operator=(const Column&) = delete;
    ~Column() { release(); }

    // Sizes (or re-sizes) to hold `logicalCount` elements and fills EVERY slot of both
    // buffers -- padding included -- with `fillValue`. Reallocation only happens when the
    // padded capacity actually changes, so a same-size reset() is just a refill.
    void allocate(size_t logicalCount, const T& fillValue) {
        size_t wantPadded = (logicalCount + 15) & ~size_t(15);
        if (wantPadded != padded_) {
            release();
            padded_ = wantPadded;
            if (padded_ == 0) return;
            front_ = static_cast<T*>(_aligned_malloc(padded_ * sizeof(T), 64));
            back_  = static_cast<T*>(_aligned_malloc(padded_ * sizeof(T), 64));
        }
        fill(fillValue);
    }

    void fill(const T& v) {
        for (size_t i = 0; i < padded_; ++i) { front_[i] = v; back_[i] = v; }
    }

    void release() {
        if (front_) { _aligned_free(front_); front_ = nullptr; }
        if (back_)  { _aligned_free(back_);  back_ = nullptr; }
        padded_ = 0;
    }

    T&       operator[](size_t i)       noexcept { return front_[i]; }
    const T& operator[](size_t i) const noexcept { return front_[i]; }
    T*       data()       noexcept { return front_; }
    const T* data() const noexcept { return front_; }

    T*     backBuffer() noexcept { return back_; }      // commit-phase scatter target
    void   swapBuffers() noexcept { std::swap(front_, back_); }
    size_t paddedCapacity() const noexcept { return padded_; }

private:
    T* front_ = nullptr;
    T* back_  = nullptr;
    size_t padded_ = 0;   // logical capacity rounded up to a multiple of 16
};

} // namespace PlatformerPhysics2D
