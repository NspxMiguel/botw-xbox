# Latte pipeline, phase 7

## Design before implementation

Mode 3 adds a recording coordinator and up to four persistent chunk workers.
Modes 0 and 2 retain their existing recorder. The coordinator owns resource-state
tracking, resource lifetime pins, queue submission, and the fence. Workers never
read guest memory, mutable register arrays, the resource registry, or the front's
binding caches. The implementation additionally shares a pinned native view's SRV
cache under its own mutex; its native definition cannot change during dispatch. Each worker owns its command lists and allocators,
shader-visible descriptor rings, staging descriptors, sampler storage, and upload
ring. A job receives immutable, owned draw inputs and an ordered barrier plan.

A batch is a bounded contiguous run of rebuildable draws. Each chunk starts with
complete root signature, heaps, pipeline, vertex/index bindings, render targets,
viewport, scissor, blend/stencil, descriptors, and runtime constants. Resource
transitions are planned in draw order and emitted at their original draw, including
the first draw of a chunk. The coordinator's preceding list and the chunk lists
are passed in order to one ExecuteCommandLists call and share a fence. Allocators,
descriptors, and upload bytes cannot be reused until that fence completes.

Readbacks, copies, uploads, resource mutation/release, output passes, and queries
are dependency boundaries. Active occlusion queries, streamout, slice-copy SRVs,
and unaligned buffer-cache uniform copies require the serial recorder until their
command-producing helpers have immutable equivalents. These are explicit fallback
reasons, not guesses about resource independence. A boundary may use one chunk;
small runs must not pay for four jobs. Safe state-only operations may pass through
without forcing a submission because each draw contains its own bindings.

Guest reads must be captured at the PM4 ordering point. The front's index cache
also computes the maximum vertex index needed by LatteBufferCache_Sync; buffer
cache range discovery, overlap handling, streamout, and flush processing share
mutable guest/cache state. Generated guest stores bypass the existing flush hooks.
Moving these reads wholesale to delayed workers would introduce races and stale
rendering. Index lookup/validation and buffer-cache discovery therefore need a
separate immutable capture/compute interface before they can leave the front.
Shader/FBO/texture discovery likewise mutates shared caches. Worker-side native
binding resolution, uniform upload packing, descriptor creation/copy, and D3D12
recording can operate on captured inputs without those reads.

Non-result operations may be asynchronous only after their payload owns every
input. Query flush, backbuffer acquisition, creation inspected by the front, and
logical destruction of front-owned adapters remain synchronous. Presentation and
other output operations are audited individually; a missing CPU return value is
not enough if an adapter is stack-owned.

## Validation plan

Use the production chunk scheduler in host ASan/UBSan and TSan tests. Compare a
captured immutable draw stream with the mode-2 abstraction (per-draw arguments,
complete state and payload bytes), independent of redundant native bind elision.
Exercise uneven and empty chunks, state changes at boundaries, original barrier
order, dependency drains, failure propagation, repeated jobs, and shutdown. Run
K=1/2/3/4 optimized benchmarks separately from sanitizers. Host scaling describes
CPU recording work, never Xbox FPS or driver correctness. Console diagnostics
must expose chunk counts, serial fallbacks, each worker's busy time, and recording
critical-path elapsed time. Native debug-layer and scene parity still need Xbox.

This design was committed as `eb056aa` before implementation. The final behavior
and measured limitations follow.


## Implemented recording path

`LocalState/latte-pipeline.txt` containing `3` selects the new mode. `2` keeps the
single back recorder, and mode 0 keeps the direct path. The host environment
switch is `CEMU_LATTE_PIPELINE=3`. The renderer creates chunk resources lazily,
only after a rebuildable draw run needs recording.

The coordinator copies the transport block and every live uniform/runtime span
before acknowledging the operation. The front can subsequently reuse the block
pool or wrap the arena. Resource IDs are resolved and pinned on the coordinator;
workers do not concurrently mutate registry lifetime fields. Pipeline objects,
attachments, vertex ranges, index views, render state, and each draw's barrier
vector are resolved into an owned record. Texture inputs retain pinned native
views plus the captured swizzle. Workers resolve their SRVs through a per-view
mutex; native definitions stay fixed and release packets drain the jobs first.
The staging heap's allocation mutex already supports concurrent allocation.

The automatic chunk count is `ceil(draws / 512)`, clamped to 1–4. Each chunk is
contiguous and contains at most 512 draws. `CEMU_LATTE_CHUNKS=1/2/3/4` overrides
the count for host/desktop investigations; the batch draw bound shrinks with it.
The normal 2,000-draw submission trigger remains. Owned draw inputs have a
16 MiB batch bound, and an idle coordinator drains a partial run after 8 ms.
An empty transport queue between PM4 draws does not immediately dispatch a tiny
job. Present/readback/copy/release and other dependency packets drain immediately.

Each worker has its own 4 MiB upload ring, 65,536-entry GPU view ring, staging
CBVs/samplers, exact descriptor-table cache, uniform shadow/reuse cache, and
sampler memo. A worker retains at most four allocator/list slots. A slot can only
reset after its fence completes. Sampler heaps belong to those slots; exhaustion
adds a heap and rebinds both descriptor heaps, reacquiring every active stage's
sampler table. Old heaps survive the submission instead of forcing a GPU-idle
wait. Upload and view rings retain their existing fence/wrap/overflow rules.

Each list establishes complete used graphics state. The coordinator plans input
transitions before target transitions in draw order and attaches the resulting
barriers to that draw. A boundary therefore neither guesses the previous chunk's
final resource state nor loses a target transition. Lists contain no split render
pass: this backend uses OM render-target binding. The preceding coordinator list
and the K worker lists are passed in order to **one ExecuteCommandLists call**,
followed by one common fence. Submission and retirement stay on the coordinator.
`WaitForRecording` explicitly drains captured draws before cache teardown.

Active occlusion queries, streamout, slice-copy SRVs, unaligned/out-of-range
uniform copies, and Series S simulation use the serial recorder. An active
query's begin/end/resolve remains on its original list; no query spans an
unrecorded worker list. Readbacks, resource mutations, copies, and native uploads
are ordering boundaries. Fresh index allocations may pass through a captured run
because no earlier draw can reference them. State-only viewport/scissor/FBO
updates can pass through because every captured draw owns its state.

## Front work and serial limits

This is a parallel native recorder with selective preparation migration. It does
**not** implement a front that only decodes PM4/register snapshots. In particular,
full shader-dependent uniform assembly and buffer-cache discovery still run on
the front. The host results do not establish a front stage below 14 ms.

| Work | Final placement and reason |
| --- | --- |
| PM4/register reads and draw decisions | Front; mutable guest register state must be observed in order. |
| Shader, FBO, logical texture discovery and sampler translation | Front; these mutate the existing global Latte caches and establish subsequent draw decisions. Sequence-scoped immutable layouts are retained. |
| Index content lookup/validation, restart metadata and maximum vertex bound | Front; the exact shadow remains the equality authority, and buffer synchronization needs the maximum immediately. No universal guest write-generation tracker exists. |
| Ordinary U16/U32 BE/LE conversion, up to 64 KiB | First consuming chunk worker, once per native allocation. The front captures raw bytes and computes the bound; sibling chunks share a call-once conversion job. Cached hits reuse converted storage. |
| AUTO/expanded primitives and larger index misses | Existing serial decoder. These were not migrated; no claim is made that primitive expansion intrinsically requires serialization. |
| Buffer-cache discovery, flush/overlap/streamout handling and guest-byte capture | Front; the cache uses global interval/page/hash/streamout state and reads guest memory. It was not made concurrently mutable. Native upload/copy commands remain ordered on the coordinator. |
| Uniform source reads and shader-specific assembly | Front. `draw_buildUniformVars` reads registers, remapped guest uniforms, texture scales and FBO state, and updates shader uniform values. It was not split into a separate raw-source packing program. |
| Uniform GPU packing/alignment, deduplication and upload | Chunk workers, with per-worker upload rings and CPU shadows. |
| Native SRV resolution, samplers, CBV/SRV tables, root/vertex bindings and draw recording | Chunk workers; native SRV cache access is locked, other mutable caches/rings are worker-owned. |
| Barrier planning, resource pins, copies, queries, readbacks, queue/fence progress | Coordinator; their order is shared across chunks. |

Deferred indices use the same captured bytes for the immediate maximum, the
front's exact cache shadow, and eventual conversion. A host regression mutates
the guest source between capture and cache insertion: the next lookup must miss,
rather than validating new guest bytes against old converted output. Restart
changes still reuse identical converted bytes while recomputing the bound.

These serial restrictions preserve the existing memory/cache contract; they
also limit the achievable speedup. A further source-capture/packing refactor and
ownership split of the logical caches are still needed for the literal
front-only decoder architecture. The implementation does not claim that the
entire requested front migration or the 60 fps target has been achieved.

## Replies

Mode 3 transfers ownership of cloned ImGui draw lists and deferred COM-release
adapters to the coordinator. Normal UI rendering and these native releases no
longer need front replies or a second synchronous adapter-retirement operation.
Custom ImGui callbacks retain the existing front-thread handoff. Modes 0/2 retain
their previous stack-adapter/reply paths. Already-asynchronous index creation,
index release and nonblocking submissions remain asynchronous.

Backbuffer acquisition, BeginFrame, result-bearing surface conversion, query
flush/readback, front-inspected native creation, and destruction of logical
texture/view/FBO adapters still wait. Presentation remains synchronous, including
the Xbox quick-menu callback that pauses guest execution and changes preferences.
This round does not claim to erase the entire measured 3.8 ms front wait or
2.2 ms `other` bucket. `async-controls/frame` measures the newly owned operations;
the reply categories show what remains on the console.

## Host validation and measurements

All of these completed successfully:

```sh
LATTE_PIPELINE_TSAN=1 LATTE_PIPELINE_BENCHMARK=0 bash tools/tests/run_latte_perf7_tests.sh
LATTE_PIPELINE_TSAN=1 LATTE_PIPELINE_BENCHMARK=0 bash tools/tests/run_latte_pipeline_tests.sh
bash tools/tests/run_latte_perf3_tests.sh
LATTE_PIPELINE_BENCH_FRAMES=200 bash tools/tests/run_latte_pipeline_tests.sh
```

The phase-7 suite executes the production scheduler and the production native
recorder/descriptor heaps with a host device stand-in. It checks K=1/2/3/4,
complete state per draw, ordered barriers/lists, sampler rollover, uniform reuse,
upload/descriptor fence lifetimes, fresh and reused allocator slots, front arena
reuse, captured index mutations, restart changes, shared first conversion, worker
failure propagation, repeated dispatch, and shutdown. ASan/UBSan and TSan pass.
The native test device models API state and lifetimes; it does not emulate GPU
rendering or certify the Xbox driver.

The parity probe captures one synthetic resolved frame stream, destroys/reuses
its producer state and arena, and compares the complete per-draw command/state
values with the mode-2 abstraction for every K. The native recorder test additionally
compares its effective draw state, constants, descriptors, barriers and arguments
with those captured inputs. No real console frame capture is supplied by this
repository/task; pixel parity and real guest timing still need hardware.

The existing suites also pass, including native and Rosetta x86 conversion,
expansion, content collisions/middle-only mutations, relocation/associativity,
root-signature layout, stage/descriptor generations, profiler transport/reset,
queue pressure, worker handoff/cancellation and nonblocking-submit parity.
Profiler tests now cover chunk-field transport and resetting those fields.

The optimized chunk benchmark uses the production scheduler and descriptor-key
hashing, uniform copies and deterministic command-word emission for 5,000 draws.
It is a synthetic CPU workload, not the Windows driver, actual shader translator,
or a complete BotW frame. Ten alternating measured windows follow warmup; each
window records twenty frames. Each frame uses the production 512-draw-per-worker
bound (ten batches at K=1, five at K=2, four at K=3, three at K=4), including the
partial final batch. No native job is modeled with more than 512 draws. There is
no new speed threshold and no relaxed gate.

```sh
clang++ -std=c++20 -O3 -Wall -Wextra -Werror -pthread \
  -Itools/tests/d3d12_stubs -Iruntime/cemu \
  tools/tests/latte_chunks_bench.cpp -o build/tests/latte_chunks_bench
build/tests/latte_chunks_bench
```

| Workers | Final host ms / 5,000 draws | Measured scaling | Ideal CPU-only scaling |
| ---: | ---: | ---: | ---: |
| 1 | 8.0312 | 1.0000x | 1x |
| 2 | 5.1177 | 1.5693x | 2x |
| 3 | 3.7243 | 2.1564x | 3x |
| 4 | 3.0117 | 2.6666x | 4x |

Dispatch, memory traffic and host scheduling prevent ideal scaling. Early
scheduler-only probes dispatched one unbounded job per frame; they are superseded
by the bounded benchmark above. The final run was made after this task's
build/tests/transport benchmark and packaging finished. Other machine activity
was not controlled; the surrounding load was approximately 7–10 with 11 GiB of
swap. These measurements establish functioning parallel CPU work, not an Xbox
FPS gain.

Both unchanged 1 ms median transport gates pass (nine paired windows, 200 frames):

| Existing workload | Paired wall overhead | Paired thread-CPU overhead |
| --- | ---: | ---: |
| Repeated bindings | 0.4597 ms/frame | 0.4583 ms/frame |
| Changing bindings | 0.8857 ms/frame | 0.8763 ms/frame |

The changing-binding maxima were 1.6640/1.3308 ms; the original gates are on the
paired medians. The workload and thresholds were not changed. The same final run
reports fused index transport at 2.23237 ms/5,000 draws, sparse retirement at
0.0029945 ms/frame, and nonblocking-submit transport at 1.08818 ms synchronous
versus 0.918683 ms asynchronous. These are existing host probes, not console data.

Logs are retained locally in `build/pipe68-final-tests.txt`,
`pipe68-protocol.txt`, `pipe68-regression.txt`, `pipe68-transport.txt`,
`pipe68-benchmark-bounded.txt`, `pipe68-build-release.txt`, `pipe68-package.txt`,
`pipe68-signature.txt`, and `pipe68-package-verification.json`.

## What to read on the console

Use the same Great Plateau scene and compare modes **0, 2, and 3**, both without
the profiler and with `latte-profile.txt`. No additional detail flag is required.

1. Check the same rendered scene, draw parameters, converted/restart indices,
   query results, surface copies/readbacks and resource/fence progress. Include
   render-target changes at chunk boundaries, streamout, loading, the quick menu,
   pause/resume, and shutdown. Run with the D3D12 debug layer where available.
2. Read `Latte chunks`: four `busy-wall` values, `critical-path`, chunks/frame,
   batches/frame, parallel/serial draws, async controls and deferred-index bytes.
   Divide chunks by batches to see actual K. Mostly one chunk means dependencies,
   the capture bound, or short runs are limiting useful parallel recording.
3. Worker busy is elapsed recording wall time, including preemption/fence waits.
   Critical-path is the sum of dispatch-to-join intervals per source frame,
   excluding serial capture/planning and GPU execution. Workers overlap; **do not
   add their four wall times together to estimate a frame**. The existing
   `back-busy` CPU field now describes the coordinator, not the sum of chunk CPUs.
4. Read the explicit serial reasons: query, streamout, alignment, slice-copy and
   simulation. A high fallback fraction may leave little useful parallel work.
   Compare front draw-state, indices, texture-bind, buffer-sync and uniforms;
   their remaining serial portions have not disappeared.
5. Read front-reply-wait and all reply categories with `async-controls/frame`.
   Check whether owned UI/native release operations materially reduce `other`
   and `release`. Presentation, results and logical lifetimes still synchronize.
6. Compare actual frame times and front/coordinator/critical-path coverage against
   the approximately 14 ms stage budget. Lower coordinator descriptor categories
   alone can mean work moved to chunks. Host timings and a linked package are not
   acceptance evidence for 60 fps, native driver parity, or the full front split.

## Build, package and commits

`nice ninja -C build/local-uwp-game -j4` completed and linked `BotWRecomp.exe` after
waiting for other Ninja jobs. The final build includes the formatting commit;
`build/` and generated guest code were retained. No simulator or server was started.

```sh
APPX_OUT=pkg/pipe68.appx sh pkg/repack.sh 1.2.2.0
osslsigncode verify -CAfile pkg/test.crt -in pkg/pipe68.appx
```

The signed x64 package is **517,079,746 bytes**, version **1.2.2.0**. Signature
verification succeeds. The manifest and all nineteen staging files were checked
against the archive before only those local staging files were removed. The
packaged executable matches the final linked executable:

```text
61d8e09469234aea39a528480183524a5080bf4cf990df13491630824fc095c4
```

APPX SHA-256:

```text
eb766d592a45b2d84d477c22b762a71983a847588c959ba65b53c5c254bd7c43
```

Implementation commits:

- `eb056aa`: design before implementation.
- `7cbd7ab`: bounded workers and owned draw/index contracts with portable parity.
- `7777364`: native mode-3 recording, dependency boundaries, selective migration,
  owned asynchronous adapters, and profiler fields.
- `ea74a6f`: native device/lifetime, deferred index and profiler regression tests.
- `985712f`: clang-format changes in a separate formatting commit.
- `903b2f0`: benchmark the production per-worker/batch bounds, retaining parity checks.

All commits use Miguel's requested identity. No push, console deployment, other
worktree modification, or restoration of removed crypto/WUD/NUS code was made.
Host validation, linking and packaging are complete. The remaining front
architecture migration and console performance/parity limitations are explicit
above; this package is not evidence that all phase-7 performance goals are met.
