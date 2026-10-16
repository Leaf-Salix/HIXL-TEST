#!/usr/bin/env bash
# Optional beta2 experimental bridge. Build only; never replace installed CANN/HIXL.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
: "${ASCEND_HOME_PATH:?先 source CANN set_env.sh}"
source_dir=$(cd -- "${1:?usage: build_domain.sh <patched-isolated-hixl-source>}" && pwd)
third=${CANN_3RD_LIB_PATH:-$source_dir/third_party}
# Require expanded offline dependencies before CMake; no fetch fallback is needed.
for file in cann-cmake/function/prepare.cmake pybind11/include/pybind11/pybind11.h \
            json/include/nlohmann/json.hpp makeself/makeself.sh makeself/makeself-header.sh; do
  if [[ ! -f "$third/$file" ]]; then
    echo "Missing offline dependency: $third/$file (see docs/experimental.md)" >&2
    exit 2
  fi
done
if [[ ! -f "$source_dir/include/hixl/experimental_domain_window.h" ]]; then
  echo 'Apply scripts/apply_domain_patch.py to a pristine isolated beta2 source first.' >&2
  exit 2
fi
out="$root/build/domain"
mkdir -p "$out/lib"
if [[ -d "$source_dir/.ccache" ]]; then
  export CCACHE_DIR="$source_dir/.ccache"
  ccache -s
fi
export CMAKE_BUILD_PARALLEL_LEVEL=4
python_bin=$(command -v "${PYTHON_BIN:-python3}")
cmake -S "$source_dir" -B "$out/cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCANN_3RD_LIB_PATH="$third" \
  -DPython3_EXECUTABLE="$python_bin" \
  -DENABLE_EXAMPLES=OFF -DENABLE_BENCHMARKS=OFF -DENABLE_SIGN=OFF
cmake --build "$out/cmake" --target cann_hixl llm_datadist --parallel 4
for library in libcann_hixl.so libllm_datadist.so; do
  path=$(find "$out/cmake" -type f -name "$library" -print -quit)
  [[ -n "$path" ]] || { echo "Missing built $library" >&2; exit 2; }
  cp "$path" "$out/lib/$library"
done
nm -D "$out/lib/libcann_hixl.so" | grep ' HixlDomainWindowAcquire$'
printf 'HIXL_LIB_DIR=%s\n' "$out/lib"
