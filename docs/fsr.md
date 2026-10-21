# Xbox D3D12 FidelityFX integration

## Implemented scope

The D3D12 TV output has an AMD FSR 1 FP32 EASU + RCAS path, using the
unaltered MIT AMD shader headers under `runtime/third_party/fidelityfx`.
It needs no guest motion vectors, depth, jitter or previous frame. This is
spatial FSR 1, **not temporal FSR 3.1 and not AMD frame generation**.

Automatic startup selection keeps the swapchain at HDMI output dimensions and
selects internal resolution independently:

| Console | Auto internal scale | Internal at 4K output |
| --- | --- | --- |
| Series S, including BOTW_SIM_SERIES_S | at most 1.5 | 1920 x 1080 |
| Series X / other D3D12 | at most 3.0 | 3840 x 2160 |

Lower output resolutions further cap internal scale. Explicit scale is clamped to
the console/output limits. Presets divide the output dimensions by 1.3 (Ultra
Quality), 1.5 (Quality), 1.7 (Balanced), or 2.0 (Performance), also respecting the
console cap and 720p minimum. At 4K, Series X Quality uses 2560 x 1440; Series S
uses its 1080p cap. Native output retains the user-selected output filter; FSR runs only when neither dimension shrinks and at least one grows. Manual graphic-pack texture
rules retain authority over render-target dimensions. No FPS claim is implied by
these caps; real console GPU measurements are still needed.

The existing output shader first draws at the source's effective resolution into
an RGBA8 intermediate. This preserves source channel swizzle, Y orientation,
TV sRGB encoding, gamma and existing graphic-pack output transformations. EASU
writes an output-sized RGBA8 intermediate; RCAS writes into the display image
rectangle, preserving letterbox/crop placement. When upscaling, RCAS uses AMD sharpness 0.2 stops
(lower means stronger); edge loads clamp to valid texels. DRC output/downscaling
retain the original path. Host ImGui/quick-menu rendering happens after FSR;
the guest HUD is already part of the spatial input and is filtered with it.

Size changes detected by the output pass are queued for the end of `SwapBuffers`,
after TV/DRC submission and presentation. The transition frame uses the selected
output filter. FSR target creation/replacement never waits for GPU idle and never
runs inside the acquired-backbuffer window. Old resources retire by submission
fence; SRVs are copied into the submission-tracked GPU descriptor ring, and RTV
CPU descriptors are consumed while recording. Allocation/shader errors log once
and fall back to the existing output shader. The shader headers are embedded as
arrays of literals of at most 12,000 bytes and joined at runtime to avoid MSVC
literal/concatenation limits. The code is built only for D3D12; Metal is untouched.

## Render scale

Render scale is selected only at startup. Adaptive scaling and its setting have
been removed: the built-in graphic-pack rules are consumed at texture allocation,
and changing the scale alone cannot safely migrate persistent GPU contents,
views and aliases. Live migration requires a separate renderer implementation.
Series S simulation instrumentation is created only when `BOTW_SIM_SERIES_S=1`.

## Settings plumbing

The quick menu need not change yet. `Settings` and UWP LocalSettings carry:

| Key | Values / default |
| --- | --- |
| `renderScalePercent` | 0 auto; explicit scale x100, capped at startup |
| `upscaler` | 0 auto, 1 off, 2 FSR1, 3 request temporal; default auto |
| `fsrQuality` | 0 auto, 1 Ultra Quality, 2 Quality, 3 Balanced, 4 Performance |
| `frameGeneration` | 0 auto, 1 off, 2 request FSR3.1; default auto |

Upscaler changes are published atomically by UWP settings; scale/preset changes
apply on the next launch. Enum values are reserved for the w17 FSR 3.1 job. Temporal requests explicitly
resolve to spatial FSR1, and all frame-generation requests explicitly resolve to
Off when settings are applied. Neither unsupported mode is offered in the UI.
The old custom interpolator remains disabled. There is no capability claim that FG is ready.

## FSR 3.1 feasibility and exact integration points

No captured game frames, guest buffer traces or validated per-build uniform
layouts are available here. Resolution/format matches are useful candidates,
not proof of a final-scene depth or camera matrix. Consequently temporal FSR
and frame interpolation are deliberately not dispatched.

### Scene color / depth and ordering

- `Core/LatteRenderTarget.cpp::LatteRenderTarget_copyToBackbuffer` updates the
  guest scanout view, obtains effective dimensions, selects output transforms,
  calls `DrawBackbufferQuad`, then overlays host UI. This view is the **final
  composed scanout**, not a guaranteed HUD-less or pre-tonemap scene.
- `LatteMRT::SetDepthAndStencilAttachment`, `LatteMRT::GetDepthAttachment`, and
  `LatteMRT_CreateDepthBuffer` identify depth attachments during scene draws.
  `D3D12RendererCore.cpp::draw_applyRenderTargets` is the backend capture hook.
  Track color/depth pairs by guest physical address, view slice/mip, shader hash,
  native/effective dimensions and frame use; exclude shadow and UI passes.
- `cemu_shims/botw_texture_rules.inc` has the 1280x720 main-resolution candidate
  whitelist: 0x001, 0x005, 0x007, 0x019, 0x01a, 0x41a, 0x80e, 0x806, 0x816, 0x820,
  excluding tile mode 0x001. These are broad scaling rules covering several
  render targets, **not a validated buffer identity table**. Validate with
  GPU captures across BotW regions, weather, camera modes and cutscenes.
- `LatteTextureD3D12` provides the native resource/state; `LatteTextureViewD3D12`
  provides view-aware SRVs. Retain the correct scene depth before any clear or
  reuse and convert to an FSR-compatible single-sample depth SRV. Determine
  reversed/infinite depth, near/far planes and render-size units from captures.

### Motion and jitter

- `D3D12RendererCore.cpp::draw_buildStageBindings` binds uniform buffers from
  `StageBindings::uniformBufferAddress/Size`. `draw_prepareUniformVars` uploads
  ALU constants from `LatteGPUState.contextRegister`. Trace CPU source bytes in
  the buffer-cache upload path before upload, with guest shader hashes; D3D12
  GPU virtual addresses alone do not identify camera semantics.
- Identify the current and previous **unjittered** view-projection matrices,
  matrix convention and byte order per supported game build. For a static world
  point reconstruct position from depth using inverse(current VP), reproject
  with previous VP, and produce current-to-previous motion in FSR's declared
  pixel/UV units. Validate sign with a controlled camera pan.
- Depth + camera matrices give camera motion only. BotW has moving characters,
  skinning, foliage, particles, water and transparency. Their per-object previous
  transforms / skin matrices or a verified guest velocity target are additionally
  required for high-quality per-pixel motion. Do not label camera-only reprojection
  as full scene motion. Optical flow in FG is not a substitute for these inputs.
- Apply the tested Halton pixel offsets from `fsr::jitter` to the verified scene
  projection (not UI, shadows or the output quad). `renderTarget_setViewport`
  alone cannot reliably distinguish those passes. Pass the exact offsets and
  render/display dimensions to FSR, avoid double jitter, and reset history on
  camera cuts, teleports, resize, pause/resume and discontinuous frame IDs.
- Supply exposure/pre-exposure and frame delta. For transparency and animated
  effects, derive reactive and transparency/composition masks from identified
  passes, with an explicit fallback until validated.

### SDK and dispatch

Use a pinned official [FidelityFX SDK 1.1.4 release](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v1.1.4)
(FSR 3.1 family), with MIT LICENSE and a verified immutable commit/archive hash.
The SDK itself is **not fetched or vendored in this change**: browser access was
denied and the shell network is disabled. The offline fallback headers are from
the older official AMD FSR1 repository; they are not misrepresented as SDK 1.1.4.
The fallback configure step is entirely offline and verifies the vendored hashes.

For the follow-up, vendor just the required SDK host contexts, D3D12 backend,
shared GPU headers and shader permutation blobs. Where SDK builds require
Windows shader tooling, consume its supplied prebuilt headers, with a build-time
check that all used permutations are present. Build neither the samples nor
unrelated FidelityFX effects. Verify shader model / wave / format support and
memory budget on **UWP Xbox**, separately from GDK console support.

Create upscaler contexts at maximum render/display sizes. Dispatch after the
identified scene composition and before UI, with color, depth, motion, jitter,
exposure, render size, frame delta, near/far/FOV and history reset. Composite UI
only once at display resolution. Reference: [AMD upscaler integration](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/).

### Frame interpolation / swapchain pacing / HUD

`D3D12Renderer::PresentSwapChain` is the scene-present gate;
`D3D12SwapChain::Present` currently forces interval 1 at 60 game fps and 2 at
30 fps (`g_target_fps`). It must be changed deliberately for AMD-generated
presents: two interval-1 presents per 30 Hz real frame on a 60 Hz display,
including proper DXGI buffer acquisition, fence ownership, frame IDs, latency
and waitable pacing. The existing `PresentInterpolated` custom block matcher
is **not FSR** and remains disabled for BotWRecomp. Repeating the final frame
or enabling this block matcher would not satisfy AMD FI integration.

Evaluate the SDK frame-generation swapchain against CoreWindow / Composition
creation, UWP restrictions and Xbox runtime interfaces before adopting it. The
SDK is not automatically deployable just because RDNA2 supports its shaders.
If unsupported, keep FI unavailable until a supported integration is proven;
do not cast UWP swapchains to unsupported desktop interfaces.

Obtain HUD-less color before guest UI and a separate UI layer with usable alpha,
then use AMD HUD-less/UI composition support. A UI callback must render or
composite UI independently for every generated/real present; supplying the
already composed final image as an opaque UI texture would overwrite the scene
and cannot fix HUD interpolation. If these layers cannot be identified, leave
FG off. Handle host ImGui in that same presentation composition boundary.

Enable FI automatically only when the required inputs/capabilities are valid,
the real game is stably near 30 Hz, and the display can pace generated frames.
Do not change the guest vblank clock or FPS++ game logic. Disable and reset on
loading, scene cuts, pause, insufficient memory, or native 60 Hz performance.

## Validation and remaining hardware work

Portable tests: `runtime/src/platform/tests/fsr_test.cc` checks console caps,
preset ratios, invalid inputs, strict upscale gating, explicit temporal/FG
fallbacks and Halton bounds. It is registered as `fsr_test` in the platform CTest
harness.
Build/run directly with C++20:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror runtime/src/platform/tests/fsr_test.cc -o /tmp/fsr-test
/tmp/fsr-test
```
 The HLSL wrapper can be compiled by glslang for
syntax/SPIR-V verification; this does not validate FXC/D3D12 driver behavior.

Requested UWP and macOS build results are recorded in the task response.
On-console image quality, gamma/orientation, full-frame/crop/resize correctness,
HUD legibility, actual shader creation, GPU time, memory and long-run stability
remain unverified. SDK 3.1 temporal, FI, and live adaptive target migration remain
planned requirements; this change implements spatial FSR1 and startup selection/settings.
