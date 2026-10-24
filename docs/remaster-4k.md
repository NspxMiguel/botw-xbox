# Remaster output, memory and category validation

Status: host implementation and synthetic checks only. No category, HDMI change,
GPU time or memory peak in this document has been verified on an Xbox. The
reported 43 fps / 4555 MiB starting point is supplied by the task, not a new run.

## Display and game mode

Microsoft's [Series X|S UWP guidance](https://walbourn.github.io/directx-and-uwp-on-xbox-series-x-s/)
explicitly supports a 3840×2160 swapchain on Series X, and recommends 1440p for
Series S. It also confirms D3D12 feature level 11.0 in UWP; this is not the retail
GDK feature set. [Game mode setup](https://walbourn.github.io/directx-and-uwp-on-xbox-one/)
is the package's **App type = Game** setting in Dev Home (or the Portal's default
for newly deployed packages). Store metadata chooses the type for retail UWP.
`expandedResources` is deprecated and fails Store submission, so the manifest
does not add it. There is no game-specific 4K manifest capability to add here.
Keep x64 and Windows.Universal; confirm Game again after deployment.

[Microsoft's resource table](https://learn.microsoft.com/en-us/previous-versions/windows/uwp/xbox-apps/system-resource-allocation)
lists 5 GB for a foreground game and 1 GB for an app. Use the actual
`MemoryManager::AppMemoryUsageLimit` logged by this app; this audit uses the
observed **5120 MiB (5 GiB)** limit. Debugger exemptions can conceal an OOM.

The sizing path is:

1. `app.cpp` maintains CoreWindow/DPI dimensions for the launcher. These may be
   1920×1080 even on a 4K TV.
2. Before creating the game renderer, `configure_output_mode()` enumerates HDMI
   modes on Series X and asynchronously requests a higher SDR resolution, up to
   3840×2160, with at least 59 Hz. 4K60 can replace 1080p120; 4K30 cannot replace
   a game mode. FSR and frame generation remain opt-in/off by default.
3. `output_pixels()` reads **GetCurrentDisplayMode**, not just supported modes,
   after the request. Rejection or an unavailable API retains the current mode;
   desktop/remote sessions fall back to CoreWindow raw pixels. 1080p-only TVs
   never get an invented 4K mode. HDMI mode changes now request a renderer resize
   even without a CoreWindow/DPI change, including builds without FFX.
4. `D3D12SwapChain` creates a flip-discard CoreWindow chain with three RGBA8
   buffers. Its actual DXGI dimensions are logged after creation. The HUD reads
   the swapchain descriptor, so `Output 3840x2160` confirms buffer dimensions,
   while the TV's own info panel confirms the physical signal.

The final Latte compositor also needs the actual size: its screen-image area
previously queried CoreWindow through `botw_window_phys_size`, so a 4K chain alone
could still receive a 1080p viewport. The presenter now publishes actual dimensions
as one atomic width/height pair and that bridge uses them for the game viewport.
Launcher/input coordinates retain the CoreWindow size. Failed resizes keep the
last actual extent and restore the old buffer views; a 1080p TV fills its complete
backbuffer. Internal texture scale changes still require a game restart.

[HdmiDisplayInformation](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.display.core.hdmidisplayinformation)
defines current/supported modes and mode requests. Microsoft's
[HDMI switching example](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/hevc-xbox)
documents the same APIs for video; its media playback allocation flags are not
used as a game memory entitlement. API availability/permission and successful
4K presentation in this particular Dev Mode configuration remain console tests.

## Internal resolution and memory

The pack's Resolution selection intentionally remains 1280×720. The built-in
`graphic_pack_shim.cpp` scales matching target families and their viewports;
identity texture rules do not override it. Shadow maps use the pack's explicit
400% size, independently of output size. Do not set both a 4K pack resolution and
a 3× host scale: explicit pack dimensions take precedence, obscuring the policy.

Automatic Series X scale is **2× on a 4K TV (2560×1440 internal)**, and **1.5× on
a 1080p TV (1920×1080 internal)**. Series S now starts at 1.25× (1600×900),
with a 1440p output cap; see [Series S policy](series-s.md). This is a
conservative memory default, not a claim that 1440p is a measured GPU limit.
The final output filter presents to the full physical swapchain; FSR is disabled
unless selected. Explicit quick-menu/helper scale choices still win, up to 3×
on X. Importing `renderScalePercent` now clears an older quick-menu override so
that helper profiles, including Auto=0, actually take effect on the next boot.

One RGBA8 1080p/1440p/2160p surface is 7.91/14.06/31.64 MiB. RGBA16 is twice that;
depth/stencil expansion and alignment depend on the driver. Three output buffers
are 23.73/42.19/94.92 MiB. Target counts vary by scene and texture cache lifetime;
shader inventory alone does not establish simultaneous residency.

Planning model (all sizes are MiB, despite the HUD's `MB` label): allow **96
bytes per internal pixel** across the active target family. For example, eight
RGBA16, four RGBA8, two D32 and two RGBA8 scratch surfaces total 96 B/pixel.
These counts are an explicit planning allowance, **not observed BotW allocations**.
Fit the fixed component to 4555 MiB at 1080p internal/output, keep shadow/cache/
guest memory fixed, then replace output buffers with the 4K chain:

`process = 4555 + (internalPixels - 1080pPixels) * Bpp / 2^20 + 71.19`

| Internal  | Scale | Target allowance, 96 B/px | 4K output buffers | Expected process, 96 B/px | Sensitivity, 128 B/px | Reserve at 96 B/px |
| --------- | ----: | ------------------------: | ----------------: | ------------------------: | --------------------: | -----------------: |
| 1920×1080 |  1.5× |                    189.84 |             94.92 |                   4626.19 |               4626.19 |             493.81 |
| 2560×1440 |    2× |                    337.50 |             94.92 |                   4773.85 |               4823.07 |             346.15 |
| 3840×2160 |    3× |                    759.38 |             94.92 |                   5195.72 |               5385.57 |         **−75.72** |

Reproduce with `python3 tools/remaster_check.py memory` and
`memory --bytes-per-pixel 128`. At 128 B/px the 1440p estimate still leaves
296.93 MiB, exceeding the chosen 256 MiB reserve. Full internal 4K is not a safe
unmeasured default. This is conditional on the supplied baseline scene; cache
growth, transitions, mods, capture/overlay resources, upload/readback buffers,
allocation padding and deferred destruction can consume more. FSR/FG history
is excluded because those features are off. A long console route is mandatory.

With `LocalState/latte-profile.txt`, successful D3D12 render/depth-capable texture
allocations log driver `GetResourceAllocationInfo` estimates for all three
resolutions. Uniform host-scale targets are projected; fixed pack targets remain
fixed. Releases carry IDs so the harness calculates **live cohort peaks**, rather
than adding all historical allocations. This cohort is a diagnostic estimate:
sampled textures can have RTV capability, lifetime may differ at another scale,
and release logging precedes GPU deferred retirement. It excludes swapchain,
slice copies, FSR, overlays and buffers. The authoritative total remains periodic
`memory: ... of ... MB`, not the cohort sum. Keep the profiler off for shipping.

## GPU budget and human procedure

The existing Series S timestamp collector also runs with the Latte profiler on
Series X. Synthetic GPU ballast is gated on simulation **enabled**, so profiling
does not turn a Series X into a simulated S. `GPU timing: mean ... ms, max ... ms,
samples ..., capped ..., skipped ...` uses queue timestamp frequency and completed
fences without an extra CPU wait. It sums each frame's command-list segments,
excluding CPU gaps between submissions. It covers game rendering, final copy and
upscaling, but not Present blocking or the separate D3D11 overlay/recorder queue
work. CPU profiler slots / `GPU fence wait` / Portal GPU utilization are not GPU
frame time. Capped/skipped windows cannot approve a scale.

Use 16.67 ms as the 60 fps GPU budget even while the CPU delivers ~43 fps.
Compare 1.5×, 2× and, only after a memory review, 3× using the baseline package.
Promote the highest tested scale only if the last 30 GPU windows have no missing
samples, their maximum stays below 16.67 ms, process peak stays at/below 4864 MiB,
and sustained FPS does not regress relative to the same CPU-bound scene. Include
a dense village, weather, water, map, inventory, teleport and long traversal.
Do not infer 4K headroom from a low utilization percentage. No GPU measurements
were taken in this task because console access was expressly prohibited.

Prepare the category manifests and inspect the complete dry run:

```sh
python3 tools/remaster_check.py prepare \
  --packs ~/.local/share/nxbox/deps/cemu_graphic_packs/src/BreathOfTheWild \
  --out build/remaster-check
python3 tools/remaster_check.py run build/remaster-check/plan.json \
  --out build/remaster-results
```

There are nine individual category variants: shadows, shadow distance, AA,
clarity, reflection range, entity distance, terrain distance, tree billboards,
and grass density. "Off" restores the original/default value; AA uses None.
Resolution is tested separately with the **same package** at 1.5×/2×/3×.
FPS++ mode/cap remain fixed, since changing timing would invalidate the comparison.

Category values cannot currently be changed without rebuilding. The Remaster
plan contains initial-memory/PPC writes, the catalog locks compiled packs, and
selections/shader constants are compiled into `generated/packs_generated.inc`. Disabling an
entire pack cannot isolate its categories. For each `baseline.json`/variant JSON:

```sh
python3 -I tools/packs/recompile.py \
  ~/.local/share/nxbox/botw-wiiu-game/code/U-King.rpx generated \
  --packs "$selection" --llvm-mc /opt/homebrew/opt/llvm/bin/llvm-mc
while pgrep -x ninja >/dev/null; do sleep 5; done
nice ninja -C build/local-uwp-game -j4
```

Then stage/package using the project's normal local signing workflow and put
each package at the corresponding `plan.json` path. Do not commit them. Reuse
generated/build trees, build sequentially and record each plan fingerprint/package
identity. Full initial Remaster recompilation here took **49 s analysis + 23 s
code generation** (1126 TUs); every variant pays analysis/codegen plus changed-TU
compile, link, package and install. Initial build has ~1297 Ninja edges; incremental
cost depends on which PPC writes changed. Shader-only changes can be much smaller;
do not promise a fixed rebuild time. Ten unique packages serve twelve default captures
and an optional thirteenth 2160p-internal experiment.
The initial `nice ninja ... -j4` build on this Mac took approximately **18 minutes**
(1077 s in `.ninja_log`); a subsequent three-object viewport rebuild and link took
only seconds. A category therefore pays about 72 s for analysis/codegen plus its
actual incremental compile/link cost, up to a cold rebuild if TU partitioning
changes, followed by packaging and installation. These host timings are not GPU
performance measurements.

Immediately after each variant build, copy `generated/packs_report.json` beside
its package as `<case>.packs.json` (for example `packages/grass.packs.json`). The
runner rejects a sidecar with different presets and verifies its fingerprint
against `Graphic pack plan: ...` in the captured runtime log. This detects an old
package selected by the Portal or an incorrectly labelled variant. The sidecars
are private build output and stay under build/, alongside the packages.

Human-only execution (install packages first, inspect the generated plan):

```sh
uv run --no-project --with pillow python tools/remaster_check.py run \
  build/remaster-check/plan.json --out build/remaster-results --execute
```

The runner holds `_tools/lockrun.sh` for the experiment, uses `dp.py` with
`PKG=BotWRecompiled`, enables profiling, applies a common helper config, boots,
waits 40 s, presses A six times at 8 s intervals, stands still 100 s, then captures
`end.png` and `runtime.log`. It follows `botw-bench.sh` without its destructive
output-directory removal. Output must be new. `--only baseline` permits a first
calibration; `--only internal-2160` is the explicit 4K-internal experiment after
review. The prepared plan includes that experiment, but the runner skips it unless
`--include-2160` or `--only internal-2160` is supplied. Add `--include-2160` to the dry
run to inspect all thirteen cases. The tool removes its profiling marker on
exit, including failed runs. Packages/preferences remain the test configuration.

Before running, choose the same manual save and camera in a scene showing ground,
grass, trees, distant actors, shadows and water. Disable autosave interference and
extra mods; prewarm shader caches. Verify the scripted A presses really load that
save. Weather, grass motion and time are nondeterministic; `baseline-repeat` exposes
that noise. One view does not certify every category: repeat at shadow/water/grass
and village scenes. If the scene did not load, reject the run even if it has FPS.
Portal screenshots can be downsampled by the OS: inspect their dimensions and
use the TV/HUD plus actual swapchain log to establish 4K, not a 1080p screenshot.

`report.json`, `report.md` and per-case `diff.png` contain RGB mean absolute error,
fraction of channels differing by >8, FPS (last three 10 s windows), GPU timings,
memory peak and cohort estimates. Recompute with `report build/remaster-results
--crop x,y,width,height` to exclude the changing HUD using screenshot pixels.
`scale-candidate.json` selects the highest full-Remaster render scale passing
the GPU/memory checks and within 3% of control FPS, with matching 4K output/scale
logs. Missing data yields no candidate. Recheck sustained FPS with profiling off.
The budget flag is a necessary condition, not a visual correctness verdict.
Compare category diffs to baseline-repeat, inspect artifacts and record a human
pass/fail per category. All categories remain **console-unverified**.
