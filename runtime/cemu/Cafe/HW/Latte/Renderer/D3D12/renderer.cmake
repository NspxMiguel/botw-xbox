# BotWRecomp: D3D12 dependencies. Include after creating cemu_gpu.
# Use Windows/UWP builds of these libraries; never link host Homebrew libraries.
find_package(glslang CONFIG REQUIRED)
find_package(spirv_cross_core CONFIG REQUIRED)
find_package(spirv_cross_glsl CONFIG REQUIRED)
find_package(spirv_cross_hlsl CONFIG REQUIRED)
# DXC ships as a host (x64-windows) vcpkg port; the x64-uwp tree has no dxcapi.h.
if(DEFINED ENV{VCPKG_INSTALLATION_ROOT} AND NOT "$ENV{VCPKG_INSTALLATION_ROOT}" STREQUAL "")
  set(BOTW_DXC_DEFAULT "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows")
else()
  set(BOTW_DXC_DEFAULT "$ENV{HOME}/.local/share/nxbox/vcpkg-uwp/x64-windows")
endif()
set(BOTW_DXC_ROOT "${BOTW_DXC_DEFAULT}" CACHE PATH "Desktop DXC headers and runtime tree")
set(BOTW_DXC_HINT "${BOTW_DXC_ROOT}")
find_path(BOTW_DXC_INCLUDE_DIR dxcapi.h PATH_SUFFIXES dxc directx-dxc HINTS "${BOTW_DXC_HINT}/include" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_file(BOTW_DXCOMPILER_DLL dxcompiler.dll HINTS "${BOTW_DXC_HINT}/bin" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
find_file(BOTW_DXIL_DLL dxil.dll HINTS "${BOTW_DXC_HINT}/bin" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
install(FILES ${BOTW_DXCOMPILER_DLL} ${BOTW_DXIL_DLL} DESTINATION . COMPONENT Runtime)

target_include_directories(cemu_gpu BEFORE PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/compat
  ${BOTW_DXC_INCLUDE_DIR})
target_compile_definitions(cemu_gpu PRIVATE ENABLE_D3D12 NOMINMAX WIN32_LEAN_AND_MEAN)
if(BOTW_PLATFORM STREQUAL "uwp" OR CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  target_compile_definitions(cemu_gpu PRIVATE CEMU_UWP)
endif()
set(BOTW_D3D12_LIBRARIES
  d3d12 dxgi dxguid d3dcompiler
  glslang::glslang glslang::SPIRV
  spirv-cross-core spirv-cross-glsl spirv-cross-hlsl)
target_link_libraries(cemu_gpu PRIVATE ${BOTW_D3D12_LIBRARIES})
# dxcompiler.dll + dxil.dll (x64) are staged next to BotWRecomp.exe above for the default DXC path.

# Offline, pinned AMD FSR1 headers; embed shader text so deployed apps need no source files.
set(BOTW_FSR1_DIR "${CMAKE_SOURCE_DIR}/runtime/third_party/fidelityfx/fsr1")
file(SHA256 "${BOTW_FSR1_DIR}/ffx_a.h" BOTW_FSR1_MATH_HASH)
file(SHA256 "${BOTW_FSR1_DIR}/ffx_fsr1.h" BOTW_FSR1_ALGORITHM_HASH)
if(NOT BOTW_FSR1_MATH_HASH STREQUAL "f60e2722fcd13989523b9164d776ab382b3692791767f3bf8bb19967f763f3fb"
   OR NOT BOTW_FSR1_ALGORITHM_HASH STREQUAL "93c3922362ea7fc99cbcc698ca30c98de4f8c246d1fbb0b09e015ddef38ce3a5")
  message(FATAL_ERROR "Vendored AMD FSR1 shader content does not match its pin")
endif()
include("${CMAKE_SOURCE_DIR}/cmake/fsr1_source.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fsr")
set(BOTW_FSR1_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/fsr/fsr1_source.h")
file(WRITE "${BOTW_FSR1_SOURCE}" "#pragma once\n")
botw_embed_fsr1_chunks("${BOTW_FSR1_DIR}/ffx_a.h" kFsrMath "${BOTW_FSR1_SOURCE}")
botw_embed_fsr1_chunks("${BOTW_FSR1_DIR}/ffx_fsr1.h" kFsrAlgorithm "${BOTW_FSR1_SOURCE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${BOTW_FSR1_DIR}/ffx_a.h" "${BOTW_FSR1_DIR}/ffx_fsr1.h")
target_include_directories(cemu_gpu PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/fsr")

# Optional SDK remains outside the source tree. Never link its import libraries.
set(BOTW_FFX_SDK "" CACHE PATH "External FidelityFX 2.3.0 Kits/FidelityFX directory")
if(BOTW_FFX_SDK)
  foreach(_file api/include/ffx_api.h upscalers/include/ffx_upscale.h
      framegeneration/include/ffx_framegeneration.h
      signedbin/amd_fidelityfx_loader_dx12.dll
      signedbin/amd_fidelityfx_upscaler_dx12.dll
      signedbin/amd_fidelityfx_framegeneration_dx12.dll docs/license.md)
    if(NOT EXISTS "${BOTW_FFX_SDK}/${_file}")
      message(FATAL_ERROR "Incomplete FidelityFX SDK: ${BOTW_FFX_SDK}/${_file}")
    endif()
  endforeach()
  target_include_directories(cemu_gpu PRIVATE "${BOTW_FFX_SDK}/api/include"
    "${BOTW_FFX_SDK}/upscalers/include" "${BOTW_FFX_SDK}/framegeneration/include")
  target_compile_definitions(cemu_gpu PRIVATE BOTW_HAS_FFX=1)
  # Also expose availability to the host's automatic resolution policy.
  add_compile_definitions(BOTW_HAS_FFX=1)
  install(FILES "${BOTW_FFX_SDK}/signedbin/amd_fidelityfx_loader_dx12.dll"
    "${BOTW_FFX_SDK}/signedbin/amd_fidelityfx_upscaler_dx12.dll"
    "${BOTW_FFX_SDK}/signedbin/amd_fidelityfx_framegeneration_dx12.dll"
    DESTINATION . COMPONENT Runtime)
  install(FILES "${BOTW_FFX_SDK}/docs/license.md" DESTINATION Notices
    RENAME FidelityFX-LICENSE.md COMPONENT Runtime)
endif()
