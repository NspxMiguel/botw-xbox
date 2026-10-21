#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
host_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_dir=$(CDPATH= cd -- "$host_dir/../../../.." && pwd)
# Genuine SDK gate for a Visual Studio developer shell. /Zs emits no objects.
if command -v cl.exe >/dev/null 2>&1; then
  host_native=$host_dir
  repo_native=$repo_dir
  if command -v cygpath >/dev/null 2>&1; then
    host_native=$(cygpath -m "$host_dir")
    repo_native=$(cygpath -m "$repo_dir")
  fi
  MSYS_NO_PATHCONV=1 cl.exe /nologo /TP /Zs /std:c++20 /EHsc /await:strict /permissive- /utf-8 \
    /DBOTW_PLATFORM_UWP=1 /DNOMINMAX /DWIN32_LEAN_AND_MEAN \
    /I"$repo_native/runtime/include" /I"$host_native/compat" \
    "$host_native/app.cpp" "$host_native/audio.cpp" "$host_native/controller.cpp" \
    "$host_native/launcher.cpp" "$host_native/runtime.cpp" "$host_native/settings.cpp" \
    "$host_native/tests/art_view_syntax.cc" "$host_native/tests/art_tests.cc" \
    "$host_native/tests/tests.cc" "$host_native/tests/drive_sound_test.cc" \
    "$host_native/tests/setup_tests.cc" "$host_native/tests/polish_tests.cc"
  echo "PASS: all host sources and tests, including app.cpp/mod_network.h, with Microsoft SDK/C++/WinRT"
  exit 0
fi
clang++ -std=c++20 -I/opt/homebrew/include -DBOTW_PLATFORM_UWP=1 -I"$repo_dir/runtime/include" \
  -I"$host_dir/compat" -Wall -Wextra -Werror -fsyntax-only \
  "$host_dir/runtime.cpp" "$host_dir/settings.cpp" "$host_dir/tests/setup_tests.cc"
# Installed Homebrew cross toolchain; no Windows execution and no generated game code.
mingw_root=/opt/homebrew/opt/mingw-w64/toolchain-x86_64
mingw_headers="$mingw_root/x86_64-w64-mingw32/include"
mingw_version=$(basename "$mingw_headers"/c++/*)
# Use the real SDK's architecture-independent zlib declarations with the cross compiler.
syntax_headers=$(mktemp -d /tmp/botw-w10-syntax.XXXXXX)
trap 'rm -rf "$syntax_headers"' EXIT HUP INT TERM
mac_sdk=$(xcrun --show-sdk-path)
cp "$mac_sdk/usr/include/zlib.h" "$mac_sdk/usr/include/zconf.h" "$syntax_headers/"

/opt/homebrew/opt/llvm/bin/clang++ --target=x86_64-w64-windows-gnu \
  --sysroot="$mingw_root" -isystem "$mingw_headers/c++/$mingw_version" \
  -isystem "$mingw_headers/c++/$mingw_version/x86_64-w64-mingw32" \
  -isystem "$mingw_headers" -std=c++20 -DBOTW_PLATFORM_UWP=1 \
  -I/opt/homebrew/include -I"$syntax_headers" -Wall -Wextra -Werror -fsyntax-only "$host_dir/audio.cpp" \
  "$host_dir/controller.cpp" "$host_dir/launcher.cpp" \
  "$host_dir/tests/art_tests.cc" "$host_dir/tests/art_view_syntax.cc" \
  "$host_dir/tests/tests.cc" "$host_dir/tests/drive_sound_test.cc" \
  "$host_dir/tests/setup_tests.cc" "$host_dir/tests/polish_tests.cc"
"$mingw_root/bin/x86_64-w64-mingw32-g++" -std=c++20 -DBOTW_PLATFORM_UWP=1 \
  -Wall -Wextra -Werror -fsyntax-only "$host_dir/audio.cpp"

# Microsoft SDK/C++/WinRT syntax gate on macOS when xwin is installed.
xwin_sdk="$HOME/.local/share/nxbox/xwin/sdk"
clang_cl=/opt/homebrew/opt/llvm/bin/clang-cl
if [ -x "$clang_cl" ] && [ -d "$xwin_sdk/crt/include" ]; then
  # SDK 26100 exposes shell RPC declarations to APP but hides their parameter types.
  # These forwards repair that SDK-only defect; no WinRT or host API is stubbed.
  cat >"$syntax_headers/sdk-forward.h" <<'SDK'
#include <windows.h>
typedef struct _CMINVOKECOMMANDINFO CMINVOKECOMMANDINFO;
typedef struct _CMINVOKECOMMANDINFOEX_REMOTE CMINVOKECOMMANDINFOEX_REMOTE;
typedef DWORD SHGDNF;
SDK
  "$clang_cl" /nologo /Zs /std:c++20 /EHsc /permissive- /utf-8 \
    /DBOTW_PLATFORM_UWP=1 /DNOMINMAX /DWIN32_LEAN_AND_MEAN \
    /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D_CRT_USE_WINAPI_FAMILY_DESKTOP_APP \
    /DUNICODE /D_UNICODE /FI"$syntax_headers/sdk-forward.h" \
    /imsvc"$xwin_sdk/crt/include" \
    /imsvc"$xwin_sdk/sdk/include/10.0.26100/ucrt" \
    /imsvc"$xwin_sdk/sdk/include/10.0.26100/um" \
    /imsvc"$xwin_sdk/sdk/include/10.0.26100/shared" \
    /imsvc"$xwin_sdk/sdk/include/10.0.26100/winrt" \
    /imsvc"$xwin_sdk/sdk/include/10.0.26100/cppwinrt" \
    /I"$repo_dir/runtime/include" /I"$host_dir/compat" \
    /I"$syntax_headers" /I/opt/homebrew/include \
    -- "$host_dir/app.cpp" "$host_dir/launcher.cpp"
  echo "PASS: clang-cl /Zs app.cpp and launcher.cpp with Microsoft SDK/C++/WinRT (APP partition)"
else
  echo "SKIP: app.cpp requires Microsoft SDK/C++/WinRT"
  if [ "${UWP_REQUIRE_SDK:-0}" = 1 ]; then exit 1; fi
fi
echo "PASS: portable host, art, audio, strings, setup/formats/crypto/QR, polish and Direct2D/WIC syntax"
