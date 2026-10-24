# Latte pipeline, phases 1–3

This is the phase-1–3 completion record. See [Phase 4](latte-pipeline-phase4.md)
for the opt-in recording worker, current mode selection and validation status.

The D3D12 consumer is still synchronous and runs on the Latte command thread.
`CEMU_LATTE_PIPELINE=0` is the default and retains the reference draw implementation.
`CEMU_LATTE_PIPELINE=1` prepares immutable draws, publishes operations into the
bounded SPSC ring, and immediately consumes them on the same thread. Neither mode
creates a recording worker. This work does not enable phase 4 or change the shader,
root-signature, descriptor-table, primitive-conversion or pipeline-wait ABI.

## Ownership and preparation

`D3D12RendererCore.cpp` retains guest interpretation in `draw_beginSequence`,
`draw_execute`, `draw_prepareStage` and `draw_snapshotUniformVars`. These functions
resolve shaders/pipelines, indices, buffer-cache ranges, texture/sampler overrides,
texture scales, ALU/remapped uniforms, special states and streamout semantics.
`draw_executeDirect` retains the reference implementation. Both implementations
use the existing cache keys, native caches and submission lifetime rules.

`FrontState` owns logical texture/FBO bindings, viewport/scissor inputs, sequence
skip state and streamout bookkeeping. `FrontBindings` holds logical vertex and
uniform ranges. The recorder's `DrawState` contains native binding caches, root
arguments, GPU addresses and dirty state only. Uniform bindings carry complete logical ranges. Record resolves their native
aligned/scratch addresses before sampler acquisition, preserving the existing
unaligned-buffer copies. On the queued path, vertex/uniform/FBO/viewport/scissor
setters only prepare the complete draw block; they do not add a handoff for each
individual binding. The reference path retains its immediate native setters.

`draw_record` consumes a draw packet and an immutable binding block. It resolves
native IDs, builds SRVs/tables, acquires all sampler tables before allocating
transient uniform GPU bytes, applies transitions/attachments and records the same
root arguments and draw call as the reference. Pipeline selection, rect-GS linkage
and the existing shader wait/skip policy remain on prepare. Native pipeline
objects are immutable after the existing compiler publication edge.

`RecordBackend` dispatches ordered creation/release, uploads, vertex/uniform/FBO
bindings, viewport/scissor, copies, clears, streamout barriers, submission,
sequence boundaries, present, screenshot, UI and shutdown-drain operations.
Nested native helpers execute inside their containing operation; they do not
recursively publish another operation. Guest-only logical setters are represented
by the subsequent complete draw snapshot, rather than redundant GPU operations.

Texture creation prepares the descriptor/format/pack decisions on front and
records native allocation separately. Textures and views have frozen
`NativeDefinition` values; native helpers no longer read their mutable inherited
cache metadata. FBOs freeze their attachment references separately from the core's
logical FBO fields. Native resource/view destruction is ordered, followed by ID
retirement; existing COM/resource fence deferral is preserved. Buffer-cache
recreation also defers the previous native buffers.

Texture decoding writes front-owned scratch. Upload preparation splits immutable
rows/planes into bounded arena chunks, including front-side depth/stencil
separation. Uniform bytes and converted-index upload bytes are copied into the
same CPU transport arena. Index conversion uses a reusable front CPU scratch area;
`IndexAllocation.rendererInternal` is only the legacy front interface adapter to
a generation ID and a native allocation reply. Packets never contain that pointer,
guest RAM pointers, mapped upload pointers or a pointer to uniform scratch.
After upload, the adapter's temporary `mem` pointer is cleared. GPU allocation
release still goes through the index allocator's submission retirement.

Query begin/end/poll/reset/attach/detach run as ordered operations; heap slots,
fragments, active native queries and resolves remain back-owned. Polls do not
turn into waits. Forced retrieval retains its existing submit/fence wait. Readback
start/poll/force/data operations likewise return synchronously after recording;
only completed-fence results become guest-visible. `ProcessFinishedSubmissions`
now requests front completion processing instead of calling the texture cache
from inside record. Front applies those completions after consumption. Surface
copy write tracking and streamout guest bookkeeping also remain on front.

Output uniforms (including SRGB/config inputs and both scaling resolutions) are
prepared before recording. UI draw lists are cloned on front and consumed as an
immutable snapshot; UI textures use generation handles and chunked CPU payloads.
Font-atlas/context accesses and screenshot conversion/callbacks stay on front.
UI sampler rollover is handled before allocating its transient vertex/index data.

## Packet, queue and lifetime contracts

| Item | Layout / capacity |
| --- | --- |
| Header | 24 bytes: opcode, payload byte count, reserved word, 64-bit operation sequence and front source-frame epoch |
| Packet | 256 bytes, 16-byte aligned; header, arena end, static typed record thunk, 208-byte inline payload |
| Draw payload | 192 bytes; pipeline/FBO/index/block IDs, dynamic render state, counts/base arguments and CPU byte spans |
| Immutable draw block | 4,680 bytes on the host/x64 layout; three stage mappings/samplers/views and sixteen logical vertex bindings |
| Binding-block pool | Four generation slots; exact value comparison of active fields, replacement only after consumption |
| Forward ring | 1,024 slots by default (256 KiB); configurable down to one slot |
| CPU transport arena | 16 MiB by default; runtime range 1–64 MiB; large texture/buffer/index/UI payloads are chunked |
| Resource registry | 262,144 stable slots; approximately 8 MiB of entries plus 1 MiB reserved free-slot storage |

The thunk is process-local code, not an owning closure or a captured data pointer.
The header byte count identifies the typed payload, not a portable file format.
The draw block's identity includes texture generations, sampler contents, binding
mappings/ranges and vertex state. Dynamic stencil/blend/viewport/scissor values
and uniform/runtime byte spans stay outside the interned block. Hash equality
never substitutes for value equality.

Environment controls are read when the renderer is constructed:

```text
CEMU_LATTE_PIPELINE=0                  reference, default
CEMU_LATTE_PIPELINE=1                  publish and immediately drain
CEMU_LATTE_PIPELINE_CHECKS=1           release-build ownership checks
CEMU_LATTE_PIPELINE_CAPACITY=1         smallest production ring
CEMU_LATTE_PIPELINE_ARENA=1048576      smallest production transport arena
```

Invalid/out-of-range capacities fall back to defaults. The arena minimum covers
a complete draw's transient uniforms; host tests separately exercise 64/128/256
byte arenas and allocation failure. An upload row larger than the configured
arena fails explicitly rather than borrowing mutable guest memory.

Producer and consumer cursors occupy separate cache lines and are lock-free
64-bit atomics on the supported targets. Slot leases avoid packet copies. Payload
and state publication precedes a release store; consumption follows an acquire
load, and slot release follows recording. CPU arena spans retire by consumed
operation; they do not wait for GPU completion. An empty arena can skip a tail
before a full-capacity upload, and refuses to reset live spans.

Resource IDs are 32-bit slot/generation pairs. Binding-block IDs pack a small
slot index and a wide generation into 64 bits (62 generation bits with four
slots), so draw-frequency replacement cannot exhaust a 32-bit block generation
in a long session. Registry entry addresses never move.
Only creation/retirement allocation takes a lock; draw lookup and use accounting
are allocation-free and do not lock. Ordered release makes an ID unresolvable
immediately, but its slot cannot be reused until both recording and GPU lifetime
requirements are satisfied. Native pinning accounts for sampler-rollover fence
changes, and reuses binding-resource pins within the same block/fence epoch.
The existing upload, sampler, view-table and root-CBV cache generations remain
independent of CPU packet/state generations.

Operation sequences never stand in for D3D12 fences. `Tickets` distinguishes
recorded, submitted and completed progress. Actual fence submissions attach a
conservative prefix of already-recorded operations; a submit inside a draw does
not acknowledge the rest of that draw as submitted. Fence completion advances
only the covered submitted prefix. The "Latte profile" line includes operation,
frame and ticket progress, sequence endings, create replies, forced readbacks,
query waits, submissions and actual allocated upload bytes. Existing exclusive
single-thread timing/coverage and game-frame divisors are unchanged.

Debug builds always check ownership; release builds can enable checks explicitly.
Checks distinguish front access from record even when both execute on the same
OS thread. Busy/recursion guards check the calling actor, so a busy consumer
cannot make the producer bypass the stream. Drained owner-handoff APIs are
available for phase 4; the production path does not invoke them. No front cache callback is permitted while consuming a packet.

## Host overhead

Final optimized host results (`build/latte-pipeline-host-ownership-final.log`, milliseconds
per 5,000-draw frame):

| Binding workload | Direct wall median | Queued wall median | Paired wall overhead | Paired CPU overhead | Maximum paired wall overhead |
| --- | ---: | ---: | ---: | ---: | ---: |
| Repeated blocks | 0.8988 | 1.6993 | 0.7985 | 0.7985 | 0.8167 |
| Block changes every draw | 0.3901 | 1.2840 | 0.8968 | 0.8968 | 0.9154 |

Both workloads pass the 1 ms/frame gate for wall and thread-CPU overhead. The
paired overhead median is calculated from paired runs, rather than subtracting
the two independently sorted absolute medians.

`tools/tests/latte_pipeline_bench.cpp` uses the production packet, SPSC ring,
arena, immutable-block pool and registry. It compares direct prepared-value
recording against packet preparation/publication/consumption, with 5,000 draws per
frame, 1,024 uniform bytes plus 64 runtime bytes per draw, three stages with eight
views/samplers each, two native view resolutions, resource pinning and a simulated
submission change every 2,000 draws. It checks equal mock command results. Both
repeated bindings and a changed binding block on every draw are measured over nine
paired runs of 20 measured frames per path after five warmup frames per run. Timed paths allocate no memory.

These are host CPU handoff measurements, not an Xbox FPS prediction or a measured
saving from overlap. The full native driver/GPU work and actual game-state decode
are not emulated. The flag stays off by default. Other agents' Ninja builds caused contention during exploratory samples. The
final measurement job waited for an idle build slot and rejected samples if
competing Ninja work appeared. On macOS the benchmark sets foreground QoS only
for its own thread. Both wall and thread-CPU paired median overhead must pass
the 1 ms/frame gate; maxima remain visible in the output.

## Validation and remaining reads

- `bash tools/tests/run_latte_pipeline_tests.sh`: ASan/UBSan tests for release/acquire
  publication, packet leases, non-power-of-two wrap, capacity-one backpressure,
  concurrent arena/registry publication, generation reuse, immutable-block
  exhaustion/reuse, ticket ordering and shutdown. Mock direct/queued command
  comparisons include the actual draw payload, state blocks, uniforms, runtime
  constants, mappings and resource generations, with caller data mutated before
  record. The optimized benchmark enforces the 1 ms/frame median overhead gate.
- `bash tools/tests/run_latte_perf3_tests.sh`: existing profiler, draw caches,
  conversion/expansion, index integration/associativity, stage tables, root
  signature and actual descriptor-heap rollover/retirement tests, including x86
  SIMD through Rosetta.
- A separate `-fsanitize=thread` build runs the concurrent host model with
  `TSAN_OPTIONS=halt_on_error=1`; enable it in the runner with
  `LATTE_PIPELINE_TSAN=1`. `LATTE_PIPELINE_BENCHMARK=0` runs correctness checks
  without attempting a timing gate during other builds.
- The requested build tree and generated corpus were copied into this worktree.
  Generated text paths were relocated, stale copied Ninja dependency records were
  discarded, and unused compiler probes containing old binary paths were removed.
  A binary-inclusive scan found no `/botw-wiiu/base` references in either tree.

The initial copied-tree rebuild linked `build/local-uwp-game/BotWRecomp.exe`.
After the final ownership changes, `nice ninja -C build/local-uwp-game -j4`
again passed and linked at step `[17/18]`; its log is
`build/latte-pipeline-build-ownership-final.log`. A following invocation passed
with **no work to do** (`build/latte-pipeline-build-no-work.log`). Every Ninja
invocation waited for the machine's existing Ninja process to exit first.

Final correctness logs are `build/latte-pipeline-ownership-unit-final.log`
(ASan/UBSan and TSan, including concurrent actor ownership),
`build/latte-pipeline-host-ownership-final.log` (both overhead gates), and
`build/latte-pipeline-regression-final.log` (existing cache/descriptor/index suite).
All passed. `git diff --check` passed. No simulator, dev server or recording worker
was started, and no changes were pushed.

The queued draw/transfer/query/readback/native-resource record path has no
`LatteGPUState`, PM4, guest-memory, active-shader lookup, texture-scale, render-target
cache or remapped-uniform reads. Those remain on prepare or in the intentionally
retained reference draw path. Backend helpers use frozen texture/view definitions.
There are still intentional non-guest global interfaces: shader compiler/cache
publication, immutable process configuration, atomic host display/FFX controls,
pad-close ingress, and the existing atomic Latte idle diagnostic counter. UI
custom callbacks remain an external synchronous callback contract; no application
custom draw callbacks are registered in this checkout. This is not a claim that
arbitrary callbacks may access front state from a future worker.

Hardware verification remains necessary: D3D12 debug-layer/PIX validation, fixed
Plateau pixel comparison for both flags, draw/query counts, real streamout/readback
bytes and UWP lifecycle/descriptor-pressure testing. No console deployment or
frame-parity measurement was performed as part of this host-only worktree task.

## Exact phase-4 work

1. Move `Drain` and native recorder/allocator/submission/present servicing to a
   back worker; rebind the back owner there. Keep PM4, logical caches, immutable
   preparation and all guest-result application on front.
2. Separate publication from the current immediate recorded acknowledgment. Add
   reverse completions for create replies, query/readback/control results, fence
   prefixes, free IDs and profiler snapshots. Preserve recorded/submitted/completed
   distinctions; never substitute an event acknowledgment for GPU completion.
3. Add capacity/work events with cursor rechecks, completion draining under
   backpressure, reserved control/completion capacity, cancellation and device-loss
   wakeups/failure for every pending ticket. Extend failure/saturation tests and
   run the threaded model under TSan.
4. Keep explicit recorded replies for first native allocations, release before
   deleting a logical adapter, UI snapshot lifetime, and output/control replies.
   Keep completed-fence replies for CPU-visible readbacks, queries and guest waits.
   Preserve PM4 wait/semaphore/timestamp publication ordering at their existing
   call sites. Transport immutable result bytes to front before mutating caches.
5. Serialize title/cache unload, resize/surface changes and device/shutdown controls
   through the worker while preserving the CoreWindow/UI contract and existing
   pad-close ingress. Drain before tearing down frontend objects; keep native COM
   resources alive through their GPU fences. Define the synchronous handoff for
   any future UI custom callback rather than invoking front code from back.
6. Replace shared synchronous reply fields/progress observations with completion
   messages. Separate front/back profiler collectors and CPU-time/idle reporting;
   the current exclusive covered percentage must not add overlapping worker time.
7. Service interpolation/present deadlines while the ring is empty. Bound queued
   frames to the current latency contract. Run the hardware parity/lifecycle
   checks above before relaxing coarse waits or enabling the mode by default.

Phase 5 can then remove proven unnecessary waits and tune occupancy/batching. It
is deliberately not part of this change.
