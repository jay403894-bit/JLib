// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <windows.h>

namespace JLib {
    class Time {
    private:
        // inline (C++17+) -- the in-class declaration IS the definition, so this header-only
        // class doesn't need a companion .cpp just to provide out-of-class definitions for these.
        inline static LARGE_INTEGER startTime{};
        inline static LARGE_INTEGER frequency{};

    public:
        static void Initialize() {
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&startTime);
        }

        static double GetTime() {
            LARGE_INTEGER currentTime;
            QueryPerformanceCounter(&currentTime);
            // Calculate elapsed time in seconds as a double
            return static_cast<double>(currentTime.QuadPart - startTime.QuadPart) /
                static_cast<double>(frequency.QuadPart);
        }
    };
}