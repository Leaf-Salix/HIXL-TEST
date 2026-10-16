#!/usr/bin/env bash
set -eo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
if [[ $# != 3 || ! "$1" =~ ^[01]$ || ! "$2" =~ ^[0-9]+$ || ! "$3" =~ ^[0-9]+$ ]]; then
  echo 'usage: bash scripts/cpu_direct_node.sh RANK DEVICE NUMA' >&2
  exit 2
fi
: "${CANN_ENV:?设置系统 CANN set_env.sh 路径}"
source "$CANN_ENV"
source "$root/configs/cpu-direct.env"
set -u
export PYTHONNOUSERSITE=1
suffix=A
if [[ "$1" == 1 ]]; then suffix=B; fi
exec "${PYTHON_BIN:-python3}" "$root/scripts/cpu_direct.py" run \
  --rank "$1" --device "$2" --numa "$3" --coordinator "$COORDINATOR" \
  --run-id "$RUN_ID" --timeout 300 --output "$root/results/$RUN_ID-$suffix"
