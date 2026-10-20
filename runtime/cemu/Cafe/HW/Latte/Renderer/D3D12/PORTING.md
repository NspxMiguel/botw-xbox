# BotW Recompiled D3D12 integration

This is a port of PrismFox67/Cemu-UWP's native Latte D3D12 backend, not a new
renderer. Provenance and the exact file inventory are in
[../../../../../VENDORED.md](../../../../../VENDORED.md).
The adjacent README is the imported upstream documentation and describes the
full Cemu frontend, including settings and features absent in this recomp.

## What is connected

The original device, submission/fence tracking, upload/readback allocators,
descriptor heaps, framebuffer/texture/view objects, queries, surface copies,
ImGui drawing, streamout, draw path, shader compiler, asynchronous shader/pipeline
workers, pipeline library/blob caches and transferable pipeline records are present.

Shaders use the original pipeline:

```
Latte -> existing GLSL decompiler (Vulkan dialect)
      -> glslang -> SPIR-V -> binding remap -> SPIRV-Cross HLSL
      -> DXC -> signed DXIL
```

DXC is the local default. The fork's FXC/DXBC fallback and optional Mesa
spirv_to_dxil code remain; the latter is not enabled or verified here.
Shader bytecode caches are separated by compiler. The existing shader-cache loader
opens/closes the D3D12 cache and preloads recorded pipelines. Driver pipeline
libraries remain disabled on Xbox through the fork's adapter detection; no new
Xbox pipeline-library workaround was invented here.

D3D12 gets Vulkan resource maps, flattened VS/GS interfaces, PS-matching stage
signatures, PS-dependent GS hashes, triangle-fan conversion, and correctly oriented
output shaders. Existing Metal paths and Metal source files are retained.
There are no new virtual methods in Renderer; the interface matched the fork.

## Host and build contract

`BOTW_RENDERER=metal` is the default. The W2 blocks in the root CMake file remove
D3D12 (including its Windows compatibility sources) from Apple builds. All new
C++ translation units are additionally guarded by `ENABLE_D3D12`, because the
separate iOS build also recursively collects vendored `.cpp` files.
`BOTW_RENDERER=d3d12` selects the backend and its dependency module. The platform
agent must still provide the Windows/UWP toolchain and platform-wide source,
compiler, linker and dependency selection: the root project outside these blocks
still has Apple language/architecture/framework settings.

Windows/UWP dependencies, built for the target architecture:

- Windows SDK: D3D12, DXGI, DXGUID, D3DCompiler; DXC's `dxcapi.h`.
- glslang CMake targets `glslang::glslang` and `glslang::SPIRV`.
- SPIRV-Cross config packages/targets `spirv_cross_core` / `spirv-cross-core`,
  `spirv_cross_glsl` / `spirv-cross-glsl`, `spirv_cross_hlsl` / `spirv-cross-hlsl`.
- Existing Cemu dependencies (fmt, Boost, GLM, zstd, libpng, ImGui, etc.).
- Package x64 `dxcompiler.dll` **and** `dxil.dll` with the UWP app. Without them,
  upstream logs the reason and falls back to FXC. No DXC import library is needed;
  the compiler is loaded dynamically.

The host must replace the Apple-only `runtime/cemu_shims/latte_glue.mm` entry
points with its UWP equivalents. W2 intentionally does not edit the host shim.
At the equivalent of renderer creation:

```cpp
g_renderer = std::make_unique<D3D12Renderer>();
D3D12Renderer::GetInstance()->InitializeSurface(
    {width, height}, true, D3D12Renderer::SurfaceType::CoreWindow, coreWindowIUnknown);
```

Continue the existing Latte/Renderer initialization and shutdown order. Use
`ResizeSurface`, `ShutdownSurface` and `GetSwapChain` for the host's surface
lifecycle. `SurfaceType::HWND` supports a desktop HWND; `Composition` creates a
swap chain for the host to attach with `ISwapChainPanelNative::SetSwapChain`.
The native CoreWindow pointer must be valid during surface creation. The renderer
no longer reads frontend-specific `WindowInfo::canvas_main/canvas_pad` fields.

Set `GetD3D12Settings()` before renderer construction (adapter/compiler) or on the
GPU thread before use (presentation/pipeline behavior). Defaults: adapter auto,
vsync on, normal upstream bounded pipeline waits, DXC, interpolation off.
The shader async switch uses the recomp's existing `GetConfig().async_compile` bool.

The compatibility directory supplies the two missing Windows FileStream files and
the small Windows platform header. The renderer CMake block excludes the Unix
FileStream/platform implementations when these are selected. A platform owner
adding its own Windows FileStream must remove the duplicate compatibility sources.

## Missing full-Cemu subsystems

- The recomp has no process-wide Cemu crash callback. Previously quarantined
  pipeline keys are read, but crashes do not automatically append new keys.
- The Cemu filesystem's scene-open observer is absent. The host may call
  `D3D12PipelineCache::OnGameFileOpened(path)`; automatic scene preloading is not
  wired to the recomp filesystem. Ordinary pipeline recording/preloading works.
- The Cemu frontend's memory-pressure monitor and cache-trimming hooks are absent.
  `D3D12SetMemoryPressure(level)` preserves pipeline-worker throttling (allocation
  failure sets level 2); the host must update/clear it after recovery. The removed
  trim call is not replaced with an unrelated texture eviction implementation.
- No Cemu settings frontend or display-mode controller was imported. Backend-local
  settings replace the missing fields; display-mode refresh is unknown (0), and
  interpolation is off by default.

## Verification on the Mac (2026-10-20)

No game files, generated guest code, simulator, running preview, commit or GPU
execution was used. The reference checkout was not modified.

**UWP syntax: 29/29 passed, zero diagnostics classified as errors.** This covers
all 21 backend `.cpp` files, `compat/Common/windows/FileStream_win32.cpp`, and:

- `Core/LatteShader.cpp`, `Core/LatteShaderCache.cpp`, `Core/LatteIndices.cpp`;
- `LegacyShaderDecompiler/LatteDecompiler.cpp`, `LatteDecompilerEmitGLSL.cpp`;
- `Renderer/Renderer.cpp`, `Renderer/RendererOuputShader.cpp`.

Compiler: Homebrew Clang 23.1.3, `-target x86_64-pc-windows-msvc -std=c++20
-fsyntax-only -DCEMU_UWP -DWINAPI_FAMILY=WINAPI_FAMILY_APP`.
Real MinGW-w64 14 headers and GCC 16.2 STL supplied Windows/D3D12/COM declarations.
DXC headers came from `/tmp/dxcwin/inc`; SPIRV-Cross headers were available in an
existing local dependency tree. glslang headers came from Homebrew.

The Microsoft SDK/MSVC STL and xwin executable were unavailable. Attempting to
reach GitHub to obtain xwin failed with `Could not resolve host: github.com`.
The direct MSVC-target invocation without a supplied STL stops at
`fatal error: 'vector' file not found`.

**Explicit validation substitutions:** the script makes a temporary PCH overlay
using `_WIN32` for Windows type selection, public QueryPerformanceCounter/Frequency
in place of MSVC STL private helpers, and standard future.wait_for in place of
future._Is_ready. It enables Clang's GNU extensions for the MinGW STL. GLM is unused
by these units; its two absent PCH includes are empty validation headers. MinGW
also omits `fileapifromapp.h`; UWP validation supplies only the used
`CreateFileFromAppW` declaration. No Renderer, Latte, D3D12, DXGI, COM, pipeline,
shader translator or shader compiler implementation is mocked.

Reproduce using actual dependency locations:

```sh
python3 runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/validation/syntax_check.py \
  --dxc-include /path/to/dxc/inc \
  --spirv-cross-include /path/to/spirv-cross/headers \
  --uwp --log /tmp/botw-w2-uwp.log
```

The SPIRV-Cross option accepts either a directory containing `spirv_cross/` or
the directory containing `spirv_hlsl.hpp` itself. For authoritative SDK validation,
add `--xwin-root /path/to/xwin/splat`; this uses xwin's SDK/STL directly without
PCH substitutions. Omit `--uwp` to check desktop branches.

**Apple syntax: 9/9 passed** with Apple Clang, C++20, `ENABLE_METAL`, the original
PCH, real Metal/Foundation headers and the same empty unused GLM includes:
LatteShader, LatteShaderCache, LatteIndices, LatteDecompiler, EmitGLSL, EmitMSL,
Renderer, RendererOuputShader, and MetalRenderer. No files in `Renderer/Metal/`
were changed. This checks the affected integration and MSL path, not a full app
build or visual regression on a device.

## Remaining compile issues and runtime gaps

- **No remaining compile errors in the 29-unit controlled UWP syntax check.**
  The official SDK/MSVC-STL compile, complete application link, package validation
  and Xbox execution remain unverified. The missing real SDK/STL is the known
  blocker for an authoritative Windows compilation on this Mac.
- W3 must connect the renderer in the Windows replacement for `latte_glue.mm`;
  W4 must finish the surrounding platform CMake/toolchain. The Apple host shim is
  still selected by the original top-level source glob until that integration.
- Windows/UWP target libraries and DXC DLL packaging are still required. Syntax
  checking does not verify target library ABI, DLL loading, GLSL/HLSL compilation,
  signed DXIL acceptance, PSO creation, GPU synchronization or rendered pixels.
- Upstream rendering limitations remain: feedback-loop copies/read-only depth
  handling, cross-family texture reinterpretation, unaligned vertex attributes,
  depth array-slice views, point sprites, alternate-format readback, and separate
  back-face stencil masks/reference. See the upstream README for details.
- None of the upstream performance or commercial-game claims was independently
  reproduced in this worktree. A Windows debug-layer run and Xbox gameplay test
  remain necessary before calling this backend runtime-validated.
