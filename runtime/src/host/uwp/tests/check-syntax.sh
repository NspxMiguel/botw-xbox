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
    "$host_native/tests/tests.cc" "$host_native/tests/drive_sound_test.cc"
  echo "PASS: all host sources and tests, including app.cpp/mod_network.h, with Microsoft SDK/C++/WinRT"
  exit 0
fi
clang++ -std=c++20 -DBOTW_PLATFORM_UWP=1 -I"$repo_dir/runtime/include" \
  -I"$host_dir/compat" -Wall -Wextra -Werror -fsyntax-only \
  "$host_dir/runtime.cpp" "$host_dir/settings.cpp"
# Installed Homebrew cross toolchain; no Windows execution and no generated game code.
mingw_root=/opt/homebrew/opt/mingw-w64/toolchain-x86_64
mingw_headers="$mingw_root/x86_64-w64-mingw32/include"
mingw_version=$(basename "$mingw_headers"/c++/*)
/opt/homebrew/opt/llvm/bin/clang++ --target=x86_64-w64-windows-gnu \
  --sysroot="$mingw_root" -isystem "$mingw_headers/c++/$mingw_version" \
  -isystem "$mingw_headers/c++/$mingw_version/x86_64-w64-mingw32" \
  -isystem "$mingw_headers" -std=c++20 -DBOTW_PLATFORM_UWP=1 \
  -Wall -Wextra -Werror -fsyntax-only "$host_dir/audio.cpp" \
  "$host_dir/controller.cpp" "$host_dir/launcher.cpp" \
  "$host_dir/tests/art_tests.cc" "$host_dir/tests/art_view_syntax.cc" \
  "$host_dir/tests/tests.cc" "$host_dir/tests/drive_sound_test.cc"
"$mingw_root/bin/x86_64-w64-mingw32-g++" -std=c++20 -DBOTW_PLATFORM_UWP=1 \
  -Wall -Wextra -Werror -fsyntax-only "$host_dir/audio.cpp"

echo "SKIP: app.cpp and mod_network.h require Microsoft SDK/C++/WinRT (not installed on this Mac)"
if [ "${UWP_REQUIRE_SDK:-0}" = 1 ]; then exit 1; fi
echo "PASS: portable host, art, audio, strings, drive synthesis and Direct2D/WIC syntax"
