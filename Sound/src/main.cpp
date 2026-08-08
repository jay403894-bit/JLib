// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

/*

#include <iostream>
#include <chrono>
#include <thread>
#include <TaskScheduler.h>
#include "../include/SoundManager.h"

int main() {
    JLib::TaskScheduler::Init();

    SoundManager sound;
    // No core parameter: mixing runs as demand-driven pool tasks (the device callback pushes a
    // hiPri refill job whenever the ring has room) -- see SoundManager.h's class comment.
    if (!sound.Initialize()) {
        std::cout << "SoundManager::Initialize failed -- is a playback device available?\n";
        return 1;
    }

    // Point these at real files to try it -- WAV/MP3/FLAC all decode via the same PlaySound/
    // PlayLoop call, miniaudio picks the right decoder from the file itself.
    SoundHandle music = sound.PlayLoop("music.mp3", 0.5f);
    if (!music.IsValid()) {
        std::cout << "PlayLoop(\"music.mp3\") failed to load -- put a real file next to the exe to test.\n";
    }
    SoundHandle effect = sound.PlaySound("effect.wav", 1.0f);
    if (!effect.IsValid()) {
        std::cout << "PlaySound(\"effect.wav\") failed to load -- put a real file next to the exe to test.\n";
    }

    std::cout << "Playing for 4 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(4));

    // Handle-based control: fade the music down and stop it early, independent of the one-shot
    // effect (which either already finished on its own or keeps playing -- IsPlaying() tells you
    // which without needing to track duration yourself).
    sound.SetVolume(music, 0.1f);
    std::cout << "effect still playing: " << (sound.IsPlaying(effect) ? "yes" : "no") << "\n";

    std::cout << "Playing for 4 more seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(4));
    sound.Stop(music);

    std::cout << "Shutting down...\n";
    sound.Shutdown();

    JLib::TaskScheduler::Instance().Join();
    return 0;
}
*/
