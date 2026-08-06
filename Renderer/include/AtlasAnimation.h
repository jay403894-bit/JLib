#pragma once
#include <vector>
#include <string>
#include <ResourceManager.h>

// AtlasPacker has no indexed-frame concept -- its sidecar is a flat name->rect map, one region
// per SOURCE FILENAME (see AtlasPacker.cpp's `img.key = path.stem()`). This builds an animation
// out of that by convention: name your source frames "<baseName>_0.png", "<baseName>_1.png", ...
// and this looks each one up individually via GetAtlasRegion, once, up front -- then cycles
// through the resulting list on a timer. Swap BatchItem::uvOffset/uvScale to GetRegion()'s fields
// each frame to actually select the current sub-rect.
class AtlasAnimation
{
public:
	AtlasAnimation() = default;
	AtlasAnimation(JLib::ResourceManager& resourceManager, JLib::AssetHandle<JLib::AtlasResource>& atlas,
	               const std::string& baseName, int frameCount, float fps)
		: fps(fps)
	{
		frames.reserve(frameCount);
		for (int i = 0; i < frameCount; ++i) {
			frames.push_back(resourceManager.GetAtlasRegion(atlas, baseName + "_" + std::to_string(i)));
		}
	}

	void Update(float dt)
	{
		if (frames.size() < 2 || fps <= 0.0f) return;
		float frameDuration = 1.0f / fps;
		timer += dt;
		while (timer >= frameDuration) {
			timer -= frameDuration;
			currentFrame = (currentFrame + 1) % (int)frames.size();
		}
	}

	// Call when switching INTO this animation (e.g. idle -> walk) so it always starts on frame 0
	// instead of resuming wherever its timer happened to be from the last time it played.
	void Reset() { timer = 0.0f; currentFrame = 0; }

	const JLib::AtlasRegion& GetRegion() const { return frames[currentFrame]; }
	bool IsValid() const { return !frames.empty(); }

private:
	std::vector<JLib::AtlasRegion> frames;
	float fps = 8.0f;
	float timer = 0.0f;
	int currentFrame = 0;
};
