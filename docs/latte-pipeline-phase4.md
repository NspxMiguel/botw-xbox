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
| First texture/FBO/index/UI allocation and the first query heap | Recorded reply supplies allocation success, formats or opaque identity needed by front. Existing query heaps do not add another create wait. |
| Native/logical adapter release | Release is recorded before front destroys or reuses the adapter. Native COM/upload retirement still uses GPU fences. Release does not recursively apply guest cache callbacks from inside a logical destructor. |
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
reference (~42 presents/s and ~21.7 ms/frame on the supplied baseline). Require
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
guest semaphore waits forever. Keep mode 2 opt-in until those checks pass. Removing
coarse waits, tuning batching/capacity and increasing frame latency are phase-5
work, not evidence established by this host implementation.
