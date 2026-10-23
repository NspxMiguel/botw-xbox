# Latte command thread profiling

`CEMU_LATTE_PROFILE=0` disables profiling. BotWRecomp builds enable it by default;
other builds can enable it with `CEMU_LATTE_PROFILE=1`. The setting is read once
when the command processor starts. No profiler or console attachment is needed:
results go to `runtime.log`.

The D3D12 present diagnostics and the profiler share a complete window of at least
10 seconds, checked at the existing 2-second display refresh checks. The profiler
line follows the existing `D3D12: ... presents/s ...` line. The first main-window
present starts the window and discards startup work. Each subsequent main-window
present counts one source game frame; interpolated and pad presents do not add
frames to the divisor. The existing D3D12 line still divides present/fence times
by actual presents.

The exact profiler format (fmt placeholders) is:

```text
Latte profile: {:.2f}s, {} game frames, {:.1f} draws/frame; ms/frame: draw-total {:.3f} (inclusive), draw-other {:.3f}, shader-pipeline {:.3f}, texture-bind {:.3f}, texture-load {:.3f}, buffer-sync {:.3f}, indices {:.3f}, uniforms {:.3f}, descriptors {:.3f}, render-targets {:.3f}, submit {:.3f}, pm4 {:.3f}; top: {} {:.3f}, {} {:.3f}, {} {:.3f}; covered {:.1f}%, unaccounted {:.1f}% ({:.3f} ms/frame); pipeline-miss/frame {:.2f}, texture-new/frame {:.2f}, texture-upload {:.0f} B/frame, buffer-upload {:.0f} B/frame
```

The slot names are fixed. All slot values and the sorted top three use
milliseconds per source game frame. They measure elapsed time on the command
thread, including preemption and any blocking inside that stage, rather than
thread CPU cycles.

| Field | Boundary / meaning |
| --- | --- |
| `draw-total` | Inclusive union of all three PM4 draw handlers and shared `beginDrawPass` / `endDrawPass` setup and teardown. Counted once across nested stages. |
| `draw-other` | Draw handler/setup/teardown time outside the more specific slots. |
| `shader-pipeline` | `LatteSHRC_UpdateActiveShaders` and `D3D12PipelineCache::GetOrCreate`, including hash, lookup and synchronous pipeline work/waits. Background compilation is excluded. |
| `texture-bind` | `LatteTexture_updateTextures` (lookups, change detection, view creation) and the per-stage texture/SRV/sampler-description block in `draw_buildStageBindings`. |
| `texture-load` | `LatteTexture_ReloadData`, `LatteTextureLoader_UpdateTextureSliceData` (decode/untile), and D3D12 `texture_loadSlice`. |
| `buffer-sync` | `LatteBufferCache_Sync`, range reservation/retrieval, invalidations/DC flush processing, and D3D12 `bufferCache_upload`. |
| `indices` | `LatteIndices_decode`, including lookup/conversion/upload. |
| `uniforms` | `draw_prepareUniformVars`. |
| `descriptors` | Remaining `draw_buildStageBindings`, `draw_acquireSamplerTables`, and actual descriptor writes in `WriteStageTable`. |
| `render-targets` | `LatteMRT::UpdateCurrentFBO`, `ApplyCurrentState`, and `draw_applyRenderTargets`. |
| `submit` | `SubmitCommandList`, including fence waits when requested. |
| `pm4` | Remainder of the ringbuffer command loop, including register parsing between fast draws. |

Slots are **exclusive**: nested time is charged only to the innermost slot.
`draw-total` is an additional inclusive value and must not be added to them.
`covered` is the sum of exclusive slots divided by the elapsed wall-clock window;
`unaccounted` is its complement. Command starvation, guest synchronization waits,
and presentation are excluded from PM4, but useful nested stages (for example a
submit during idle) still count. Work without a separate stage, such as command
loop housekeeping, remains in PM4. These are elapsed-time measurements, so covered
is not an OS CPU utilization reading.

`draws/frame` counts attempted renderer draws, including draws skipped by the
backend. `pipeline-miss/frame` counts unsuccessful in-memory pipeline lookups,
including retries while shaders are pending; it is not a count of compilations.
`texture-new/frame` counts guest texture cache creations through
`LatteTexture_CreateTexture`, including render targets. Upload bytes count actual
D3D12 copy payload: texture rows/planes without alignment padding, and buffer
cache uploads. GPU-to-GPU copies, uniform constants, and index uploads are not
included in those byte counters.

The collector uses fixed thread-local arrays, a steady clock, and scoped stack
objects. Only `LatteCP_ProcessRingbuffer` enables its thread's state. Other
threads cannot change its counters. Same-slot nested scopes reuse the enclosing
timer. The hot path has no locks, allocation, logging, or environment lookups;
disabling profiling skips the clock reads. Reporting checkpoints active scopes
before resetting counters, so scopes that cross a window boundary remain valid.
