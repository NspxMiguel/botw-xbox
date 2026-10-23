# Static FSR 3.1 validation — 2026-10-23

## Result

`nice ninja -C build/local-uwp-game -j4` successfully links the full game
`BotWRecomp.exe` using the existing clang-cl/xwin UWP toolchain. The copied base
build and generated sources were placed in this worktree, generated text paths
were rewritten, and the worktree's PCH and source objects were rebuilt. The
copied binary Ninja dependency cache contained an invalid node record; it was
discarded and rebuilt for this worktree before the final validation. A subsequent
exact build command reports `ninja: no work to do`, with no cache warnings. Only one
Ninja process was run at a time, waiting for other worktrees before starting.
The SDK objects use `/MD`, `WINAPI_FAMILY_APP`, and the same Store CRT search
order as the app. The build has the existing clang-cl `/await:strict` unused-flag
warnings; no compilation or link errors remain.

SDK: **FidelityFX SDK v1.1.4**, commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`, native **FSR upscaler 3.1.4**, MIT.
The external checkout is `/Users/miguel/.local/share/nxbox/deps/fidelityfx-1.1.4`
and remains unmodified. No SDK source was vendored and no frame-generation
component was linked.

Neither AMD's tag checkout nor its release ZIP ships precompiled permutation
headers. The build therefore uses the SDK's own shipped shader compiler and DXC
through an isolated CrossOver bottle on the Mac. All 40 wave/precision permutation
headers compiled successfully at build time. This departs from the task's assumed
precompiled-header route, without introducing any runtime FidelityFX DLL.

## Executable size

| Artifact | Bytes | MiB |
| --- | ---: | ---: |
| Copied baseline executable | 417,592,832 | 398.25 |
| Static FSR executable | 420,019,200 | 400.56 |
| Delta | +2,426,368 | +2.31 |

SHA-256: `8d0d2ef82064e4335669d958308101bd99c259e6e52c7e4a598a70532f55658f`.

The executable contains the static-upscaler success message and no
`amd_fidelityfx` DLL-name string. D3D12Ffx's object references the native
`ffxFsr3UpscalerContextCreate/Dispatch/Destroy` functions, with no FidelityFX
loader or `GetProcAddress` reference.

## Final executable imports

Inspected with:

```sh
/opt/homebrew/opt/llvm/bin/llvm-objdump -p build/local-uwp-game/BotWRecomp.exe
```

No `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, `MSVCP140.dll`, `USER32.dll`,
`WINMM.dll`, `VERSION.dll`, or FidelityFX DLL is imported. The `_APP` CRT DLLs
below are the app's existing **Store CRT**, not the incompatible desktop CRT.
Complete unique DLL list (54 DLLs, original spelling retained):

```text
bcrypt.dll
dxgi.dll
d2d1.dll
d3d11.dll
d3d12.dll
MFPlat.DLL
MFReadWrite.dll
api-ms-win-core-handle-l1-1-0.dll
api-ms-win-core-com-l1-1-0.dll
api-ms-win-core-synch-l1-1-0.dll
api-ms-win-core-file-l1-2-0.dll
api-ms-win-core-file-fromapp-l1-1-0.dll
api-ms-win-core-processthreads-l1-1-0.dll
DWrite.dll
api-ms-win-core-file-l1-1-0.dll
api-ms-win-core-localization-l1-2-0.dll
api-ms-win-core-libraryloader-l1-2-0.dll
api-ms-win-core-errorhandling-l1-1-0.dll
api-ms-win-core-sysinfo-l1-1-0.dll
api-ms-win-core-processthreads-l1-1-1.dll
api-ms-win-core-libraryloader-l2-1-0.dll
api-ms-win-core-heap-l2-1-0.dll
api-ms-win-core-file-l2-1-0.dll
api-ms-win-core-string-l1-1-0.dll
api-ms-win-core-debug-l1-1-0.dll
api-ms-win-core-profile-l1-1-0.dll
api-ms-win-core-processthreads-l1-1-3.dll
api-ms-win-core-memory-l1-1-3.dll
api-ms-win-core-memory-l1-1-0.dll
z.dll
zstd.dll
MSVCP140_APP.dll
api-ms-win-core-synch-l1-2-0.dll
api-ms-win-core-winrt-l1-1-0.dll
api-ms-win-core-threadpool-l1-2-0.dll
OLEAUT32.dll
api-ms-win-core-heap-l1-1-0.dll
api-ms-win-core-interlocked-l1-1-0.dll
api-ms-win-core-winrt-error-l1-1-0.dll
api-ms-win-core-com-l1-1-1.dll
api-ms-win-core-winrt-error-l1-1-1.dll
VCRUNTIME140_APP.dll
VCRUNTIME140_1_APP.dll
api-ms-win-crt-runtime-l1-1-0.dll
api-ms-win-crt-locale-l1-1-0.dll
api-ms-win-crt-stdio-l1-1-0.dll
api-ms-win-crt-math-l1-1-0.dll
api-ms-win-crt-filesystem-l1-1-0.dll
api-ms-win-crt-time-l1-1-0.dll
api-ms-win-crt-convert-l1-1-0.dll
api-ms-win-crt-environment-l1-1-0.dll
api-ms-win-crt-string-l1-1-0.dll
api-ms-win-crt-heap-l1-1-0.dll
api-ms-win-crt-utility-l1-1-0.dll
```

## Tests and packaging

- `runtime/src/platform/tests`: 6/6 CTest cases pass, including `fsr_test` and
  `fsr3_policy_test`; NativeAA coverage checks equal dimensions, invalid sizes,
  upscaling, and `fsr1`/`off` exclusion.
- `bash tools/tests/run_latte_perf3_tests.sh`: all 11 native/x86 executable
  invocations pass with ASan/UBSan, including Rosetta SIMD coverage.
- `uv run --cache-dir "$PWD/build/uv-cache" python -P -m unittest discover
  -s tools/tests -v`: the FST inheritance fixture passes.
- D3D12Ffx syntax checks pass with static FSR, SDK omitted, and legacy SDK 2.3
  DLL headers. The full static build also compiles the native-AA output routing.
- A clean runtime install under `build/ffx-stage` includes
  `Notices/FidelityFX-LICENSE.txt` byte-for-byte identical to `sdk/LICENSE.txt`,
  and contains no FidelityFX DLL. The staged executable matches the linked size.
  The temporary staging copy and downloaded release ZIP were removed afterward.
- No SDK-local build directory or SDK build intermediates were created. Generated
  blobs and all compiler intermediates live under this project's `build/`.
  The project build remains available for inspection and incremental rebuilding.

## Files changed

| File | Change |
| --- | --- |
| `cmake/ffx_static.cmake` | External source option, static source selection, UWP backend adaptations, shader build dependency, MIT notice installation |
| `tools/compile_ffx_shaders.py` | Host shader compilation with AMD permutation/reflection output and Wine include workaround |
| `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/renderer.cmake` | Static source selection takes precedence over the DLL SDK |
| `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12Ffx.cpp` | Direct native context calls, required shared UAVs, teardown, static success log; DLL/FG layer compiled out |
| `runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12SurfaceCopy.cpp` | Allow equal-size temporal dispatch for NativeAA |
| `runtime/src/fsr3_policy.h` | Portable NativeAA eligibility predicate |
| `runtime/src/platform/tests/fsr3_policy_test.cc` | NativeAA selection/fallback boundary coverage |
| `docs/FSR3-INTEGRATION.md` | Static build and integration guide, input limits and console checks |
| `docs/FSR3-LEGACY-DLL.md` | Preserve previous DLL integration notes as historical reference |
| `docs/fsr.md` | Update spatial/temporal scope and policy documentation |
| `docs/FSR3-STATIC-VALIDATION.md` | This measured validation report |

`runtime.cpp` already had the native-through-1080p render-scale policy and needed
no modification. Equal 1920x1080 input/output is NativeAA at ratio 1.0; above
1080p the existing ratio policy remains.

## Console verification still required

No console deployment or game execution was performed here. Verify:

1. At 1080p output, the first successful dispatch logs
   `FidelityFX: FSR 3.1 static upscaler ready (native AA, 1920x1080)`.
   Test 1440p/4K quality upscaling and measure GPU time/VRAM.
2. D3D12 debug-layer acceptance of the DXIL shader models, typed UAV formats,
   descriptors, resource transitions, and repeated frame dispatches.
3. Pans, foliage, transparency, HUD, cuts, and disocclusions compared with FSR1.
   The game provides **no usable motion vectors**: the adapter keeps the real
   RG16 zero-motion texture, both R8 masks at one, jitter (0,0), heuristic scene
   depth and image-cut resets. It performs no camera reconstruction. NativeAA
   names the ratio, not a claim of full temporal-AA quality with these inputs.
4. Missing-depth and failure fallback, `BOTW_UPSCALER=fsr1|off|fsr3`, resize,
   quick-menu/screenshot/resume, shutdown, and normal CoreWindow presentation.
   Static frame generation must remain off.
