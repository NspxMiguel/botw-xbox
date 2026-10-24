# Latte pipeline, phase 6

This round preserves exact rendering decisions while reducing descriptor and PM4
bookkeeping, rejecting changed index candidates earlier, and removing unnecessary
front waits for nonblocking submissions. The console's approximately 15 ms/thread
target is not established by host tests. Unchanged guest index buffers still need
complete validation; this round does not claim to eliminate their 6.26 MB/frame.

## Recording and reply attribution

The old `other` value subtracted only eleven worker categories. It also included
known work in `DrawOther`, texture uploads, buffer uploads, native creation,
release, copies, queries, readbacks, and operation transport. It was not a single
7.09 ms function that could be identified by its name.

`Latte back slots (exclusive ms/frame)` now prints every nonzero worker slot,
including future or unexpected categories, and the residual against
`back-record-wall`. New categories include:

- `draw-setup`, `resource-pin`, and `transport`;
- `backend-create`, `backend-release`, `backend-upload`, `backend-copy`,
  `backend-present`, `backend-query`, `backend-readback`, and
  `backend-maintenance` (including worker idle servicing);
- `descriptor-prepare`, `descriptor-samplers`, `descriptor-lookup`, and
  `descriptor-copy`.

The existing `descriptors` field remains an aggregate of its exclusive parent
and four new children, on both actors. The new worker-slots line is strictly
exclusive: do not add that aggregate to its children. `descriptor-copy` includes
ring allocation, staging descriptor preparation, native copy, and cache insertion.
Existing `descriptor-miss/frame` counts tables actually copied; cache hits do not
copy GPU descriptors. Ring generations, source generations, and submission
retirement remain necessary and unchanged.

`Latte replies (ms/frame)` separates `create`, `release`, `present`, `submit`,
`readback`, and `other`. The last group contains explicit front-result operations
such as UI acquisition/rendering, BeginFrame, SurfaceCopy, debug snapshots,
query flush, and shutdown. Direct readback waits are included in `readback`.
These are elapsed front waits, not worker CPU times.

`SubmitCommandList(false)` and `Flush(false)` no longer request or await a reply.
They carry no CPU result, and FIFO recording preserves their ordering relative to
uploads, draws, later submissions, readbacks, and shutdown. `waitIdle == true`
still blocks. Readbacks, query flushes, presentation, resource creation, and
resource destruction retain their synchronization. In particular, native release
registers a stack-owned COM adapter, texture creation is inspected by front upload
and readback code, and framebuffer creation supplies front pipeline formats;
removing those waits without transferring ownership/data would be unsafe.
`async-submit/frame` exposes the operations changed here.

Profiling adds clock reads only when enabled. The next hardware run must establish
whether `transport`, `backend-maintenance`, or any residual exceeds 1 ms and needs
another split. A smaller reported residual alone is accounting, not a speedup.

## Descriptor and register work

The front's translated stage layout now carries a monotonically increasing
identity within its renderer. A changed uniform/vertex binding still creates a
new immutable draw block, but copying it retains texture/sampler spans already
present under that identity. Dynamic uniform ranges and bytes always copy.
Changed shaders and sequence boundaries issue new identities; identity zero
keeps the full-copy fallback for manually built blocks. Full value equality
remains independent of the identity. The protocol test changes dynamic bindings
on 2,000 draws and rebuilds layouts every seventeen draws, comparing all active
values against the source after each copy. This adds about 24 KiB to the bounded
1,024-block pool.

The worker borrows sampler arrays directly from its immutable draw block instead
of copying them into, and zeroing, temporary sampler arrays on every draw. The
block remains live until the current operation is consumed, including across a
sampler heap rollover. A sampler-table memo uses the block's generation-bearing
identity and sampler-heap generation to skip repeated full sampler comparisons.
Changed block IDs fall back to exact descriptor equality; direct/internal passes
use identity zero and always compare values. Rollover invalidates all hits.

Stage-table hashing uses four independent accumulation lanes instead of one
serial dependency chain across all CBV and SRV fields. Full-key equality still
checks hash collisions. Tests force a full 64-bit hash collision, as well as
bounded probe-chain collisions and source/heap generation changes.

The register decoder already uses template packet handlers and direct register
arrays; there is no per-packet virtual dispatch or register map to replace.
`StoreBigEndianWords` now specializes packets with up to four unshadowed words,
avoiding the general vector-loop setup/tail path. Shadowed writes retain their
original ordered scalar handler. Longer packets retain the existing vectorizable
loop. Host parity exercises every length through 67, unaligned inputs, unchanged
values, and mutations, including the x86 conversion/integration suites.

## Exact index validation

Both the owning address-bank lookup and relocated-address hints still called
`ContentSnapshot::Matches`, which compared the complete CPU shadow. Round 5 only
bounded the content-directory fingerprint; it deliberately did not make samples
an equality authority. An address hint and content-directory lookup could also
compare the same failed candidate twice.

This round compares four 8-byte samples first for inputs of at least 256 bytes.
A mismatch rejects a candidate immediately. Matching samples still require the
complete byte comparison, so changes outside the samples cannot reuse stale
converted indices. A bounded list prevents repeating a failed comparison through
the owning bank, address hint, and content directory in one lookup. Its bound
covers both the four-way persistent bank and eight-entry transient bank.

The production relocation test changes an unsampled byte in a 64 KiB source and
checks that only one full 65,536-byte comparison occurs. The complete snapshot test
mutates every byte of a 1 KiB source, checking sampled and unsampled rejection.
Transient stress repeatedly mutates one source through all eight slots.

`index-address-compared` and `index-relocated-compared` split the total comparison
counter. The counted amount is the full span offered to an exact comparison (an
upper bound if libc exits early), or 32 bytes on a sampled rejection.
`index-sample-reject/frame` counts the latter. Successful sampled prechecks add
32 bytes of reads beyond the reported full comparison span.

There is still no comprehensive guest write-generation authority. Generated PPC
stores and HLE/raw pointer writes bypass flush hooks; the existing diagnostic
page watcher opens a page and reprotects it later. It cannot certify unchanged
bytes between draws. Broad page protection also needs coordination with guest
I/O destinations. Skipping exact checks based only on matching samples or existing
flush hooks would violate the required middle-only-mutation parity. The console
counters now distinguish the remaining address and relocated costs explicitly.

## Host validation

Host microbenchmarks measure bookkeeping only, not Xbox driver or frame times.
The machine was concurrently compiling elsewhere, with load around 13–17 and
about 12 GiB of swap in use. Nine alternating before/after windows give these
component medians; baseline headers/sources were extracted into this worktree's
ignored build directory, without creating another checkout.

| Host workload                                         |       Before |       After |
| ----------------------------------------------------- | -----------: | ----------: |
| Stage-table hash                                      |    51.648 ns |   20.710 ns |
| Stage-table lookup                                    |   102.887 ns |   44.452 ns |
| Short PM4 packet, 2–4 words                           |     2.404 ns |    1.142 ns |
| Reject a changed tail in 64 KiB of indices            | 2,316.270 ns |    1.515 ns |
| Sampler memo: exact values / immutable identity       |    16.863 ns |    1.407 ns |
| Copy stable stage layouts with changing uniform range |    62.366 ns |   23.294 ns |
| Nonblocking-submit transport, 5,000 draws             |  0.855058 ms | 0.868354 ms |
| Its front reply wait                                  |  0.271229 ms | 0.055479 ms |

The submit probe records exactly the same ordered draws/submissions and changes
only whether seven nonblocking submissions request replies. An explicit final
blocking submission remains in each frame: replies fall from eight to one.
It does not model native driver work. Wall time did not improve in this run;
the reduced reply wait alone is not a frame-time claim. Sampler probes use an
opaque function boundary to prevent the compiler folding the exact comparison.
The stage-copy probe calls the production layout producer and copy policy.

Unchanged index hits do not establish a speedup. The same paired run measured
64 KiB owning-address hits at 2,237.76 → 2,362.82 ns, and relocated hits at
2,588.43 → 2,421.65 ns. Small relocated hits also regressed (16 bytes:
30.32 → 43.34 ns; 1 KiB: 77.87 → 91.56 ns). The index work retained 100% hit
rates after warmup and one upload across 100,000 relocated calls. Sampled
rejection and duplicate avoidance help changed candidates, not unchanged bytes.

The unchanged 1 ms transport gates are not relaxed. In adjacent 200-frame runs
of the original workload, both baseline and this implementation failed the
changing-binding gate under contention:

| Transport overhead per 5,000-draw frame | Baseline wall / CPU | Implementation wall / CPU |
| --------------------------------------- | ------------------: | ------------------------: |
| Repeated binding blocks                 |  0.7583 / 0.7229 ms |        0.6456 / 0.6017 ms |
| Changing binding blocks                 |  1.1682 / 1.1088 ms |        1.1841 / 1.0573 ms |

An earlier implementation run also failed at 1.3528 / 1.1726 ms for changing
bindings. The benchmark's manually built blocks have no producer identity, so
retain the exact full-copy fallback. The separate stage-copy workload exercises
the new production layout identities without changing the existing gate.

After the active builds finished, the final release benchmark passed both
unchanged wall/CPU median gates. This comparison uses the initial pipe66 host
baseline and final pipe67, each with nine paired windows of 200 measured frames:

| Transport overhead per 5,000-draw frame | Initial baseline wall / CPU | Final release wall / CPU |
| --------------------------------------- | --------------------------: | -----------------------: |
| Repeated binding blocks                 |          0.5247 / 0.5163 ms |       0.4857 / 0.4827 ms |
| Changing binding blocks                 |          0.9695 / 0.9464 ms |       0.8750 / 0.8645 ms |

The timing windows had different machine contention; this is acceptance evidence,
not a controlled console speedup. Both earlier failures remain recorded above.
`pipe67-release-transport.txt` and its status file record the final pass with the
4,704-byte block layout. Draw/command parity, workload, and thresholds are retained.

The host correctness suites pass with ASan/UBSan, including native and Rosetta
x86 index conversion/integration, sampler generations, exact hash collisions,
index mutations, transient-bank churn, state lifetimes, and queue pressure.
The pipeline, index-worker, and nonblocking-submit protocol probes also pass
TSan. The last stage-layout changes passed the complete ASan/UBSan and TSan
protocol run again. No sanitizer timing is compared with optimized timings.

Reproduce with:

```sh
bash tools/tests/run_latte_perf6_tests.sh
LATTE_PIPELINE_TSAN=1 LATTE_PIPELINE_BENCHMARK=0 bash tools/tests/run_latte_pipeline_tests.sh
LATTE_PIPELINE_BENCH_FRAMES=200 bash tools/tests/run_latte_pipeline_tests.sh
```

The third command includes the unchanged timing gates; the final release run
passes, while the earlier contended runs above returned nonzero. Logs are local under `build/`: `pipe67-final-host.txt`,
`pipe67-final-protocol.txt`, `pipe67-stage-protocol.txt`,
`pipe67-sampler-validation.txt`, `pipe67-paired-host.json`,
`pipe67-sampler-bench.txt`, `pipe67-stage-copy.txt`, and the
`pipe67-{baseline,final}-transport.txt` files.

## Console checks

Use the same Great Plateau scene, mode 2, and `latte-profile.txt`. No detail flag
is required for the new exclusive worker and reply lines.

1. Read `Latte back slots` alongside `back-record-wall` and `back-busy`. Look for
   the largest named operation and a residual below 1 ms. Any larger residual,
   transport, or maintenance bucket needs another hardware-led split.
2. Compare descriptor preparation, samplers, lookup, and copy separately. Look
   at the existing descriptor hit/miss counts before attributing 3.4 ms to actual
   native table copies. Sampler and layout reuse must survive generation resets.
3. Read reply categories with `async-submit/frame`; nonblocking submit calls
   should no longer contribute to `submit` reply time. Essential blocking
   readbacks, resource lifetimes, queries, and presentation must still progress.
4. Read owning-address versus relocated comparison bytes, sample rejections,
   and index-content misses/evictions. Unchanged exact comparisons remain.
5. Compare `draw-state` and `pm4`, identical draw counts/parameters, restart and
   converted indices, scene output, and resource/fence progress. Repeat modes
   0 and 2 without profiling. Added instrumentation itself changes measured CPU
   time; host microbenchmarks do not establish console parity, FPS, or the
   approximately 15 ms target.

## Commits

- `a375da8`: pipeline timers, reply attribution/nonblocking submissions, descriptor
  hashing and sampler reuse, short PM4 decode, exact sampled index rejection,
  duplicate avoidance, and host coverage/probes.
- `217b07d`: retain translated stage spans across dynamic binding changes, with
  identity invalidation, value parity checks, and the stage-copy benchmark.

No push or console deployment is performed. Other worktrees are not modified.

## UWP build and signed package

`nice ninja -C build/local-uwp-game -j4` completed and linked `BotWRecomp.exe`.
The build waited for the previous Ninja before starting. Its log is
`build/pipe67-build-final.txt`; source/header modification times precede the
compiled objects and final linked executable. The existing generated guest code
and build tree were retained.

```sh
APPX_OUT=pkg/pipe67.appx sh pkg/repack.sh 1.2.1.0
osslsigncode verify -CAfile pkg/test.crt -in pkg/pipe67.appx
```

The signed package is `pkg/pipe67.appx`, version **1.2.1.0**, x64,
**517,027,010 bytes** (493.1 MiB). Signature verification succeeds. Inspection of
`AppxManifest.xml` confirms the version, and streaming SHA-256 verification
confirms that the packaged executable matches the final linked executable:

```text
ac92d55210e135c5a7f7f20b58db613d3423271f49dfb77722e22c923d97d887
```

APPX SHA-256:

```text
6c249843a45ed0053ee7b5bb43bfeead7a1b0d3539055ae7fc0b6adc167556ac
```

Metadata is retained in `build/pipe67-package-verification.json`, and signature
output in `build/pipe67-signature.txt`. Only the nineteen verified staging files
were removed after packaging; both `pipe66.appx` and `pipe67.appx`, build outputs,
and generated sources remain. No files outside this worktree were deleted.
The branch does not reintroduce any removed crypto/WUD/NUS files.

Host acceptance and packaging are complete. Console frame parity, identification
of the largest newly exposed worker bucket, residual coverage below 1 ms, and
both actors reaching approximately 15 ms/frame still require the hardware run.
The exact comparisons of unchanged guest indices remain a documented limitation.
