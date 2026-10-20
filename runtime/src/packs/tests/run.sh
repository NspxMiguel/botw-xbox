#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
work_dir=$(mktemp -d /tmp/botw-pack-hooks.XXXXXX)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/runtime/src/packs/packs.cpp" \
  "$repo_dir/runtime/src/packs/loaded_rpx.cpp" \
  "$repo_dir/runtime/src/latte/pack_hooks_metal.cpp" \
  "$repo_dir/runtime/src/packs/tests/hooks_test.cc" -o "$work_dir/hooks_test"
UBSAN_OPTIONS=halt_on_error=1 "$work_dir/hooks_test" "$work_dir"
