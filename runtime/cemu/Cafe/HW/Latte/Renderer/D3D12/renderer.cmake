# BotWRecomp: D3D12 dependencies. Include after creating cemu_gpu.
# Use Windows/UWP builds of these libraries; never link host Homebrew libraries.
find_package(glslang CONFIG REQUIRED)
find_package(spirv_cross_core CONFIG REQUIRED)
find_package(spirv_cross_glsl CONFIG REQUIRED)
find_package(spirv_cross_hlsl CONFIG REQUIRED)
find_path(BOTW_DXC_INCLUDE_DIR dxcapi.h PATH_SUFFIXES dxc REQUIRED)

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
# The host must package dxcompiler.dll + dxil.dll (x64) for the default DXC path.
