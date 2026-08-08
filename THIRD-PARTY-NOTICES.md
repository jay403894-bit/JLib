# Third-party notices

JLib itself is **BSD 3-Clause** (see `LICENSE`). It vendors the components below, each under its own
permissive licence. Every one of them is either MIT, BSD, Boost, or public domain, so a consumer who
ships a binary built from this tree owes attribution and nothing else — no copyleft obligations, no
source-disclosure requirement, no per-seat terms.

Each entry names where the component lives in this tree and where its full licence text sits. Nothing
here is a substitute for those files: the licences require the text itself to travel with the code,
which is why it is vendored alongside rather than only summarised here.

| Component | Licence | Location in tree | Licence text |
|---|---|---|---|
| **moodycamel::ConcurrentQueue** | Simplified BSD **or** Boost Software 1.0 (dual) | `Scheduler/include/concurrentqueue.h`, `blockingconcurrentqueue.h`, `lightweightsemaphore.h` | `Scheduler/include/LICENSE.md` |
| **Dear ImGui** | MIT — © 2014-2025 Omar Cornut | `Renderer/include/imgui/` | `Renderer/include/imgui/LICENSE.txt` |
| **cgltf** | MIT — © 2018-2021 Johannes Kuhlmann | `Renderer/include/cgltf.h`, `cgltf_write.h` | `Renderer/include/LICENSE` |
| **tinyobjloader** | MIT | `Renderer/include/tiny_obj_loader.h` | header preamble |
| **miniaudio** | Public domain **or** MIT-0 (dual, your choice) | `Sound/include/miniaudio.h` | header preamble |
| **Jolt Physics** | MIT — © 2021 Jorrit Rouwe | `ThirdParty/Jolt/Jolt/` (source, built by `Jolt.vcxproj`) | `ThirdParty/Jolt/LICENSE` |
| **DirectXTex** | MIT — © Microsoft | `ThirdParty/DirectXTex/` | header preamble |
| **DirectX-Headers** | MIT — © Microsoft | `ThirdParty/DirectX-Headers/include/` (33 files) | per-header preamble |

## Notes

**`blockingconcurrentqueue.h` embeds a zlib-licensed component** — Jeff Preshing's semaphore. It is
called out in moodycamel's own `LICENSE.md` rather than being separately vendored. zlib is permissive
and attribution-only, so it changes nothing practically, but it is a distinct licence and is listed
here so nobody has to re-derive that from a comment buried in the header.

**The DirectX-Headers set is more than `d3dx12.h`.** Microsoft split that helper into
`d3dx12_core.h`, `_barriers.h`, `_root_signature.h` and the rest, and the same repo also ships
`d3d12.h`, `dxgiformat.h`, `dxcore.h` and their `.idl`s — all under MIT, which is why the whole
folder can be redistributed. **Two files were deliberately NOT vendored**: `DirectML.h` and
`D3D12MarkerApiEnums.idl` were the only two in the source folder carrying no MIT grant, and neither
is referenced by any JLib source. Verified mechanically rather than by eye — every remaining file in
that directory contains an explicit "Licensed under the MIT" line. **Re-run that check after any
refresh from upstream**, because a single non-MIT file arriving silently would make the row above
false.

**DirectXTex ships as a prebuilt `.lib`, not source.** MIT permits binary redistribution with the
notice, which this file plus the header preamble satisfies. Replacing it with a vendored source build
is a wanted cleanup — it would remove the only binary blob in the tree and let it participate in the
`Development` configuration properly instead of borrowing the release CRT build.

**Jolt's LICENSE is now vendored** at `ThirdParty/Jolt/LICENSE` (2026-08-08), copied from the
upstream release rather than reconstructed — MIT is boilerplate but the copyright line and year are
not, and an invented licence file is worse than a missing one. This closes the gap that previously
rested on the per-file `SPDX-License-Identifier: MIT` declarations alone. Those are a valid licence
declaration, so the old position was defensible; it was simply weaker than shipping the text, and
"the SPDX tag is in a source file we did not ship you" is not an argument worth having.
**Re-copy it on any Jolt update** — the copyright year moves.

**BINARY INSTALLS SHIP THE LICENCE TEXTS TOO.** `CMakeLists.txt` installs the files named in the
table above into `share/licenses/`, not just this summary. That matters because a `make install`
consumer never sees the source tree, so "the text is vendored alongside the code" — the principle
stated at the top of this file — would otherwise hold for a git clone and quietly fail for exactly
the distribution path where attribution obligations actually bite. Components whose licence lives in
a header preamble (tinyobjloader, miniaudio, DirectXTex, DirectX-Headers) are covered automatically,
because those headers are themselves installed.

**EnTT (MIT) is not vendored yet.** Add a row here at the same time as the code, not afterwards — a
notices file that lags the tree is worse than none, because it reads as a complete list.

## Why this file exists

Attribution is the entire price of every dependency in this tree, and it is a price that is trivial
to pay and embarrassing to be caught not paying. An adopter evaluating JLib for commercial use will
look for exactly this file, and its absence reads as "nobody has checked", which is a stronger signal
against adoption than any missing feature.
