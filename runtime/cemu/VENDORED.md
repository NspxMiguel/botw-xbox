# Vendored Cemu source

Files under this directory are copied from Cemu (https://github.com/cemu-project/Cemu, commit a53cf0e,
MPL-2.0) and keep Cemu's paths so its includes resolve. They are the Latte GPU layer (PM4 command
processor, shader decompiler, texture/surface cache, Metal renderer) plus the Common headers they need.
Changes for this runtime are marked `// BotWRecomp:`; Cemu subsystems that are not vendored (config,
graphic packs, overlay, disk caches, OS) are provided by shims in `runtime/cemu_shims/`.


## Xbox D3D12 renderer import (2026-10-20)

Source: [PrismFox67/Cemu-UWP](https://github.com/PrismFox67/Cemu-UWP/tree/aeaba14902a98fe21fca0a5a03f60ebc2a3448d7),
branch `d3d12-backend`, commit `aeaba14902a98fe21fca0a5a03f60ebc2a3448d7`.
Copied from the local reference checkout; no game files or generated guest code were used.
All imported Cemu code remains **MPL-2.0**, including the shader translator and pipeline caches.
MPL notices have been retained or added explicitly to the imported C++ files.

The complete upstream `src/Cafe/HW/Latte/Renderer/D3D12/` directory was copied to
`Cafe/HW/Latte/Renderer/D3D12/` (41 files, including the upstream README):

- `CachedFBOD3D12.cpp`
- `CachedFBOD3D12.h`
- `D3D12BindingModel.h`
- `D3D12Common.cpp`
- `D3D12Common.h`
- `D3D12DescriptorHeaps.cpp`
- `D3D12DescriptorHeaps.h`
- `D3D12Format.cpp`
- `D3D12Format.h`
- `D3D12FrameInterpolator.cpp`
- `D3D12FrameInterpolator.h`
- `D3D12ImGui.cpp`
- `D3D12ImGui.h`
- `D3D12InternalPipelines.h`
- `D3D12Memory.cpp`
- `D3D12Memory.h`
- `D3D12PipelineCache.cpp`
- `D3D12PipelineCache.h`
- `D3D12Query.cpp`
- `D3D12Renderer.cpp`
- `D3D12Renderer.h`
- `D3D12RendererCore.cpp`
- `D3D12RootSignature.h`
- `D3D12ShaderCompiler.cpp`
- `D3D12ShaderCompiler.h`
- `D3D12ShaderTranslate.cpp`
- `D3D12ShaderTranslate.h`
- `D3D12ShaderTranslateHLSL.cpp`
- `D3D12SpirvRemap.cpp`
- `D3D12SurfaceCopy.cpp`
- `D3D12SwapChain.cpp`
- `D3D12SwapChain.h`
- `D3D12TextureReadback.cpp`
- `D3D12TextureReadback.h`
- `LatteTextureD3D12.cpp`
- `LatteTextureD3D12.h`
- `LatteTextureViewD3D12.cpp`
- `LatteTextureViewD3D12.h`
- `README.md`
- `RendererShaderD3D12.cpp`
- `RendererShaderD3D12.h`

Additional upstream imports and hunks:

- `src/Common/windows/FileStream_win32.{h,cpp}` →
  `Cafe/HW/Latte/Renderer/D3D12/compat/Common/windows/FileStream_win32.{h,cpp}`.
  The vendored FileCache and texture-loader interfaces require this missing Windows implementation.
- `src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h`: additive
  `declareAllPSInputs` and `flattenV2GInterface` options.
- `src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSL.cpp` and
  `LatteDecompilerEmitGLSLHeader.hpp`: D3D12 stage signature and geometry-interface emission.

Local integration changes (all under Latte, plus the renderer CMake blocks):

- `Renderer/Renderer.h`: append D3D12 to RendererAPI and add the Vulkan-GLSL helper.
  No virtual Renderer methods changed. The unavailable DXGIWrapper is bypassed for
  D3D12 in this header and `Renderer/Renderer.cpp`; D3D12 queries DXGI directly.
- `Core/LatteShader.cpp`: Vulkan resource mapping, GLSL graphic-pack selection,
  D3D12 decompiler options, and geometry-shader hashes that include pixel inputs.
- `LegacyShaderDecompiler/LatteDecompiler.cpp`: enable the existing GLSL emitter for D3D12.
- `Renderer/RendererOuputShader.cpp`: Vulkan output GLSL with D3D12's Y orientation.
- `Core/LatteShaderCache.cpp`: open/close D3D12 shader caches and preload recorded pipelines.
- `Core/LatteIndices.cpp`: reuse existing Metal triangle-fan conversion for D3D12.
- Backend-local `D3D12Runtime.h` replaces absent Cemu frontend settings and supplies
  a memory-pressure signal. `compat/Common/windows/platform.h` supplies Windows types
  and GetLastError without importing the rest of Cemu's host platform.
- Surface initialization takes an explicit HWND/CoreWindow/composition target and native
  handle. No changes to the shared WindowSystem shim or Metal sources are required.
- Cemu's process-wide crash callback and filesystem scene observer are absent and are
  not installed. Pipeline quarantine reading and the scene notification entry point remain.
- New translation units are guarded by `ENABLE_D3D12` for the independent iOS source glob.
- Default shader compiler is DXC (the upstream FXC fallback remains); cache contents are
  separated by compiler. Renderer destruction also closes the bytecode cache after workers stop.

See [D3D12 integration and verification](Cafe/HW/Latte/Renderer/D3D12/PORTING.md)
for the exact validation limits, host contract, dependencies and remaining work.
