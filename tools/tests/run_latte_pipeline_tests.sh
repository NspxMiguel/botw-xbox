#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"
mkdir -p build/tests
cxx="${CXX:-clang++}"
common=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined)
export UBSAN_OPTIONS=halt_on_error=1
"$cxx" "${common[@]}" -Itools/tests/d3d12_stubs -Iruntime/cemu tools/tests/latte_pipeline_test.cpp -o build/tests/latte_pipeline_test
build/tests/latte_pipeline_test
if [[ "${LATTE_PIPELINE_TSAN:-0}" == 1 ]]; then
  "$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=thread -Itools/tests/d3d12_stubs -Iruntime/cemu tools/tests/latte_pipeline_test.cpp -o build/tests/latte_pipeline_tsan_test
  TSAN_OPTIONS=halt_on_error=1 build/tests/latte_pipeline_tsan_test
fi
if [[ "${LATTE_PIPELINE_BENCHMARK:-1}" == 1 ]]; then
  "$cxx" -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Werror -Wno-unused-parameter -pthread -Itools/tests/d3d12_stubs -Iruntime/cemu tools/tests/latte_pipeline_bench.cpp -o build/tests/latte_pipeline_bench
  build/tests/latte_pipeline_bench
fi
