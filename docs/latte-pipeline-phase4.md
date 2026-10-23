# Latte pipeline, phase 4

Phase 4 adds an opt-in D3D12 recording worker. The production default remains the
reference implementation. Hardware frame parity, lifecycle testing and the 60 fps
performance target have **not** been verified on the console by this change.

## Selection

| Setting | Behavior |
| --- | --- |
| `CEMU_LATTE_PIPELINE=0` or unset | Reference draw path, no worker |
| `CEMU_LATTE_PIPELINE=1` | Immutable packets, immediately consumed on Latte |
| `CEMU_LATTE_PIPELINE=2` | Immutable packets, consumed by `Latte D3D12 recorder` |

On UWP, `LocalState/latte-pipeline.txt` containing `2` (surrounding whitespace is
allowed) selects mode 2. Every other file content, including an empty file, selects
mode 1. Removing the file leaves the environment/default selection unchanged.
`LocalState/latte-profile.txt` enables the existing profile report.
`CEMU_LATTE_PIPELINE_CHECKS=1` enables ownership checks in release builds; debug
builds always check. Ring and arena size overrides retain their existing ranges.

## Ownership and transport

Device and CoreWindow surface construction still happen on the existing host
caller. `Initialize` explicitly transfers that drained ownership to Latte, then
starts the worker after native initialization. This matters for checked builds:
the constructor's thread is not the eventual command processor thread.

Latte retains PM4 parsing, guest memory/register interpretation, shader/pipeline
preparation and compiler publication, logical texture/buffer/index/query caches,
streamout bookkeeping, uniform snapshots, UI preparation, and guest result
application. The worker consumes the existing complete draw/transfer packets and
owns command recording, barriers, native resources and descriptors, upload/index/
readback allocations, root arguments and all existing recording caches, query
fragments, command allocators, submission, fences, scanout and presentation.

The forward ring remains bounded (1,024 packets by default); packet headers use a
reserved-bit flag to request a synchronous reply. Publication no longer implies a
recorded acknowledgment in mode 2. A reverse SPSC ring transports operation
acknowledgments, recorded/submitted/completed prefixes, the actual completed native
fence, allocation/control replies, query results, owned readback bytes and profile
snapshots, and reclaimed resource generations. Replies are taken by their exact
operation sequence before applying front cache callbacks, so a nested front operation cannot overwrite its caller's
answer. Operation sequences and GPU fence values remain separate.

The reverse ring has forward capacity plus 32 slots by default. Front drains it
before publication and while waiting for forward space or replies. Completed-fence
notifications while idle are coalescible and never block a surface handoff.
Shutdown, cancellation and failure do not need a free forward slot: stop waits for
the already-published prefix, then requests exit and joins. A worker exception is
published through an independent failure channel, wakes work/space waiters, and
fails subsequent publication and every pending sequence wait. Exceptions from a
native surface handoff also fail the stream after returning ownership.

Work/space notifications use atomic epochs and condition variables. A waiter arms
under the event mutex and rechecks the epoch; publishers take the mutex only when
a waiter is armed. Empty workers sleep with a 1 ms timeout and service pending
interpolated presents, pad-close ingress, requested submissions and completed
fences even when Latte publishes no new packets. They do not spin on an empty
ring. The ring itself still uses release/acquire cursor publication.

The immutable draw-block pool now has 1,024 slots (about 4.6 MiB), rather than four
synchronous slots. It reuses the current consumed slot or searches forward from
it; it does not scan the entire queued prefix on every changing draw. Exhaustion
waits for consumed progress and is counted as backpressure. Each draw reserves a
contiguous arena group for its runtime data and uniforms before copying its first
span. Arena pressure also waits for consumption; a tail reset cannot invalidate
live packet spans. Native fence retirement and CPU snapshot retirement remain
independent. Resource registry free slots are returned under its existing
allocation/retirement lock, after both lifetime conditions pass. Mode 2 returns
those reclaimed generations through reverse messages before making them available
to new allocations.

No recording cache key, descriptor rollover order, root-CBV ABI, index conversion,
shader wait/skip decision or draw count is intentionally changed. Modes 0 and 1
continue using the same native helpers and lifetime rules.

## Synchronization retained

| Boundary | Reason / behavior |
| --- | --- |
| First texture/FBO/UI allocation and the first query heap | Recorded reply supplies allocation success, formats or opaque identity needed by front. Existing query heaps do not add another create wait. Index IDs are allocated on front and created/uploaded in order without a reply. |
| Native/logical adapter release | Release is recorded before front destroys or reuses the adapter. Index release transfers destruction to the recorder and needs no front wait. Native COM/upload retirement still uses GPU fences. Release does not recursively apply guest cache callbacks from inside a logical destructor. |
| Explicit `Flush` / submit / `WaitForIdle` | Worker performs the existing submit/fence behavior before acknowledging. PM4 wait, semaphore, draw-done and timestamp call sites retain their existing ordering. |
| Query polling | At most one asynchronous poll per logical query. Reverse results are matched by ID/generation and begin/reset epoch. Front publishes only completed fragment sums. |
| Forced query flush | Existing native submission/fence wait, followed by an owned batch of finished query sums before front resumes the forced-finish loop. A recorded ticket alone is insufficient. |
| Readback polling | Asynchronous. A finished response contains copied CPU bytes; completion and fence changes trigger front application. Front never borrows the worker's mapped readback ring. |
| Forced readback / screenshot | Existing completed-fence wait and immutable bytes returned before guest retile/hash updates or screenshot conversion/save callbacks. |
| Surface format conversion | Recorded boolean reply preserves the existing conditional logical texture-write tracking. |
| Begin-frame / UI acquisition / UI snapshot | Recorded control replies; stack-owned cloned ImGui lists remain alive through recording. Font atlas/context access stays on front. |
| Debug overlay | Worker returns an ordered native statistics snapshot; front draws ImGui text using that snapshot and its logical buffer/pipeline statistics. |
| Swap / empty frame | Present is recorded and serviced before front advances its frame epoch. At most the current source frame is outstanding, preserving the coarse latency contract. |
| Cache/title teardown | Drain before destroying logical shader/pipeline records; native releases execute on worker and retain fence deferral. Native teardown uses a drained exclusive handoff and waits for GPU idle. Worker joins before renderer members are destroyed. |

Normal draws, uploads, copies, streamout barriers, query markers, idle notifications
and sequence endings publish without waiting. In particular, `draw_endSequence`
does not drain merely because a sequence ended. Capacity/state/arena exhaustion
can still block front; those are bounded storage pressure, not GPU completion.

Resize ingress is an atomic size request, consumed at ordered backbuffer
acquisition after GPU-idle handling. Surface creation/destruction and native
swapchain attachment use an exclusive handoff: worker consumes the captured
published prefix and parks, the existing caller temporarily owns native state,
then worker resumes. This preserves the CoreWindow calling-thread contract without
introducing another SPSC producer. Pad-close ingress remains supported.

UWP HUD/recorder/quick-menu work stays within the worker's ordered present, after
native submission; front waits at that present boundary. A custom ImGui callback
uses a different, explicit synchronous handoff: front waits for recording, parks
the worker and renders that snapshot itself. Front ownership checks are permitted
inside this handoff, native wrappers execute immediately, and reentrant operation
publication is rejected rather than deadlocking behind the parked worker. There
are no registered application custom draw callbacks in this checkout.

## Profiling

The `Latte profile` line adds:

- `front-busy` / `back-busy`: each actor's OS thread CPU time divided by source
  game frames. This includes transport/service CPU work, excludes blocked waits
  and preemption, and avoids double-subtracting a reply wait nested in PM4 idle
  (for example, an OSScreen present while guest ingress is empty).
- `back-record-wall`: elapsed operation consumption plus native idle-service work
  per frame. This includes waits inside native operations (GPU fence, Present or
  a paused quick menu); compare it with back busy and the existing fence/Present
  diagnostics to distinguish CPU cost, scheduling and native wait costs.
- `front-backpressure`: waits for packet, state-block or arena capacity per frame.
- `front-reply-wait`: explicit recorded/control/completed-result waits per frame.
- `back-idle`: sleeping for work per frame; reverse-ring pressure is separate from
  native recording work. `ring-high-water` is the producer-observed occupancy
  maximum over the renderer lifetime, bounded by the configured forward capacity.
- `front/back CPU`: separate OS thread CPU time as percentages of one core over
  the report interval. The existing D3D12 diagnostics label their thread as
  `recording CPU`, since mode 2 presents on the worker.

The collectors are thread-local. At the existing approximately ten-second source
frame boundary, back transports its snapshot; front combines **counters only** and
reports on front. The old exclusive slot timings and `covered` percentage remain
front-only in mode 2; back slot times are never added to that wall-time coverage.
Modes 0/1 report their single actor's CPU time as front busy and zero separate
back busy. Ignore the initial interval containing initialization/loading; subsequent
intervals use snapshot deltas. Source-frame IDs and ticket prefixes still allow
progress correlation with submissions.

## Host validation

`tools/tests/latte_pipeline_test.cpp` exercises the production stream with two
actors and forward capacities 1, 2, 3 and 8, each with 20,000 changing immutable
operations and a one-slot reverse ring. It verifies ordered replies and tickets,
owned arena publication, state-block pressure, concurrent registry generation
reuse, exact-sequence reply retrieval, repeated exclusive handoffs, foreign UI
handoffs, constructor-to-Latte ownership transfer and callback publication guards.
Additional cases cover deterministic full-ring shutdown, cancellation, recorder
failure, failure during a surface handoff, idle sleeping and contiguous arena
reservation. Existing packet, direct/queued command equivalence, generation,
arena, descriptor rollover and ticket tests remain in the suite.

Validation logs in the existing build tree:

- `build/latte-phase4-tests.log`: ASan/UBSan and ThreadSanitizer, with
  `TSAN_OPTIONS=halt_on_error=1`.
- `build/latte-phase4-regression.log`: existing profiler, exact draw caches, index
  conversion/expansion/integration/associativity, stage tables, root signature and
  actual descriptor rollover/retirement tests, including x86 SIMD under Rosetta.
- `build/latte-phase4-build-final.log`: requested
  `nice ninja -C build/local-uwp-game -j4` build/link, after waiting for the existing
  machine-wide Ninja process to exit. No additional build tree was created.

The optimized synchronous handoff benchmark passed both the wall and thread-CPU
1 ms/frame median overhead gates (`build/latte-phase4-benchmark-final.log`):

| 5,000-draw workload | Direct wall median | Queued wall median | Paired wall overhead | Paired CPU overhead | Maximum wall overhead |
| --- | ---: | ---: | ---: | ---: | ---: |
| Repeated bindings | 0.9032 ms | 1.6812 ms | 0.7780 ms | 0.7779 ms | 0.8012 ms |
| Changing bindings | 0.3957 ms | 1.3221 ms | 0.9182 ms | 0.9177 ms | 0.9600 ms |

Competing Ninja work in another worktree invalidated exploratory timing samples.
The final benchmark waited for an idle Ninja slot and monitored for competing
Ninja work throughout the measurement; none appeared. These host measurements
test transport correctness/cost, not native D3D12 parity or Xbox frame rate.
ASan/UBSan, ThreadSanitizer and the existing cache/descriptor/index suite passed.
The final requested UWP builds linked `BotWRecomp.exe`; the subsequent verification
build is recorded in `build/latte-phase4-build-verify.log`. A following invocation
reported **no work to do** (`build/latte-phase4-build-no-work.log`).
`git diff --check` passed. No console deployment, simulator or dev server was
started, no extra build tree was created, and no changes were pushed.

## Console verification required

Use the same Plateau checkpoint, native 1080p, FSR 3.1 NativeAA and Remaster packs.
Capture mode 0, then 1, then 2 after shaders/loading stabilize. Mode 0 is the
reference; the later fixed Plateau baseline is 46 presents/s (profiler off). Require
identical image output, attempted/recorded draw and query counts, streamout bytes,
readback data, and index/descriptor/root-CBV cache behavior before trusting speed.

For the 60 fps objective, both steady-state `front-busy` and `back-busy` should be
well below 16.6 ms/frame, with low backpressure and reply-wait time. The original
rough 11 ms front / 8 ms back split is a planning estimate, **not** a measured
result. If backpressure/high-water remain high, back is limiting throughput; if
back sleeps heavily, front or game submission is limiting it. Large reply waits
identify semantic drains or native wait costs. Present/fence waits or low thread
CPU percentages can explain elapsed recording time without a CPU bottleneck.
CPU busy budgets alone do not establish 60 fps: require the observed present rate
and frame latency as well.

Check ownership assertions and D3D12 debug-layer/PIX validation; rapidly mutate
CPU buffers, exercise alias/surface/feedback copies, shader misses, query forces,
streamout dependencies, screenshot plus recorder/HUD, TV/DRC swaps, resize and pad
close. Stress pause/resume, quick menu, title/cache unload, shutdown with queued
work, tiny ring/arena settings and device removal. Verify pending presents still
meet deadlines when guest command ingress stops, and that no pending ticket or
guest semaphore waits forever. Keep mode 2 opt-in until those checks pass. The console-regression changes below
remove index round trips and batch progress; they do not increase frame latency
or establish console performance or image parity.


## Console regressions

The supplied Plateau measurements were 46 presents/s before, 38 in mode 0,
35 in mode 1 and 7.5 in mode 2, with profiling disabled. The profiled mode-0
comparison was 22.008 -> 27.781 ms inclusive draw time. These are input measurements,
not results from the repaired build. The task's appended status confirms that the
attempted mode-2 profile measured another installed version; no valid mode-2
console profile was available during this repair.

### Baseline and index reuse

The actual merge base with `xbox/port` is `aaff582`, whose index cache is unchanged
in the original pipeline branch. However, the supplied before profile contains
round-5 counters and comes from `a2ff93c` (`xbox/latte-perf5`). That commit was never
an ancestor of this branch. The comparison therefore mixed two cache
implementations: 256 four-way entries here versus 2,048 small plus 256 large
entries and exact content-validated relocation reuse in round 5. This is why the
before report has `index-relocated` hits and the old mode-0 report does not.
Round 5's own commit records 46.1 versus 46.0 presents/s with profiling off;
its profiled cache improvement alone cannot explain the entire 38 fps regression.

This repair integrates round 5's index banks, full-byte-validated content hints,
texture-stage descriptor/sampler reuse, reusable prepared-stage storage, paired
profiler-scope reduction and PM4 ingress batching. Hash equality is never enough
for reuse. Existing mutation, restart, stale-hint, deliberate hash-collision,
associativity and native SIMD tests remain required. The small/large cache bounds
are the baseline's 24 MiB input plus 36 MiB converted output, excluding allocator
capacity.

`latte_index_stream_bench.cpp` adds the missing repeated-stream gate: 1,024
independent small streams, selected below the restored bank's associativity,
5,000 draws/frame for 100 frames after warmup, including guest wait invalidation.
It runs the production `LatteIndices.cpp` with host renderer allocation stubs and
requires at least 99% hits. The old branch fails with **0% hits and 500,000
misses**; the repair passes with **100% hits and zero misses**. Time fell from
0.3510 to 0.0770 ms/5,000 draws. This is a capacity regression reproducer, not a
reconstruction of the game's exact index stream or an Xbox timing estimate.

### Direct path: buffer sync, indices and uniforms

Mode 0 previously constructed backend packets even for vertex/uniform buffer
bindings, viewport/scissor/FBO updates, sequence begin/end and common native
operations. Vertex bindings additionally copied through the staging arena.
Buffer uploads copied guest bytes into the CPU transport arena and then into the
native upload ring. Index misses allocated a logical adapter/registry entry,
requested a create reply, decoded into scratch, staged another copy and copied
again into coherent upload memory. Uniform preparation similarly copied scratch
through the arena before checking the existing uniform cache, with extra nested
profiling scopes. These costs occurred despite the draw itself using the direct
recorder.

Mode 0 now calls native helpers for bindings, buffer uploads, sequence boundaries,
and common copy/clear/submit operations. Index conversion writes directly to the
original coherent native allocation and upload is a no-op apart from byte counters;
it allocates no logical index ID and requests no reply. Uniform preparation and
cache comparison read the CPU scratch directly, with no transport allocation,
copy, retirement or extra nested scope. Native readback callbacks still run at
completion changes outside recording scopes, including the deferred callback path
for an operation that submits. Modes 1/2 retain immutable owned snapshots.

The old `create-reply` counter counted logical resource creation even in mode 0,
where there was no worker round trip. It now counts actual threaded create waits
centrally. Mode 0 and mode 1 have zero such waits, and index creation adds none in
mode 2.

The reported **6,187,154 upload B/frame** is not 6 MB of buffer-cache sync:
`UploadBytes` counts all native `AllocateUpload` requests (including 256-byte
padded transient shader uniforms, texture uploads, internal/UI work and buffer
uploads), plus copied converted index bytes. `BufferBytes` counts only the
**113,385 raw buffer-cache bytes/frame**. `StageBytes` copies do not increment
`UploadBytes`. Many of those native uploads existed before the pipeline counter;
the excess index misses add conversion/upload traffic, and mode 0 unnecessarily
staged indices and uniforms as well. The old report cannot partition the 6.2 MB
exactly. New `index` and `uniform` byte fields beside `upload` expose that split;
index bytes count coherent decoder writes in mode 0 too, and uniform bytes count
actual padded allocation misses, not uniform-cache hits.

### Synchronous packet path and worker stalls

Mode 1 still prepares and consumes immutable draw values, but consumes a local
snapshot through the same recording/lifetime helper instead of encoding it into a
ring, publishing cursors, copying it back and indirectly dispatching it on the
same thread. Native commands, resource pinning and arena retirement are preserved.
The existing optimized 5,000-draw host benchmark enforces both wall and thread-CPU
median overhead <= 1 ms/frame, using nine paired samples after warmup:

| Workload | Before paired wall / CPU overhead | After paired wall / CPU overhead | After maximum wall overhead |
| --- | ---: | ---: | ---: |
| Repeated bindings | 0.8316 / 0.8330 ms | 0.7865 / 0.7816 ms | 0.8399 ms |
| Changing bindings | 0.9687 / 0.9595 ms | 0.8980 / 0.8962 ms | 0.9207 ms |

An exploratory sample exceeded 1 ms for changing bindings before the typed local
snapshot optimization. The final measurements above were taken after Ninja
exited; there was no competing Ninja build during that measurement.

Mode 2 previously waited on each index create **and** release. Front now allocates
the logical ID and conversion scratch without a recorded reply. Ordered create,
upload and draw packets resolve the native allocation on back. Ordered release
frees its fence-deferred native allocation, retires its generation and destroys
the adapter on back after the final CPU use; front never deletes a queued adapter.
Allocation failure fails the worker stream instead of publishing a null native
allocation to a draw. Other replies remain at actual control/result/lifecycle
boundaries, including present and GPU-visible synchronization.

Ordinary reverse acknowledgments are coalesced to one per 32 consumed operations,
or the empty-queue tail. Explicit replies, scalar/byte results, query/readback
results, profiles and reclaimed generations remain lossless. Progress notifications
advance only when actually published; a partial batch cannot strand `Finish` or a
capacity wait. Event waits use 256 bounded processor-pause polls before the
existing armed, epoch-checked timed sleep. Busy consumers avoid repeated kernel
sleep/wake cycles; idle workers still sleep and service deadlines.

`latte_worker_bench.cpp` exercises production transport, registry and owned arena
bytes with 1,707 index misses and 5,000 draws/frame for ten frames. Native D3D12
allocations/fences are modeled. It verifies draw/upload totals, uploaded values,
ordered destruction and generation recycling; front synchronizes only at the frame
boundary in the asynchronous case. Compiling the same workload against the old
header with the old create/release reply policy provides the host comparison:

| Worker churn | Before | After |
| --- | ---: | ---: |
| Wall time / 5,000 draws | 19.5231 ms | 2.3174 ms |
| Index create/release replies / frame | 3,414 | 0 |
| Reply wait / frame (includes final frame drain) | 15.6250 ms | 0.0644 ms |
| Capacity wait / frame | 0.0688 ms | 0.0237 ms |

### Validation and remaining console check

The updated worker workload passes ASan/UBSan and TSan as well as the existing
worker suite. A new deterministic batching test holds a 512-operation burst,
requires at most 18 reverse messages, retrieves an exact reply inside the burst,
and checks the final consumed prefix. Existing tiny-ring/reverse-saturation,
shutdown, failure, exclusive handoff, query/result ticket and ownership tests pass.

Logs in the existing build tree:

- `build/latte-console-before-index.log`, `latte-console-before-pipeline.log`:
  pre-repair repeated-index and packet-handoff measurements.
- `build/latte-console-final-pipeline.log`: ASan/UBSan and TSan transport,
  asynchronous index lifetime workload, batch and existing worker coverage.
- `build/latte-console-final-regression.log`: full profiler/cache/index/descriptor/
  root-CBV/texture-stage/PM4 suites, including Rosetta x86 SIMD, relocation and
  repeated-stream gate.
- `build/latte-console-final-benchmark.log`, `latte-console-final-worker-before.log`,
  `latte-console-final-worker-after.log`: optimized final host measurements above.
- `build/latte-console-build-final.log`: successful requested
  `nice ninja -C build/local-uwp-game -j4` build/link after waiting for the
  machine-wide Ninja slot.

No extra build tree, console deployment, dev server or simulator was used. Host
stub timings do not measure native D3D12 buffer-sync or uniform GPU allocations.
A fresh console package must still establish mode 0 within 2% of the 46 presents/s
baseline, mode 1 <= 1 ms/frame overhead, mode-2 throughput and visual/query/readback
parity. These console thresholds are **not verified by host tests**.
