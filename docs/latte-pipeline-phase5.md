# Latte pipeline, phase 5

The pipeline now starts from the rewritten `xbox/port` history. The new branch is
`xbox/latte-pipeline-v2`; the original branch remains unchanged. No push was made.
The UWP build and signed package are complete. Acceptance item 1 is only partially
met: correctness and component benchmarks pass, but the changing-binding transport
benchmark still exceeds its unchanged 1 ms wall/CPU gates on this busy machine.
Console frame parity and the 60 fps target still require a hardware run.

## Replant

The following pipeline commits were cherry-picked in their original order:

| Original | Replanted |
| --- | --- |
| `fad87b7` | `626d71c` |
| `27e3bb3` | `55d31b7` |
| `c3f3091` | `e80bfc1` |
| `f169242` | `78f5e5a` |
| `87d001b` | `6975294` |
| `91c116c` | `f502f7b` |
| `680e6e3` | `186b5a2` |
| `f558608` | `a30b590` |
| `ec44e94` | `ce58b5e` |
| `d567537` | `f9bff5d` |
| `9d256cf` | `aff50b5` |
| `f05eeb1` | `b47c2ba` |
| `ff6e602` | `a6d782e` |
| `ff76f94` | `d5e8af4` |
| `1c2ca79` | `b31a5ad` |

The abort/terminate logging and removable-drive rescan commits were skipped:
subjects and stable patch IDs match `ab1c9fd` and `e161463` on port. FSR conflicts
preserve the port's opt-in selection and NativeAA behavior; the immutable output
packet carries the front-selected NativeAA decision. The portable profiler
configuration on port is also preserved. `build/` and `generated/` were retained.

Verification:

```sh
git merge-base --is-ancestor xbox/port HEAD
git diff xbox/port --name-only | rg -i 'crypto|wud|nus'
```

The first command succeeds; the second produces no matches. The removed extractor,
crypto, and associated test files are absent from the new branch's tracked tree.

## Worker changes

- Worker exclusive slots cross the existing profile completion, separately from
  front slot coverage. The report includes descriptors, root CBVs, vertex buffers,
  render state, barriers, draw calls, submission, uniforms, render targets,
  pipeline binding, root signature, and residual recording time.
- Registry retirement visits only released resources awaiting CPU/GPU completion.
  Empty retirement does not take the allocation mutex. Live scene resources no
  longer extend that critical section and block front-side index registration.
  Reclaimed generations still require the reverse completion before front reuse.
- Index native creation and the first upload share one immutable packet. Split
  uploads still work when an allocation exceeds staging capacity. This removes
  one operation per index miss without adding synchronous replies.
- Permanent zero-CBV and main buffer-cache GPU addresses are captured at native
  creation, rather than queried repeatedly through COM on draws.
- Per-operation worker recording clock reads run only with the Latte profiler
  enabled. Fence, reply, capacity, idle servicing, and failure behavior are retained.

## Index validation and the mode-2 cost

The measured mode-0/mode-2 logs compare about 6.36/6.23 MB per frame: mode 2 does
not perform twice the index byte work. The `indices` slot is elapsed front time,
including publication, polling, registry allocation contention, and waits inside
index creation/upload/release. Worker migration also adds a conversion scratch
and staging copy where direct mode writes upload memory. Sparse retirement and
fused create/upload remove concrete sources of that extra cost; the exact
2.94-to-5.66 ms attribution requires the next console measurement.

`DCFlushRange`, `GX2Invalidate`, PM4 surface synchronization, and the buffer cache's
flush queue exist, but do not observe all guest stores. Generated stores write
through `ppc.h` directly. The platform's existing protected-page write watcher is
diagnostic and reports unavailable on UWP (`BOTW_HAS_VEH == 0`). None of these is
a sound universal page-generation authority for this cache.

Relocated addresses now retain bounded non-owning address hints, avoiding repeated
fingerprinting of a previously found source. A content lookup fingerprints at
most four 8-byte samples; a candidate always passes complete shape and CPU-shadow
comparison. Stale hints, collisions, and mutations outside the sampled locations
cannot return stale converted indices. The owning cache's memory bounds are
unchanged; hints add approximately 160 KiB.

Full comparisons remain intentionally exact. Skipping them on matching samples
alone could miss a middle-only write and change rendering. Reducing the remaining
`index-compared` bytes safely requires comprehensive write tracking, rather than
assuming that flush hooks cover every store. The sampled fingerprint accelerates
candidate discovery without weakening draw correctness.

## Front draw preparation

The command processor ends a draw sequence when texture, sampler, or context
registers change. Stable texture/sampler layouts are therefore translated once
per sequence and shader identity. The persistent front block retains translated
fields on a memo hit, avoiding another full layout copy. Uniform ranges and
uniform bytes stay dynamic.
Texture rebinding/release also explicitly clears the memo.

Changed vertex/uniform bindings invalidate the prepared immutable draw block.
Unchanged bindings reuse its ID and extend its last-use sequence; no large block
comparison or reconstruction is required. Shader changes and sequence boundaries
invalidate reuse. Viewport, scissor, blend/stencil values, runtime constants,
index selection, draw counts, and uniform bytes are captured for every draw.
Changed blocks copy only active stages, textures, and samplers; equality and the
recorder read the same active spans. The bounded slot pool and CPU/GPU lifetime
rules remain intact.

## Implementation files and commits

| Files | Change |
| --- | --- |
| `LatteProfiler.h`, `LattePerformanceMonitor.cpp` | Separate worker categories, front barrier/root-CBV slots, block-reuse counter |
| `LattePipeline.h` | Sparse retirement, explicit immutable reuse, active-copy policy, disabled-profile clock bypass |
| `D3D12Renderer.cpp`, `D3D12Renderer.h`, `D3D12RendererCore.cpp`, `D3D12DrawPacket.h` | Fused index upload, cached GPU addresses, sequence layouts and binding mutation tracking |
| `LatteContentIndex.h`, `LatteIndices.cpp` | Bounded fingerprints and relocated address hints |
| `tools/tests/latte_*`, `tools/tests/run_latte_pipeline_tests.sh` | Regression coverage and host benchmarks |
| `.gitignore` | Local packages and staging excluded |

| Commit | Purpose |
| --- | --- |
| `a01fa0a` | Separate worker recording categories |
| `065d13a` | Sparse retirement, fused index create/upload, cached GPU addresses |
| `89c42c3` | Bounded fingerprints and relocated hints |
| `7dc72f5` | Index benchmark formatting |
| `d03d2e1` | Sequence-scoped stage layouts |
| `97556da` | Front timing fields and package exclusions |
| `2204a3b` | Recording clocks disabled with the profiler |
| `602abef` | Immutable block reuse and active-span copying |
| `3d04459` | Barrier clocks only for emitted batches |
| `58c7e34` | Optimized benchmark parity remains checked |
| `98a1ec3` | Retain stable translated fields without copying them again |
| `f1d56d3` | Optional longer timing windows; unchanged workload and gates |

## Host validation

Host microbenchmarks isolate CPU bookkeeping and do not predict Xbox FPS. This
machine concurrently ran other compilation jobs (load around 30, about 11.8 GiB
swap in use), so wall timings are noisy. Do not compare sanitizer timings with
optimized benchmark timings.

Initial component measurements:

| Workload | Before | After |
| --- | ---: | ---: |
| Sparse registry retirement: 60,000 live, 300 released, 7 collections/frame | 0.596153 ms | 0.002875 ms |
| Relocated 1,024-byte indices | 242.900 ns/call | 61.308 ns/call |
| Relocated 65,536-byte indices | 33,709.700 ns/call | 2,319.090 ns/call |
| Repeated index stream, 5,000 draws | 0.117921 ms | 0.075128 ms |
| Synthetic stage translation, production sequence memo, 5,000 stages | 0.289673 ms | 0.198967 ms |

The index stream retained 100% hits with zero uploads after warmup; relocated
benchmarks upload once in 100,000 calls. The synthetic stage benchmark checks
matching dynamic bindings and is not the actual shader/sampler translator.

The final component reruns measured sparse retirement at **0.004623 ms/frame**;
split/fused asynchronous index stress at **3.19138/2.84053 ms per 5,000 draws**;
and **10,121/8,414 operations/frame**, with zero synchronous index replies in
both paths. The profiling-on stress run also passed, including its recording-clock
assertion. These are modeled native allocations, not Xbox driver measurements.

Final transport comparison (wall overhead / thread-CPU overhead in ms per
5,000-draw frame, user-initiated QoS):

| Binding workload | Replanted baseline | Latest implementation | Gate result |
| --- | ---: | ---: | --- |
| Repeated | 1.0500 / 1.0655 | 0.5061 / 0.5051 | Pass |
| Changes every draw | 1.2104 / 1.1349 | 1.2331 / 1.1517 | **Fail** |

Both use nine paired windows of 200 measured frames after five warmup frames.
Other jobs remained active. An earlier paired implementation run measured
0.5467/0.5274 ms for repeated blocks and 1.0500/1.0200 ms for changing blocks;
the latter still failed. Default 20-frame runs also failed the changing-block
gate. The results do not establish a changing-block speedup under this contention.
No timing threshold was relaxed. The final synthetic stage translation probe
measured 0.177521 ms rebuilding versus 0.074188 ms retaining the layout.

`LATTE_PIPELINE_BENCH_FRAMES` optionally selects 20–2,000 measured frames per
window; the default remains 20. The workload, warmup, nine paired windows,
command equality check, and both 1 ms gates remain unchanged. Longer windows
reduce scheduling noise but did not resolve the failed gate here. Re-run it in
a quieter session before treating acceptance item 1 as complete.

Validation logs remain local under `build/`: `pipe66-validated-indices.txt`,
`pipe66-validated-pipeline.txt` (ASan/UBSan, TSan, then the failed timing gate),
`pipe66-last-timing.txt`, `pipe66-final-baseline-long.txt`, the
`pipe66-worker-*-final.txt` files, `pipe66-registry-final.txt`,
`pipe66-build.txt`, and `pipe66-signature.txt`.

Reproduce correctness and optimized measurements with:

```sh
bash tools/tests/run_latte_perf5_tests.sh
LATTE_PIPELINE_TSAN=1 LATTE_PIPELINE_BENCHMARK=0 bash tools/tests/run_latte_pipeline_tests.sh
LATTE_PIPELINE_BENCH_FRAMES=200 bash tools/tests/run_latte_pipeline_tests.sh
```

The last command currently returns nonzero at the changing-binding timing gate;
the correctness-only command passes.

The suites cover index conversion (native and x86 SIMD), restart, mutation,
unsampled-byte/hash collisions, stale hints, bounded retention, descriptors,
root signature, profiler transport/reset, sequence layout invalidation, immutable
block reuse/lifetimes, sparse retirement, queue pressure, and worker shutdown,
handoff, cancellation, and failure.

## Console run

Use `LocalState/latte-pipeline.txt` containing `2`, plus `latte-profile.txt`, and
repeat the Great Plateau benchmark. Compare:

1. `back ms/frame` categories against `back-record-wall` and `back-busy`. The
   category timers are exclusive; `other` is the residual recording wall time,
   including transport and uncategorized work. OS CPU busy excludes preemption.
   Root CBVs and barriers are new splits of previously broader front slots;
   compare combined categories when relating the new front line to package 65.
2. `indices`, `index-hashed`, `index-compared`, and `index-miss`. Hashing should
   shrink; exact byte comparisons still remain. Misses now publish one fewer
   packet. Compare `front-backpressure` and `front-reply-wait` as well.
3. `draw-state`, `texture-bind`, `descriptors`, `texture-stage-hit/miss`, and the
   new `draw-block-reuse/frame` counter. Dynamic binding changes still rebuild.
4. The same draw count, draw parameters, scene output, and resource/fence progress.

Also compare modes 0 and 2 with profiling disabled. The extra category clocks
change instrumented time; neither host bookkeeping nor instrumented timings
establishes a 60 fps result.

## UWP build and package

`nice ninja -C build/local-uwp-game -j4` completed successfully and linked
`BotWRecomp.exe`. It waited for the other
Ninja jobs before starting. The existing generated guest code was retained.

The supplied parent `pkg/repack.sh` was copied into this worktree's ignored
`pkg/` directory, with only the `base/` source/build prefixes changed to this
worktree. Its version substitution and packing/signing commands were retained:

```sh
APPX_OUT=pkg/pipe66.appx sh pkg/repack.sh 1.2.0.0
osslsigncode verify -CAfile pkg/test.crt -in pkg/pipe66.appx
```

The signed package is `pkg/pipe66.appx`, version **1.2.0.0**, x64,
**517,008,007 bytes** (493.1 MiB). Signature verification succeeds. Inspection of
`AppxManifest.xml` confirms the version, and the packaged executable's SHA-256
matches the just-linked executable:

```text
10f6afa3f73db0f67c46a3ebf88753be55d5ee8c697da609942111ac32c2a604
```

Only the 19 verified local staging files were removed after verification. The
signed package, `build/`, and `generated/` remain. Nothing outside this worktree
was deleted. No console deployment, simulator, or development server was started.
