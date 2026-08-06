#include "../include/SpatialGrid.h"
#include <algorithm>
#include <cmath>

namespace PlatformerPhysics2D {

SpatialGrid::SpatialGrid(int worldW, int worldH, int cellSize, int capacity)
    : cellSize(cellSize), capacity(capacity) {
    gridW = worldW / cellSize + 1;
    gridH = worldH / cellSize + 1;
    cellStart.assign(binCountTotal(), 0);
    cellCount.assign(binCountTotal(), 0);
}

int SpatialGrid::binOf(float x, float y, float w, float h) const {
    // Oversized (or NaN -- the negated comparison catches it) rows go to the overflow
    // bin: a row spanning multiple cells can't be represented by one center cell, and
    // scanning the few large rows on every query is far cheaper than the old
    // insert-into-every-overlapped-cell scheme it replaces.
    if (!(w <= (float)cellSize) || !(h <= (float)cellSize)) return overflowBin();
    if (std::isnan(x) || std::isnan(y)) return overflowBin();

    // Same independent clamping as the old grid (see the 7-06 clamp fix): out-of-range
    // rows collapse onto the boundary cell instead of producing an unindexable bin.
    int gx = std::clamp((int)(x / cellSize), 0, gridW - 1);
    int gy = std::clamp((int)(y / cellSize), 0, gridH - 1);
    return gy * gridW + gx;
}

// Compatibility wrapper -- the real iteration lives in forEachCandidate (SpatialGrid.h).
// The one-full-cell margin there covers center-binned neighbors whose own half-extent
// (<= cellSize/2 by the binning rule) reaches into the query rect from a cell the rect
// doesn't touch, plus slack for movement since the last commit rebuilt the runs. Bounds
// are clamped independently to [0, gridDim-1] -- one-sided clamps let minX exceed maxX
// once the rect leaves the grid entirely, silently emptying the loop (the pre-port
// collision-vanishes bug).
void SpatialGrid::query(int idx, float x, float y, float w, float h, std::vector<int>& neighbors, QueryScratchpad&) {
    neighbors.clear();
    forEachCandidate(idx, x, y, w, h, [&neighbors](int j) { neighbors.push_back(j); });
}

} // namespace PlatformerPhysics2D
