#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"
mkdir -p build/tests
cxx="${CXX:-clang++}"
common=(-std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined)
indices=(-Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Itools/tests/latte_stubs)
index_source=runtime/cemu/Cafe/HW/Latte/Core/LatteIndices.cpp
export UBSAN_OPTIONS=halt_on_error=1

for test in latte_profiler latte_draw_cache latte_index_conversion latte_index_expansion; do
  "$cxx" "${common[@]}" -Iruntime/cemu "tools/tests/${test}_test.cpp" -o "build/tests/${test}_test"
  "build/tests/${test}_test"
done
for test in latte_indices latte_index_cache_associativity; do
  "$cxx" "${common[@]}" "${indices[@]}" -Iruntime/cemu "tools/tests/${test}_test.cpp" "$index_source" -o "build/tests/${test}_test"
  "build/tests/${test}_test"
done
for test in latte_stage_table latte_root_signature; do
  "$cxx" "${common[@]}" -Itools/tests/d3d12_stubs -Iruntime/cemu "tools/tests/${test}_test.cpp" -o "build/tests/${test}_test"
  "build/tests/${test}_test"
done
"$cxx" "${common[@]}" -Wno-unused-private-field -DENABLE_D3D12 -Itools/tests/d3d12_stubs -Iruntime/cemu tools/tests/latte_descriptor_heap_test.cpp runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.cpp -o build/tests/latte_descriptor_heap_test
build/tests/latte_descriptor_heap_test

# On Apple Silicon, Rosetta executes the same SSSE3/AVX2 code as the UWP build.
# Elsewhere the native x86 compiler exercises it directly when available.
if [[ "$(uname -s)" == Darwin ]]; then
  x86=(-arch x86_64 -mavx2)
elif [[ "$(uname -m)" == x86_64 ]]; then
  x86=(-mavx2)
else
  exit 0
fi
"$cxx" "${common[@]}" "${x86[@]}" -Iruntime/cemu tools/tests/latte_index_conversion_test.cpp -o build/tests/latte_index_conversion_x86_test
build/tests/latte_index_conversion_x86_test
"$cxx" "${common[@]}" "${indices[@]}" "${x86[@]}" -DARCH_X86_64 -Iruntime/cemu tools/tests/latte_indices_test.cpp "$index_source" -o build/tests/latte_indices_x86_test
build/tests/latte_indices_x86_test
