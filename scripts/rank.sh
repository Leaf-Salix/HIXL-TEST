#!/usr/bin/env bash
# MPI remote shells need the same compiler/runtime/library environment as the submit host.
set -eo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
if [[ -n "${CANN_ENV:-}" ]]; then
  source "$CANN_ENV"
fi
set -u
: "${ASCEND_HOME_PATH:?先设置 CANN 环境}"
if [[ -n "${HIXL_LIB_DIR:-}" ]]; then
  export LD_LIBRARY_PATH="$HIXL_LIB_DIR:${LD_LIBRARY_PATH:-}"
fi
export PYTHONNOUSERSITE=1
exec "${PYTHON_BIN:-python3}" "$root/scripts/run.py" "$@"
