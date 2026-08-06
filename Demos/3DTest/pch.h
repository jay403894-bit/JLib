#pragma once

// Precompiled header: heavy, stable, shared headers that all translation units in this
// project include. Adding a header here means it only gets parsed once per configuration
// instead of once per .cpp file, which is where most of this project's build time was going
// (Renderer2D.h alone was ~35% of total build time before this PCH was introduced).

// Windows / CRT
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>

// STL
#include <vector>
#include <memory>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <atomic>
#include <array>

// DirectX
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxgi.h>
#include <wrl.h>
#include <d3dx12.h>
#include <DirectXMath.h>

// Engine libs shared by (almost) every translation unit in this project
#include <Renderer2D.h>
#include <ResourceManager.h>
#include <TaskScheduler.h>
#include <AssetManager.h>
