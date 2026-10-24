# Static FidelityFX FSR 3.1 for Xbox UWP

`BOTW_FFX_SOURCE` selects an external, unmodified [FidelityFX SDK v1.1.4
checkout](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4),
commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`. Its upscaler component reports
**FSR 3.1.4**. The SDK and generated shader binaries are MIT licensed;
`cmake --install ... --component Runtime` ships `sdk/LICENSE.txt` as
`Notices/FidelityFX-LICENSE.txt`. The SDK is not vendored.

The source option takes precedence over the older `BOTW_FFX_SDK` SDK 2.3 DLL
option. Static builds neither compile D3D12Ffx's DLL loader/API-pointer layer nor
install FidelityFX DLLs. The old DLL integration remains optional for desktop
experiments, including its frame-generation path; its detailed notes are retained
in [FSR3-LEGACY-DLL.md](FSR3-LEGACY-DLL.md). **Static builds have no frame
generation**, no swapchain replacement, and no manual generated-frame presents.

## Build on the Mac

```sh
git clone --depth 1 --branch v1.1.4 \
  https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git \
  ~/.local/share/nxbox/deps/fidelityfx-1.1.4
```

Neither the tag checkout nor AMD's `FidelityFX-SDK-v1.1.4.zip` release archive
contains precompiled permutation headers. The shipped compiler is Windows-only.
The Mac build therefore runs AMD's **shipped** `FidelityFX_SC.exe` and adjacent
DXC through the existing CrossOver installation, entirely at build time. This is
a necessary departure from the assumed shipped-header route. No DLL extraction,
third-party shader distribution, or runtime FSR shader compilation is involved.

Hosts without Windows or CrossOver (a paid product) can skip the shader compiler:
the generated permutation headers come from MIT HLSL, so an `ffx-shaders`
directory from any earlier build (on Windows, or a Mac with CrossOver) can be
reused with `-DBOTW_FFX_SHADERS_DIR=<dir>` (it must contain `compiled.stamp`).
With that option set, neither `BOTW_FFX_WINE` nor a bottle is needed.

Otherwise, create an isolated build-local bottle once (do not reuse a personal bottle):

```sh
export CX_BOTTLE_PATH="$PWD/build/ffx-bottles"
/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/cxbottle \
  --bottle ffx --create --template win10_64
cmake -S . -B build/local-uwp-game \
  -DBOTW_FFX_SOURCE=~/.local/share/nxbox/deps/fidelityfx-1.1.4 \
  -DBOTW_FFX_SDK= \
  -DBOTW_FFX_WINE=/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine \
  -DBOTW_FFX_BOTTLE_PATH="$CX_BOTTLE_PATH"
# Wait for other worktrees' Ninja builds before starting this one.
while pgrep -x ninja >/dev/null; do sleep 10; done
nice ninja -C build/local-uwp-game -j4
```

Use the existing `uwp-clang-cl.cmake` toolchain and game-generated sources. When
copying an existing build, rewrite its generated text paths, discard its binary
`.ninja_deps` cache, and recreate its PCH. Copy only after the source build has
stopped: an active writer can leave a partial dependency record. The binary
cache and PCH still embed the original worktree paths. Build outputs stay in `build/`;
there is no SDK-local build directory to clean. The isolated bottle and generated
shader headers can be deleted after a build; recreate the bottle and remove
`ffx-shaders/compiled.stamp` to regenerate the headers.

`cmake/ffx_static.cmake` builds one static library containing only the FSR3
upscaler, its shared host utilities, the DX12 compute backend, and the FSR3 shader
blob accessor. It inherits clang-cl, `/MD`, the Store CRT library search order,
and AppContainer flags from the app. It excludes optical flow, frame
interpolation, the desktop swapchain wrapper, Anti-Lag, and PIX. A hash-checked
build-local copy of the backend disables PIX, binds the already-linked
`D3D12SerializeRootSignature` directly, and uses `VirtualAllocFromApp` for the
unused breadcrumb allocation path. The external checkout is unchanged.

`tools/compile_ffx_shaders.py` reproduces AMD's six permutation axes, ten passes,
and four wave/precision variants (40 headers, 64 logical permutations each).
The compiler deduplicates identical binaries. To work around its include-handler
failure on Wine's Z: drive, the driver expands SDK includes while retaining their
preprocessor guards and macros; DXC still preprocesses and compiles every variant.
Only two compiler threads run at once. CMake tracks the shader inputs and driver.
The existing renderer's color/depth conversion shaders retain their existing
D3DCompile path; the FSR compute permutations are embedded DXIL.

## Dispatch and native AA

D3D12Ffx owns a native `FfxFsr3UpscalerContext`, backend scratch memory, and all
three required shared UAVs (dilated depth, dilated motion, reconstructed previous
nearest depth), using SDK-reported descriptions. Shared UAVs begin/end dispatch in
the states registered with the SDK. Resize, retirement, and destruction drain GPU
work before releasing the context and resources. The existing FSR1 EASU/RCAS
fallback and `BOTW_UPSCALER=off|fsr1|fsr3` behavior remain.

The Series X automatic render-scale policy already renders natively at output
heights up to 1080p. Previously the strictly-larger-output gate bypassed FSR at
that size. Static builds also accept equal positive input/output dimensions when
FSR3 is selected: this is the SDK's NativeAA ratio **1.0**. At 1920x1080, FSR3
therefore runs at 1920x1080. Above 1080p the existing 1.5 quality ratio and output
caps remain; Series S retains its existing policy. Explicit scale/graphic-pack
rules retain authority. Downscaling and DRC remain on their previous output path.

After the first successful dispatch for each created context, the log prints:

```text
FidelityFX: FSR 3.1 static upscaler ready (native AA, 1920x1080)
```

Other sizes/modes report `upscaling` and their output dimensions. Context creation
alone does not emit a success line. Creation/dispatch failures log once and fall
back to FSR1, then the ordinary output filter if that path is unavailable. Missing
scene depth falls back for that frame and clears temporal history. At equal
sizes this fallback uses the existing EASU/RCAS pipeline at a 1.0 ratio.

## Exact input limitations

- **Depth:** the renderer's most-used full-size depth attachment paired with color
  in the current frame. Its dimensions, mip, slice, sample count, and sampleable
  D16/D24/D32 format are checked, then copied through a depth sampling pass to R32
  float. Normal depth, near 0.1, far 10000, and vertical FOV 1.04719755 radians
  remain nominal assumptions rather than recovered camera calibration.
- **Motion:** an actual RG16 float texture cleared to **zero**, with scale (1,1).
  There is no usable guest velocity buffer, camera-matrix reconstruction,
  previous-depth reprojection, or object motion. Reactive and composition masks
  are both the same R8 texture cleared to **one**.
- **Jitter:** **(0,0)**. The renderer has no reliable guest projection hook, so it
  does not query an offset and pretend that the guest applied it.
- **History:** the existing asynchronous three-slot 16x9 color probe detects image
  discontinuities; the first frame, >100 ms gaps, and detected cuts reset history.
  It may miss similar cuts or reset on pans/flashes. Missing depth clears history.
- **Color/UI:** existing display-sRGB to linear FP16 conversion, FSR3 dispatch,
  conversion back, and RCAS output. Guest HUD is already composed; host UI follows.

This is the existing **depth plus zero-motion approximation**, not a special AMD
mode that recovers missing velocities. NativeAA describes the resolution ratio;
with no applied jitter and conservative masks it does not promise the detail or
stability of a fully integrated temporal AA implementation.

## Validation and console checks

The task's exact build, host-test results, executable size delta, and complete
import list are recorded in `FSR3-STATIC-VALIDATION.md` beside this document.
Cross-compilation cannot establish successful execution on Xbox.

On Series X, check the success log at 1080p native output, then 1440p/4K quality
upscaling; capture the D3D12 debug layer for resource-state/UAV hazards and shader
model support. Compare pans, foliage, transparencies, HUD, cuts, and disocclusions
against `BOTW_UPSCALER=fsr1`; check missing-depth fallback, off override, resize,
quick-menu/screenshot/resume, and shutdown. Measure GPU time and VRAM. Confirm
frame generation remains off and normal CoreWindow presentation is unchanged.
