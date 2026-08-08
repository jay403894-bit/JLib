// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// Standalone usage example for JLib::Input. Kept COMMENTED OUT on purpose: this project builds a
// static library that apps link, and a stray main() here can collide with the consumer's entry
// point. Copy it into your own project to try the library.
//
// No SDK, no NuGet package, no external dependency: Raw Input and hid.lib are both in-box, and
// hid.lib is linked by a #pragma inside InputManager.cpp. Gamepads (including Xbox pads) are read
// over HID, so the input subsystem contributes ZERO threads to your process.
/*
#include "../include/InputManager.h"
#include <cstdio>

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* input = reinterpret_cast<JLib::InputManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_INPUT:                   // <-- the entire delivery mechanism. No threads, no polling.
        if (input) input->OnRawInput(lp);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"JLibInputDemo";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"JLib::Input -- focus me, then type/move/click",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 200,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    JLib::InputManager input;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&input));
    if (!input.Initialize(hwnd)) { printf("Initialize failed\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    bool running = true;
    while (running) {
        // Drain the queue every frame -- a `while`, never an `if`. WM_INPUT arrives at the mouse's
        // polling rate (125-1000 Hz), so handling one message per frame backlogs the queue and the
        // app ends up replaying minutes-old input as if something else were driving it.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        input.Update();                                   // once per frame, before any query
        if (input.IsKeyPressed(VK_ESCAPE)) running = false;

        uint8_t key;
        if (input.GetAnyKeyPressed(key))                                        printf("key 0x%02X\n", key);
        if (input.IsMouseButtonPressed(JLib::InputManager::MouseLeftButton))    printf("LMB\n");
        float dx = input.GetMouseDeltaX(), dy = input.GetMouseDeltaY();         // TRUE per-frame delta
        if (dx || dy)                                                           printf("mouse %+.0f %+.0f\n", dx, dy);

        for (uint32_t p = 0; p < 4; ++p) {
            if (!input.IsGamepadConnected(p)) continue;
            if (input.IsButtonPressed(p, JLib::InputManager::GamepadA)) printf("pad%u: A\n", p);
            float lx = input.GetLeftStickX(p), ly = input.GetLeftStickY(p);
            if (lx * lx + ly * ly > 0.25f) printf("pad%u: stick %+.2f %+.2f\n", p, lx, ly);
        }
        Sleep(16);
    }
    input.Shutdown();
    return 0;
}
*/
