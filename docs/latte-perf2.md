# Latte CPU round 2 change report

## Result and limits

This work adds value-checked shortcuts to the existing single-thread D3D12 path.
The two-thread proposal is design only in [latte-pipelining.md](latte-pipelining.md).
No deployment or commit is part of this task.

The supplied Series X profile is the baseline: 18.532 ms/frame inclusive draw
work, 3,574.1 draws/frame, about 96% covered wall time. There is no new Xbox capture
from this worktree, so neither a speedup nor the 13 ms target has been established.
Host unit tests cannot prove frame-identical native rendering. Compare the same
plateau checkpoint on Xbox, with the same profile window and draw density, before
accepting a performance claim.

## Slot audit and estimated savings

These are deliberately approximate **unmeasured planning ranges**, in ms per
source frame at the supplied workload. Zero saving or a regression is possible
if adjacent inputs rarely match. They are based on the amount of work removed,
not extrapolated from the Mac's CPU speed. They must not be subtracted from the
baseline as if they were measurements. The slot values remain exclusive;
draw-total is inclusive and does not get added again.

| Slot (baseline ms/frame) | Finding and implemented change | Potential saving |
| --- | --- | --- |
| descriptors (3.154) | Round 1 already uses a fixed 128-entry direct map, not per-draw `vector`/`unordered_map` allocations. It still expands 54 64-bit key words and serially hashes them for every stage. A per-stage complete-value comparison now returns before key building/hash on adjacent repeats. The previous cache and one bulk descriptor copy on misses remain. New hit/fast-hit/miss counters distinguish both levels and actual table writes. | 0.15–0.50 |
| draw-state (2.519) | Much of this is orchestration, not repeated PSO translation. Cache viewport/scissor translation by raw words plus all target scale dimensions. The final native binding comparisons were already present. Sequence setup and streamout semantics remain. | 0.02–0.15 |
| render-state (2.204) | Each texture transition used to inspect all relevant mip/slice/plane states even after an identical request. A per-texture exact range/state shortcut now bypasses that scan. Every subresource state mutation clears the shortcut. Write content-version advancement stays before the shortcut. | 0.10–0.50 |
| pm4 (2.178) | Dispatch is already a switch; no function-pointer table to remove. Bulk non-shadowed register stores now expose raw endian swaps and a word-wide XOR/OR change reduction to the vectorizer. Scalar one-word writes, shadow-memory stores, callbacks and special-range behavior are preserved. The compiler may already optimize much of the original loop. | 0.00–0.20 |
| texture-bind (2.028) | Content validation already hashes at most once per texture/frame. Keep that policy and forced invalidations; bypass its out-of-line call only when its exact early-out predicate applies. Keep aliases, usage and feedback checks. Cache rescale arithmetic by dimension/override values. Sampler cache hits now avoid initializing a descriptor that would immediately be overwritten; unused sampler defaults use a prebuilt array copy. `GetSRV` still runs, including slice-copy refresh. | 0.02–0.15 |
| indices (1.387) | Original cache had eight LRU entries and discarded every allocation on waits. D3D12 now retains 64 bounded direct-map slots, checks all input bytes on every reuse, and keys primitive restart as well as address/count/type/mode. Eight larger transient entries retain the wait boundary. Auto draws requiring no converted indices return before cache lookup. | 0.10–0.60 |
| submit (1.489) | Submissions carry query/readback/fence/upload-retirement obligations. No safe general suppression was established from the capture; preserve the 2,000-draw threshold and explicit submit sites. Add actual/empty submit counters so the next capture can identify whether redundant calls are material. No added waits or changed split policy. | 0.00 |
| render-targets (1.245) | Native `OMSetRenderTargets` was already conditional. Add complete attachment-set comparison before FBO hash/list lookup. Still run guest texture/alias updates and binding comparison; transitions benefit from the separate render-state shortcut. Scene-depth usage tracking remains per draw. | 0.02–0.10 |
| shader-pipeline (0.953) | Blend/depth/raster translation already happens only on pipeline misses. An exact snapshot of every existing pipeline-hash input now bypasses repeated hashing/map lookup; shader identities are unique IDs, not recyclable pointers. | 0.00–0.08 |

The ranges total roughly 0.4–2.3 ms/frame under favorable reuse. This work does
not justify expecting a 5.5 ms saving or claiming 60 fps. If profiling confirms
that the remaining work is distributed across thousands of required draws,
the front/back split is the larger next experiment.

## Invalidation and correctness

- **Descriptors:** compare all CBV addresses/sizes, SRV handles and UAV resource/size;
  invalidate by both shader-visible view heap and staging source generations.
  Root-signature/internal-pass invalidation still forces root rebinding. The new
  shortcut holds values only for the existing submission/allocation lifetime.
- **Viewport/scissor:** compare all raw inputs and real/effective dimensions;
  invalidate on renderer construction, render-target unload and FBO deletion.
  The desired native viewport/scissor remains in `DrawState` across command-list
  resets, which still mark both bindings dirty. A first D3D12 scissor translation
  explicitly reaches the backend even when legacy global scissor values match.
- **Pipeline:** compare all eight blend controls and full stencil/raster words,
  shader IDs, fetch keys, format hash and index type; clear on pipeline-cache
  clear. Pending-shader misses are not inserted into the shortcut. The existing
  async pipeline behavior is preserved.
- **Texture states:** remember a completed range/state request only after the
  normal transition walk; all state setters and native allocation initialization
  clear it. Disjoint writes may over-invalidate, never under-invalidate. Content
  versions still advance for each write request, keeping copied slice SRVs fresh.
- **Texture contents/scale:** retain forced checks and dynamic alias reloads.
  A scale entry stores only computed numbers, never a texture pointer, and is
  compared against current dimensions and overrides before publishing each bind.
- **Indices:** full CPU shadow comparison rather than a partial hash, complete
  conversion key, overlap invalidation for writes starting before an entry,
  explicit owning-renderer cleanup, and bounded persistent allocation sizes.
  Evictions still use the D3D12 allocator's submission-fenced deferred free.
  Legacy backends keep the existing wait boundary. Eight transient slots may
  retain larger CPU vector capacities; the 4/6 MiB bounds apply to the 64
  persistent slots' input/output payload, not allocator chunks or transient data.

## Counters

The profiler line adds these source-frame averages:

```text
descriptor-hit/frame, descriptor-fast/frame, descriptor-miss/frame
dynamic-state-hit/frame, dynamic-state-miss/frame
transition-hit/frame, transition-miss/frame
index-hit/frame, index-miss/frame
fbo-hit/frame, fbo-miss/frame
pipeline-state-hit/frame
submit/frame, submit-empty/frame
```

`descriptor-fast` is a subset of `descriptor-hit`. Calculate table hit rate as
`descriptor-hit / (descriptor-hit + descriptor-miss)` and the fast share as
`descriptor-fast / descriptor-hit`. The matching hit/miss pairs use the same
rate formula. An index-free implicit draw increments neither index counter.
A transition miss need not issue a barrier. `submit-empty` is diagnostic and
cannot establish whether a fence-ordering obligation can be removed. Definitions
and exact log format are in [latte-profiler.md](latte-profiler.md).

## Files

| Files (relative to repository) | Purpose |
| --- | --- |
| `runtime/cemu/Cafe/HW/Latte/Core/LatteDrawCache.h` | Shared exact-value cache, complete content snapshot, index key/bucket selection, overlap check and bulk register store helper. |
| `runtime/cemu/Cafe/HW/Latte/Core/LatteCommandProcessor.cpp` | Bulk register paths and D3D12-aware index wait invalidation. |
| `runtime/cemu/Cafe/HW/Latte/Core/LatteIndices.cpp`, `LatteIndices.h` | Retained index cache, complete keys/content checks, lifecycle and overlap handling. |
| `runtime/cemu/Cafe/HW/Latte/Core/Latte.h`, `LatteRenderTarget.cpp` | Viewport/scissor and FBO lookup value caches plus renderer lifecycle invalidation. |
| `runtime/cemu/Cafe/HW/Latte/Core/LatteTextureLegacy.cpp` | Cheap equivalent content-check predicate and rescale reuse. |
| `runtime/cemu/Cafe/HW/Latte/Core/LatteProfiler.h`, `LattePerformanceMonitor.cpp` | New counters and log fields; existing timing boundaries stay intact. |
| `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h`, `D3D12RendererCore.cpp` | Last stage-table cache and reduced sampler setup work. |
| `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.cpp` | Transition shortcut, index/cache lifecycle hooks and submit counters. |
| `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/LatteTextureD3D12.h`, `LatteTextureD3D12.cpp` | Exact transition memo and invalidation on all state mutations. |
| `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12PipelineCache.h`, `D3D12PipelineCache.cpp` | Value-checked shortcut around pipeline hashing/lookup. |
| `tools/tests/latte_draw_cache_test.cpp` | Exact keys/epochs, middle-byte changes, overlap and PM4 endian/change tests with ASan/UBSan. |
| `tools/tests/latte_indices_test.cpp`, `tools/tests/latte_stubs/` | Integration tests compiling production index code against a CPU-only renderer stand-in and six forwarding headers. |
| `tools/tests/latte_profiler_test.cpp` | Verify all new counters reset and disabled collection stays inert. |
| `docs/latte-profiler.md`, `docs/latte-perf2.md`, `docs/latte-pipelining.md` | Counter contract, this change report, and thread-split design. |

## Validation

Completed on this worktree:

- Full UWP build and link with `nice ninja -C build/local-uwp-game -j4` passed.
  Logs: `build/perf2-build.log`, `build/perf2-build-final.log`, and the final
  consistency build in `build/perf2-build-verified.log`.
- `latte_draw_cache_test` passed with AddressSanitizer and UndefinedBehaviorSanitizer.
- `latte_indices_test` passed with both sanitizers, compiling production index code.
- `latte_profiler_test` and `latte_descriptor_heap_test` passed.
- `git diff --check` passed; the published profiler format matches its C++ format.

The copied Ninja dependency log contained restarted node IDs and triggered a
redundant rebuild. Its records were recovered into a clean dependency log inside
this worktree; the final consistency build rebuilt the one remaining stale guest
object and linked successfully. No metadata in `base` was modified.


Run the host commands in `latte-profiler.md`: new cache helpers, production index
integration, profiler accounting/reset, and existing descriptor heap lifetime
checks. Sanitized cache/index checks cover complete content mutation, restart,
bucket collision, wait retention, oversized input/output release and shutdown.
These CPU tests do not substitute for native GPU validation.

The required build setup copies `base/build/local-uwp-game` into this worktree,
rewrites absolute base source/build paths in the copied CMake/Ninja metadata, and
copies the ignored generated guest corpus into this worktree so compilation does
not consume source files from the base checkout. Build with:

```sh
pgrep -x ninja # must show no other ninja build before starting
nice ninja -C build/local-uwp-game -j4
```

The generated sources, executables, object files and logs stay under ignored
worktree directories. No source changes or build processes belong in `base`.
Device validation remains: match plateau screenshots/checkpoints and query or
readback results, capture the new counters, then compare every exclusive slot
and inclusive draw-total at comparable draws/frame. Test heap rollover, copied
array-slice views, post-wait index writes, streamout and forced readbacks.
