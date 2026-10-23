> Historical SDK 2.3 desktop-DLL integration. These DLLs fail to load on Xbox
> UWP (error 126). Use [the static source build](FSR3-INTEGRATION.md) for Xbox.
> The validation claims below concern the earlier desktop-DLL experiment.

# Optional FidelityFX 3.1 integration

`BOTW_FFX_SDK` is an empty-by-default CMake cache path to the external
`Kits/FidelityFX` directory from SDK 2.3.0. No SDK files are vendored. Runtime
installation places the three signed DX12 DLLs beside the executable and the
redistribution notice at `Notices/FidelityFX-LICENSE.md`. No FidelityFX import
library is linked. The loader and all five API entry points are resolved at
renderer initialization; provider versions are queried and explicitly pinned to
3.1, rather than silently selecting the SDK's newer upscaler.

## What runs

| Condition | Rendering path |
| --- | --- |
| SDK omitted | Existing FSR 1 EASU + RCAS, or the existing output filter |
| Upscale loader, dependency, entry point, provider, context, or dispatch unavailable | One runtime `LOG` failure line; FSR 1, then the output filter if FSR 1 fails |
| FG provider, context, wrapper, prepare, callback, or configuration fails | One FG failure line; retain FSR 3.1 upscaling without FG |
| FSR 3.1 contexts available and usable scene depth exists this frame | Actual FidelityFX upscale dispatch, followed by the existing RCAS output pass |
| This frame has no usable scene depth | FSR 1 for this frame; invalidate temporal history |
| FG context available, frame prepared, refresh rule satisfied | HWND: SDK callback/wrapper presentation; CoreWindow: direct FG dispatch and caller-owned paired presentation |
| `BOTW_UPSCALER=fsr1` / `off` | Force spatial FSR / output scaling; the optional loader is not loaded |
| `BOTW_UPSCALER=fsr3` | Attempt FSR 3.1 with the same mandatory fallbacks |

The signed desktop DLLs import desktop CRT, USER32, WINMM and VERSION. Successful
cross-compilation does **not** establish that these binaries load on Xbox UWP.
Missing dependencies and unsupported manual presentation are expected failure cases;
they must leave the game playable; FG-only failures retain FSR 3.1 upscaling. There is no custom
blend/interpolator substituted for FidelityFX frame generation.

## Automatic console policy

Series X and UWP desktop development use quality (1.5x) to the available display
resolution, capped at 2160p. Series S and `BOTW_SIM_SERIES_S=1` use balanced (1.7x),
capped at 1440p; a 1080p output remains 1080p. Internal scale may be below 1 to
preserve balanced at 1080p. Automatic scale is used only when the quick menu, saved scale, and quality preset
leave it unset. Explicit resolution graphic packs retain their existing texture-rule authority.

The HWND wrapper requires a reported display mode of at least 59 Hz and a prepared real frame.
It enables after a 30-frame mean above 18.0 ms and disables after a 120-frame mean
below 15.5 ms. Timing transitions are at least two seconds apart. Stable 60 fps,
including on 120 Hz displays, does not cross the enable threshold. Unknown refresh,
invalid timing and pause immediately suppress FG without changing its timing latch.
HDMI mode changes refresh the cached rate; desktop falls back to the current DXGI
output mode, never inferred Present throughput. Generated frames do not advance
the guest clock.

CoreWindow uses the frame-generation context directly, with
`FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY`, a null swapchain/callback,
and `ffxDispatchDescFrameGeneration::presentColor` / `outputs[0]`. The HWND
wrapper path is retained. Automatic CoreWindow FG is Series X only: guest 30 on
60 Hz produces 60 presents/s; guest 60 on 120 Hz produces 120 presents/s.
Guest 30 on 120 Hz is ineligible: select a 60 Hz panel mode for 30 -> 60 FG,
or keep FG off to avoid its cost at 120 Hz.
Fractional 59.94/119.88 Hz modes are accepted; unknown/other modes and Series S
fail closed. Guest 60 on 60 Hz is always ineligible, even if measured performance
drops below 60. The existing real-frame hysteresis/cooldown remains, using actual
frame time at both refresh rates; stable guest 60 does not cross the enable
threshold. Guest clock selection includes the FPS++ cutscene cap.

Manual presentation copies the final composed backbuffer into an owned RGBA8
texture and dispatches FG into a second UAV texture on the same direct queue.
It copies/presents generated then real, using HDMI-derived vblank intervals to
space them half a guest frame apart. A waitable CoreWindow swapchain with maximum
frame latency one bounds the queued presentation latency; each wait is capped
at one guest-frame duration. No additional frame-history presentation queue or
CPU sleep is introduced. If waitable creation fails, native creation retries
without it and FG stays off. Missing pacing, allocation, prepare, configuration,
dispatch or present errors retire only FG and preserve the upscaler. A latency
wait timeout skips generation for that frame without retiring FG; three timeouts
without a completed generated/real pair trigger a two-second retry cooldown.
Failed history or final real presents fall back to normal real presentation. First
frames after enable, pause and image cuts prime history and present only real
color at the normal present interval. Both owned textures are drained before
resize/retirement; they add about 63.3 MiB at 3840x2160.

The manual path intentionally interpolates the final image including HUD, text,
RCAS and host overlays; HUD ghosting/distortion is an accepted tradeoff. There
is no trivially available HUD-less final color.

The quick menu disables FG and drains pending SDK presents (where wrapped) while retaining all
contexts, resources and the wrapped swapchain. Its snapshot and screenshots use
the wrapped current backbuffer that received the final output pass. On close, the
renderer refreshes the backbuffer index and resumes normal preparation on the
next real frame. Conversion shaders/PSOs are created once per renderer, surviving
menu visits and resolution changes.

Resize, surface replacement, failure retirement and shutdown drain SDK presents
and wait for GPU work before destroying affected contexts or resources. FG-only
retirement leaves the upscale context and its resources intact. SDK 2.3.0's
replacement contract consumes the input chain reference and returns a new chain;
all other references and buffer views are released before the call. Its HWND-only
replacement implementation is used only for HWND chains. CoreWindow retains its
native chain and uses manual FG; composition retains upscaling without FG. On FG retirement,
the disabled replacement remains presentable, as supported by the SDK. Recovery
uses the shared native creation path if there is no usable chain; recreation is
non-throwing and preserves an existing presentable chain on failure. Failed
recovery disables FFX, and Present/Resize guard missing native chains. SDK chains
are released before the loader is unloaded during renderer destruction.

## Inputs and quality ceiling

- **Depth:** the current frame's most frequently used full-size depth attachment
  paired with a color target. This is a renderer heuristic, not recovered camera
  metadata. Dimensions, mip, slice, sample count and sampleable depth format are
  checked. D16, D24, D32 and D32+stencil are sampled into an R32 float texture;
  missing/incompatible depth selects FSR 1. Normal depth and nominal near/far/FOV
  are assumed; reversed projection and precise camera calibration remain unverified.
- **Motion:** a real RG16 float texture cleared to zero. No camera reconstruction,
  object velocities, previous-depth reprojection or guest motion buffer exists.
  Reactive and transparency/composition masks are conservatively all one, rather
  than pretending to identify individual transparent materials.
- **Jitter:** disabled. There is no reliable guest projection hook. Querying an
  offset without applying it to scene rendering would provide incorrect input.
- **Cuts:** a three-slot asynchronous 16x9 sparse color readback ring compares
  completed probes, normally one or two real frames late. Copies remain in the
  normal render command list, with no probe submission or GPU wait. Mapping is
  gated by the renderer submission fence; a full ring skips sampling rather than
  overwriting pending data. First frames, gaps over 100 ms, sufficiently large
  image changes and history invalidation reset upscale and FG history.
  Similar cuts can be missed; fast pans, flashes or changing HUD can cause extra
  resets. This is an image discontinuity heuristic, not a camera-cut hook.
- **Color:** the already transformed SDR output is converted from display sRGB
  to linear FP16 before FSR 3.1 and back to display sRGB before RCAS. It does not
  depend on color flags introduced for newer providers.
- **HUD:** scene and HUD arrive composed. No HUD-less or separate UI resource is
  claimed. Interpolation can distort text, HUD, transparent objects and disocclusions.

The `ponytail:` comments mark these compromises in the implementation. Zero
motion and no jitter cannot deliver the temporal detail or stability of a native
FSR 3.1 integration. The cut probe uses about 216 KiB of readback storage;
actual performance and the delayed cut response need measurement on hardware.
There is no claim of measured Xbox image quality, frame-rate improvement, or
successful desktop-DLL loading on console.

## Validation

Use one build directory for both configurations:

```sh
cmake --preset local-uwp-clang -B build -DBOTW_FFX_SDK=/external/Kits/FidelityFX
cmake --build build --target BotWRecomp -j2
cmake --build build --target clean
cmake --preset local-uwp-clang -B build -DBOTW_FFX_SDK=
cmake --build build --target BotWRecomp -j2
clang++ -std=c++20 -Wall -Wextra -Werror \
  runtime/src/platform/tests/fsr3_policy_test.cc -o /tmp/fsr3_policy_test
/tmp/fsr3_policy_test
```

The selection/history test is also registered in the existing macOS platform
CTest harness. It covers console model, simulation, all overrides, SDK/DLL
availability, display limits, refresh eligibility, timing windows, threshold boundaries, the dead band, jitter,
minimum toggle intervals, pause/invalid inputs and history invalidation.
The D3D12 syntax harness accepts `--ffx-sdk` and `--generated-include`; its UWP
CRT flags now match the clang-cl preset, and it accepts versioned xwin SDK layouts.

Validated on macOS with the local clang-cl/xwin UWP preset (`BOTW_BUILD_GAME=0`):

- Full executable builds with the external SDK and with an empty SDK path.
- D3D12 syntax checks with and without SDK headers: 33/33 translation units each.
- Portable selection/history test and the existing FSR 1 portable test pass.
- All four embedded conversion shader entry points pass glslang HLSL validation.
- Runtime installation reproduces the three DLLs and license byte for byte.
- Executable import inspection finds no FidelityFX load-time imports.

Temporary package files and build intermediates were removed after validation.
No game execution or Xbox hardware validation was performed.

### CoreWindow hardware validation still required

The SDK distribution here supplies public headers and signed provider DLLs, not
the frame-generation provider source. Build/syntax validation cannot establish
that the pinned 3.1 provider accepts the no-notify configuration on Xbox or that
its desktop dependencies load there. On Series X, verify DLL loading and the
D3D12 debug layer (prepare/dispatch states and UAV hazards), 30->60 and 60->120
cadence with HDMI 60/120 modes, fractional refresh, generated/real ordering,
input-to-photon latency, HUD artifacts, GPU cost, resize, quick-menu resume and
forced FG failure while upscaling continues. The one-frame queue bound is a
configuration guarantee; measured presentation/scanout latency remains unverified.

The CoreWindow change passed `fsr3_policy_test`, full clang-cl/xwin UWP executable
builds with and without the SDK, and `validation/syntax_check.py --uwp` with and
without SDK headers (33/33 translation units each). One build directory was
reused and removed afterward. These checks do not execute the game or SDK DLLs.
