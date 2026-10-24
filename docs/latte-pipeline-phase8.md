# Latte pipeline, phase 8

## Release decision

`pkg/pipe69.appx`, version **1.2.3.0**, ships **mode 2** when
`LocalState/latte-pipeline.txt` is absent. Explicit `0`, `1`, `2`, and `3`
remain selectable, with surrounding whitespace accepted. Previously, a file
containing `0` incorrectly selected mode 1. A host test now reads real temporary
configuration files and checks these cases. Empty/invalid existing files retain
the previous mode-1 behavior; an unreadable file uses the shipping default.

Mode 3 remains experimental. It does **not** beat the single-recorder CPU model
on the console-shaped host benchmark below. The supplied console results remain
the only hardware measurements: mode 2 at 42.8–43.2 fps, mode 3 at 11.7–12.1 fps.
This release does not claim a newly measured Xbox speedup or verified pixel parity.

## Why worker 0 recorded almost everything

The phase-7 coordinator drained and submitted captured draws before nearly every
backend operation except a short whitelist. In particular, **IndexRelease** fell
through to `draw_drainParallel()`. The console reports 360.3 index set evictions
per frame in the final window. These evictions are asynchronous transport
operations, but each could still force a worker dispatch, join, command-list
submission, fence signal, and coordinator reset. Registry retirement and query
maintenance/result polling were additional unnecessary drains.

This is not a render-target-change boundary or a hardcoded worker-0 dispatch.
Framebuffer, viewport, and scissor packets already passed the initial boundary
switch; a non-readback EndSequence also passed. The scheduler selects
`ceil(draws / 512)` workers automatically. For the measured
`4992.1 / 470.9 = 10.60` parallel draws per batch, that is **one** worker. A
one-worker job goes to worker 0. Starting four workers for every ten draws would
increase overhead without fixing the excessive submissions.

The shared production boundary policy now lets these operations pass:

- Fresh index creation and deferred creation, index release, and registry retirement.
- Framebuffer, vertex, viewport, and scissor state updates.
- Query completion maintenance; result polling also avoids a query-recorder drain.

Index release is safe because captured draws already own the native index view
and a shared deferred-conversion job. `D3D12UploadHeapAllocator::Free` tags storage
with the current recording fence; it does not immediately free or reuse it.
Registry retirement invalidates an ID, while captured draws already contain
resolved inputs. Actual native view/texture/FBO destruction still drains before
its synchronous logical destructor can finish. Query begin/end/reset still drain.

Uploads, overwrites, copies, clears, readbacks, native releases, and presentation
remain ordered dependencies. BindUniform also remains a boundary because its
unaligned-buffer helper can emit a copy. These dependencies must not be silently
removed to improve a benchmark. The 16 MiB capture limit, 512 draws per worker,
normal submission limit, and 8 ms idle flush remain in effect.

The state/retirement replay now produces three batches at K=4, twelve chunks,
and 1,250 draws on each worker. Automatic K produces three batches and ten chunks
(the partial final batch uses two workers). This establishes batching for the
specified state/retirement stream. It does **not** establish three batches for a
real console frame containing additional copy/upload/query dependencies. The
supplied aggregate log has no ordered packet trace to reconstruct those exactly.

## Synchronous waits

`reply-other 25.048 ms` and `reply-release 12.648 ms` are **front wait times for
ordered queue prefixes**, not the CPU execution times of those operations alone.
In the same window the backend reports 28.105 ms in release handling, 28.230 ms
in submit, and 37.170 ms of chunk critical path. Chunk critical path and submit
alone account for about 65.4 ms of the 82.846 ms recording wall interval. The
front then waits for this work at inherited synchronous operations and also
spends 27.323 ms under queue backpressure. These overlapping front/backend
measurements must not be added together as independent frame costs.

`IndexRelease` itself has no reply. `ReplyRelease` covers logical native releases
and RegistryRetire acknowledgments, which may be delayed by earlier index jobs.
`ReplyOther` covers result-bearing surface conversion, backbuffer acquisition,
BeginFrame, debug snapshots, query flush, and explicit recording drains. The
normal mode-2/mode-3 reply predicate is unchanged; mode 3 does not introduce a new
per-draw/per-segment synchronous request. Its owned UI/native-release adapters
actually remove some mode-2 replies. The existing mode-3 RecordingDrain is a
pipeline-cache teardown boundary, not a gameplay-per-segment operation.

Removing the unnecessary index/retirement drains addresses the work that inflates
those waits. Making all logical destruction asynchronous would require transferring
adapter ownership; simply dropping their acknowledgments would permit use after
free. That shortcut was not taken.

The old log cannot attribute the exact 25.048/12.648 ms to individual calls. This
release adds `Latte reply operations/frame`, listing each backend operation's
call count and wait time, and `Latte backend chunk drains/frame`, listing the
operations that actually drain a nonempty batch. Both use the existing profiler
window, with no per-draw logging. The drain list covers backend packets; draw
fallbacks, query actions, capacity and idle drains retain their other diagnostics.
Profiler transport/reset tests cover the added state.

## Rendering investigation and binding parity

I opened and compared both supplied PNGs, found under the sibling artifact root:

- `/Users/miguel/.local/share/nxbox/artifacts/botw-p66m2p0/end.png`
- `/Users/miguel/.local/share/nxbox/artifacts/botw-p68m3p0/end.png`

The mode-3 cliff looks darker/flatter. Sky color, grass positions, and stair
lighting also differ. These are not a controlled pixel comparison, and those
other differences do not prove the cliff correct. **The screenshot discrepancy
remains a probable correctness issue pending console validation.**

The worker previously received emulator view pointers and resolved SRVs later
through a separate worker lookup path. Both queued recorders now use the same
`ResolveTextureDescriptors` implementation and `GetSRV` call on the coordinator,
at the ordered draw-capture point. Workers receive the completed native SRV table,
not mutable texture-view pointers or a second interpretation of register/swizzle
bindings. Existing resource pins and destruction drains protect the descriptors
until copying, and the common fence protects the GPU resources afterward.
Sampler inputs remain the same owned StageBlock values consumed by mode 2.

The worker table cache now also receives the real staging-source generation.
An initial hypothesis was that its constant source generation caused stale
textures. A mutation experiment **did not support that as the root cause**:
restoring the constant still passed, because OnSubmit already invalidates the GPU
view-table generation and current batches exclude source destruction. This is
cache-contract hardening, not evidence that the cliff was caused by that field.

The strengthened host test checks every native SRV and sampler slot at every draw
against an independent mode-2 binding rule. It exercises reversed register
mappings, absent views, dimensional-null encoding, swizzle changes, reordered
shader stages, CPU descriptor-address reuse, sampler-heap rollover, allocator
reuse, in-flight descriptor/upload storage, and K=1/2/3/4. Descriptor **contents**
are compared, not just CPU/GPU handles. An injected wrong-swizzle mutation fails
at the per-draw SRV assertion; the real implementation passes ASan/UBSan and TSan.

This closes the previously untested worker binding path and removes divergent
texture resolution. It does not identify a proven root cause for the supplied
cliff image, emulate the GPU, or establish an end-to-end rendering fix. Shipping
mode 2 avoids exposing that unresolved mode-3 regression by default.

## Console-shaped host benchmark

`tools/tests/latte_segments_bench.cpp` uses 5,000 draws in 470 segments, each
containing ten or eleven draws. Segments change targets and textures and carry
state, index-retirement, and query-maintenance traffic. The production boundary
policy and scheduler determine dispatches. Mode 3 is charged for immutable input
capture; every path runs the same per-draw descriptor hashing and 448-byte uniform
copy kernel. The capture byte bound uses `sizeof(D3D12ResolvedDraw)` plus payloads.
Every output is checked against the single-recorder reference.

The phase-7 policy replay deliberately drains at each segment and automatically
selects K=1. Assertions require 470 batches/chunks and 5,000 worker-0 draws. Fixed
K=4 and automatic-K assertions require the larger batch distributions described
above. Warmup precedes nine measured windows of twenty frames, with alternating
path order. Final medians on this Mac:

| Path | Host ms / 5,000 draws | Batches / chunks | Draws on workers 0/1/2/3 |
| --- | ---: | ---: | --- |
| Mode-2 CPU model | 0.9047 | 0 / 0 | Runs on the single caller |
| Phase-7 boundary replay, automatic K | 6.9470 | 470 / 470 | 5000 / 0 / 0 / 0 |
| Revised mode 3, K=1 | 1.7848 | 10 / 10 | 5000 / 0 / 0 / 0 |
| Revised mode 3, K=2 | 1.4053 | 5 / 10 | 2500 / 2500 / 0 / 0 |
| Revised mode 3, K=3 | 1.3093 | 4 / 12 | 1666 / 1667 / 1667 / 0 |
| Revised mode 3, K=4 | 1.1885 | 3 / 12 | 1250 / 1250 / 1250 / 1250 |

Even K=4 takes about **31% longer** than the mode-2 CPU model. Therefore the
shipping default is mode 2. This benchmark reproduces the console's **batch and
worker starvation pattern**, and the large regression, not its absolute 83.5 ms
frame time or exact approximately 3.6x ratio. It measures a CPU model with the
production policy/scheduler, not the Windows driver, native GPU submission, guest
execution, full mode-2 renderer, or a captured console packet trace. Host timings
must not be labeled Xbox FPS. Literal numerical reproduction of the console
frame time has not been achieved.

The round-7 benchmark omitted all inter-draw control/retirement traffic. It
constructed large batches directly, assumed them as scheduler input, compared
against K=1 of the same worker path rather than a single-recorder baseline, and
performed 96 repeated descriptor hashes per draw. That magnified parallel work
while hiding capture, dispatch and boundary costs. Its reported scaling did not
test whether the console could actually form those batches. The new replay is
included in `run_latte_perf7_tests.sh`; the older benchmark remains explicitly a
separate scheduler-work probe.

## Verification and package

Successful checks:

```sh
LATTE_PIPELINE_TSAN=1 LATTE_PIPELINE_BENCHMARK=0 bash tools/tests/run_latte_perf7_tests.sh
LATTE_PIPELINE_TSAN=1 LATTE_PIPELINE_BENCHMARK=0 bash tools/tests/run_latte_pipeline_tests.sh
bash tools/tests/run_latte_perf3_tests.sh
LATTE_PIPELINE_BENCH_FRAMES=200 bash tools/tests/run_latte_pipeline_tests.sh
```

The profiler test was additionally rebuilt/run under ASan/UBSan after its new
operation counters were added. The native suite's final run includes the shared
resolver, all-slot parity and real configuration-file tests. Native and Rosetta
index, root signature, descriptor lifetime, transport, arena, worker cancellation,
shutdown, and synchronization tests pass. The deliberate wrong-swizzle mutant
fails as expected. The unchanged 1 ms paired-median transport gates pass:

| Workload | Wall overhead, ms/frame | Thread-CPU overhead, ms/frame |
| --- | ---: | ---: |
| Repeated bindings | 0.4846 | 0.4817 |
| Changing bindings | 0.8534 | 0.8506 |

After checking that no Ninja was running, the single
`nice ninja -C build/local-uwp-game -j4` invocation completed and linked the release
executable. Then:

```sh
APPX_OUT=pkg/pipe69.appx sh pkg/repack.sh 1.2.3.0
osslsigncode verify -CAfile pkg/test.crt -in pkg/pipe69.appx
```

The signed x64 package is **517,086,463 bytes**. Signature verification passed;
the manifest version/architecture and all nineteen staging files were checked
against the archive. The archived executable matches the final linked executable.
Only this repack's verified local staging files were then removed.

Executable SHA-256:

```text
6b959f03c6cd0f7843f29204d684a16cee4ff5c679ad0721f31522add509dfc7
```

APPX SHA-256:

```text
b449e79e0ae2b7b94040d3685950ef70940a39d8818bbe0e9b3ecc0a5bb6655f
```

Local evidence is retained in `build/pipe69-final-native.txt`,
`pipe69-protocol.txt`, `pipe69-regression.txt`, `pipe69-transport.txt`,
`pipe69-segments-bench.txt`, `pipe69-swizzle-mutation.txt`,
`pipe69-descriptor-mutation.txt` (the rejected source-generation hypothesis),
`pipe69-build-release.txt`, `pipe69-package.txt`, `pipe69-signature.txt`, and
`pipe69-package-verification.json`.

Implementation: `55d0b17`. Separate formatter-only commit: `1db70f3`.
All commits use the requested Miguel identity. No push, console deployment,
q-wt modification, simulator, dev server, or Apple account/keychain change occurred.

For console acceptance, first remove `latte-pipeline.txt` and verify the mode-2
baseline. Then compare explicit modes 2/3 in the same saved scene at the same
in-game time, inspect cliff/normal detail, and collect the new per-operation wait
and drain breakdowns. Loading, queries, resource eviction, copies, pause/resume,
and shutdown also need hardware coverage. Mode-3 pixel correctness, its exact
console speedup, and few-batch behavior across real native dependencies remain
unverified; this report does not mark those goals complete.
