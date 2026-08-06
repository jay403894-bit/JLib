#pragma once
#include <DirectXMath.h>
#include "ResourceManager.h"

// For pre-made sprite sheets (Brackeys-style free asset packs, etc.) that already ARE one image
// sliced into a uniform grid -- unlike AtlasAnimation (named-region lookup via AtlasPacker's
// per-SOURCE-FILE packing), this needs no packer/sidecar at all. Load the sheet itself with
// ResourceManager::LoadTexture (NOT LoadAtlas -- there's no per-region sidecar for a single
// pre-made sheet), then slice frames out of it by grid math: frame index -> (col, row) -> UV rect.
//
// Only fits a UNIFORM grid (every cell the same pixel size, no trimming/rotation). Some asset
// packs instead ship their own JSON/XML atlas (e.g. TexturePacker output) with per-region rects --
// that's a third case, closer to AtlasAnimation's shape than this one, and needs a small parser
// for whatever format that specific pack uses.
namespace JLib
{
	// A single animation (e.g. "walk", "idle", "jump") from a pre-made sprite sheet, which is one
	// image sliced into a uniform grid of frames. Each animation is a contiguous subset of the
	// sheet's cells, and this class tracks the current frame and timing for that animation.
	class GridSpriteAnimation
	{
	public:
		GridSpriteAnimation() = default;
		// framesX/framesY: how many columns/rows the WHOLE sheet is sliced into.
		// firstFrame/frameCount: which cells (row-major: left-to-right, then top-to-bottom) belong to
		// THIS animation -- e.g. a walk cycle might be frames 8-11 of a larger sheet, while idle is 0-1.
		// loop: true (default, matches every prior caller's behavior unchanged) wraps back to frame 0
		// forever. false plays through once and HOLDS on the last frame instead of wrapping -- for
		// one-shot animations like a death pose that shouldn't visibly restart while still displayed
		// (e.g. during a multi-second respawn/Game-Over delay after it finishes).
		GridSpriteAnimation(JLib::TextureHandle sheet, int framesX, int framesY, int firstFrame, int frameCount, float fps, bool loop = true)
			: sheet(sheet), framesX(framesX), framesY(framesY), firstFrame(firstFrame), frameCount(frameCount), fps(fps), loop(loop)
		{}

		void Update(float dt)
		{
			if (frameCount < 2 || fps <= 0.0f || finished) return;
			float frameDuration = 1.0f / fps;
			timer += dt;
			while (timer >= frameDuration) {
				timer -= frameDuration;
				if (currentFrame + 1 < frameCount) {
					currentFrame++;
				} else if (loop) {
					currentFrame = 0;
				} else {
					finished = true;
					break; // stay on the last frame -- don't keep draining timer past it
				}
			}
		}

		// True once a non-looping animation has played through to its last frame and stopped.
		// Always false for a looping animation (there's no "finished" for something that repeats).
		bool IsFinished() const { return finished; }

		// Call when switching INTO this animation so it always starts on its own frame 0.
		void Reset() { timer = 0.0f; currentFrame = 0; finished = false; }

		JLib::TextureHandle GetTexture() const { return sheet; }

		DirectX::XMFLOAT2 GetUVOffset() const
		{
			int frame = firstFrame + currentFrame;
			int col = frame % framesX;
			int row = frame / framesX;
			return { ((float)col + kUVInset) / (float)framesX, ((float)row + kUVInset) / (float)framesY };
		}

		DirectX::XMFLOAT2 GetUVScale() const
		{
			return { (1.0f - 2.0f * kUVInset) / (float)framesX, (1.0f - 2.0f * kUVInset) / (float)framesY };
		}

	private:
		// See GridSpriteSheet::kUVInset -- same edge-to-edge boundary-bleed guard for animation
		// frames (a frame's neighbor in the sheet is usually another pose, so the stray line is
		// extra visible on characters).
		static constexpr float kUVInset = 0.001f;
		JLib::TextureHandle sheet;
		int framesX = 1, framesY = 1;
		int firstFrame = 0, frameCount = 1;
		float fps = 8.0f;
		float timer = 0.0f;
		int currentFrame = 0;
		bool loop = true;
		bool finished = false;
	};
};