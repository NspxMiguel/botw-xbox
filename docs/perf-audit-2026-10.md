# BotW 60 fps audit outside the Latte command thread

Date: 2026-10-24. Target: Xbox Series, UWP/D3D12. Host: Apple M3 Pro,
arm64 macOS, Apple clang 21.0.0. UWP compiler: Homebrew clang-cl 23.1.3,
Release, `/O2 /arch:AVX2 /fp:strict`. No console was contacted.

## What the evidence establishes

There is no new gameplay CPU/GPU capture in this audit. It does **not** establish
that the guest CPU, audio, or HUD is the current bottleneck, or that these changes
achieve 60 fps. Earlier [Latte round 3](latte-perf3.md) evidence reports 37.2
presents/s and 23.44 ms/frame inclusive draw work in one scene. That is historical
command-thread evidence, not a measurement of the components audited here.

The 60 fps source-frame budget is **16.667 ms**, with 33.333 ms at 30 fps.
Guest workers, AX, the renderer, and the GPU overlap: adding their CPU time and
Present's wall-clock wait is invalid. A healthy renderer can spend its spare
budget waiting in Present. Conversely, a short Present wait does not prove that
the GPU is fast. Source frames, flip notifications, successful presents, and
generated frames are different counters.

The ranked list below is a **measurement/optimization priority**, not a measured
Xbox time breakdown. `M` means a host measurement or a structural timing limit;
`E` means a conditional model. Dynamic instruction counts are unknown. Where a
Mac has no meaningful D3D12 analogue, the unknown is explicit instead of a
fabricated measured cost. Numeric host costs are expanded in the benchmark table.

## Ranked budget and blockers

| Rank | Item                                                                                            | Measured or estimated ms/source frame                                                                                                                                                                                                                 | Finding and next decision                                                                                                                                                                                                                                                             |
| ---- | ----------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1    | Guest/host timing mismatch, missed refresh, flip/vsync dependencies                             | **M:** at 60 Hz, interval 1 permits 16.667 ms/frame; interval 2 imposes 33.333 ms/frame, an extra **16.667 ms**. One missed refresh adds 16.667 ms. Glue flip/vsync waits have **100 ms** timeout paths; retirement/fence waits can be longer.        | Verify clocks and wait reasons before changing arithmetic. Present wait is not necessarily wasted work. FPS++ and presentation must agree.                                                                                                                                            |
| 2    | Guest CPU critical path: memory locality, context traffic, FP/paired singles, call boundaries   | **E:** `N * kernel_ns / 1e6` ms/frame. For example, **1 million dependent word loads/frame** costs the table's LD32 ns numerically in ms. Cache misses and real game work can make it much worse. Actual Xbox total: **unknown**.                     | `Generator.simple`, `fp_mem`, `fpu`, `runtime/include/ppc.h`. The 4 GB mapping has no per-load translation, bounds check, virtual dispatch, or lock. Fix measured hot functions before replacing already-inline helpers.                                                              |
| 3    | Actual GPU execution: render targets, shaders, copies, FSR/output resolution                    | **Unknown ms/frame** without Xbox GPU timestamps; it must fit an overlapping 16.667 ms budget. 3840x2160 has **9x** the pixels of 1280x720, not automatically 9x total frame time.                                                                    | Outside command-thread optimization, but still a possible blocker. Separate internal size from output size. Remaster's extreme shadows, longer draw distance, grass and reflections also increase guest/GPU work. Use the existing remaster ablations; no settings were changed here. |
| 4    | Guest worker synchronization, scheduler contention, filesystem/decompression/upload preparation | **E:** an extra **1 ms** on a dependency's critical path consumes **6%** of the frame budget. Actual total and stalls: **unknown**.                                                                                                                   | Guest core IDs do not pin Windows threads. Main/AX/alarms/GX2 callbacks have elevated priority; kernel synchronization shares `g_klock`. Worker CPU totals cannot be added to main-thread wall time. Capture wait reasons and runnable threads together.                              |
| 5    | AX guest callbacks, AUX/final mix, protected voice lists                                        | **E:** there are **5.556 audio blocks/video frame** at 60 fps. If callback/locking work takes **0.1 ms/block**, that is **0.556 CPU ms/frame**; **1 ms/block** is **5.556 CPU ms/frame**, before mixing. Actual callbacks/locks: **unknown**.         | Real callbacks execute recompiled guest code through `guest_call`; long callbacks or shared voice locks can affect the game. Benchmarking the decoder alone cannot clear the audio system.                                                                                            |
| 6    | Audio decoding/resampling/filtering                                                             | **M/E:** multiply the table's **ns/144-sample block** by `active_voices * 5.556 / 1e6`. Example: **100 voices at 600 ns/block = 0.333 CPU ms/frame**, for that kernel alone.                                                                          | Real PCM16/ADPCM linear and biquad code is benchmarked. Include bus mixing, state sync, TV/DRC conversion, callback work and device calls in an eventual AX profile. This is parallel CPU demand, not automatically a frame delay.                                                    |
| 7    | Indirect guest calls and host-to-guest callbacks                                                | **M/E:** **100,000 rt_call/frame** costs `rt_call_ns / 10` ms; incremental lookup cost is the difference from the direct-call control, only if stable across trials. Callback save/restore is measured separately.                                    | Text/cave/thunk lookup already uses direct arrays. Normal text dispatch does **not** take `g_lock`; the thread-local 1024-entry cache/shared lock only covers fallback import stubs. A new universal cache would add work and invalidate assumptions.                                 |
| 8    | CR/XER updates and packing                                                                      | **M/E:** **100,000 full mfcr/frame** costs `mfcr_ns / 10` ms. Savings depend on executed sites, not the number of source sites. `CR_compare`/`XER_carry` normalize to their ns value in ms per million operations.                                    | Implemented full CR gathering and guarded adjacent extraction. No flag liveness across calls, no dropped carry/overflow updates, and no weakened atomics. Partial CR writes stay unchanged.                                                                                           |
| 9    | Debug HUD / D3D11On12 / D2D / DWrite                                                            | **M:** **0 ms** drawing work when disabled (default). Enabled Xbox cost: **unknown**; test against an explicit **0.5 ms/frame investigation threshold**, not an asserted measurement.                                                                 | Text layouts are cached, but every enabled frame acquires/releases a wrapped backbuffer and **Flushes** the D3D11 context. Draw/driver/queue costs are absent from a text-layout-only benchmark. Compare HUD off/on with recorder off.                                                |
| 10   | Recorder, GPU copies, encoder, CPU readback fallback                                            | **M:** 0 capture work when disabled; host row-copy cost is in the table. **E:** one **1920x1080 BGRA** copy is **8.294 MB/frame**, **497.7 MB/s** at 60 fps, excluding source reads, rescaling, further copies and encoding. Xbox total: **unknown**. | `Map(...DO_NOT_WAIT)` avoids an explicit wait for an unfinished texture, but completed readback still allocates/copies rows on the presentation path. Shared D3D11 context and MF add contention. The recorder can drop to 30 capture fps; that is not a game 30 fps cap.             |
| 11   | FPS++ arithmetic / host housekeeping                                                            | **E:** roughly **0.001–0.01 ms/frame** for hundreds of ordinary host operations at one timing update/frame; a planning estimate, not a gameplay measurement. Any pacing wait is accounted separately above.                                           | The rolling average is 8 frames; there is no reason to attack this short loop before proving it material. The retained host hook handles pause/telemetry, with host speed scaling gated off.                                                                                          |
| 12   | Timer oversleep, diagnostics, simulation settings                                               | **M:** host wake lateness is reported separately in microseconds; it is not Xbox time. **E:** one extra **1 ms** wake delay is 6% of a 60 Hz frame; timeout/stall tails can be much larger.                                                           | Turn off CPU probes, HLE tracing, simulator throttling and recording for a baseline. `BOTW_SIM_SERIES_S` intentionally inserts time; its synthetic factors are not console performance measurements. Ordinary AX work does not log per sample/block.                                  |

These rows are not additive. In particular, ranks 2/7/8 overlap, ranks 5/6
overlap, and GPU/Present/waits overlap downstream work.

## Host measurements and reproduction

The benchmark emits current generator output and an authored pre-change `mfcr`
expression into a separate translation unit. It links the actual
`runtime/src/dispatch.cpp` and `runtime/src/snd/ax_mix.cpp`; no game or GPU is
loaded. Calls cannot be optimized away across translation units. Results include
loop/input mutation/observation overhead; they are **not** isolated instruction
latencies. Memory tests use a dependent **16 KiB** pointer chain, not a streaming
game working set. AX uses synthetic active, looping voices at a 1:1 sample ratio,
144 samples/block; biquad trials include input/state copies. No XAudio2, complete
AX frame, real guest callback body, GPU transfer, D2D, MF encoder or D3D12 driver
cost is measured.

```sh
uv run python -I tools/bench_guest_cpu.py
uv run python -I tools/bench_guest_cpu.py --arch x86_64
uv run python -I tools/bench_guest_cpu.py --opt 1
uv run python -I tools/bench_guest_cpu.py --arch x86_64 --opt 1
```

Each reports the median/min/max of seven trials after warmup. The x86_64 build
uses AVX2 to match the UWP feature baseline, but runs under **Rosetta**, not on a
Zen 2 CPU. ARM and Rosetta results cannot be scaled into Xbox predictions.
The O1 runs check a lower optimization level; this UWP build actually compiles
guest sources at O2, including `big_*.cpp`. CMake's O1/O0 size exceptions apply to
the non-MSVC path.

Median **ns/operation** (rounding shown here; raw CSV retains min/max):

| Kernel                             | ARM O2 | x86 AVX2/Rosetta O2 | ARM O1 | x86 AVX2/Rosetta O1 |
| ---------------------------------- | -----: | ------------------: | -----: | ------------------: |
| `baseline_mfcr`                    |   2.44 |               22.74 |   5.38 |                6.57 |
| `current_mfcr`                     |   1.79 |                9.20 |   1.82 |                8.73 |
| `unfolded_extract`                 |   0.95 |                3.38 |   0.86 |                3.27 |
| `current_extract`                  |   0.85 |                1.99 |   0.87 |                1.70 |
| `baseline_mtcrf`                   |   2.14 |               14.12 |   6.28 |               10.93 |
| `candidate_mtcrf`                  |   1.68 |               21.54 |   2.26 |                3.68 |
| `CR_compare`                       |   0.92 |                2.24 |   0.96 |                2.07 |
| `XER_carry`                        |   4.83 |                2.56 |   4.92 |                2.56 |
| `direct_call`                      |   0.91 |                1.13 |   0.87 |                1.12 |
| `rt_call_monomorphic`              |   2.14 |                3.13 |   1.94 |                3.09 |
| `rt_call_64_targets`               |   2.46 |                3.66 |   2.29 |                3.81 |
| `guest_callback_save_restore`      |  20.87 |               42.59 |  22.15 |               35.00 |
| `LD32_BE_dependent_L1`             |   1.66 |                0.89 |   1.75 |                1.75 |
| `LD32_native_dependent_L1`         |   0.47 |                1.16 |   1.15 |                0.46 |
| `ST32_BE_L1`                       |   0.48 |                0.43 |   0.43 |                0.43 |
| `lfs_bit_exact`                    |   0.78 |                0.93 |   0.69 |                0.92 |
| `AX_PCM16_linear_144_samples`      | 195.06 |              271.84 | 183.29 |              285.89 |
| `AX_ADPCM_linear_144_samples`      | 483.45 |              470.67 | 462.30 |              419.77 |
| `AX_biquad_b1_zero_144_samples`    | 561.13 |              693.56 | 550.17 |              570.55 |
| `AX_biquad_b1_nonzero_144_samples` | 578.34 |              706.29 | 584.26 |              587.61 |

The selected O2 `mfcr` trials were **2.391–2.577 → 1.776–1.816 ns** on ARM and **22.295–24.261 → 8.892–9.981 ns** under AVX2/Rosetta. At a hypothetical 100,000 full reads/frame this is **0.065 ms/frame** saved on ARM, versus **1.354 ms/frame** under translation. Neither is an Xbox estimate. The folded one-bit extraction saved **0.10 ns** on ARM O2 and **1.39 ns** under Rosetta O2 relative to the new, unfused gather; ARM O1 was within noise. **Full gather regressed at x86 O1 (6.57 → 8.73 ns)**. This is evidence for the selected O2 build, not for a universal optimizer win.

The rejected full-CR scatter candidate was **1.68 vs 2.14 ns** on ARM O2 but **21.54 vs 14.12 ns** under AVX2/Rosetta O2. The two unchanged partial-write controls can also differ through layout/measurement effects. Do not choose an optimization from one host or from assembly length alone.

The padded 1080p row copy measured **0.130 ms/frame** on ARM O2 (range **0.125–0.139**) and **0.227 ms/frame** on Rosetta O2 (range **0.225–0.250**). These are CPU copy costs only. For 100 synthetic voices, ADPCM plus the b1=0 biquad model is **0.580 CPU ms/video frame** on ARM O2 and **0.647 CPU ms/video frame** under Rosetta O2, before bus mixing/callbacks/device work.

| Host wake test, O2   | ARM median / p95 / max (ms late) | Rosetta median / p95 / max (ms late) |
| -------------------- | -------------------------------: | -----------------------------------: |
| `wake_3000_us_late`  |            1.003 / 1.335 / 1.506 |                1.002 / 1.183 / 1.507 |
| `wake_16667_us_late` |            5.342 / 7.942 / 8.099 |                5.209 / 8.034 / 8.342 |

The machine had an existing high swap allocation and other work. Initial trials
overlapped compilation and were noisy; the final runs were taken after this
task's ninja completed. They are host microbenchmarks, not a controlled console
experiment. Large min/max spreads, sub-loop-cost differences, or Rosetta AVX
translation effects are not evidence for a portable speedup. Timer coalescing on
macOS is especially unsuitable as a model of UWP wake latency.

## Guest code generation decisions

`tools/recompile.py` uses `cafe/codegen.py`; the pack wrapper subclasses that same
generator. The raw player RPX contains 9,248,515 text words, including some data
decoded as instructions. Its static counts include 1,111,775 `lwz`, 826,444
`stw`, 282,094 `lfs`, 307,293 `stfs`, 118,005 `psq_l`, 93,411 `psq_st`,
122,646 `bcctr`, and 2,520 `mfcr`. These are **not execution frequencies**.
There were no decoded overflow-enabled arithmetic forms or `mtcrf` in that raw
inventory; this does not authorize dropping overflow/CR semantics in generated
patches or other inputs.

The final pack-aware analysis reports **201,832 functions**, **9,349,542 emitted
instruction sites**, 1,126 translation units and 458 switches; overlap and pack
code explain the difference from the raw text inventory. It applies **2,237 full
CR gathers** and **290 folded extraction pairs**, counted in `report.txt` as
static optimizations. The second generation changed 153 translation units.

- **Implemented: full `mfcr`.** Four alias-safe 64-bit `memcpy` loads gather eight
  canonical 0/1 CR bytes at a time with `0x8040201008040201`, then assemble the
  architectural word. Supported hosts are little-endian. It does not alter the
  context's CR layout, ABI, or flag values. The Xbox compiler emits scalar
  loads/multiplies rather than the original AVX2 widen/variable-shift/reduction
  chain. Both contain about 20 assembly instructions; instruction count alone
  is not a cycle estimate. The new form avoids that vector reduction and its
  constant tables.
- **Implemented: `mfcr; rlwinm` self-overwrite.** For an immediately adjacent
  rotate/mask selecting at most eight bits into the **same** GPR, emit only the
  required CR bits. Keep both guest address markers and the second instruction's
  record/CR0 behavior. Refuse this transformation at a second-instruction
  function entry, label, direct-branch target, switch target or probe, and when
  a hook observes the intermediate full word. A probe before the pair and a
  hook after it remain valid. Different destination registers retain the full
  first result. No general register/flag liveness assumptions are introduced.
- **Rejected: multiply-based `mtcrf` expansion.** The candidate helped ARM and
  x86 without AVX2, but regressed in the AVX2/Rosetta experiment. Partial writes
  also regressed in early trials. The candidate stays in the benchmark for
  reproduction, not in production code. It had no raw game instruction sites
  anyway. A shorter UWP assembly listing did not establish a safe target win.
- **Kept: memory helpers and byte swaps.** They are always-inline, fixed-size
  `memcpy` plus byte-swap intrinsics. Optimized host assembly has no helper or
  memcpy call for a scalar word access: ARM uses load/`rev`; x86 uses load/swap
  or MOVBE according to target features. Replacing them with pointer casts adds
  alignment/aliasing risk without removing a translation layer. A BE/native
  pointer-chain difference on Rosetta is not a general cost of swapping.
- **Kept: CR/XER and FP correctness.** CR0/SO and carry can be consumed by later
  instructions, callbacks or callees. Float load/store helpers preserve NaN
  payloads and denormals; paired singles implement non-native rounding and GQR
  quantization. Native casts or fast-math need independent bit-exact evidence.
  Bounds/zero/overflow division behavior, atomics and barriers also remain.
- **Kept: indirect calls.** Direct branches already call named host functions;
  switches already use C++ switches. Virtual calls lack a proven constant target.
  Broad devirtualization would break callbacks, imports or pack continuations.
  No cache or speculative direct target was added.

The linked executable's initial audit `.text` section was about **388 MiB** for
the entire runtime plus generated code. That is not a measured resident hot set,
but motivates instruction-cache/call-stack sampling rather than assuming all
9 million instructions execute every frame. Cross-TU calls limit optimization of
the context; huge functions and paired-single helpers can also increase register
pressure. Dynamic profiles are required to rank individual guest functions.

## FPS++, presentation and audio clocks

The preserved remaster plan has fingerprint
`9758ae9b1e28c96a2e8ca08512c111e556d8b4f2e1d4cc08e5aec14352f27af6`, four
packs and 3,315 writes. FPS++ owns timing, has `vsyncFrequency=60`, an eight-frame
average, a 20 fps speed floor, and performance fence method 1. Pack byte patches
are compiled before analysis. `PackGenerator` removes replaced host timing hooks
and retains only the pause/CPU-telemetry housekeeping at `0x031fa97c`.
`fps60()` returns false for a compiled timing pack, preventing a second host
delta/scaling implementation. `vsyncFrequency` detection validates/logs timing
ownership; it does **not** install a second host sleep loop.

`g_target_fps` does not rewrite the compiled pack's limit. Outside frame generation,
`D3D12SwapChain::Present` forces interval 1 when it is >=60 and interval 2
otherwise, even if a caller requested an unsynced present. These are refresh
intervals, not an independent precise frame limiter:

| Source clock / host setting | 60 Hz display                                    | 120 Hz display, ideal DXGI cadence                                                   |
| --------------------------- | ------------------------------------------------ | ------------------------------------------------------------------------------------ |
| FPS++ 60 / host >=60        | Up to 60 source frames/s                         | Still at most 60 source frames/s; interval 1 alone permits 120 presents/s            |
| FPS++ 60 / host <60         | Presentation capped at 30/s; likely backpressure | Interval 2 permits 60/s, so the host setting alone is not a universal 30 fps limiter |
| FPS++ 30 / host >=60        | At most 30 distinct source frames/s              | At most 30 distinct source frames/s                                                  |

Actual Xbox application vsync can differ from the HDMI mode; use observed DXGI
statistics and accepted presents. With no timing pack, `fps.cpp` instead owns
interval/delta patches and intentionally caps specific cutscenes to 30. With the
pack active, its cutscene policy owns those exceptions. Do not diagnose every
30 fps cutscene as a CPU failure.

The ordinary presentation path submits without an unconditional GPU wait, then
runs overlays and Present. Resource/fence pressure can still block earlier.
The build used here defines `BOTW_FFX_STATIC=1`: `Generating()` and
`PresentGenerated()` are false in that path, so two FG presents are **not** the
explanation for its cadence. Dynamic/manual FG paths elsewhere wait on the
frame-latency handle and present generated/real frames; two synced presents on a
60 Hz surface can halve source throughput. FG must be isolated in any future
comparison, not counted as twice as many game frames. No presentation code under
`runtime/cemu/Cafe/HW/Latte` was changed.

AX uses a separate 3 ms clock: 144 samples at 48 kHz, normally 333.3 blocks/s,
independent of 30/60 video fps. Queue feedback requests 2.9/3.1 ms periods, with
a 1.7 ms minimum spacing and a 100 ms skip-ahead threshold. Low/high queue
thresholds are 1,152 and 4,800 frames: **24 ms and 100 ms of audio latency**, not
CPU work. UWP `push_tv` locks the voice, remixes/downmixes, scans bounded slots,
submits XAudio2 buffers and optionally copies recording audio. It drops excess
blocks instead of blocking for a free slot; `queued_frames` shares the mutex and
queries device state. Device latency and a 3 ms sleep are not 3 ms of main-thread
CPU per game frame. Underruns, callback time, shared locks and wake tails still
need real-device observation.

## Verification and console handoff

Host verification executes the emitted CR code with ASan/UBSan at O1, native O2
and x86_64 O2; it exhausts each 16-bit half of CR, tests all 256 write masks,
checks full-context preservation, and compares folded/unfolded execution across
all 32 rotations, single-bit/high-nibble/wrapping masks and record forms. Separate
tests reject observation/control-flow barriers. Python pack integration tests
exercise the actual recompilation driver and continuation/hook rules.

The final UWP generated sources preserve the pack fingerprint and report no
unsupported instructions, undefined callees or analysis problems. Host runtime
suites, tool suites and the builder suite passed; exact results and commits
are recorded in the task's uncommitted root report. After the final regeneration,
`nice ninja -C build/local-uwp-game -j4` completed successfully and linked
`BotWRecomp.exe`.
Host correctness/linking does not validate actual game state, pixels or timing.

For the human console test:

1. Compare base and changed builds in the same warmed scene/save/camera, after
   caches settle, using several identical 10-second windows and a longer pass
   for p95/p99 frame times. Start with HUD/recorder/FG/simulation/tracing off.
   Record console, HDMI/DXGI refresh, pack fingerprint, internal/output sizes,
   upscaler, guest limit and `g_target_fps`.
2. Pair source-frame/flip/present counts with main-thread active CPU, its wait
   reasons, worker CPU, GPU duration and Present/fence waits. Existing
   `main_wait_report()` prints **ms/second**, not ms/frame: divide by the source
   frames per second in the same window. Existing D3D12 Present diagnostics use
   presents as their denominator; FG requires conversion to source-frame units.
   Cumulative CPU from overlapping threads is not critical-path time.
3. Measure the HUD and recorder separately on/off, including GPU capture versus
   CPU fallback, encoder/drop/queue statistics and D3D11On12 overhead. The HUD
   counter increments at the overlay call before Present, so use accepted DXGI
   presents to validate it, especially on failure/occlusion/FG paths.
4. Capture AX block duration, callback/AUX/final-mix time, voice counts, lock
   stalls, underruns/queue depth and wake lateness. Test busy combat and a
   streaming area, not only an idle plateau. Preserve 3 ms audio cadence.
5. Verify menu/HUD animations, map scrolling, gameplay speed, audio loops,
   pause/resume and known 30 fps cutscenes. Exercise virtual callbacks and
   pack code caves. Verify frame pacing on both 60 and 120 Hz modes and a
   deliberate host-30/FPS++-60 mismatch before proposing a timing change.

Only those captures can establish the remaining ms/frame deficit and whether
these small CPU improvements move the frame rate at all.
