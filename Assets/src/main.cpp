// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#include <iostream>
#include <chrono>
#include <thread>
#include <TaskScheduler.h>
#include "../include/AssetManager.h"

struct DummyAsset {
    int value = 0;
};

int main() {
    JLib::TaskScheduler::Init();

    JLib::AssetManager<DummyAsset> manager;

    // Synchronous load + cache hit.
    auto h1 = manager.Load("thing", [](DummyAsset& a) { a.value = 42; return true; });
    auto h2 = manager.Load("thing", [](DummyAsset& a) { a.value = 999; return true; }); // cache hit -- loader not called
    std::cout << "sync load value: " << manager.Resolve(h1).value << " (expect 42)\n";
    std::cout << "cache hit same handle: " << (h1 == h2 ? "yes" : "no") << " (expect yes)\n";

    // Failed load.
    auto hFail = manager.Load("bad", [](DummyAsset&) { return false; });
    std::cout << "failed load handle valid: " << (hFail.IsValid() ? "yes" : "no") << " (expect no)\n";

    // Async load.
    auto hAsync = manager.LoadAsync("async-thing", [](DummyAsset& a) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        a.value = 7;
        return true;
        });
    std::cout << "async load state right away: "
        << (manager.GetLoadState(hAsync) == JLib::AssetManager<DummyAsset>::LoadState::Ready ? "Ready (unexpected)" : "Loading (expected)")
        << "\n";
    while (manager.GetLoadState(hAsync) == JLib::AssetManager<DummyAsset>::LoadState::Loading) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cout << "async load value: " << manager.Resolve(hAsync).value << " (expect 7)\n";

    // Unload + stale handle.
    manager.Unload(h1);
    try {
        manager.Resolve(h1);
        std::cout << "ERROR: Resolve(unloaded handle) should have thrown\n";
    }
    catch (const std::runtime_error&) {
        std::cout << "Resolve(unloaded handle) correctly threw\n";
    }

    JLib::TaskScheduler::Instance().Join();
    return 0;
}
