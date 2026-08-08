// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <vector>
#include <cmath>
#include <type_traits>

namespace PlatformerPhysics2D {

// Kept only so existing call sites (sensors declare one and pass it through) keep
// compiling -- the sorted-runs grid below returns each row at most once by construction
// (every row lives in exactly ONE bin), so the old lastSeen/version dedup is dead.
struct QueryScratchpad {
    std::vector<int> lastSeen;
    int queryVersion = 0;
    void reset(int) {}
};

// Sorted-runs spatial grid. There is no insert()/clear() anymore: PhysicsWorld::commit(&grid)
// physically reorders the live world rows by bin (counting-sort scatter, fused with
// kill-compaction), then fills cellStart/cellCount here. After a commit, bin b's rows are
// exactly the world indices [cellStart[b], cellStart[b] + cellCount[b]) -- contiguous in
// every SoA column, which is what makes queries cache-friendly and SIMD-able.
//
// Binning rule (see binOf): a row whose AABB fits within one cell on both axes goes in the
// cell under its CENTER; anything larger (wide ground tiles, multi-cell platforms) goes in
// the single OVERFLOW bin, which every query scans unconditionally. Because small rows are
// center-binned (not inserted into every overlapped cell like the old grid), query() expands
// its cell range by one full cell of margin: a small neighbor's center can sit at most
// cellSize/2 outside the query rect while still overlapping it, and the extra half-cell of
// slack absorbs intra-frame movement between commits (the grid is rebuilt once per commit,
// not per substep).
//
// Until the first commit() runs, cellStart/cellCount are all zero and queries return nothing.
struct SpatialGrid {
    int gridW, gridH, cellSize;
    int capacity;

    // Filled by PhysicsWorld::commit(&grid). Sized binCountTotal(); last entry = overflow.
    std::vector<int> cellStart;
    std::vector<int> cellCount;

    // capacity should be PhysicsWorld::maxEntities (the base row count) --
    // SpatialGrid indexes by base row index throughout, never by entity/platform slot.
    SpatialGrid(int worldW, int worldH, int cellSize, int capacity);

    int binCountTotal() const { return gridW * gridH + 1; }
    int overflowBin()   const { return gridW * gridH; }

    // (x, y) = center, (w, h) = FULL extents (PhysicsWorld::size convention).
    // NaN-safe: unrepresentable rows land in the overflow bin rather than UB-casting.
    int binOf(float x, float y, float w, float h) const;

    // (x, y) = center, (w, h) = HALF extents -- same convention the sensors use.
    // Appends every candidate in the overlapped cell range (plus margin, plus the
    // overflow bin) except `idx` itself. No dedup needed: one bin per row.
    // Compatibility wrapper over forEachCandidate below -- prefer the callback form in
    // hot paths: it materializes no list (no per-query heap allocation, no copy) and is
    // reentrancy-safe (all iteration state on the stack, so a collision callback that
    // itself queries can't clobber an in-flight iteration the way a shared/thread_local
    // buffer could). A fixed-size neighbor array was considered and rejected: neighbor
    // count is DENSITY-bounded, not geometry-bounded -- any cap silently drops
    // candidates the moment particles cluster, which reads as "collision randomly
    // stopped working under load."
    void query(int idx, float x, float y, float w, float h, std::vector<int>& neighbors, QueryScratchpad& scratch);

    // Calls f(candidateRowIndex) for every candidate; same semantics as query(). f may
    // return void (visit everything) or bool (return false to stop early -- how the
    // directional sensors bail on the first hit).
    template <class F>
    void forEachCandidate(int idx, float x, float y, float w, float h, F&& f) const {
        auto visit = [&](int j) -> bool {
            if constexpr (std::is_void_v<decltype(f(j))>) { f(j); return true; }
            else { return f(j); }
        };

        if (std::isnan(x) || std::isnan(y) || std::isnan(w) || std::isnan(h)) return;

        // One full cell of margin -- see query()'s definition for the derivation.
        const float m = (float)cellSize;
        int minX = clampCell((int)((x - w - m) / cellSize), gridW);
        int maxX = clampCell((int)((x + w + m) / cellSize), gridW);
        int minY = clampCell((int)((y - h - m) / cellSize), gridH);
        int maxY = clampCell((int)((y + h + m) / cellSize), gridH);

        for (int cy = minY; cy <= maxY; ++cy) {
            for (int cx = minX; cx <= maxX; ++cx) {
                int b = cy * gridW + cx;
                int end = cellStart[b] + cellCount[b];
                for (int j = cellStart[b]; j < end; ++j)
                    if (j != idx && !visit(j)) return;
            }
        }
        {
            int b = overflowBin();
            int end = cellStart[b] + cellCount[b];
            for (int j = cellStart[b]; j < end; ++j)
                if (j != idx && !visit(j)) return;
        }
    }

    // Hands the caller whole candidate RUNS ([begin, end) world-index ranges) instead of
    // individual indices -- the shape a SIMD narrow-phase prefilter wants: contiguous
    // per-axis loads straight out of the sorted columns. Same cell-range/margin/overflow
    // semantics as forEachCandidate. Does NOT exclude any index (including the querying
    // row itself) -- the caller's per-candidate logic self-skips.
    template <class F>
    void forEachRun(float x, float y, float w, float h, F&& f) const {
        if (std::isnan(x) || std::isnan(y) || std::isnan(w) || std::isnan(h)) return;

        const float m = (float)cellSize;
        int minX = clampCell((int)((x - w - m) / cellSize), gridW);
        int maxX = clampCell((int)((x + w + m) / cellSize), gridW);
        int minY = clampCell((int)((y - h - m) / cellSize), gridH);
        int maxY = clampCell((int)((y + h + m) / cellSize), gridH);

        for (int cy = minY; cy <= maxY; ++cy) {
            for (int cx = minX; cx <= maxX; ++cx) {
                int b = cy * gridW + cx;
                if (cellCount[b] > 0) f(cellStart[b], cellStart[b] + cellCount[b]);
            }
        }
        {
            int b = overflowBin();
            if (cellCount[b] > 0) f(cellStart[b], cellStart[b] + cellCount[b]);
        }
    }

private:
    static int clampCell(int v, int dim) { return v < 0 ? 0 : (v >= dim ? dim - 1 : v); }
};

} // namespace PlatformerPhysics2D
