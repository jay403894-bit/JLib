// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once

class Scene
{
public:
	Scene() = default;
	Scene(const Scene&) = delete;
	Scene& operator=(const Scene&) = delete;
	virtual void Update(bool& isRunning, float dt = 0.0f) = 0;
	virtual void Draw() = 0;
	virtual void HandleInput(float dt) = 0;
	virtual ~Scene() {};
};
