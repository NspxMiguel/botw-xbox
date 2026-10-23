# Latte command thread profiling

`CEMU_LATTE_PROFILE=0` disables profiling. BotWRecomp builds enable it by default;
other builds can enable it with `CEMU_LATTE_PROFILE=1`. The setting is read once
when the command processor starts. No profiler or console attachment is needed:
results go to `runtime.log`.

The D3D12 present diagnostics and the profiler share a complete window of at least
10 seconds, checked at the existing 2-second display refresh checks. The profiler
line follows the existing `D3D12: ... presents/s ...` line. The first main-window
present starts the window and discards startup work. Each subsequent main-window
present counts one source game frame; interpolated and pad presents do not add
frames to the divisor. The existing D3D12 line still divides present/fence times
by actual presents.

The exact profiler format (fmt placeholders) is:

```text
Latte profile: {:.2f}s, {} game frames, {:.1f} draws/frame; ms/frame: draw-total {:.3f} (inclusive), draw-other {:.3f}, shader-pipeline {:.3f}, texture-bind {:.3f}, texture-load {:.3f}, buffer-sync {:.3f}, indices {:.3f}, uniforms {:.3f}, descriptors {:.3f}, render-targets {:.3f}, submit {:.3f}, pm4 {:.3f}, draw-state {:.3f}, pipeline-bind {:.3f}, vertex-buffers {:.3f}, root-signature {:.3f}, render-state {:.3f}, draw-call {:.3f}; top: {} {:.3f}, {} {:.3f}, {} {:.3f}; covered {:.1f}%, unaccounted {:.1f}% ({:.3f} ms/frame); pipeline-miss/frame {:.2f}, texture-new/frame {:.2f}, texture-upload {:.0f} B/frame, buffer-upload {:.0f} B/frame; descriptor-hit/frame {:.1f}, descriptor-fast/frame {:.1f}, descriptor-miss/frame {:.1f}; root-cbv-set/frame {:.1f}, root-cbv-skip/frame {:.1f}; dynamic-state-hit/frame {:.1f}, dynamic-state-miss/frame {:.1f}; transition-hit/frame {:.1f}, transition-miss/frame {:.1f}; index-hit/frame {:.1f}, index-miss/frame {:.1f}; fbo-hit/frame {:.1f}, fbo-miss/frame {:.1f}, pipeline-state-hit/frame {:.1f}; submit/frame {:.2f}, submit-empty/frame {:.2f}
```

The slot names are fixed; the six draw breakdown fields extend the original
format. All slot values and the sorted top three use
milliseconds per source game frame. They measure elapsed time on the command
thread, including preemption and any blocking inside that stage, rather than
thread CPU cycles.

| Field | Boundary / meaning |
| --- | --- |
| `draw-total` | Inclusive union of all three PM4 draw handlers and shared `beginDrawPass` / `endDrawPass` setup and teardown. Counted once across nested stages. |
| `draw-other` | Draw handler/setup/teardown time outside the more specific slots. Its former remainder is now split into the six slots below. |
| `draw-state` | PM4 draw packet decoding, D3D12 sequence setup and draw orchestration outside more specific slots, including streamout preparation/finish. |
| `pipeline-bind` | PSO and primitive topology binding when changed, separate from pipeline lookup. Unchanged-state checks remain in `draw-state`. |
| `vertex-buffers` | Vertex buffer dirty-span setup and index buffer comparison/binding. Index conversion remains in `indices`. |
| `root-signature` | Root-signature/heap binding, including bindings during command list reset. The unchanged-signature check remains in `draw-state`. |
| `render-state` | Input/resource transitions and barriers plus viewport, scissor, blend factor and stencil ref comparison/binding. Nested render-target work remains in `render-targets`. |
| `draw-call` | The actual `DrawInstanced` / `DrawIndexedInstanced` call. |
| `shader-pipeline` | `LatteSHRC_UpdateActiveShaders` and `D3D12PipelineCache::GetOrCreate`, including hash, lookup and synchronous pipeline work/waits. Background compilation is excluded. |
| `texture-bind` | `LatteTexture_updateTextures` (lookups, change detection, view creation) and the per-stage texture/SRV/sampler-description block in `draw_buildStageBindings`. |
| `texture-load` | `LatteTexture_ReloadData`, `LatteTextureLoader_UpdateTextureSliceData` (decode/untile), and D3D12 `texture_loadSlice`. |
| `buffer-sync` | `LatteBufferCache_Sync`, range reservation/retrieval, invalidations/DC flush processing, and D3D12 `bufferCache_upload`. |
| `indices` | `LatteIndices_decode`, including lookup/conversion/upload. |
| `uniforms` | Uniform variable remapping/preparation, final byte comparison and upload/reuse. Transient uploads happen after sampler acquisition so a sampler heap reset cannot retire their allocation before the draw. |
| `descriptors` | Remaining `draw_buildStageBindings`, `draw_acquireSamplerTables`, table cache lookup/writes in `WriteStageTable`, and draw root descriptor tables/runtime constants. |
| `render-targets` | `LatteMRT::UpdateCurrentFBO`, `ApplyCurrentState`, and `draw_applyRenderTargets`. |
| `submit` | `SubmitCommandList`, including fence waits when requested. |
| `pm4` | Remainder of the ringbuffer command loop, including register parsing between fast draws. |

Slots are **exclusive**: nested time is charged only to the innermost slot.
`draw-total` is an additional inclusive value and must not be added to them.
`covered` is the sum of exclusive slots divided by the elapsed wall-clock window;
`unaccounted` is its complement. Command starvation, guest synchronization waits,
and presentation are excluded from PM4, but useful nested stages (for example a
submit during idle) still count. Work without a separate stage, such as command
loop housekeeping, remains in PM4. These are elapsed-time measurements, so covered
is not an OS CPU utilization reading.

Use `draw-total` for before/after draw-path comparisons at similar draws/frame.
The smaller `draw-other` value partly reflects the six new slots and the movement
of root table/runtime-constant binding into `descriptors`; reclassification alone
is not a performance improvement.

`draws/frame` counts attempted renderer draws, including draws skipped by the
backend. `pipeline-miss/frame` counts unsuccessful in-memory pipeline lookups,
including retries while shaders are pending; it is not a count of compilations.
`texture-new/frame` counts guest texture cache creations through
`LatteTexture_CreateTexture`, including render targets. Upload bytes count actual
D3D12 copy payload: texture rows/planes without alignment padding, and buffer
cache uploads. GPU-to-GPU copies, uniform constants, and index uploads are not
included in those byte counters.

The collector uses fixed thread-local arrays, a steady clock, and scoped stack
objects. Only `LatteCP_ProcessRingbuffer` enables its thread's state. Other
threads cannot change its counters. Same-slot nested scopes reuse the enclosing
timer. The hot path has no locks, allocation, logging, or environment lookups;
disabling profiling skips the clock reads. Reporting checkpoints active scopes
before resetting counters, so scopes that cross a window boundary remain valid.

Host regression checks for exclusive profiler accounting and descriptor lifetime
bookkeeping (using a minimal device stand-in, without a GPU):

```sh
mkdir -p build/tests
clang++ -std=c++20 -Wall -Wextra -Werror -Iruntime/cemu tools/tests/latte_profiler_test.cpp -o build/tests/latte_profiler_test
build/tests/latte_profiler_test
clang++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-private-field -DENABLE_D3D12 -Itools/tests/d3d12_stubs -Iruntime/cemu tools/tests/latte_descriptor_heap_test.cpp runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.cpp -o build/tests/latte_descriptor_heap_test
build/tests/latte_descriptor_heap_test
```

## Round-2 cache counters

All new counters use the same source-frame divisor and report the average over
one profile window, not a separate per-present log. Compute hit rate as
`hit / (hit + miss)` from these per-frame values (when the denominator is nonzero).

| Counter | Meaning |
| --- | --- |
| `descriptor-hit/frame` | Reused CBV/SRV/UAV stage tables, including internal passes. |
| `descriptor-fast/frame` | Subset of descriptor hits resolved by the per-stage value comparison before building/hashing the full key. |
| `descriptor-miss/frame` | Newly allocated/written stage tables; each takes one bulk `CopyDescriptors` call, plus a UAV creation when needed. |
| `root-cbv-set/frame`, `root-cbv-skip/frame` | Native graphics root-CBV writes versus equal-address skips, including zero bindings on signature/heap/list reset and internal passes. |
| `dynamic-state-hit/frame`, `dynamic-state-miss/frame` | Viewport/scissor translation checks against raw register words and actual/effective target dimensions. These count helper calls, not native D3D12 binds. |
| `transition-hit/frame`, `transition-miss/frame` | Texture range/state requests answered by the last-range shortcut or sent through subresource inspection, respectively. Misses can still produce zero barriers. Write content versions always advance, even on hits. |
| `index-hit/frame`, `index-miss/frame` | Reused converted index allocations versus conversions/uploads. Implicit draws needing no generated indices are excluded from both. |
| `fbo-hit/frame`, `fbo-miss/frame` | D3D12 attachment-set reuse before FBO hash/list lookup. Native binding comparison still runs. |
| `pipeline-state-hit/frame` | PSO lookup bypassed because all relevant register words, shader/fetch identities, format hash and index type matched the preceding successful lookup. |
| `submit/frame` | Actual submitted command lists, including explicit synchronization requests. |
| `submit-empty/frame` | Subset with no recorded work flag and no pending barriers, checked after query fragments are closed. Diagnostic only: it is not permission to drop a submit carrying an ordering/fence obligation. |

The descriptor fast path is invalidated by both view-ring and staging heap
generations. It survives native root-signature invalidation because immutable
descriptor contents remain usable within their allocation epoch; root arguments
are still rebound by the existing native binding state. Pipeline lookup state is
cleared by `D3D12PipelineCache::Clear`.

D3D12 converted indices now use 64 four-way sets (256 persistent entries).
The conversion identity is `(address, count, type, primitive mode)` plus all
input bytes, with AUTO addresses canonicalized to zero. Restart is cached bound
metadata: changing it reuses the converted allocation and recalculates the
maximum when necessary. Input shadows are never replaced by a partial hash.
Persistent inputs are at most 64 KiB and output at most 96 KiB per entry
(16 MiB input and 24 MiB output payload at capacity, excluding vector spare
capacity and GPU allocator granularity). Eight transient LRU entries retain the
wait boundary for larger inputs and other backends. Explicit overlap invalidation
releases intersecting guest ranges; AUTO has no input range. Renderer destruction
releases allocations through the owning renderer even after the global pointer
is reset. Content changes replace their own way; otherwise prefer an empty way,
then the least recently used one.

Host checks for the new helpers and the production index-cache/conversion code
with a minimal renderer stand-in (no GPU):

```sh
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Iruntime/cemu tools/tests/latte_draw_cache_test.cpp -o build/tests/latte_draw_cache_test
build/tests/latte_draw_cache_test
clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -fsanitize=address,undefined -Itools/tests/latte_stubs -Iruntime/cemu tools/tests/latte_indices_test.cpp runtime/cemu/Cafe/HW/Latte/Core/LatteIndices.cpp -o build/tests/latte_indices_test
build/tests/latte_indices_test
```

The index integration test compiles the real `LatteIndices.cpp`. It exercises
cross-wait reuse, full-content mutation, primitive restart, partial overlap,
bucket collisions, oversized input/output retirement, auto indices, explicit
renderer cleanup and the legacy wait boundary. Sanitizers check CPU bounds and
allocation cleanup; native GPU fence behavior still requires device validation.
See [latte-perf2.md](latte-perf2.md) for the change report and unmeasured savings
estimates, and [latte-pipelining.md](latte-pipelining.md) for the thread-split design.

## Round-3 diagnostics and validation

The main exclusive-slot log format is unchanged. A separate line reports:

```text
Latte index misses/frame: content {:.1f}, set-eviction {:.1f}, transient {:.1f}
```

`content` means a complete key was present but its full byte comparison failed.
`set-eviction` means a new persistent key replaced a full four-way set.
`transient` counts conversions outside the persistent size limits. These are
exclusive miss reasons; the remainder of `index-miss/frame` consists of fills
into empty/invalidated ways. They do not by themselves prove which reason caused
the historical 37.7% miss rate. Restart-only changes now count as index hits.

Set `CEMU_LATTE_PROFILE_DETAIL=1` before command-thread startup for temporary
sub-step diagnostics, reported on the same source-frame window:

```text
Latte detail (inclusive ms/frame): sequence {:.3f}, shader-resolve {:.3f}, packet-decode {:.3f}, streamout {:.3f}, bindings-prepare {:.3f}
```

These intervals are **inclusive**, separate from coverage and the exclusive
slots. `sequence` includes sequence setup/present servicing and nests
`shader-resolve`; shader resolution is already charged to `shader-pipeline` in
the exclusive log. `packet-decode` covers the three per-draw base/instance
register loads. `streamout` covers active prepare/finish calls;
`bindings-prepare` covers stage binding preparation and sampler acquisition,
including nested texture/descriptor work. Do not sum these with the main slots
or subtract them directly from `draw-state`. They distinguish setup, decoding,
shader/fetch resolution and orchestration in the next device capture. Default
off means no additional clock reads. Same-detail nesting counts once, and
window checkpoint/reset preserves active diagnostic scopes.

Texture views now remember the base resource's state version after a completed
shader-read transition. Any subresource/all-state/allocation mutation advances
that version; hits compare one version in the view. The resource-level exact
range shortcut remains for other requests. Write-content versions advance even
on transition hits, and barriers remain before their dependent draw/copy.

Stage table misses go directly from the previous-value check to the linear ring,
without a secondary hash cache. SRV/sampler sources remain persistent; CBVs are
rewritten only on address/size change, UAV sources on resource/size change or
buffer recreation. Each changed stage uses one bulk `CopyDescriptors`. Table
reuse still checks view-ring and staging-source generations. Internal native
state invalidation still forces root argument rebinding.

Run all round-3 host checks with:

```sh
tools/tests/run_latte_perf3_tests.sh
```

The runner compiles production conversion/cache code, uses ASan/UBSan (fatal
UBSan), and checks native and x86 AVX2 implementations on this Mac. It includes
exact scalar-output parity over widths, primitive modes, tails and unaligned
spans; content mutation, restart-only bound changes, four-way LRU, waits,
canonical AUTO keys, overlap/release; real stage-table equality/epochs; view
versions; profiler detail nesting/window resets; descriptor heap retirement.
See [latte-perf3.md](latte-perf3.md) for estimates and device-validation limits,
and [latte-bindless.md](latte-bindless.md) for the design-only SM 6.6 assessment.
