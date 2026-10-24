#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"
bash tools/tests/run_latte_perf5_tests.sh
cxx="${CXX:-clang++}"
"$cxx" -std=c++20 -O3 -Wall -Wextra -Werror \
  -Itools/tests/d3d12_stubs -Iruntime/cemu \
  tools/tests/latte_perf6_benchmark.cpp -o build/tests/latte_perf6_benchmark
build/tests/latte_perf6_benchmark
