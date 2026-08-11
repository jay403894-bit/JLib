// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

// Exercises the NAMED event path -- WaitOnEvent / GetEvent().SignalAll() -- which the benchmark
// suite never touches. Written when Event went from a std::mutex around an unordered_set to a
// lock-free intrusive stack, because that change had no coverage otherwise.
//
//   clang++ -std=c++17 -O2 -g -I include tests/event_smoke.cpp build/libScheduler.a -o eventsmoke
//   ./eventsmoke ; echo "exit $?"
//
// What it actually tries to break, in order:
//   1. Many fibers pushing onto the waiter stack CONCURRENTLY -- the CAS loop in AddWaiter is the
//      only place two threads race, so the test wants real contention, not one waiter at a time.
//   2. A signal that arrives while registrations are still in flight -- the interesting ordering,
//      since a waiter that registers after the exchange must not be lost. Repeated rounds with no
//      settling delay give that window a chance to open.
//   3. Signalling an event nobody is waiting on, and one that has already been drained -- both
//      must be no-ops rather than faults.
#include <TaskScheduler.h>
#include <Event.h>     // TaskScheduler.h only forward-declares Event; GetEvent() returns Event&
#include <cstdio>
#include <atomic>
#include <thread>
#include <chrono>

static std::atomic<int> g_resumed{ 0 };
static std::atomic<int> g_entered{ 0 };

int main() {
    JLib::TaskScheduler::SetAffinityPolicy(JLib::TaskScheduler::AffinityPolicy::None);
    JLib::TaskScheduler::Init();
    JLib::TaskScheduler& sched = JLib::TaskScheduler::Instance();

    bool ok = true;

    // ---- empty and double signal: must not fault ----
    sched.GetEvent("never_waited").SignalAll();
    sched.GetEvent("never_waited").SignalAll();
    printf("empty signal        : ok\n");

    // ---- the real test: N waiters per round, many rounds ----
    constexpr int kRounds  = 200;
    constexpr int kWaiters = 24;

    for (int round = 0; round < kRounds; ++round) {
        char name[32];
        snprintf(name, sizeof(name), "round_%d", round % 8);   // bounded name set, as documented
        auto& ev = sched.GetEvent(name);

        g_entered.store(0, std::memory_order_relaxed);
        const int before = g_resumed.load(std::memory_order_relaxed);

        JLib::WaitGroup wg;
        wg.n.fetch_add(kWaiters, std::memory_order_relaxed);
        for (int i = 0; i < kWaiters; ++i) {
            // noFiber=false: these suspend, so they need a fiber under them.
            JLib::Task* t = sched.CreateTask([&sched, name] {
                g_entered.fetch_add(1, std::memory_order_relaxed);
                sched.WaitOnEvent(name);
                g_resumed.fetch_add(1, std::memory_order_relaxed);
            }, false, JLib::FiberSize::Standard, false);
            if (!t) { printf("FAIL: CreateTask returned null (round %d)\n", round); return 1; }
            t->waitGroup = &wg;
            sched.Push(t);
        }

        // Signal repeatedly rather than waiting for all to register. A waiter that pushes AFTER an
        // exchange has to be caught by a later one -- that is exactly the race worth hammering, and
        // sleeping until everyone registered would hide it.
        while (wg.n.load(std::memory_order_acquire) > 0) {
            ev.SignalAll();
            std::this_thread::yield();
        }
        sched.WaitFor(wg);

        const int gained = g_resumed.load(std::memory_order_relaxed) - before;
        if (gained != kWaiters) {
            printf("FAIL round %d: %d of %d waiters resumed\n", round, gained, kWaiters);
            ok = false;
            break;
        }
    }

    printf("concurrent waiters  : %s (%d rounds x %d waiters = %d resumes)\n",
        ok ? "ok" : "FAILED", kRounds, kWaiters, g_resumed.load());

    // ---- signal a drained event again ----
    sched.GetEvent("round_0").SignalAll();
    printf("drained re-signal   : ok\n");

    sched.Join();
    printf("\n%s\n", ok ? "ALL CHECKS PASSED" : "FAILURES ABOVE");
    return ok ? 0 : 1;
}
