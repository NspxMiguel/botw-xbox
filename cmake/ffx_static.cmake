# Source-only FidelityFX 1.1.4 integration. The SDK's stock build also includes
# desktop presentation/PIX, so select only the upscaler and compute backend.
set(BOTW_FFX_SOURCE
    ""
    CACHE PATH "External FidelityFX-SDK v1.1.4 checkout")
set(BOTW_FFX_WINE
    ""
    CACHE FILEPATH
          "CrossOver wine executable for the host shader compiler (macOS)")
set(BOTW_FFX_BOTTLE_PATH
    "${CMAKE_BINARY_DIR}/ffx-bottles"
    CACHE PATH "Isolated CrossOver bottle directory (contains ffx)")
if(NOT BOTW_FFX_SOURCE)
  return()
endif()
set(_ffx "${BOTW_FFX_SOURCE}/sdk")
file(SHA256 "${_ffx}/src/backends/dx12/ffx_dx12.cpp" _ffx_backend_hash)
# Keep generated portability edits tied to the exact reviewed SDK source.
if(NOT _ffx_backend_hash STREQUAL
   "3bad3a805daead55b061f8594e7fb789991eb466852757cf508ec425fc7066aa")
  message(FATAL_ERROR "BOTW_FFX_SOURCE must point to the unmodified v1.1.4 SDK")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_ffx_shaders "${CMAKE_BINARY_DIR}/ffx-shaders")
set(_ffx_host_args)
if(CMAKE_HOST_APPLE)
  if(NOT EXISTS "${BOTW_FFX_WINE}"
     OR NOT EXISTS "${BOTW_FFX_BOTTLE_PATH}/ffx/cxbottle.conf")
    message(
      FATAL_ERROR
        "Static FSR on macOS needs BOTW_FFX_WINE and an isolated ffx CrossOver bottle at BOTW_FFX_BOTTLE_PATH; see docs/FSR3-INTEGRATION.md"
    )
  endif()
  list(APPEND _ffx_host_args --wine "${BOTW_FFX_WINE}" --bottle-path
       "${BOTW_FFX_BOTTLE_PATH}")
endif()
file(
  GLOB_RECURSE _ffx_shader_inputs CONFIGURE_DEPENDS
  "${_ffx}/include/FidelityFX/gpu/*.h" "${_ffx}/include/FidelityFX/gpu/*.hlsl"
  "${_ffx}/src/backends/dx12/shaders/fsr3upscaler/*.hlsl")
add_custom_command(
  OUTPUT "${_ffx_shaders}/compiled.stamp"
  COMMAND
    "${Python3_EXECUTABLE}" -P
    "${CMAKE_SOURCE_DIR}/tools/compile_ffx_shaders.py" --source
    "${BOTW_FFX_SOURCE}" --output "${_ffx_shaders}" ${_ffx_host_args}
  DEPENDS ${_ffx_shader_inputs}
          "${CMAKE_SOURCE_DIR}/tools/compile_ffx_shaders.py"
  COMMENT "Compiling FidelityFX FSR 3.1 DXIL permutations on the build host"
  VERBATIM)
add_custom_target(botw_ffx_shaders DEPENDS "${_ffx_shaders}/compiled.stamp")

# Generate the UWP compute-only backend without modifying the external checkout.
# PIX is optional desktop instrumentation. Root-signature serialization already
# comes from our linked d3d12 library; no module lookup is needed in
# AppContainer.
file(READ "${_ffx}/src/backends/dx12/ffx_dx12.cpp" _ffx_backend)
string(REPLACE "#define ENABLE_PIX_CAPTURES 1"
               "// PIX disabled in the UWP static backend." _ffx_backend
               "${_ffx_backend}")
string(REPLACE "HMODULE d3d12ModuleHandle = GetModuleHandleW(L\"D3D12.dll\");"
               "const bool d3d12ModuleHandle = true;" _ffx_backend
               "${_ffx_backend}")
string(
  REPLACE
    "(D3D12SerializeRootSignatureType)GetProcAddress(d3d12ModuleHandle, \"D3D12SerializeRootSignature\")"
    "&D3D12SerializeRootSignature"
    _ffx_backend
    "${_ffx_backend}")
# Breadcrumb allocation is unrelated to upscaling; use UWP's
# VirtualAllocFromApp.
string(
  REPLACE
    "VirtualAlloc(nullptr, (SIZE_T)resDesc->Width, MEM_COMMIT, PAGE_READWRITE)"
    "VirtualAllocFromApp(nullptr, (SIZE_T)resDesc->Width, MEM_COMMIT, PAGE_READWRITE)"
    _ffx_backend
    "${_ffx_backend}")
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/ffx-uwp/ffx_dx12.cpp" CONTENT
     "@_ffx_backend@" @ONLY)
add_library(
  botw_ffx STATIC
  "${CMAKE_BINARY_DIR}/ffx-uwp/ffx_dx12.cpp"
  "${_ffx}/src/backends/shared/ffx_shader_blobs.cpp"
  "${_ffx}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
  "${_ffx}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
  "${_ffx}/src/shared/ffx_assert.cpp"
  "${_ffx}/src/shared/ffx_message.cpp"
  "${_ffx}/src/shared/ffx_object_management.cpp"
  "${_ffx}/src/shared/ffx_breadcrumbs_list.cpp")
add_dependencies(botw_ffx botw_ffx_shaders)
set_property(TARGET botw_ffx PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
target_compile_definitions(botw_ffx PRIVATE FFX_FSR3UPSCALER NOMINMAX
                                            WIN32_LEAN_AND_MEAN)
target_include_directories(
  botw_ffx
  PUBLIC "${_ffx}/include"
  PRIVATE "${_ffx}/src/shared" "${_ffx}/src/components"
          "${_ffx}/src/backends/shared" "${_ffx_shaders}")
target_link_libraries(botw_ffx PUBLIC d3d12 dxgi dxguid)
if(MSVC)
  target_compile_options(botw_ffx PRIVATE /O2 /w /FIbit)
endif()
target_include_directories(cemu_gpu PRIVATE "${_ffx}/include")
target_compile_definitions(cemu_gpu PRIVATE BOTW_HAS_FFX=1 BOTW_FFX_STATIC=1)
add_compile_definitions(BOTW_HAS_FFX=1)
list(APPEND BOTW_D3D12_LIBRARIES botw_ffx)
install(
  FILES "${_ffx}/LICENSE.txt"
  DESTINATION Notices
  RENAME FidelityFX-LICENSE.txt
  COMPONENT Runtime)
