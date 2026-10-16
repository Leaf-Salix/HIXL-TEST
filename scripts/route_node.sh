#!/usr/bin/env bash
# 在单个节点的预约任务内执行；对端需同时启动另一个 rank。
set -eo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
if [[ $# != 3 || ! "$1" =~ ^[01]$ || ! "$2" =~ ^[0-9]+$ || ! "$3" =~ ^[0-9]+$ ]]; then
  echo 'usage: bash scripts/route_node.sh RANK DEVICE NUMA' >&2
  exit 2
fi
: "${CANN_ENV:?设置系统 CANN set_env.sh 路径}"
source "$CANN_ENV"
source "$root/configs/route.env"
set -u
export PYTHONNOUSERSITE=1
python_bin=${PYTHON_BIN:-python3}
suffix=A
if [[ "$1" == 1 ]]; then suffix=B; fi
output="$root/results/$RUN_ID-$suffix"
"$python_bin" "$root/scripts/route.py" run --rank "$1" --device "$2" --numa "$3" \
  --coordinator "$COORDINATOR" --run-id "$RUN_ID" --bytes "$BYTES" \
  --iterations "$ITERATIONS" --idle-ms "$IDLE_MS" --timeout 900 --output "$output"
# run 返回前有跨端最终 barrier，两个节点的全部采样均已停止。
"$python_bin" "$root/scripts/route.py" export --output "$output"
