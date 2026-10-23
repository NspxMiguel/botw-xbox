#!/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
work_dir=$(mktemp -d /tmp/botw-pack-hooks.XXXXXX)
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM
clang++ -I"$repo_dir/runtime/include" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$repo_dir/runtime/src/packs/packs.cpp" \
  "$repo_dir/runtime/src/packs/loaded_rpx.cpp" \
  "$repo_dir/runtime/src/latte/pack_hooks_metal.cpp" \
  "$repo_dir/runtime/src/packs/tests/hooks_test.cc" -o "$work_dir/hooks_test"
UBSAN_OPTIONS=halt_on_error=1 "$work_dir/hooks_test" "$work_dir"
for variant in TEST_CAVES TEST_NO_CAVES; do
  clang++ -std=c++20 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
    -fsanitize=address,undefined -D"$variant" \
    -I"$repo_dir/runtime/include" -I"$repo_dir/runtime/src" \
    "$repo_dir/runtime/src/dispatch.cpp" \
    "$repo_dir/runtime/src/packs/tests/dispatch_test.cc" \
    -o "$work_dir/dispatch_test"
  UBSAN_OPTIONS=halt_on_error=1 "$work_dir/dispatch_test"
done
