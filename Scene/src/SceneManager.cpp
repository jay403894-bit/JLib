// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#include "../include/SceneManager.h"

std::stack<std::unique_ptr<Scene>> SceneManager::scenes;

void SceneManager::PushScene(std::unique_ptr<Scene> newScene) {
    scenes.push(std::move(newScene));
}

void SceneManager::PopScene() {
    if (!scenes.empty()) {
        scenes.pop();
    }
}

void SceneManager::Clear() {
    // Top-down, so a scene is always destroyed before the one it was pushed on top of.
    while (!scenes.empty())
        scenes.pop();
}

void SceneManager::ReplaceScene(std::unique_ptr<Scene> newScene) {
    PopScene();
    PushScene(std::move(newScene));
}

void SceneManager::Update(bool& isRunning, float dt) {
    if (!scenes.empty()) {
        scenes.top()->Update(isRunning, dt);
    }
}

void SceneManager::Draw() {
    if (!scenes.empty()) {
        scenes.top()->Draw();
    }
}

void SceneManager::HandleInput(float dt) {
    if (!scenes.empty()) {
        scenes.top()->HandleInput(dt);
    }
}

Scene* SceneManager::GetCurrentScene() {
    return scenes.empty() ? nullptr : scenes.top().get();
}
