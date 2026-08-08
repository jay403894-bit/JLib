// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
namespace JLib {
	// A high resolution timer class for measuring time intervals.
	// It uses the Windows QueryPerformanceCounter API for high precision timing.
	class GameTimer {
	public:
		GameTimer();
		float TotalTime() const; // in seconds
		float DeltaTime() const; // in seconds

		void Reset();
		void Start();
		void Stop();
		void Tick();

	private:
		double mSecondsPerCount;
		double mDeltaTime;
		__int64 mBaseTime;
		__int64 mPausedTime;
		__int64 mStopTime;
		__int64 mPrevTime;
		__int64 mCurrTime;
		bool mStopped;
	};
};