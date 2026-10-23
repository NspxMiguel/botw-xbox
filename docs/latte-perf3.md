# Latte CPU round 3

## Result and measurement boundary

Implemented in the single-thread renderer on `xbox/latte-perf3`, without a commit
or deployment. The baseline is the supplied fixed plateau scene: 4,980
draws/frame, 37.2 presents/s, 23.44 ms/frame inclusive draw work and 97.1%
covered wall time. This worktree has no new Xbox capture. The <=13 ms target,
60 fps and frame-identical native rendering have **not** been demonstrated.

The changes remove CPU work while preserving draw ordering, generated index
bytes, resource transitions, upload/fence lifetimes and shader bindings. Exact
host tests cover the CPU algorithms, not the UWP driver or actual pixels.
Bindless is design only in [latte-bindless.md](latte-bindless.md).

## Slot audit and unmeasured estimates

The ranges below are planning estimates in ms/source frame at the supplied
workload, not measurements or guarantees. A regression is possible, especially
if the larger cache has little reuse or the discarded secondary descriptor
cache had unusually valuable non-adjacent hits in another scene. Do not subtract
the ranges from the baseline as an achieved result. Exclusive slots do not add
`draw-total` again.

| Slot | Baseline ms/frame | Change / decision | Potential saving |
| --- | ---: | --- | ---: |
| indices | 2.563 | Fused SSSE3 `pshufb` endian swap and quad/strip expansion, SIMD line-loop copy, unsigned max reduction, exact-width unaligned loads/stores, direct mapped-upload output. Four-way 256-entry cache replaces 64-entry direct map. AUTO pointer normalization; restart-only bound changes reuse output. | 0.20–1.20 |
| draw-state | 3.384 | Disabled streamout prepare/finish eliminated per draw, with sequence-boundary reset. Four active-shader getters consolidated. Optional sub-timings identify remaining orchestration. Base vertex/instance registers remain live reads: their PM4 packets can change inside a sequence, and caching three loads would add work. | 0.03–0.25 |
| render-state / transitions | 1.618 | Immutable views compare one resource-state version before calling the transition helper; resource hit path is inline. Preserve write content versions and every required barrier. | 0.08–0.35 |
| descriptors | 4.004 | Remove 54-word key expansion and serial hash after previous-table failure. Stable CPU SRV/sampler descriptors were already present. Persistent CBV/UAV sources, no submission-only CBV rewrite, one bulk copy per changed stage, no per-draw heap containers. | 0.20–0.90 |
| shader-pipeline | 1.163 | Value-check PS input registers before decoding; retain decoded active table on hits. Last shader-state node bypasses map lookup, still checks auxiliary state each call and clears before removal. Fetch resolution already belongs to this cache; do not add an unchecked program-pointer cache. | 0.02–0.15 |
| texture-bind | 2.632 | Existing view/swizzle SRVs and sampler caches remain. Slice-copy refresh still runs. No changed content/alias policy. | 0 |
| buffer-sync | 1.613 | Preserve queues, page/content validation and range growth. Blindly enabling incremental sync would skip flush/deallocation processing and can retain stale guest buffers. | 0 |
| uniforms | 1.565 | Preserve byte validation and post-sampler transient-upload ordering. CBV descriptor creation savings belong to descriptors. | 0 |
| pm4 | 2.548 | Round 2 already bulk-swaps/stores register words. No speculative dispatcher/dirty-tracking rewrite without device sub-timings. | 0 |
| submit | 1.801 | Keep explicit waits, query/readback/fence obligations and 2,000-draw split. 7.33 submits/frame does not establish redundant work. | 0 |
| render-targets | 1.498 | Existing attachment/FBO caches unchanged; transitions benefit in render-state. | 0 |
| draw-other / texture-load / pipeline-bind / vertex-buffers / root-signature / draw-call | 0.770 / 0.020 / 0.220 / 0.408 / 0.014 / 0.268 | No changed draw count, PSO binding, viewport/scissor semantics or native draw-call behavior. | 0 |

The favorable ranges total about **0.5–2.9 ms/frame**, substantially less than
the **10.44 ms** needed to take inclusive draw work from 23.44 to 13 ms. This
round cannot support a 60 fps claim. Further work should follow the measured
sub-timings; the broader thread split remains in
[latte-pipelining.md](latte-pipelining.md).

## Why index misses are not fixed by weakening a content hash

There is no content hash in this path: round 2 compares every byte against a CPU
shadow. That comparison is required because guest memory can change without a
usable write generation. A first/last-byte hash, frame-only validation, or
assuming no write after a wait would render stale indices.

The observed miss fraction is 1,860.5 / (3,071.8 + 1,860.5) = **37.7%**. The
aggregate counter cannot tell how much comes from conflicts, true mutations or
oversized data. The old direct map evicted on any same-bucket key. Four ways and
four times the entries preserve more alternating geometry. New counters split
content misses, full-set evictions and transient misses; empty-way fills are
the remainder. An actual before/after hit-rate improvement still needs capture.

Restart was unnecessarily part of conversion identity: it changes the bound
metadata, not the emitted bytes. The cache now stores both raw and effective
maxima and the last restart value. On a restart-only hit it updates the bound,
scanning guest indices only when the raw maximum equals restart. No upload is
needed. Width, endian, primitive mode, count, address and every input byte remain
validated. AUTO has no input address, so null and differing caller pointers now
share its generated allocation. Null AUTO arithmetic in unused scalar pointers
was removed. Restart filtering also handles a restart token in the first
position, and the existing U32 little-endian conversion now sets its host type.

## Invalidation and lifetime

- **Indices:** content mutations replace their own way; empty way before LRU.
  Explicit overlap invalidation uses the full input span. D3D12 small entries
  survive waits only with full byte revalidation. Larger/legacy entries keep
  wait retirement. Allocations are still freed through the renderer's fenced
  deferred allocator. Persistent payload limits increase from 4/6 MiB to
  16/24 MiB input/output; vector capacity, GPU allocator granularity and eight
  transient entries are additional. This is a deliberate memory/performance
  tradeoff, not an unbounded cache.
- **PS input decode:** all 33 raw words used by the decoder are compared. The
  active table has one writer; unchanged words leave it in place. Unload clears
  the memo. The shader-state shortcut stores a stable `unordered_map` node and
  is cleared before every removal and unload. Rehash preserves node pointers.
  Auxiliary hashes and their variant selection remain evaluated on every call.
- **Streamout:** any changed context register, including `VGT_STRMOUT_EN`, ends
  the PM4 draw sequence. Disabled state is reset at sequence start even after
  an aborted active draw; active prepare/finish, offsets, allocation and UAV
  ordering remain per draw. Native command-list resets do not change this
  logical sequence state.
- **Transitions:** allocation, all-state and individual-state setters advance
  the base texture's state version and clear its exact range memo. Views store
  no mutable range/key: their range is immutable. Any state mutation invalidates
  every view conservatively. Write requests always advance the content version
  before the resource shortcut; shader-read hits never suppress content writes.
- **Barriers:** keep the batch immediately before its dependent draw/copy.
  Deferring barriers to sequence end would place them after dependent commands;
  moving them all to sequence start would cross uploads, feedback and alias
  changes. Stable draws already issue no barriers. No speculative batching
  change is included.
- **Descriptors:** last table compares complete values plus GPU ring and staging
  source generations. CPU CBVs contain address/size, independent of submission;
  the upload allocator still controls data lifetime. CPU UAVs compare resource
  and size and are explicitly invalidated on buffer recreation, including COM
  address reuse. Those four source slots live until staging-heap destruction.
  SRV/sampler source descriptors retain their existing ownership. A native root
  reset still rebinds arguments even when table contents are reused.

The supplied descriptor counters show only **212.6** non-adjacent hits/frame
(4,967.8 minus 4,755.2), against **4,953.4** misses paying for hash construction.
Removing the secondary table cache may increase copies by that non-adjacent hit
count; this tradeoff must be measured in other scenes too. It does not change
shaders or the descriptor table ABI.

## Temporary profiling

`CEMU_LATTE_PROFILE_DETAIL=1` enables sequence, shader/fetch resolution, register
loads, active streamout and stage-binding preparation timers. They are inclusive
and separate from the exclusive main log; never add them to coverage. Same-slot
nesting and profile-window reset/checkpoint are tested. Default off adds no
clock reads. Full contracts and counter semantics are in
[latte-profiler.md](latte-profiler.md). No Xbox sub-step values were collected
from this worktree; implementation choices here follow code inspection and the
supplied aggregate profile.

## Files

| Files | Purpose |
| --- | --- |
| `Core/LatteIndexConversion.h`, `Core/LatteIndices.cpp` | Fused conversion, unaligned SIMD stores, four-way reuse, AUTO normalization and restart metadata. |
| `Core/LatteDrawCache.h` | Version memo for immutable views. |
| `Core/LatteShader.cpp`, `Core/LatteShader.h` | PS-input decode memo, last-node lookup and combined active-shader snapshot. |
| `Core/Latte.h`, `Core/LatteStreamoutGPU.cpp` | Explicit inactive streamout reset at sequence boundary. |
| `Core/LatteCommandProcessor.cpp`, `Core/LatteProfiler.h`, `Core/LattePerformanceMonitor.cpp` | Temporary sub-timings, safe window accounting and miss-reason reporting. |
| `D3D12/D3D12Renderer.h`, `D3D12/D3D12Renderer.cpp`, `D3D12/D3D12RendererCore.cpp` | Inline transition fast path, stage table miss path and CPU descriptors; draw setup changes. |
| `D3D12/D3D12StageTable.h` | Production descriptor-table value definition shared with host tests. |
| `D3D12/LatteTextureD3D12.h`, `D3D12/LatteTextureD3D12.cpp`, `D3D12/LatteTextureViewD3D12.h` | Resource state versions and immutable view memo. |
| `tools/tests/latte_indices_test.cpp`, `latte_index_cache_associativity_test.cpp`, `latte_index_conversion_test.cpp`, `latte_index_expansion_test.cpp` | Production cache/conversion integration, scalar parity, winding, tails, guards and LRU. |
| `tools/tests/latte_draw_cache_test.cpp`, `latte_stage_table_test.cpp`, `latte_profiler_test.cpp`, test stubs and `run_latte_perf3_tests.sh` | Memo/epoch/equality/reset/lifetime coverage and repeatable sanitized runner. |
| `docs/latte-profiler.md`, `docs/latte-perf3.md`, `docs/latte-bindless.md` | Runtime contracts, this report, and bindless feasibility. |

`Core/` means `runtime/cemu/Cafe/HW/Latte/Core/`; `D3D12/` means
`runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/`.

## Validation

The repeatable host runner is `tools/tests/run_latte_perf3_tests.sh`; its captured
output is `build/perf3-host-tests.log`. All native and x86 AVX2 host checks pass
with AddressSanitizer and fatal UndefinedBehaviorSanitizer. The x86 checks run
under Rosetta on this Mac. They compile the production `LatteIndices.cpp`, not
a copied cache implementation. Stage-table tests use the production value type.

The required full UWP build and link passed with
`nice ninja -C build/local-uwp-game -j4` (exit 0), producing
`build/local-uwp-game/BotWRecomp.exe`. Build output is in
`build/perf3-build.log`. A second invocation passed with **no work to do**;
see `build/perf3-build-verified.log`. `git diff --check` also passed. Existing
warnings include clang-cl's unused `/await:strict` argument and unrelated sound
variables; no compilation or link errors remain.

The copied build and generated corpus are local to this worktree, with absolute
paths rewritten. Copied dependency records were replayed into a canonical node
table to handle restarted IDs. CMake regenerated its local metadata; the full
source corpus was rebuilt rather than relying on relocated command hashes.
No metadata in `base` is modified. Each Ninja invocation started only after
`pgrep -x ninja` showed no other Ninja build. All changes remain uncommitted.

Device validation still required: fixed plateau screenshot parity and the same
draw density; primary and detail profile windows; index hit/miss reasons;
streamout/query/readback equivalence; overlapping views/mips/planes and slice
copies; descriptor ring rollover, source recycling and resource recreation.
