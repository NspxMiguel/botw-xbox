#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"
mkdir -p build/tests
cxx="${CXX:-clang++}"
common=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread)
device=(-Itools/tests/d3d12_stubs -Iruntime/cemu)
"$cxx" "${common[@]}" -fsanitize=address,undefined tools/tests/latte_pipeline_config_test.cpp -o build/tests/latte_pipeline_config_test
UBSAN_OPTIONS=halt_on_error=1 build/tests/latte_pipeline_config_test
"$cxx" "${common[@]}" "${device[@]}" -fsanitize=address,undefined tools/tests/latte_chunks_test.cpp -o build/tests/latte_chunks_test
UBSAN_OPTIONS=halt_on_error=1 build/tests/latte_chunks_test
native=(-Wno-unused-private-field -DENABLE_D3D12 -Itools/tests/d3d12_parallel_stubs)
index=(-Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Itools/tests/latte_stubs)
native_sources=(tools/tests/latte_parallel_native_test.cpp runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12ParallelRecorder.cpp runtime/cemu/Cafe/HW/Latte/Renderer/D3D12/D3D12DescriptorHeaps.cpp)
index_sources=(tools/tests/latte_deferred_index_test.cpp runtime/cemu/Cafe/HW/Latte/Core/LatteIndices.cpp)
"$cxx" "${common[@]}" "${native[@]}" "${device[@]}" -fsanitize=address,undefined "${native_sources[@]}" -o build/tests/latte_parallel_native_test
UBSAN_OPTIONS=halt_on_error=1 build/tests/latte_parallel_native_test
"$cxx" "${common[@]}" "${index[@]}" "${device[@]}" -fsanitize=address,undefined "${index_sources[@]}" -o build/tests/latte_deferred_index_test
UBSAN_OPTIONS=halt_on_error=1 build/tests/latte_deferred_index_test
if [[ "${LATTE_PIPELINE_TSAN:-0}" == 1 ]]; then
  "$cxx" "${common[@]}" "${device[@]}" -fsanitize=thread tools/tests/latte_chunks_test.cpp -o build/tests/latte_chunks_tsan_test
  TSAN_OPTIONS=halt_on_error=1 build/tests/latte_chunks_tsan_test
  "$cxx" "${common[@]}" "${native[@]}" "${device[@]}" -fsanitize=thread "${native_sources[@]}" -o build/tests/latte_parallel_native_tsan_test
  TSAN_OPTIONS=halt_on_error=1 build/tests/latte_parallel_native_tsan_test
  "$cxx" "${common[@]}" "${index[@]}" "${device[@]}" -fsanitize=thread "${index_sources[@]}" -o build/tests/latte_deferred_index_tsan_test
  TSAN_OPTIONS=halt_on_error=1 build/tests/latte_deferred_index_tsan_test
fi
if [[ "${LATTE_PIPELINE_BENCHMARK:-1}" == 1 ]]; then
  "$cxx" -std=c++20 -O3 -Wall -Wextra -Werror -pthread -Itools/tests/d3d12_stubs -Iruntime/cemu tools/tests/latte_chunks_bench.cpp -o build/tests/latte_chunks_bench
  build/tests/latte_chunks_bench
  "$cxx" -std=c++20 -O3 -Wall -Wextra -Werror -Wno-unused-private-field -pthread "${native[@]}" "${device[@]}" tools/tests/latte_segments_bench.cpp -o build/tests/latte_segments_bench
  build/tests/latte_segments_bench
fi
