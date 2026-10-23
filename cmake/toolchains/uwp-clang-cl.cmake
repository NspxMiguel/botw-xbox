# Cross-compile UWP with Microsoft's unpacked SDK/CRT and native LLVM tools.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Preserve the maintainer's existing layout; new users use the builder data dir.
set(_builder_data "$ENV{HOME}/.botw-builder")
if(DEFINED ENV{USERPROFILE} AND NOT DEFINED ENV{HOME})
  set(_builder_data "$ENV{USERPROFILE}/.botw-builder")
endif()
set(_xwin_default "${_builder_data}/xwin")
set(_vcpkg_default "${_builder_data}/vcpkg-uwp")
if(EXISTS "$ENV{HOME}/.local/share/nxbox/xwin/sdk")
  set(_xwin_default "$ENV{HOME}/.local/share/nxbox/xwin/sdk")
endif()
if(EXISTS "$ENV{HOME}/.local/share/nxbox/vcpkg-uwp")
  set(_vcpkg_default "$ENV{HOME}/.local/share/nxbox/vcpkg-uwp")
endif()
if(DEFINED ENV{XWIN_ROOT})
  set(_xwin_default "$ENV{XWIN_ROOT}")
endif()
if(DEFINED ENV{VCPKG_UWP_ROOT})
  set(_vcpkg_default "$ENV{VCPKG_UWP_ROOT}")
endif()
set(XWIN_ROOT
    "${_xwin_default}"
    CACHE PATH "Unpacked xwin SDK and MSVC CRT")
set(VCPKG_UWP_ROOT
    "${_vcpkg_default}"
    CACHE PATH "Unpacked vcpkg UWP and desktop trees")

set(_llvm_hints
    /opt/homebrew/opt/llvm/bin
    /opt/homebrew/opt/lld/bin
    /usr/local/opt/llvm/bin
    /usr/local/opt/lld/bin
    /usr/lib/llvm/bin
    "$ENV{ProgramFiles}/LLVM/bin"
    "$ENV{ProgramFiles\(x86\)}/LLVM/bin"
    "C:/Program Files/LLVM/bin")
file(
  GLOB _linux_llvm
  LIST_DIRECTORIES TRUE
  "/usr/lib/llvm-*/bin")
list(
  SORT _linux_llvm
  COMPARE NATURAL
  ORDER DESCENDING)
list(APPEND _llvm_hints ${_linux_llvm})
# find_program searches PATH before common install locations. Every tool is a
# cache FILEPATH, so -DCMAKE_C_COMPILER=..., etc. always wins.
find_program(
  CMAKE_C_COMPILER
  NAMES clang-cl
  PATHS ${_llvm_hints} REQUIRED)
find_program(
  CMAKE_CXX_COMPILER
  NAMES clang-cl
  PATHS ${_llvm_hints} REQUIRED)
find_program(
  CMAKE_LINKER
  NAMES lld-link
  PATHS ${_llvm_hints} REQUIRED)
find_program(
  CMAKE_AR
  NAMES llvm-lib
  PATHS ${_llvm_hints} REQUIRED)
find_program(
  CMAKE_RC_COMPILER
  NAMES llvm-rc
  PATHS ${_llvm_hints} REQUIRED)
find_program(
  CMAKE_MT
  NAMES llvm-mt
  PATHS ${_llvm_hints} REQUIRED)
set(CMAKE_C_COMPILER_TARGET
    x86_64-pc-windows-msvc
    CACHE STRING "C compiler target")
set(CMAKE_CXX_COMPILER_TARGET
    x86_64-pc-windows-msvc
    CACHE STRING "C++ compiler target")
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    XWIN_ROOT
    VCPKG_UWP_ROOT
    XWIN_SDK_INCLUDE_DIR
    CMAKE_C_COMPILER
    CMAKE_CXX_COMPILER
    CMAKE_LINKER
    CMAKE_AR
    CMAKE_RC_COMPILER
    CMAKE_MT)

# xwin normally flattens includes; the existing maintainer SDK is versioned.
set(_sdk_include "${XWIN_ROOT}/sdk/include")
if(EXISTS "${_sdk_include}/10.0.26100/um")
  set(_sdk_include "${_sdk_include}/10.0.26100")
endif()
set(XWIN_SDK_INCLUDE_DIR
    "${_sdk_include}"
    CACHE PATH "SDK include root containing ucrt, um, shared, winrt, cppwinrt")

set(_uwp_includes "/imsvc\"${XWIN_ROOT}/crt/include\"")
foreach(_part ucrt um shared winrt cppwinrt)
  string(APPEND _uwp_includes " /imsvc\"${XWIN_SDK_INCLUDE_DIR}/${_part}\"")
endforeach()
# The 14.44 STL imports the C environment functions unconditionally. Expose CRT
# declarations while preserving UWP Windows API partition guards. Xbox Series
# CPUs are Zen 2: AVX2 is always present (Cemu has AVX2 fast paths).
set(CMAKE_C_FLAGS_INIT
    "${_uwp_includes} /arch:AVX2 /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D_UNICODE /DUNICODE /D_CRT_USE_WINAPI_FAMILY_DESKTOP_APP"
)
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} /EHsc")
set(_uwp_libpaths "")
foreach(_dir crt/lib/x86_64/store crt/lib/x86_64 sdk/lib/um/x86_64
             sdk/lib/ucrt/x86_64)
  string(APPEND _uwp_libpaths " /LIBPATH:\"${XWIN_ROOT}/${_dir}\"")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_uwp_libpaths} /APPCONTAINER WindowsApp.lib")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "${_uwp_libpaths} /APPCONTAINER WindowsApp.lib")
# Desktop defaults include kernel32/user32; WindowsApp supplies the UWP API set.
set(CMAKE_C_STANDARD_LIBRARIES
    "WindowsApp.lib"
    CACHE STRING "UWP system libraries")
set(CMAKE_CXX_STANDARD_LIBRARIES
    "WindowsApp.lib"
    CACHE STRING "UWP system libraries")
set(BOTW_DXC_ROOT
    "${VCPKG_UWP_ROOT}/x64-windows"
    CACHE PATH "Desktop DXC headers and runtime tree")
list(PREPEND CMAKE_PREFIX_PATH "${VCPKG_UWP_ROOT}/x64-uwp")
set(CMAKE_FIND_ROOT_PATH "${VCPKG_UWP_ROOT}/x64-uwp" "${XWIN_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
