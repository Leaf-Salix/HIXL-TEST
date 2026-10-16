# 每节点一块 DRAM：六参与者、六存储位置

使用 `--layout node-shared` 明确验证：每台机器只有一个实际的 Fabric DRAM allocation，
本机两个 NPU 与远端 NPU 都访问该 owner 导出的同一个窗口。
它不是两个 DRAM 窗口互相复制，也不是各 NPU 分别持有 DRAM 的数据副本。

| 节点 | 逻辑计算参与者 | 域内实际存储位置 |
| --- | --- | --- |
| A | CPU_A、NPU_A0、NPU_A1 | DRAM_A、HBM_A0、HBM_A1 |
| B | CPU_B、NPU_B0、NPU_B1 | DRAM_B、HBM_B0、HBM_B1 |

两机各两卡时，合计六个逻辑参与者、六个唯一窗口。
每个窗口 2 MiB；每节点首个 NPU rank 同时承担 CPU actor 和 DRAM owner。
因此仍是四个 NPU 进程／四个 MPI rank，**不能把 `mpirun -np` 改成六**。
CPU 通过 HIXL 域操作、ACL 本地 copy 或实际 CPU memcpy 访问；NPU 使用真实 AIV kernel。

## 唯一分配与映射

- A 的 rank0 分配 DRAM_A、HBM_A0；rank1 **只分配 HBM_A1**。
- B 的 rank2 分配 DRAM_B、HBM_B0；rank3 **只分配 HBM_B1**。
- kernel/combined 中，每个 NPU 的地址表都引用唯一 owner 的 Fabric token。
- `SHARED_DRAM_VIEW` 打印各 NPU 的实际本地映射地址及 owner；不同 VA 不代表不同物理窗口。
- `ALLOC_OK` 日志必须与每节点一块 DRAM、每 rank 一块 HBM 一致，否则 launcher/collect 拒绝通过。
- CPU 校验使用的普通临时数组不属于域资源，不额外注册或导出 Fabric DRAM 窗口。
- 正常退出仍须所有 importer 撤销后，再由 owner 释放 export 与物理内存。

NUMA 只由每节点 owner 的 `numa_ids` 元素决定；其他 rank 不再申请 DRAM。
本布局使用 `vmm` allocator；固定 2 MiB 并发 slot 布局目前最多支持 16 个 NPU rank。

## 四种模式

| 模式 | 新布局中的覆盖 |
| --- | --- |
| `baseline` | DRAM owner 验证实际 DRAM↔自己的 HBM；其余 rank 验证普通 CPU staging↔HBM，不额外申请域内 DRAM |
| `hixl` | 每节点一个 CPU actor，对所有唯一窗口分别 READ/WRITE，覆盖同步/异步请求及两种长度 |
| `kernel` | 每个 NPU 对所有唯一窗口分别主动 READ/WRITE；加同轮共享访问与 NPU 间同址交接 |
| `combined` | 前两类完整访问矩阵共用同一组物理窗口，并增加 CPU↔kernel 经共享 DRAM 的交接 |

CPU 访问自己的 DRAM 使用实际 memmove，访问自己持有的 HBM 用 ACL 本地 copy；
其余域窗口通过 HIXL READ/WRITE。日志 `path` 明确报告所走路径。
本地 copy 在返回时已经完成，即使请求类别为 ASYNC，也不会伪装成 HIXL 异步请求。
输出字段 `hixl_cases` 在本布局统计全部 CPU 访问用例，**包含这些本地路径**。

READ 将目标窗口的数据读到 actor 自己的 DRAM/HBM 结果区；WRITE 从 actor 自己的源区写入目标窗口。
源区与目标区不重叠；4 KiB、64 KiB 两种长度均检查所有实际 allocation 的完整 2 MiB 与 guard。

## 同一块 DRAM 的专门验收

除基本矩阵外，每块 DRAM 还覆盖：

1. **同轮读取同一区域**：所有 NPU 独立提交 kernel，读取该 DRAM 相同源区，分别写入自己的 HBM；校验结果全部一致。
2. **同轮写入不同子区间**：所有 NPU 独立提交 kernel，将各自不同的数据写到同一 DRAM 的不同 slot；CPU owner 直接校验全部写入及 guard。
3. **同址有序写后读**：NPU0 写 DRAM 指定区间，完成后 NPU1 读取这个确切区间，反方向也测。双机时覆盖所有有序 NPU 对及两块 DRAM。
4. **CPU↔kernel 交接**：CPU 域写入 DRAM→NPU kernel 读取；NPU kernel 写入 DRAM→CPU 域读取。中间只用完成状态和控制通知，不插入 ACL 数据回读。

“同轮”指进入同一控制屏障后，各进程向各自 NPU stream 独立提交并等待完成，
不对这些访问逐个排序；不声称设备内存指令必然在同一个时刻执行。
无同步地同时覆盖同一字节没有确定的最终值，不属于本测试的通过判据。
这也不证明原子操作、全局缓存一致性或常驻 kernel 的无锁协议。

## 单机两卡

编译、SDK 和实验桥准备沿用 README。单机是三个 actor、三个窗口的对应子模型：

```bash
source /path/to/cann/set_env.sh
bash scripts/build.sh
bash scripts/rank.sh single --devices 0,1 --layout node-shared --mode baseline
bash scripts/rank.sh single --devices 0,1 --layout node-shared --mode hixl
bash scripts/rank.sh single --devices 0,1 --layout node-shared --mode kernel

# combined 才需要可选实验桥；变量指向实际构建目录。
export HIXL_LIB_DIR="$PWD/build/domain/lib"
bash scripts/rank.sh single --devices 0,1 --layout node-shared --mode combined
```

共享机器须在预约任务中执行。README 的 task-submit 示例添加 `--layout node-shared` 即可。
`--scheduled-devices`、NUMA、可见设备编号规则不变。

## 双机六参与者／六窗口：A、B 完整操作步骤

下面使用每台两张 NPU、两台真实机器的模型；不需要公网，也不要求安装 MPI。
前提是两机已经处于**同一 A3 HCCS/Fabric 超节点**，具有 Host DRAM 的 Fabric 访问权限。
普通以太网/RoCE 连通不能代替这个前提。

所有硬件命令都在现场调度器已预约两卡的环境中执行；脚本不会替两台机器预约或 SSH 拉起进程。
配置中的设备号是该预约环境内的 **ACL 逻辑 ID**，不是直接照抄物理卡号。
例如预约掩码把两卡重新编号为 `0,1`，就填 `0,1`；检查日志 `DEVICE` 的物理映射。
`node` 子命令不接受 `--scheduled-devices`，也不会替配置做设备重编号。

### 1. 两台机器准备同一版本并编译

把同一个新版源码包离线解包到两机，或使用已经更新到相同提交的 checkout。
必须包含 `src/node_shared.h`；不能沿用旧版 `build/matrix`。
下面的 `/path/to/...` 都换成现场路径；两机 SDK/适配库版本应一致，安装路径可不同。
若上次四模式全通过时已有适配库，可以复用；不要把系统 HIXL 覆盖掉。

**机器 A：**

```bash
export TEST_ROOT=/path/to/HIXL-TEST
export CANN_ENV=/path/to/cann/set_env.sh
# 指向原版库，或上次 hixl 模式已通过的隔离兼容库；不是 combined 实验桥。
export HIXL_INCLUDE_DIR=/path/to/hixl/include
export HIXL_LIB_DIR=/path/to/hixl/lib64
# 上次 combined 已通过的实验桥目录；默认构建产物是下面这个路径。
export DOMAIN_LIB_DIR="$TEST_ROOT/build/domain/lib"
export PYTHONNOUSERSITE=1
cd "$TEST_ROOT"
source "$CANN_ENV"
test -f src/node_shared.h
bash scripts/build.sh
mkdir -p results
# 本次四轮使用一个新的本地父目录；不要重复使用旧结果目录。
export RUN_A=$(mktemp -d "$TEST_ROOT/results/node-shared-A-XXXXXXXX")
printf 'RUN_A=%s\n' "$RUN_A"
```

**机器 B：**

```bash
export TEST_ROOT=/path/to/HIXL-TEST
export CANN_ENV=/path/to/cann/set_env.sh
export HIXL_INCLUDE_DIR=/path/to/hixl/include
export HIXL_LIB_DIR=/path/to/hixl/lib64
export DOMAIN_LIB_DIR="$TEST_ROOT/build/domain/lib"
export PYTHONNOUSERSITE=1
cd "$TEST_ROOT"
source "$CANN_ENV"
test -f src/node_shared.h
bash scripts/build.sh
mkdir -p results
export RUN_B=$(mktemp -d "$TEST_ROOT/results/node-shared-B-XXXXXXXX")
printf 'RUN_B=%s\n' "$RUN_B"
```

`build.sh` 默认同时编译 Host 程序和 AIV kernel，不能加 `--host-only`。
若 HIXL 随 CANN 安装，include/lib 路径通常分别为 `$ASCEND_HOME_PATH/include` 和 `$ASCEND_HOME_PATH/lib64`。
这些变量及 `RUN_A/RUN_B` 在后面继续使用，请保留各自终端；重新进入预约环境时重新设置。

**只有没有准备好实验桥时，才在 A、B 各执行下面这段准备 combined。**
需要离线带入指定 beta2 的全新独立源码和已展开依赖，版本与依赖清单见 [实验桥说明](experimental.md)。
已有适配库直接跳过本段，把上面的 `DOMAIN_LIB_DIR` 指向实际目录；不要对已打补丁的源码重复应用补丁。

```bash
cd "$TEST_ROOT"
export HIXL_SRC=/path/to/isolated/pristine-hixl-v9.1.0-beta.2
export CANN_3RD_LIB_PATH=/path/to/expanded-hixl-third-party
python3 scripts/apply_domain_patch.py "$HIXL_SRC" --check
python3 scripts/apply_domain_patch.py "$HIXL_SRC"
bash scripts/build_domain.sh "$HIXL_SRC"
export DOMAIN_LIB_DIR="$TEST_ROOT/build/domain/lib"
```

### 2. A 生成配置，B 使用完全相同的配置

优先复用上次双机四模式通过的 `configs/site.json`，确认每台两卡、逻辑设备号及 NUMA 与本次预约一致。
在 A 执行：

```bash
cd "$TEST_ROOT"
cp configs/site.json configs/site-node-shared.json
```

如果没有旧配置，才改用下面这段在 A 新建。示例地址是文档保留地址，**必须把两个 IP、两组设备号和 NUMA 改为现场值**。
每台 `numa_ids` 第一项决定该机唯一 DRAM 的 NUMA；第二项保持与设备列表对应。
控制端口 `39000`、两机各自的 HIXL 端口 `39100/39101` 必须空闲且按现场要求放通。

```bash
cd "$TEST_ROOT"
cat > configs/site-node-shared.json <<'JSON'
{
  "coordinator": "198.51.100.10:39000",
  "nodes": [
    {"name": "node-a", "ip": "198.51.100.10", "devices": [0, 1], "numa_ids": [0, 2], "engine_port_base": 39100},
    {"name": "node-b", "ip": "198.51.100.11", "devices": [0, 1], "numa_ids": [0, 2], "engine_port_base": 39100}
  ]
}
JSON
# 运行前编辑为真实配置；不要直接测试保留地址。
"${EDITOR:-vi}" configs/site-node-shared.json
```

以下假定配置节点名为 `node-a/node-b`；复用旧配置时名称不同，就同步替换后面命令中的 `--node`。
在 A 将配置传给 B（替换 B 的 SSH 登录及仓库绝对路径）：

```bash
export B_SSH=user@node-b-hostname
export B_TEST_ROOT=/path/to/HIXL-TEST
rsync -av --exclude '._*' --exclude '.DS_Store' \
  configs/site-node-shared.json "$B_SSH:$B_TEST_ROOT/configs/site-node-shared.json"
sha256sum configs/site-node-shared.json
```

B 执行以下命令，摘要必须与 A 相同。若现场没有 SSH，用获准的离线通道复制该文件。

```bash
cd "$TEST_ROOT"
sha256sum configs/site-node-shared.json
```

### 3. 两边先 dry-run 检查六窗口模型

下面两条命令不访问 NPU，也不建立通信：

**A：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-a \
  --layout node-shared --allocator vmm --mode combined --dry-run
```

**B：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-b \
  --layout node-shared --allocator vmm --mode combined --dry-run
```

两边预期均为 `world=4`、`cpu_owners=[0,2]`、`npu_actors=[0,1,2,3]`，六个窗口为：
`DRAM(owner0), HBM(owner0), HBM(owner1), DRAM(owner2), HBM(owner2), HBM(owner3)`。
预期 combined 计数为 `96/96/80/8`，合计 280。

### 4. 按轮运行：baseline → hixl → kernel → combined

**每轮先启动 A，再在 300 秒内启动 B；两边均退出且没有错误后，才执行下一轮。**
不要把 A 的四轮一次性连续粘贴后才去启动 B。
`--control-timeout 300` 是控制等待时限，`--timeout 3600` 是本机 launcher 总时限，调度预约也要留足时间。
所有命令都显式指定 `node-shared/vmm`；每轮结果目录独立。

**第 1 轮 baseline，A：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-a \
  --layout node-shared --allocator vmm --mode baseline \
  --control-timeout 300 --timeout 3600 --output "$RUN_A/baseline"
```

**第 1 轮 baseline，B：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-b \
  --layout node-shared --allocator vmm --mode baseline \
  --control-timeout 300 --timeout 3600 --output "$RUN_B/baseline"
```

**第 2 轮 hixl，A：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-a \
  --layout node-shared --allocator vmm --mode hixl \
  --control-timeout 300 --timeout 3600 --output "$RUN_A/hixl"
```

**第 2 轮 hixl，B：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-b \
  --layout node-shared --allocator vmm --mode hixl \
  --control-timeout 300 --timeout 3600 --output "$RUN_B/hixl"
```

**第 3 轮 kernel，A：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-a \
  --layout node-shared --allocator vmm --mode kernel \
  --control-timeout 300 --timeout 3600 --output "$RUN_A/kernel"
```

**第 3 轮 kernel，B：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-b \
  --layout node-shared --allocator vmm --mode kernel \
  --control-timeout 300 --timeout 3600 --output "$RUN_B/kernel"
```

**第 4 轮 combined，A：**

```bash
# 先确认实验库提供两个非官方接口；只有该轮切换库，不改变后续终端的原版库变量。
nm -D "$DOMAIN_LIB_DIR/libcann_hixl.so" | grep -E ' HixlDomainWindow(Acquire|Release)$'
HIXL_LIB_DIR="$DOMAIN_LIB_DIR" bash scripts/rank.sh node \
  --config configs/site-node-shared.json --node node-a \
  --layout node-shared --allocator vmm --mode combined \
  --control-timeout 300 --timeout 3600 --output "$RUN_A/combined"
```

**第 4 轮 combined，B：**

```bash
nm -D "$DOMAIN_LIB_DIR/libcann_hixl.so" | grep -E ' HixlDomainWindow(Acquire|Release)$'
HIXL_LIB_DIR="$DOMAIN_LIB_DIR" bash scripts/rank.sh node \
  --config configs/site-node-shared.json --node node-b \
  --layout node-shared --allocator vmm --mode combined \
  --control-timeout 300 --timeout 3600 --output "$RUN_B/combined"
```

`nm` 应列出 Acquire、Release 两个符号；缺失时先纠正实验库路径，不要把原版库当成 combined 适配库。
检查 combined 的 manifest 中 `selected_libraries`：`libcann_hixl.so` 和 `libllm_datadist.so` 应来自实验目录。
任一轮失败，保留两边目录并停止后续轮次；重试时在两边使用新的输出目录，不覆盖旧证据。

### 5. B 打包结果，A 收齐四轮再汇总

两机四轮都结束后，在 **B** 执行：

```bash
export B_RESULT_ARCHIVE="$RUN_B.tar.gz"
tar -C "$(dirname "$RUN_B")" -czf "$B_RESULT_ARCHIVE" "$(basename "$RUN_B")"
printf 'B_RESULT_ARCHIVE=%s\n' "$B_RESULT_ARCHIVE"
```

在 **A** 执行；将 `B_RESULT_ARCHIVE` 改成上一步 B 打印的绝对路径，`B_SSH` 沿用配置同步时的登录：

```bash
export B_RESULT_ARCHIVE=/path/from/B/node-shared-B-XXXXXXXX.tar.gz
export COLLECT_ROOT=$(mktemp -d "$TEST_ROOT/results/node-shared-collect-XXXXXXXX")
rsync -avP --exclude '._*' --exclude '.DS_Store' \
  "$B_SSH:$B_RESULT_ARCHIVE" "$COLLECT_ROOT/"
tar -xzf "$COLLECT_ROOT/$(basename "$B_RESULT_ARCHIVE")" -C "$COLLECT_ROOT"
export B_RESULTS="$COLLECT_ROOT/$(basename "$B_RESULT_ARCHIVE" .tar.gz)"
(
  set -euo pipefail
  for mode in baseline hixl kernel combined; do
    python3 scripts/collect.py "$RUN_A/$mode" "$B_RESULTS/$mode" \
      | tee "$COLLECT_ROOT/global-$mode.json"
  done
)
```

四轮都应出现 `GLOBAL_PASS`，且 `layout=node-shared`、`world=4`、`observed_hosts=2`。
combined 应包含六窗口模型与 `hixl_cases=96, kernel_cases=96, handoff_cases=80, shared_cases=8`。
`LOCAL_RANKS_PASS` 只代表本地两 rank；必须收齐同一次运行的 A/B 结果，不能代替全局通过。
原始结果含现场路径、主机名、地址和运行时句柄，只在现场允许的通道保存/传输，不提交到公开仓库。

MPI 入口也支持 `--layout node-shared`，仍是 **4 ranks**；hostfile 和环境传递规则见 [多机说明](multinode.md)。
本节的两终端启动与 MPI 启动是两种选择，不要同时使用。

### 预期全局计数

| 内容 | 单机两卡 | 双机各两卡 |
| --- | --- | --- |
| CPU 访问（`hixl_cases`） | 24 | 96 |
| NPU kernel 访问（`kernel_cases`） | 24 | 96 |
| kernel 模式的 NPU 同址交接 | 4 | 48 |
| combined 模式全部交接（`handoff_cases`） | 12 | 80 |
| 同轮共享 DRAM 访问组（`shared_cases`） | 4 | 8 |
| combined 合计 | 64 | 280 |

这些数字是同一全局矩阵的计数，各 rank 重复打印，不要再乘进程数。
每个 NPU actor 对六个窗口 READ/WRITE 的组合都会执行；两种长度均覆盖，CPU 另含两类完成请求。

## 结果与验证状态

```bash
python3 scripts/collect.py /path/to/node-a-result /path/to/node-b-result
```

manifest 和汇总输出包含 `layout=node-shared` 与唯一窗口模型；不同布局的日志不能混用。
汇总检查实际分配数量、每类用例数量、全局 run ID、源码摘要和所有 rank 的正常退出。

2026-10-16 已通过 task-submit 在单机两卡上执行新布局四种模式，combined 共 64 项，
并用同一版本回归默认布局的 120 项。该次单机新布局只有一块实际 DRAM 和两块 HBM。
**新版 node-shared 双机六窗口矩阵尚待真机运行**；之前用户反馈的双机四模式通过属于默认 rank-private 布局，
不能直接记成新版 280 项已运行。详见 [验证记录](validation.md)。
