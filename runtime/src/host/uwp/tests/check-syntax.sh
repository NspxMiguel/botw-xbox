#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
host_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_dir=$(CDPATH= cd -- "$host_dir/../../../.." && pwd)
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
  "$host_dir/controller.cpp" "$host_dir/launcher.cpp"
"$mingw_root/bin/x86_64-w64-mingw32-g++" -std=c++20 -DBOTW_PLATFORM_UWP=1 \
  -Wall -Wextra -Werror -fsyntax-only "$host_dir/audio.cpp"
