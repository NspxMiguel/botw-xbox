#!/bin/sh
# SPDX-License-Identifier: MIT
# Real Microsoft SDK/C++/WinRT syntax check on macOS; no objects or game files.
set -eu
host_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo_dir=$(CDPATH= cd -- "$host_dir/../../../.." && pwd)
sdk_root="$HOME/.local/share/nxbox/xwin/sdk"
clang_cl=/opt/homebrew/opt/llvm/bin/clang-cl
headers=$(mktemp -d /tmp/botw-sdk-syntax.XXXXXX)
trap 'rm -rf "$headers"' EXIT HUP INT TERM
mac_sdk=$(xcrun --show-sdk-path)
cp "$mac_sdk/usr/include/zlib.h" "$mac_sdk/usr/include/zconf.h" "$headers/"
# SDK 26100 shell RPC forwards omitted by the APP partition.
cat >"$headers/sdk-forward.h" <<'SDK'
#include <windows.h>
typedef struct _CMINVOKECOMMANDINFO CMINVOKECOMMANDINFO;
typedef struct _CMINVOKECOMMANDINFOEX_REMOTE CMINVOKECOMMANDINFOEX_REMOTE;
typedef DWORD SHGDNF;
SDK
"$clang_cl" --target=x86_64-pc-windows-msvc /nologo /Zs /std:c++20 /EHsc /permissive- /utf-8 \
  /DBOTW_PLATFORM_UWP=1 /DNOMINMAX /DWIN32_LEAN_AND_MEAN \
  /DWINAPI_FAMILY=WINAPI_FAMILY_APP /D_CRT_USE_WINAPI_FAMILY_DESKTOP_APP \
  /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /FI"$headers/sdk-forward.h" \
  /imsvc"$sdk_root/crt/include" \
  /imsvc"$sdk_root/sdk/include/10.0.26100/ucrt" \
  /imsvc"$sdk_root/sdk/include/10.0.26100/um" \
  /imsvc"$sdk_root/sdk/include/10.0.26100/shared" \
  /imsvc"$sdk_root/sdk/include/10.0.26100/winrt" \
  /imsvc"$sdk_root/sdk/include/10.0.26100/cppwinrt" \
  /I"$repo_dir/runtime/include" /I"$host_dir/compat" \
  /I"$headers" /I/opt/homebrew/include \
  -- "$host_dir/app.cpp" "$host_dir/launcher.cpp" "$host_dir/runtime.cpp"
echo "PASS: clang-cl /Zs app.cpp launcher.cpp runtime.cpp (Microsoft SDK 26100, APP partition)"
