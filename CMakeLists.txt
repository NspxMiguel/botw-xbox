cmake_minimum_required(VERSION 3.24)
project(BotWRecomp LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.3)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Recompiled guest code (tools/recompile.py writes generated/).
file(GLOB GUEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/generated/fn_*.cpp
  ${CMAKE_SOURCE_DIR}/generated/big_*.cpp
  ${CMAKE_SOURCE_DIR}/generated/huge_*.cpp
  ${CMAKE_SOURCE_DIR}/generated/table.cpp
  ${CMAKE_SOURCE_DIR}/generated/hle_stubs.cpp)
add_library(guest OBJECT ${GUEST_SOURCES})
target_include_directories(guest PRIVATE runtime/include generated)
target_compile_options(guest PRIVATE -O2 -gline-tables-only -w -fwrapv -fno-strict-aliasing -mcpu=apple-m1
  -ffp-contract=off)
file(GLOB BIG_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/big_*.cpp)
file(GLOB HUGE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/huge_*.cpp)
set_source_files_properties(${BIG_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1")
set_source_files_properties(${HUGE_SOURCES} PROPERTIES COMPILE_OPTIONS "-O0")

# Vendored Cemu Latte GPU layer (runtime/cemu, MPL-2.0) + shims, built in Cemu's own environment.
file(GLOB_RECURSE CEMU_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/*.mm
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Common/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu/util/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cemu/FileCache/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/imgui/*.mm
  ${CMAKE_SOURCE_DIR}/runtime/cemu_shims/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu_shims/*.mm)
list(FILTER CEMU_SOURCES EXCLUDE REGEX "LatteOverlay\\.cpp$|/dependencies/|SystemInfo(Win|Linux|BSD)\\.cpp$")
# BEGIN W2 renderer source selection. Platform/toolchain setup is owned by W4.
if(NOT DEFINED BOTW_RENDERER)
  set(BOTW_RENDERER metal CACHE STRING "GPU backend: metal or d3d12")
endif()
set_property(CACHE BOTW_RENDERER PROPERTY STRINGS metal d3d12)
if(BOTW_RENDERER STREQUAL "metal")
  list(FILTER CEMU_SOURCES EXCLUDE REGEX "/Renderer/D3D12/")
elseif(BOTW_RENDERER STREQUAL "d3d12")
  if(NOT WIN32)
    message(FATAL_ERROR "D3D12 requires a Windows/UWP toolchain; use the backend syntax-check script on macOS")
  endif()
  list(FILTER CEMU_SOURCES EXCLUDE REGEX "/Renderer/Metal/|/Renderer/MetalView\\.mm$|/imgui/.*\\.mm$|/Common/unix/|SystemInfoMac\\.cpp$")
else()
  message(FATAL_ERROR "Unknown BOTW_RENDERER: ${BOTW_RENDERER}")
endif()
# END W2 renderer source selection.
list(APPEND CEMU_SOURCES
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_draw.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_tables.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_widgets.cpp)
add_library(cemu_gpu OBJECT ${CEMU_SOURCES})
target_include_directories(cemu_gpu PRIVATE runtime/cemu_shims runtime/cemu runtime/cemu/Cafe
  runtime/cemu/dependencies/metal-cpp runtime/cemu/dependencies/imgui /opt/homebrew/include)
target_compile_definitions(cemu_gpu PRIVATE BOOST_NOWIDE_NO_LIB BOTWRECOMP
  EMULATOR_VERSION_MAJOR=2 EMULATOR_VERSION_MINOR=6 EMULATOR_VERSION_PATCH=0 EMULATOR_HASH=a53cf0e)
# BEGIN W2 renderer dependencies.
if(BOTW_RENDERER STREQUAL "metal")
  target_compile_definitions(cemu_gpu PRIVATE ENABLE_METAL IMGUI_IMPL_METAL_CPP)
elseif(BOTW_RENDERER STREQUAL "d3d12")
  include(${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/renderer.cmake)
endif()
# END W2 renderer dependencies.
target_compile_options(cemu_gpu PRIVATE -w -O2 -g1 "SHELL:-include Common/precompiled.h")

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/runtime/src/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/src/*.mm)
# ---- W1 platform sources (owned by W1) -------------------------------------------------------
# Keep test mains and the other platform's implementation out of the application.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/runtime/src/platform/")
list(APPEND RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/src/platform/clock.cpp)
if(WIN32 OR BOTW_PLATFORM STREQUAL "win32" OR BOTW_PLATFORM STREQUAL "uwp")
  list(APPEND RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/src/platform/win32/platform.cpp)
else()
  list(APPEND RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/src/platform/apple/platform.cpp)
endif()
# ---- end W1 platform sources ---------------------------------------------------------------
# BEGIN W3 UWP host sources (W3-owned block)
if(BOTW_PLATFORM STREQUAL "uwp")
  list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/runtime/src/(main|settings)\\.cpp$|/audio/output\\.cpp$|/host/(window\\.mm|menu\\.cpp|touch\\.cpp)$")
  list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/host/uwp/tests/")
else()
  # Do not let the recursive source glob pull Windows code or the test main into Apple builds.
  list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/host/uwp/")
endif()
# END W3 UWP host sources
add_executable(BotWRecomp ${RUNTIME_SOURCES} $<TARGET_OBJECTS:guest> $<TARGET_OBJECTS:cemu_gpu>)
# BEGIN W2 renderer link dependencies (cemu_gpu is consumed as object files).
if(BOTW_RENDERER STREQUAL "d3d12")
  target_link_libraries(BotWRecomp PRIVATE ${BOTW_D3D12_LIBRARIES})
endif()
# END W2 renderer link dependencies.
target_include_directories(BotWRecomp PRIVATE runtime/include runtime/src generated)
# BEGIN W3 UWP host target configuration (W3-owned block)
if(BOTW_PLATFORM STREQUAL "uwp")
  target_compile_definitions(BotWRecomp PRIVATE BOTW_PLATFORM_UWP=1 NOMINMAX WIN32_LEAN_AND_MEAN)
  target_include_directories(BotWRecomp PRIVATE runtime/src/host/uwp/compat)
  target_link_libraries(BotWRecomp PRIVATE windowsapp xaudio2)
  set(W3_UWP_PACKAGE ${CMAKE_SOURCE_DIR}/runtime/src/host/uwp)
  set(W3_UWP_ASSETS
    ${W3_UWP_PACKAGE}/Assets/StoreLogo.png
    ${W3_UWP_PACKAGE}/Assets/Square44x44Logo.png
    ${W3_UWP_PACKAGE}/Assets/Square150x150Logo.png
    ${W3_UWP_PACKAGE}/Assets/SplashScreen.png)
  target_sources(BotWRecomp PRIVATE ${W3_UWP_PACKAGE}/Package.appxmanifest ${W3_UWP_ASSETS})
  set_source_files_properties(${W3_UWP_ASSETS} PROPERTIES
    VS_DEPLOYMENT_CONTENT TRUE VS_DEPLOYMENT_LOCATION "Assets")
  if(MSVC)
    target_compile_options(BotWRecomp PRIVATE /await:strict /permissive- /utf-8)
    target_link_options(BotWRecomp PRIVATE /SUBSYSTEM:WINDOWS /APPCONTAINER)
  endif()
endif()
# END W3 UWP host target configuration
target_compile_options(BotWRecomp PRIVATE -Wall -Wno-unused-function -Wno-unused-variable -fwrapv -mcpu=apple-m1
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
find_package(SDL3 REQUIRED CONFIG)
find_package(fmt REQUIRED)
target_link_libraries(BotWRecomp PRIVATE z SDL3::SDL3 fmt::fmt
  /opt/homebrew/opt/openssl@3/lib/libcrypto.dylib /opt/homebrew/opt/zstd/lib/libzstd.dylib
  "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit" "-framework MetalFX"
  "-framework CoreGraphics" "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")

# Native Metal GPU path (docs/native-renderer.md): option BOTW_NATIVE_GPU, default OFF (gpu_native.cmake).
include(${CMAKE_SOURCE_DIR}/runtime/src/gpu_native/gpu_native.cmake OPTIONAL)
