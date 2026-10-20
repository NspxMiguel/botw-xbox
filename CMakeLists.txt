cmake_minimum_required(VERSION 3.24)
project(BotWRecomp LANGUAGES C CXX)

option(BOTW_BUILD_GAME "Link player-generated guest code into the runtime" ON)
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  set(BOTW_DEFAULT_PLATFORM uwp)
elseif(WIN32)
  set(BOTW_DEFAULT_PLATFORM win32)
elseif(APPLE)
  set(BOTW_DEFAULT_PLATFORM apple)
else()
  message(FATAL_ERROR "Supported hosts are Apple and Windows")
endif()
set(BOTW_PLATFORM
    "${BOTW_DEFAULT_PLATFORM}"
    CACHE STRING "Host platform: apple, win32, uwp")
set_property(CACHE BOTW_PLATFORM PROPERTY STRINGS apple win32 uwp)
if(NOT BOTW_PLATFORM MATCHES "^(apple|win32|uwp)$")
  message(FATAL_ERROR "Invalid BOTW_PLATFORM: ${BOTW_PLATFORM}")
endif()
if(BOTW_PLATFORM STREQUAL "apple")
  set(BOTW_DEFAULT_RENDERER metal)
  enable_language(OBJC OBJCXX)
  set(CMAKE_OSX_ARCHITECTURES arm64)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 13.3)
else()
  set(BOTW_DEFAULT_RENDERER d3d12)
endif()
set(BOTW_RENDERER
    "${BOTW_DEFAULT_RENDERER}"
    CACHE STRING "GPU backend: metal, d3d12")
set_property(CACHE BOTW_RENDERER PROPERTY STRINGS metal d3d12)
if(NOT BOTW_RENDERER MATCHES "^(metal|d3d12)$")
  message(FATAL_ERROR "Invalid BOTW_RENDERER: ${BOTW_RENDERER}")
endif()
if((BOTW_PLATFORM STREQUAL "apple" AND NOT BOTW_RENDERER STREQUAL "metal")
   OR (NOT BOTW_PLATFORM STREQUAL "apple" AND NOT BOTW_RENDERER STREQUAL "d3d12"
      ))
  message(
    FATAL_ERROR
      "${BOTW_PLATFORM} does not support the ${BOTW_RENDERER} renderer")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(BOTW_BUILD_GAME)
  # Recompiled guest code (tools/recompile.py writes generated/).
  file(
    GLOB
    GUEST_SOURCES
    CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/generated/fn_*.cpp
    ${CMAKE_SOURCE_DIR}/generated/big_*.cpp
    ${CMAKE_SOURCE_DIR}/generated/huge_*.cpp
    ${CMAKE_SOURCE_DIR}/generated/table.cpp
    ${CMAKE_SOURCE_DIR}/generated/hle_stubs.cpp)
  if(NOT GUEST_SOURCES)
    message(
      FATAL_ERROR
        "BOTW_BUILD_GAME=ON requires player-generated sources in generated/. Use OFF for CI."
    )
  endif()
  add_library(guest OBJECT ${GUEST_SOURCES})
  target_include_directories(guest PRIVATE runtime/include generated)
  if(MSVC)
    target_compile_options(guest PRIVATE /O2 /w /fp:strict)
  else()
    target_compile_options(guest PRIVATE -O2 -w -fwrapv -fno-strict-aliasing
                                         -ffp-contract=off)
  endif()
  if(BOTW_PLATFORM STREQUAL "apple")
    target_compile_options(guest PRIVATE -gline-tables-only -mcpu=apple-m1)
  endif()
  file(GLOB BIG_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/big_*.cpp)
  file(GLOB HUGE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/huge_*.cpp)
  if(NOT MSVC)
    set_source_files_properties(${BIG_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1")
    set_source_files_properties(${HUGE_SOURCES} PROPERTIES COMPILE_OPTIONS "-O0")
  endif()
endif()

# Vendored Cemu Latte GPU layer (runtime/cemu, MPL-2.0) + shims, built in Cemu's
# own environment.
file(
  GLOB_RECURSE
  CEMU_SOURCES
  CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/*.mm
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Common/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/util/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cemu/FileCache/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/imgui/*.mm
  ${CMAKE_SOURCE_DIR}/runtime/cemu_shims/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu_shims/*.mm)
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
list(
  APPEND
  CEMU_SOURCES
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_draw.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_tables.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_widgets.cpp)
add_library(cemu_gpu OBJECT ${CEMU_SOURCES})
target_include_directories(
  cemu_gpu PRIVATE runtime/cemu_shims runtime/cemu runtime/cemu/Cafe
                   runtime/cemu/dependencies/imgui)
target_compile_definitions(
  cemu_gpu
  PRIVATE BOTW_BUILD_GAME=$<BOOL:${BOTW_BUILD_GAME}>
          BOOST_NOWIDE_NO_LIB
          BOTWRECOMP
          EMULATOR_VERSION_MAJOR=2
          EMULATOR_VERSION_MINOR=6
          EMULATOR_VERSION_PATCH=0
          EMULATOR_HASH=a53cf0e)
target_precompile_headers(cemu_gpu PRIVATE runtime/cemu/Common/precompiled.h)
if(MSVC)
  target_compile_options(cemu_gpu PRIVATE /w /O2)
else()
  target_compile_options(cemu_gpu PRIVATE -w -O2 -g1)
endif()
if(BOTW_RENDERER STREQUAL "metal")
  target_include_directories(
    cemu_gpu PRIVATE runtime/cemu/dependencies/metal-cpp /opt/homebrew/include)
  target_compile_definitions(cemu_gpu PRIVATE ENABLE_METAL IMGUI_IMPL_METAL_CPP)
endif()
# BEGIN W2 renderer dependencies.
if(BOTW_RENDERER STREQUAL "d3d12")
  include(${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/renderer.cmake)
endif()
# END W2 renderer dependencies.

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
if(NOT BOTW_PLATFORM STREQUAL "apple")
  list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "\\.mm$")
endif()
add_executable(BotWRecomp ${RUNTIME_SOURCES} $<TARGET_OBJECTS:cemu_gpu>)
target_include_directories(BotWRecomp PRIVATE runtime/include runtime/src)
target_compile_definitions(BotWRecomp
                           PRIVATE BOTW_BUILD_GAME=$<BOOL:${BOTW_BUILD_GAME}>)
if(BOTW_BUILD_GAME)
  target_sources(BotWRecomp PRIVATE $<TARGET_OBJECTS:guest>)
  target_include_directories(BotWRecomp PRIVATE generated)
endif()
# BEGIN W2 renderer link dependencies (cemu_gpu is consumed as object files).
if(BOTW_RENDERER STREQUAL "d3d12")
  target_link_libraries(BotWRecomp PRIVATE ${BOTW_D3D12_LIBRARIES})
endif()
# END W2 renderer link dependencies.
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
if(MSVC)
  target_compile_options(BotWRecomp PRIVATE /W3)
else()
  target_compile_options(BotWRecomp PRIVATE -Wall -Wno-unused-function
                                            -Wno-unused-variable -fwrapv)
endif()
find_package(fmt REQUIRED)
find_package(ZLIB REQUIRED)
if(BOTW_PLATFORM STREQUAL "apple" AND NOT DEFINED OPENSSL_ROOT_DIR)
  set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3) # the shipped Mac build links OpenSSL 3
endif()
find_package(OpenSSL REQUIRED)
find_package(zstd REQUIRED CONFIG)
if(TARGET zstd::libzstd_shared)
  set(BOTW_ZSTD_TARGET zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
  set(BOTW_ZSTD_TARGET zstd::libzstd_static)
else()
  set(BOTW_ZSTD_TARGET zstd::libzstd)
endif()
target_link_libraries(cemu_gpu PRIVATE fmt::fmt OpenSSL::Crypto ${BOTW_ZSTD_TARGET})
target_link_libraries(BotWRecomp PRIVATE ZLIB::ZLIB fmt::fmt OpenSSL::Crypto ${BOTW_ZSTD_TARGET})
if(BOTW_PLATFORM STREQUAL "apple")
  target_compile_options(
    BotWRecomp PRIVATE -mcpu=apple-m1 $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  find_package(SDL3 REQUIRED CONFIG)
  target_link_libraries(
    BotWRecomp
    PRIVATE SDL3::SDL3
            "-framework Metal"
            "-framework QuartzCore"
            "-framework Foundation"
            "-framework AppKit"
            "-framework MetalFX"
            "-framework CoreGraphics"
            "-framework ImageIO"
            "-framework UniformTypeIdentifiers"
            "-framework VideoToolbox"
            "-framework CoreMedia"
            "-framework CoreVideo")
endif()

install(TARGETS BotWRecomp RUNTIME DESTINATION . COMPONENT Runtime)
if(BOTW_PLATFORM STREQUAL "uwp")
  install(
    FILES ${W3_UWP_PACKAGE}/Package.appxmanifest
    DESTINATION .
    RENAME AppxManifest.xml
    COMPONENT Runtime)
  install(
    FILES ${W3_UWP_ASSETS}
    DESTINATION Assets
    COMPONENT Runtime)
  install(
    FILES $<TARGET_RUNTIME_DLLS:BotWRecomp>
    DESTINATION .
    COMPONENT Runtime)
endif()

# Native Metal GPU path (docs/native-renderer.md): option BOTW_NATIVE_GPU,
# default OFF (gpu_native.cmake).
include(${CMAKE_SOURCE_DIR}/runtime/src/gpu_native/gpu_native.cmake OPTIONAL)
