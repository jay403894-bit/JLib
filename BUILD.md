# JLib — unified build

**Clone, open `JLib.slnx`, press Build.** No install step, no `C:\libs`, no build order to get right.

Everything lands in `build\x64\<Config>\`. Delete `build\` to clean the whole tree.

---

## Why this exists

Before the umbrella, an adopter had to clone ~8 separate repos, build each **in the right order**,
and hand-deploy headers and `.lib`s into `C:\libs` before anything would link. That order was
invisible and undocumented, and every project hard-coded `C:\libs` paths — so nothing built on a
machine that wasn't the author's.

It also caused a subtler bug: because each library lived in its own solution, MSBuild could not see
that changing a header in one required rebuilding its consumers. The result was stale-ABI crashes
that surfaced as mystery access violations deep in unrelated code (`ExecuteCommandLists`, typically)
instead of honest link errors. A single solution makes that structurally impossible.

## How it works — three mechanisms, nothing clever

1. **`ProjectReference`** — a consumer names a dependency's `.vcxproj`, not its `.lib`. MSBuild
   derives the build order and the link from that. See `Scheduler/bench/Bench.vcxproj`: it links the
   scheduler without ever naming `Scheduler.lib`.

2. **`JLib.Common.props`** — imported by every project. Defines the shared `OutDir`/`IntDir` and every
   public include directory. *Include dirs live here rather than travelling through
   `ProjectReference`, because MSVC does not inherit them that way* — a reference gives you the link
   and the ordering only.

3. **No absolute paths.** Everything resolves through `$(SolutionDir)` or
   `$(MSBuildProjectDirectory)`.

## Using JLib from your own project

Inside this solution, a consumer adds a `ProjectReference` and is done. From a **separate** solution
— a game in its own repo — import one property sheet:

```xml
<Import Project="C:\JLib\UseJLib.props" />
```

or Visual Studio: *View > Property Manager*, right-click the project, *Add Existing Property Sheet*.
That is the entire setup — every include directory, the library path, and all nine `.lib` names,
none of which you type or maintain. Build `JLib.slnx` once so `build\x64\<Config>\` exists, then
`#include <TaskScheduler.h>` and go.

Naming every library costs nothing in your binary: the linker discards objects from libraries you
never reference, so a project that only touches the scheduler does not pay for the renderer. That is
also why there is no merged `JLib.lib` — it would solve only the link list, leaving include
directories to configure by hand, and it would need regenerating on every build.

**Your `RuntimeLibrary` must match the JLib configuration you link** (Debug is `/MDd`; Development
and Release are `/MD`). `UseJLib.props` maps configuration names automatically and errors with a
sentence telling you what to build if the output is missing, rather than emitting forty unresolved
externals. Override the mapping with a `<JLibConfig>` property if you use non-standard names.

Verified with a from-scratch project outside the tree: one import line, four libraries plus vendored
Jolt, compiled, linked and ran.

## Adding a library to the suite

1. Copy its folder in (source + `include/`, no `x64/`, `.vs/`, or `obj/`).
2. Write a `.vcxproj` modelled on `Scheduler/Scheduler.vcxproj` — x64 only, `StaticLibrary`, import
   `JLib.Common.props`, list sources explicitly, no absolute paths.
3. Add one line to `AdditionalIncludeDirectories` in `JLib.Common.props`.
4. Add a `<Project Path="..."/>` line to `JLib.slnx`.
5. In consumers, add a `<ProjectReference>` and delete the corresponding `.lib` from
   `AdditionalDependencies`.

Gotcha: **XML comments cannot contain `--`.** MSBuild rejects the project file outright (MSB4025, or
MSB4024 for an imported props file). Worth restating because it is easy to hit twice: a row of dashes
used as a comment separator is enough to do it.

## Configurations

Three, not two:

| | optimized | symbols | assertions | use it for |
|---|---|---|---|---|
| **Debug** | no | yes | yes | stepping through a specific bug |
| **Development** | yes | yes | **yes** | **playing / profiling / everyday work** |
| **Release** | yes | yes | no | shipping |

**Development exists because full Debug is unusable at content scale.** Game01's larger levels run at
~2 FPS in Debug while Release holds hundreds — that's not a bug, it's what an unoptimized build plus
`_ITERATOR_DEBUG_LEVEL=2` costs when per-frame work means iterating thousands of tiles and bodies.
Every engine ships this third configuration (Unreal calls it Development, CMake RelWithDebInfo). You
keep breakpoints, call stacks and asserts; you lose some locals to inlining.

It is defined **once**, in `JLib.Common.props`. That is not tidiness — `_ITERATOR_DEBUG_LEVEL` and the
runtime library must match across *every* linked library, and a mismatch is an ODR violation that
surfaces as heap corruption or `0xC0000409` rather than a clean error. One shared props file makes a
mismatch structurally impossible; per-project settings make it inevitable. This is the same argument
as the stale-ABI trap, and it's the second thing the umbrella build fixes for free.

Note `Development` does **not** define `NDEBUG` (that's what keeps asserts live), so any code
branching on `NDEBUG` to mean "optimized" must also check `JLIB_DEVELOPMENT` — the scheduler's
ParallelFor threshold does exactly this.

Adding a build type to `.slnx` requires listing **every** `<BuildType>` explicitly; once the set isn't
just Debug/Release, MSBuild rejects any configuration not named.

## Status

Done and verified building + running:

- `Scheduler` (static lib, C++17, MASM context switch wired via `masm.props`/`masm.targets`)
- `Scheduler/bench` → `SchedulerBench.exe`, the proof that `ProjectReference` alone is sufficient
- `Time`, `Scene`, `Input` — leaf libraries, no suite dependencies
- `Sound` — first `ProjectReference` consumer (needs `Scheduler`; mixing is pool work)
- `PlatformerPhysics2D` — needs `Scheduler`. **C++20**, not C++17: `PhysicsSystem.cpp` uses
  `std::numbers::pi_v`. Standard level is per-project, and only `Scheduler` makes a public C++17 claim.
- `Assets`, `Geometry` — **header-only**, so they are an include directory in `JLib.Common.props` and
  deliberately have no `.vcxproj`. Not every library needs a project.

All six build in Debug, Development and Release.

- `Renderer` (from DirectX12) — 7 sources + 8 ImGui translation units + **28 shaders** compiled by
  fxc at build time. Vendors **DirectXTex** (prebuilt lib — no source tree exists locally) and
  **DirectX-Headers** (`d3dx12.h`, previously a NuGet package, which was itself an install step the
  umbrella is supposed to delete). ImGui stays under `Renderer/include/imgui` rather than moving to
  `ThirdParty/`, because that is the path its own sources and every consumer already use.
  **Its shader items carry no per-configuration conditions on purpose** — the originals were written
  for `Debug|x64`/`Release|x64` only, so under `Development` every shader would have lost its type,
  entry point and model and silently compiled to nothing.

- `Jolt` + `Physics3D` — **Jolt is vendored as SOURCE and built here** (`ThirdParty/Jolt`, 4.4MB, 153
  `.cpp`). Note what was vendored and what was not: its *prebuilt* libraries are 64MB and 83MB, which
  is past where binaries belong in a git repo, but the *source* is smaller than ImGui plus the
  DirectX headers already in this tree.
  **No CMake step.** Jolt ships a CMake build, but the library needs nothing from it — no code
  generation, no configure stage — so `Jolt.vcxproj` is a wildcard over `Jolt\**\*.cpp`. The
  wildcard is deliberate: updating Jolt is replacing the folder, with no file list to re-enumerate
  and no chance of a new source file being silently skipped.
  **Why source rather than a prebuilt lib — this is the real payoff.** `ThirdParty/Jolt/Jolt.props`
  holds the `JPH_*` define set and is imported by *both* `Jolt.vcxproj` and `Physics3D.vcxproj`.
  Those defines change Jolt's structure layouts, so compiling the wrapper against a different set
  than the library is an ODR violation — `LNK2001 GetSubmergedVolume` if you are lucky, silent
  corruption if you are not. `deploy_lib.bat` duplicated that list with nothing checking the two
  agreed. Now one file feeds both and the mismatch is not merely unlikely, it is unrepresentable.
  Same argument as `JLib.Common.props` owning `_ITERATOR_DEBUG_LEVEL`.

All nine libraries build in all three configurations, from a clone, with nothing installed.

Remaining, in dependency order (each is mechanical — same recipe as above):
- `Physics3D` — needs vendored **Jolt** with a hand-written `.vcxproj` so no CMake step is required.
  Watch the `JPH_*` define set: it must be identical between Jolt and Physics3D or you get silent ABI
  corruption rather than a link error. Keep this project **optional**, so anyone evaluating just the
  scheduler and renderer never touches it.
- `PlatformerPhysics2D`
- `Demos/` — 3DTest, Game01, Pong, Space Invaders, Tetris. "Clone, build, run something" is the
  conversion moment, so these ship with it.

Third-party to vendor into `ThirdParty/` (all permissively licensed, ship the licence alongside):
**EnTT** (MIT, single-header `entt.hpp`), **Jolt**, **DirectXTex**, **ImGui**. Precedent already
exists in the scheduler: `ThirdParty/concurrentqueue.h` + `LICENSE.md`.

## Note on the old per-library folders

This tree is now **primary**. `C:\T_Threads`, `source\repos\DirectX12`, etc. are stale copies —
develop here, and push to the individual published repos when cutting a release. Two working copies
of one library is exactly how `C:\T_Threads` silently sat two days behind and nearly shipped a
changelog describing code that wasn't in it.

**It happened again, to this tree, within a day (2026-08-06).** `Scheduler/` here was a snapshot taken
on the afternoon of 08-05, so it predated the affinity-policy work that landed that evening: no
`SetAffinityPolicy`, `Thread.cpp` still calling `SetThreadAffinityMask` unconditionally (permanent
hard pinning — the policy that was *measured as worst*), and no `JLIB_DEVELOPMENT`-aware ParallelFor
threshold. It was re-synced from `source\repos\T_Threads` and now matches file-for-file.

The lesson is not "be more careful", it's that the window stays open until every consumer builds from
this tree and the per-library working copies stop being edited. Until then, **diff before you trust a
folder here** — `diff -rq` across the two trees takes seconds and would have caught this instantly.
