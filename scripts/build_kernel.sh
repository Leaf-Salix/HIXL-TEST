#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
: "${ASCEND_HOME_PATH:?先 source CANN set_env.sh}"
compiler="$ASCEND_HOME_PATH/tools/bisheng_compiler/bin/bisheng"
ascendc="$ASCEND_HOME_PATH/compiler/tikcpp/tikcfw"
mkdir -p "$root/build"
"$compiler" -xcce -std=c++17 -O2 -fPIC --cce-aicore-arch=dav-c220-vec \
  -I"$ascendc" -I"$ascendc/interface" -I"$ascendc/impl" -I"$ASCEND_HOME_PATH/include" \
  -c "$root/src/kernel_copy.cpp" -o "$root/build/kernel_copy.o"
"$compiler" --cce-fatobj-link --cce-aicore-arch=dav-c220-vec -shared \
  "$root/build/kernel_copy.o" -L"$ASCEND_HOME_PATH/lib64" -lascendcl -lruntime \
  -Wl,-rpath,"$ASCEND_HOME_PATH/lib64" -o "$root/build/libfabric_copy.so"
nm -D "$root/build/libfabric_copy.so" | grep ' launch_fabric_copy$'
