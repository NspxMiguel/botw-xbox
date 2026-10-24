# Latte pipeline, phase 7

## Design before implementation

Mode 3 adds a recording coordinator and up to four persistent chunk workers.
Modes 0 and 2 retain their existing recorder. The coordinator owns resource-state
tracking, resource lifetime pins, queue submission, and the fence. Workers never
read guest memory, mutable register arrays, the resource registry, texture state,
or the front's binding caches. Each worker owns its command lists and allocators,
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

Implementation, measurements, build, and package results will be appended after
validation; this design section is committed before implementation.
