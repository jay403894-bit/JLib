#pragma once
#include <DirectXMath.h>
#include "ResourceManager.h"

// Static sibling of GridSpriteAnimation -- for pre-made TILESETS (world_tileset.png: bricks,
// ground, water, decorations, all distinct art, nothing meant to cycle) rather than character
// animation sheets. No timer/frame-advance at all: you pick a (col, row) cell once per draw call
// (e.g. from tileID -> a lookup table you define) and get that cell's UV rect back. Same grid-math
// assumption as GridSpriteAnimation -- a UNIFORM grid, every cell the same pixel size.
namespace JLib
{
	class GridSpriteSheet
	{
	public:
		GridSpriteSheet() = default;
		GridSpriteSheet(JLib::TextureHandle sheet, int framesX, int framesY)
			: sheet(sheet), framesX(framesX), framesY(framesY)
		{}

		JLib::TextureHandle GetTexture() const { return sheet; }
		// Grid dims -- lets sprite-index encode/decode (index = row * framesX + col) use THIS
		// sheet's real column count instead of assuming 16-wide (which silently mis-sliced any
		// non-16-column sheet, e.g. the 11x16 four-seasons tileset or the 4x4 platform sheet).
		int GetFramesX() const { return framesX; }
		int GetFramesY() const { return framesY; }

		DirectX::XMFLOAT2 GetUVOffset(int col, int row) const
		{
			return { ((float)col + kUVInset) / (float)framesX, ((float)row + kUVInset) / (float)framesY };
		}

		DirectX::XMFLOAT2 GetUVScale() const
		{
			return { (1.0f - 2.0f * kUVInset) / (float)framesX, (1.0f - 2.0f * kUVInset) / (float)framesY };
		}

	private:
		// Cell UVs used to run exactly edge-to-edge (col/framesX). The quad's edge pixels then
		// interpolate to a UV mathematically ON the cell boundary, and FP error occasionally lands
		// a hair past it -- point sampling snaps to the NEIGHBORING cell's texel row, drawing a
		// 1px line of the tile above/beside this one. Sub-pixel (interpolated) render positions
		// decide whether a given frame hits the bad rounding, which is why it flickered rarely
		// instead of always. Insetting the rect by 0.1% of a cell (0.016 texels on a 16px cell --
		// far below anything visible) keeps every interpolated UV strictly inside the cell.
		static constexpr float kUVInset = 0.001f;
		JLib::TextureHandle sheet;
		int framesX = 1, framesY = 1;
	};
};