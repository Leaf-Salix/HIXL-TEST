#!/usr/bin/env bash
set -eo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
if [[ -n "${CANN_ENV:-}" ]]; then source "$CANN_ENV"; fi
set -u
: "${ASCEND_HOME_PATH:?先 source CANN set_env.sh，或设置 CANN_ENV}"
mkdir -p "$root/build"
prefix=()
if command -v ccache >/dev/null; then
  export CCACHE_DIR="$root/.ccache"
  ccache -s
  prefix=(ccache)
fi
"${prefix[@]}" "${CXX:-g++}" -std=c++17 -O2 -g -Wall -Wextra -Werror \
  -I"$ASCEND_HOME_PATH/include" "$root/src/cpu_direct.cpp" \
  -L"$ASCEND_HOME_PATH/lib64" -Wl,--enable-new-dtags,-rpath,"$ASCEND_HOME_PATH/lib64" \
  -lascendcl -lpthread -o "$root/build/cpu_direct"
ldd "$root/build/cpu_direct"
"${PYTHON_BIN:-python3}" "$root/scripts/cpu_direct.py" stamp
