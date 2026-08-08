// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <stack>
#include <memory>
#include "Scene.h"

class SceneManager {
private:
	static std::stack<std::unique_ptr<Scene>> scenes;
	SceneManager() = default;
public:
	SceneManager(const SceneManager&) = delete;
	SceneManager& operator=(const SceneManager&) = delete;

	static void PushScene(std::unique_ptr<Scene> newScene);
	static void PopScene();
	// Destroy every scene NOW. `scenes` has static storage duration, so without this the scenes --
	// and everything they own -- outlive main and are destroyed at static destruction time, after the
	// renderer, audio, fonts and physics globals they hold references to are already gone. Call this
	// from the host's teardown block, before those subsystems are shut down.
	static void Clear();
	static void ReplaceScene(std::unique_ptr<Scene> newScene);
	static void Update(bool& isRunning, float dt = 0.0f);
	static void Draw();
	static void HandleInput(float dt);
	static Scene* GetCurrentScene();
};
