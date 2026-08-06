# 🧩 JLib

A C++ game engine stack for Windows — fiber scheduler, DirectX 12 renderer, 2D and 3D physics, audio, input — built as **independently consumable libraries** rather than an engine you have to adopt whole.

**Windows x64 · MSVC · DirectX 12 · BSD licensed**

---

## ⚡ Quick start

```
git clone <this repo>
open JLib.slnx
Build
```

That is the whole setup. No CMake, no package manager, no install step, no build order to get right, nothing to put in `C:\libs`. Nine libraries and their third-party dependencies build from the clone into `build\x64\<Config>\`.

To use it from **your own** project in another solution, import one property sheet:

```xml
<Import Project="C:\JLib\UseJLib.props" />
```

That supplies every include directory, the library path, and all nine `.lib` names. Then `#include <TaskScheduler.h>` and go. See [BUILD.md](BUILD.md).

---

## 📦 What's in it

| Library | What it is |
|---|---|
| **Scheduler** | Fiber-based work-stealing task scheduler. Hand-written x64 context switching, lock-free Chase-Lev deques, frame DAGs with logic gates, topology-aware stealing. [Its own README](Scheduler/README.md) is the deep one. |
| **Renderer** | DirectX 12. 2D sprite/shape batching and a 3D PBR path: three shadow caster types, SSAO, IBL, skinning, GPU particles, glTF loading. |
| **Physics3D** | [Jolt](https://github.com/jrouwe/JoltPhysics) wrapper with a plain-types PIMPL interface — no Jolt header reaches your code. Bodies, constraints, character controller, mesh shapes, raycasts, and a 2D-locked plane mode. |
| **PlatformerPhysics2D** | Hand-written 2D solver for tile-based platformers. Deliberately *not* a rigid-body engine: animation-driven movement with the feel hand-tuned. |
| **Input** | Raw Input + XInput. Gamepads read over HID, so **input contributes zero threads** to your process. No SDK, no NuGet. |
| **Sound** | miniaudio-backed mixing, run as demand-driven pool work rather than a dedicated audio thread. |
| **Scene** | Minimal scene stack. |
| **Time** | Frame timing. |
| **Assets**, **Geometry** | Header-only: generational asset handles, rectangle maths. |

Everything runs on the scheduler. That is the point of the stack rather than an implementation detail: physics stepping, command-list recording, particle updates and asset decoding are all tasks on one pool, so they compose instead of each spawning threads that fight each other.

---

## 📈 Measured, not asserted

Numbers on an i9-13900K, Release. The scheduler README has the full set.

| | |
|---|---|
| Task enqueue → dequeue latency | **6.3 µs** |
| 5-node frame DAG, build + validate + execute | **31.9 µs** |
| `Task` struct | **exactly 64 bytes**, `static_assert`-enforced |

Two findings that contradicted the received wisdom, including advice I had been given:

- **Hard-pinning workers made things worse.** Every talk says pin your pool. Measured, `SetThreadAffinityMask` cost **~45% wake latency** versus `SetThreadIdealProcessor` and nearly 2× on a frame-shaped DAG — a pinned worker can only be woken onto its own core. The default changed to `Ideal`. *Caveat kept honest: one machine, idle, hybrid CPU; the contention case hard affinity is supposed to win was not tested.*
- **`ParallelFor`'s serial/parallel crossover is not an element count.** It is **~75 µs of work**. Sweeping per-element cost against N, the crossover *count* moved 400× while the crossover *work* stayed pinned at 70–92 µs. The old `N > 10000` gate was wrong in both directions. It now probes and extrapolates.

---

## 🎯 Where this sits

Closest in spirit to **raylib** — same "get something on screen without ceremony" goal — but this is *similar to* raylib, **not** a raylib API. Only the shape primitives are near-identical; sprites go through an explicit `BatchItem`. That is a design choice rather than a gap: raylib hides batching, JLib exposes it, which is why 256K instances work.

The honest use case is the migration: **prototype in raylib, move over when you hit the ceiling** — particle-heavy scenes, anything wanting real parallelism, anything that wants a modern explicit API. Ports of Pong, Space Invaders and Tetris exist as evidence that the move is a weekend, not a rewrite.

Against the DX12/Vulkan engines (bgfx, Diligent, The Forge, Wicked), the difference is distribution shape rather than feature count: those are adopt-the-engine or clone-and-fight-CMake. This is a set of libraries you can take one piece of. Taking just the scheduler is a reversible decision; adopting an engine is not.

---

## ⚠️ Status and honest limitations

**Young, solo-maintained, and Windows-only by construction.** The context switch is hand-written x64 assembly and the renderer is DirectX 12, so portability is not a missing feature — it is a decision. Constraining the problem is what makes one person able to do this well.

- **No editor and no GUI tools.** Scenes are authored in code.
- **No asset streaming yet.** Levels load up front.
- **The renderer is not finished.** FP16 + real tonemapping, AA, bloom and HDR output are the current milestone; today it is Reinhard straight to 8-bit.
- **Physics3D does not wrap all of Jolt.** Vehicles, ragdolls and soft bodies are unwrapped; the plumbing for them exists.
- **Demos are not in this repo yet.** They are the next thing to land.
- Third-party code is vendored and built here, so a clone is self-contained. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) — everything is MIT, BSD, Boost or public domain, so shipping a binary owes attribution and nothing else.

---

## 📄 License

JLib is **BSD 3-Clause** — see [LICENSE](LICENSE). Vendored dependencies keep their own licences, all permissive, itemised in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
