#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"
bash tools/tests/run_latte_perf3_tests.sh

# Microbenchmarks deliberately omit sanitizers; never use host times as Xbox estimates.
cxx="${CXX:-clang++}"
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
  -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable \
  -Itools/tests/latte_stubs -Iruntime/cemu \
  tools/tests/latte_perf5_benchmark.cpp runtime/cemu/Cafe/HW/Latte/Core/LatteIndices.cpp \
  -o build/tests/latte_perf5_benchmark
build/tests/latte_perf5_benchmark

"$cxx" -std=c++20 -O3 -Wall -Wextra -Werror \
  -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable \
  -Itools/tests/latte_stubs -Iruntime/cemu \
  tools/tests/latte_index_stream_bench.cpp runtime/cemu/Cafe/HW/Latte/Core/LatteIndices.cpp \
  -o build/tests/latte_index_stream_bench
build/tests/latte_index_stream_bench
