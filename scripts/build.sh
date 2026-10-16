#!/usr/bin/env bash
set -eo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
if [[ -n "${CANN_ENV:-}" ]]; then
  source "$CANN_ENV"
fi
set -u
: "${ASCEND_HOME_PATH:?先 source CANN set_env.sh，或设置 CANN_ENV}"
hixl_include=${HIXL_INCLUDE_DIR:-$ASCEND_HOME_PATH/include}
hixl_lib=${HIXL_LIB_DIR:-$ASCEND_HOME_PATH/lib64}
compiler=${CXX:-g++}
mkdir -p "$root/build"
if [[ -d "$root/.ccache" ]]; then
  export CCACHE_DIR="$root/.ccache"
  ccache -s
fi
prefix=()
if command -v ccache >/dev/null; then
  prefix=(ccache)
fi
"${prefix[@]}" "$compiler" -std=c++17 -O2 -g -Wall -Wextra -Werror \
  -D_GLIBCXX_USE_CXX11_ABI="${HIXL_CXX11_ABI:-0}" \
  -I"$hixl_include" -I"$ASCEND_HOME_PATH/include" "$root/src/matrix.cpp" \
  -L"$hixl_lib" -L"$ASCEND_HOME_PATH/lib64" \
  -Wl,--enable-new-dtags,-rpath,"$hixl_lib:$ASCEND_HOME_PATH/lib64" \
  -lcann_hixl -lllm_datadist -lascendcl -lmetadef -lpthread -ldl -o "$root/build/matrix"
if [[ "${1:-}" != "--host-only" ]]; then
  bash "$root/scripts/build_kernel.sh"
fi
ldd "$root/build/matrix"
export HIXL_TEST_BUILD_ROOT="$root"
"${PYTHON_BIN:-python3}" - <<'PY'
import hashlib, json, os, platform, shutil, subprocess
from pathlib import Path
root = Path(os.environ['HIXL_TEST_BUILD_ROOT'])
info = dict(architecture=platform.machine(), cann=os.environ['ASCEND_HOME_PATH'],
            hixl_include=os.environ.get('HIXL_INCLUDE_DIR', os.environ['ASCEND_HOME_PATH']+'/include'),
            hixl_lib=os.environ.get('HIXL_LIB_DIR', os.environ['ASCEND_HOME_PATH']+'/lib64'),
            cxx11_abi=os.environ.get('HIXL_CXX11_ABI', '0'),
            source_sha256={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in sorted((root/'src').glob('*')) if p.is_file()})
info['git_commit'] = 'archive-without-git'
if shutil.which('git') and (root/'.git').exists():
    result = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    if result.returncode == 0:
        info['git_commit'] = result.stdout.strip()
(root/'build/build-info.json').write_text(json.dumps(info, indent=2)+'\n')
PY
