# Latte front/back recording pipeline (design only)

Status: proposal, not implemented. Scope: the BotW D3D12 backend. The OpenGL,
Vulkan and Metal command paths remain single threaded. Reprofile after the round-2
caches before assigning the final boundary.

## Goal and budget

The supplied Series X capture reports 18.532 ms/frame inclusive draw time,
2.178 ms PM4 time, 3,574 draws/frame and almost no GPU/Present waits. A plausible
partition is approximately 11 ms of guest-facing work and 8 ms of D3D12 recording.
These are planning allocations, not independently measured thread times: slots
such as render-targets and texture-bind currently mix guest and backend work.

In steady state, independent work could approach `max(front, back) + handoff`,
rather than their sum. An 11 ms front and 8 ms back leave about 2 ms of overhead
before the 13 ms CPU target. This does not imply 60 fps if game simulation,
GPU work, or synchronization becomes the next bottleneck. The first draw still
pays both stages; repeated draining can erase the overlap entirely.

At 3,574 draws/frame, 256-byte draw records carry about 0.87 MiB/frame, or
52 MiB/s at 60 fps, excluding staging bytes/state blocks. A 1,024-slot ring is
256 KiB and holds about 3 ms of front work at that draw rate. Start there and
measure occupancy; cap outstanding staging separately (initially 16 MiB). Avoid
queuing whole frames merely to show high utilization: bound input-to-present
latency and compare it with the current implementation.

## Ownership and shared-state inventory

Everything in the following table is currently reachable, directly or indirectly,
from the command thread. Introducing a second caller to `g_renderer` is unsafe.
Ownership means the only thread permitted to mutate the structure. Communication
transfers immutable values/handles and explicit completion messages.

| Structures / implementation | Owner and required split |
| --- | --- |
| PM4 ring/indirect command pointers, `DrawPassContext`, `LatteDrawcallContext`, TCL retirement and guest register shadow addresses (`LatteCommandProcessor.cpp`) | Front. Parse, endian conversion, guest register writes and draw sequencing stay here. Never enqueue pointers into mutable PM4 memory. |
| `LatteGPUState.contextRegister/contextNew`, active shader pointers, special state, draw/frame/texture counters, texture scales (`Latte.h`, `LatteTextureLegacy.cpp`) | Front. Back must not read these globals, including indirect reads from `GetOrCreate`, uniform helpers, internal shaders or diagnostics. |
| Texture lookup generation, view lookup tables, occupancy/alias relations, `sliceMipInfo`, hashes, overwrite rules, reload flags, access/update events (`LatteTextureCache.cpp`, `LatteTexture.cpp`, `LatteTextureView.cpp`) | Front owns logical textures/views and alias decisions. Separate backend resource records from these mutable objects; back receives versioned IDs. Deletion is an ordered release request. |
| `LatteTextureD3D12` resources, format descriptors, subresource states, content versions; `LatteTextureViewD3D12` SRV/RTV/DSV and slice-copy caches | Back. Creation and format-support-dependent view resolution require a reply on first use, or an earlier asynchronous create request. Front cannot call `GetSRV` because it can refresh a slice copy and record commands. |
| Texture decode/untile scratch and upload payloads (`LatteTextureLoader.cpp`, `texture_loadSlice`) | Front decodes into an owned CPU staging arena; back copies immutable bytes into its upload allocation and records copies. Preserve row/plane pitch and source order. Do not share `m_textureUploadBuffer`. |
| Buffer interval tree, `BufferCacheNode`, page hashes, `s_pageUploadBuffer`, lookup scratch, deallocation and DC-flush queues (`LatteBufferCache.cpp`) | Front owns guest mappings, hashes and logical buffer ranges. Back owns `m_bufferCache`, scratch buffer, resource states, GPU offsets/allocations. Front allocation decisions need stable reserved logical ranges and generation IDs; a range cannot be recycled while queued packets reference it. |
| Guest RAM, physical/virtual translation, CPU/DC-flush notifications, async commands (`LatteAsyncCommands.cpp`) | Guest memory remains externally mutable. Front snapshots needed bytes at the same ordering boundaries as today. External notification producers use the existing ingress synchronization; front translates notifications into ordered backend operations. A pointer plus an unchecked write-generation is insufficient. |
| Converted indices and their CPU shadows (`LatteIndices.cpp`), fetch input ranges | Front decides conversion/reuse and computes index max. Back owns uploaded index allocations and their retirement. Cache entries carry logical upload IDs instead of `IndexAllocation.rendererInternal` pointers. Eviction cannot free an allocation still referenced by queued draws or in-flight submissions. |
| `sLatteCurrentRendertargets`, `sLatteRenderTargetState`, update lists, viewport/scissor globals (`LatteRenderTarget.cpp`) | Front. Snapshot attachment IDs, slices/mips, stencil presence and dimensions. Back owns native `CachedFBOD3D12` handles. Tracking writes stays logically ordered on front; GPU content/state changes stay on back. |
| `LatteSHRC` active shaders/fetch shaders, decompiler mappings and uniform-remap descriptions | Front resolves shaders and immutable program IDs. Existing compiler workers publish immutable compiled results. Shader deletion must retain snapshots until both queue consumption and GPU retirement permit release. |
| `D3D12PipelineCache`, pipeline compile jobs, rect-emulation GS, disk/scene/preload bookkeeping | Split immutable prepared pipeline descriptions from guest state reads. Back owns PSO lookup/recording. Existing compiler pool owns compilation. Front provides complete linkage/format/fixed-state keys, including PS input linkage needed for rect GS. Publish ready PSOs with the existing acquire/release discipline; do not silently add new draw-skipping behavior. File-open scene notifications remain atomic ingress events. |
| Uniform scratch, remapped bytes, texture rescale values, streamout uniforms | Front computes immutable uniform bytes. Back allocates upload addresses, CBVs and deduplicates within submission lifetime. Never enqueue a pointer to `m_uniformScratch` or guest ALU constants. |
| `DrawState`, bound textures/vertex buffers, root arguments, sampler/view/CBV caches and epochs | Back owns native recording state. Front has a separate logical binding state. Native descriptor handles and GPU addresses are resolved on back; packet identity cannot rely on dirty flags shared between threads. |
| Staging descriptor heaps, view ring, sampler heap, upload/index/readback allocators, deferred releaser | Back, including fence markers. Heap rollover must acquire samplers before allocating transient uniform data, as the current draw path does. Front staging arenas are separate CPU memory and retire after copying/consumption, not after GPU completion. |
| `m_cmdList`, allocators, pending barriers, queue/fence/event, `m_submitSoon`, submission/completion IDs | Back exclusively. Front uses monotonic operation sequence IDs; these are distinct from D3D12 fence values. Querying a fence or requesting submit from front is a message, not a direct method call. |
| Scheduled/active texture readbacks (`LatteTextureReadback.cpp`) and `LatteTextureReadbackInfoD3D12` | Front schedules logical requests and applies guest-memory results. Back records copies, owns readback allocations and reports completed immutable payloads. `ProcessFinishedSubmissions` must stop calling front cache mutation directly. |
| Query event counters, logical query objects (`LatteQueryObject*`, Latte query core), D3D12 heap/free indices/fragments/results (`D3D12Query.cpp`) | Front owns logical begin/end/result visibility. Back owns heap slots, per-command-list fragments and resolve/fence state. Results return on a completion queue and are summed/published on front. |
| Streamout counters, ring ranges, copyback and UAV ordering (`LatteStreamout.cpp`, D3D12 streamout/buffer helpers) | Front owns GX2 streamout semantics; back records UAV writes, barriers, copies and readbacks. A dependency on generated bytes/counters may require a completed-fence reply, not merely recording acknowledgment. |
| Texture/surface copies and special draws/clears (`LatteSurfaceCopy.cpp`, `D3D12SurfaceCopy.cpp`, special states 5/8) | Front resolves guest semantics and aliases; enqueue explicit copy/clear operations in draw order. Back performs transitions and internal passes and invalidates native bindings afterwards. |
| TV/DRC scanout, swap chains, output passes, FSR/FFX, interpolation and scene-depth heuristic | Back records/presents and owns native output resources. Front sends scanout configuration and frame IDs. Service interpolation deadlines on back even when the ring is empty; window/surface changes become ordered control messages. UI ownership of CoreWindow/swap-chain attachment still follows the existing platform contract. |
| ImGui, overlays, display settings, resource-support tables, title initialization/shutdown | Copy UI data/config snapshots at frame boundaries; serialize control mutations through back. Device capability tables can be immutable after initialization. Drain before title switch, device reset, resource destruction and shutdown. |
| Profiler, performance counters, logging/device-removed diagnostics | Separate thread-local collectors. Front counts attempted draws/source frames; back counts recorded draws/submits. Correlate both by frame/sequence ranges. Do not add overlapping front/back time into the existing wall-time `covered` percentage. |

This inventory is the migration checklist. During implementation, audit every
`g_renderer` call and every back-thread helper transitively for guest-global
reads; the compiler cannot enforce ownership of existing global structures.

## Packets and lifetime contract

Use one single-producer/single-consumer operation ring, with a reverse SPSC
completion ring. Draws, resource changes, uploads, copies, query operations,
barriers/sync requests and presents share the forward order. A draw-only ring
with a separate unordered upload queue would permit a draw to precede its data.

A fixed record header contains opcode, byte count, operation sequence and source
frame ID. Keep the common draw record around 192–256 bytes by referring to
immutable state blocks rather than copying a full pipeline or 18 sampler
structures into each record. A draw snapshots or references all of:

- Vertex/pixel/geometry/fetch program IDs and immutable pipeline state: primitive
  topology/index type, blend/logic/write masks for every slot, depth/stencil masks,
  cull/fill/depth-bias/clip/raster state, linkage and attachment formats.
- Base vertex/instance, counts, instance count, converted index range/type, vertex
  buffer ranges/strides, and input-step/divisor data.
- Texture/view IDs with object generations, dimensions, subresource ranges,
  swizzles and comparison usage; complete sampler descriptions including graphic
  pack overrides and border colors. IDs resolve to back-owned resources.
- Immutable uniform blocks/variable bytes, per-stage binding mappings and runtime
  constants. Any byte needed later must already be copied out of guest RAM.
- Attachment/view IDs and generations, viewport, scissor, blend factor, stencil
  reference, streamout binding ranges/counters and special draw state.
- Ordered resource/upload dependencies, query markers and explicit feedback/copy
  requirements. A logical data version must identify which upload a draw sees.

State blocks are immutable once published and use value equality/interning on
front. Back may cache native translations by block ID plus generation. IDs are
not raw pointers: use a slot/generation pair and retain the object through its
last queued reference and last GPU fence. Queue acknowledgment only releases CPU
snapshot storage; it does not make a GPU resource safe to destroy.

Front writes a slot and release-stores the producer cursor; back acquire-loads
before reading it, then release-stores the consumer cursor. Put producer and
consumer cursors on separate cache lines and use monotonically increasing 64-bit
sequence numbers. Confirm the chosen atomic type is always lock-free on x64/UWP.
Publish immutable arena payloads with the same release/acquire edge. Batching
cursor updates is optional only if latency/deadline measurements permit it.

The data path uses no mutex, allocation or reference-count increment per draw.
Use bounded arenas and retire spans by consumed sequence. Full/empty handling
can spin briefly then use an event; the ring algorithm remains lock-free, while
waiting for capacity/work is deliberately blocking. Signals are notifications,
not ordering primitives: recheck cursors when arming/rearming events.

Always drain reverse completions while waiting for forward capacity. Back must
not synchronously call front while front is blocked publishing a request. Size
and reserve control capacity so completion publication, cancellation and device
failure cannot deadlock behind a full draw queue. A device-removed message wakes
both threads and fails every outstanding ticket.

## Synchronization and visibility

Define three different ticket levels: **recorded** (back consumed the operation),
**submitted** (a queue fence value was assigned), and **completed** (GPU fence
passed). A CPU event acknowledging consumption is never GPU completion.

| Boundary | Required front behavior |
| --- | --- |
| Ordinary independent draws and uploads | Publish and continue; immutable byte snapshots and generation-pinned resources allow overlap. Back preserves operation/barrier order. |
| First resource/view creation requiring native format/allocation information | Publish create request. Continue independent work if possible; wait for its recorded/create reply before consuming returned metadata. Prefer immutable capability tables to avoid repeated round trips. |
| `IT_WAIT_REG_MEM`, memory semaphores, GX2 draw-done/waits and bottom-of-pipe events | End the front draw sequence and publish an ordered submit/sync ticket before polling/waiting. If the condition depends on GPU output, wait for completed status and apply readbacks first. Preserve guest-visible timestamp/event publication semantics and ordering with RAM stores. A CPU-only condition need not drain unrelated GPU work. Audit each existing packet handler separately. |
| `IT_SURFACE_SYNC`, DC flush/cache invalidation | Front processes notifications and snapshots replacement bytes in order; enqueue updates after earlier uses. Reusing a GPU range in place is safe only with queue ordering and lifetime protection. A flush alone does not mandate a CPU fence wait. |
| Texture/streamout/readback result used by CPU or front alias decisions | Publish force-readback/submit, wait for completed status, copy/retile into guest RAM on front, update hashes/events, then resume. Front cannot infer fresh pixels from logical draw counters. |
| Occlusion query result polling / forced result | Poll reverse messages without blocking; force a submit when needed. Only publish after all query fragments' completion fences pass. Forced retrieval waits on completed status. |
| GPU-produced streamout buffers reused as input | Keep the dependency entirely on back when possible with ordered UAV/copy barriers. If front needs index bounds, counters or CPU hash decisions from the data, read back and wait. |
| Texture feedback / overlapping alias copies / internal depth-to-color draws | Preserve order inside back's stream; generally no CPU wait. Logical alias metadata advances in PM4 order on front. Actual resource states/content versions advance only on back. |
| TV/DRC swap, flip waits, present and interpolation | Publish frame boundary and scanout references. Wait only where the current guest flip contract requires acknowledgment; present completion is distinct from GPU data completion. Limit queued frames to the existing latency budget. |
| Resource eviction, shader deletion, sampler/view heap rollover | Front releases its logical reference by sequence. Back defers native destruction through the last GPU fence. Heap reset/submit is internal to back; no front-global binding state is touched. |
| Title switch, cache unload, output resize, device reset, shutdown | Stop publishing draws, drain through a control ticket, complete required GPU work, collect pending results, release generations, then resume or stop. Never destroy caches while packets reference them. |

Do not put a full drain at every `draw_endSequence`: context/sampler changes often
end sequences between adjacent draws. Keep render-target update events and
readback initiation in PM4 order without forcing the back to catch up unless a
CPU-visible result is actually needed.

## Implementation phases and validation gates

1. **Measure and establish ownership (3–5 days).** Capture a new stable plateau
   after round 2. Count sequence endings, resource-create replies, forced
   readbacks, query waits, submits and upload bytes. Add operation sequence/frame
   IDs and assertions for thread ownership. Preserve the current synchronous path
   as a runtime-selectable reference.
2. **Immutable packets on one thread (1–2 weeks).** Refactor `draw_execute` and
   backend resource/upload calls into prepare/record interfaces, but execute them
   immediately. Remove back-facing register/global reads. Implement stable
   resource IDs and retirement, immutable state blocks and CPU staging arenas.
   Run identical gameplay, compare frames and existing counters before adding
   concurrency. This is the largest correctness step.
3. **Bounded queue, synchronous consumer (3–5 days).** Publish every operation,
   immediately drain it on the same thread. Test wrapping, generation reuse,
   descriptor rollover, allocation pressure and shutdown with tiny capacities.
   Record ticket-level semantics and compare the emitted command sequence.
4. **Back thread behind a flag (1–2 weeks).** Move the D3D12 recorder/allocators,
   submit and present servicing to back. Initially drain at coarse explicit
   synchronization boundaries. Add reverse messages and failure propagation.
   Stress CPU writes, alias copies, queries, streamout, pause/resume and device
   removal; verify idle operation cannot stall guest waits.
5. **Recover overlap (1–2 weeks).** Remove only proven unnecessary drains,
   measure both workers' busy/idle/backpressure time and queue/staging high-water
   marks. Tune batching/capacity with a latency limit. Reassign expensive pipeline
   preparation only if the measured back exceeds front. Keep all semantic waits.

Expected production scope: **about 25–35 files, 5,000–8,000 changed/new C++ lines**,
plus **1,000–2,000 test lines** and documentation. Planning total: **6–10 engineer
weeks** including Xbox debugging, assuming the existing renderer remains stable.
Representative file groups: command processor/draw interface (3–4), logical
texture/view/loader/cache (5–7), buffer/indices/streamout (3–4), D3D12 renderer and
packet/arena/registry code (5–7), descriptors/pipeline/surface copy (3–4),
readback/query/present integration (4–6). Count ranges overlap; they are scope
estimates, not an implementation promise.

Main risks are guest/GPU visibility mistakes, resource ABA/lifetime bugs,
recorded-vs-completed ticket confusion, queue deadlocks, unexpected back-thread
register reads, sampler reset retiring uniform uploads, fragment queries across
submissions, UI/present thread contracts, and increased frame latency. Extra
CPU copying/cache traffic may consume the expected savings. Resource creation or
readback-heavy scenes may have little overlap even when plateau gameplay gains.

Required checks before enabling by default: D3D12 debug-layer/PIX captures on a
supported PC, repeatable Xbox frame comparisons at fixed checkpoints, same draw
and query counts, streamout/readback data comparisons, and UWP lifecycle stress.
Host tests cover SPSC wrap/release-acquire behavior, tiny-ring backpressure,
reverse-queue saturation, generation reuse, ticket ordering and cancellation;
ThreadSanitizer on a host model complements actual GPU tests. Report front/back
wall time and critical-path frame latency separately, including p95/p99. Do not
reuse the single-thread exclusive `covered` calculation for overlapping workers.
