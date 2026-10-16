# 每节点一块 DRAM：六参与者、六存储位置

推荐使用 `--layout node-shared --mode acl`，只依赖已安装 CANN/驱动的公开接口。
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
CPU 按模式通过 ACL 域拷贝、HIXL 域操作或实际 CPU memmove 访问；NPU 使用真实 AIV kernel。

## 唯一分配与映射

- A 的 rank0 分配 DRAM_A、HBM_A0；rank1 **只分配 HBM_A1**。
- B 的 rank2 分配 DRAM_B、HBM_B0；rank3 **只分配 HBM_B1**。
- kernel/combined/acl 中，每个 NPU 的地址表都引用唯一 owner 的 Fabric token。
- `SHARED_DRAM_VIEW` 打印各 NPU 的实际本地映射地址及 owner；不同 VA 不代表不同物理窗口。
- `ALLOC_OK` 日志必须与每节点一块 DRAM、每 rank 一块 HBM 一致，否则 launcher/collect 拒绝通过。
- CPU 校验使用的普通临时数组不属于域资源，不额外注册或导出 Fabric DRAM 窗口。
- 正常退出仍须所有 importer 撤销后，再由 owner 释放 export 与物理内存。

NUMA 只由每节点 owner 的 `numa_ids` 元素决定；其他 rank 不再申请 DRAM。
本布局使用 `vmm` allocator；固定 2 MiB 并发 slot 布局目前最多支持 16 个 NPU rank。

## 测试模式

| 模式 | 新布局中的覆盖 |
| --- | --- |
| `baseline` | DRAM owner 验证实际 DRAM↔自己的 HBM；其余 rank 验证普通 CPU staging↔HBM，不额外申请域内 DRAM |
| `hixl` | 每节点一个 CPU actor，对所有唯一窗口分别 READ/WRITE，覆盖同步/异步请求及两种长度 |
| `kernel` | 每个 NPU 对所有唯一窗口分别主动 READ/WRITE；加同轮共享访问与 NPU 间同址交接 |
| `combined` | 前两类完整访问矩阵共用同一组物理窗口，并增加 CPU↔kernel 经共享 DRAM 的交接；需要实验桥 |
| **`acl`（推荐）** | CPU ACL＋NPU kernel 完整矩阵、共享访问及交接，使用同一组物理窗口；只用系统 CANN，不需要实验桥 |

旧 `hixl/combined` 模式中，CPU 访问自己的 DRAM 使用实际 memmove，访问自己持有的 HBM 用 ACL 本地 copy；
其余域窗口通过 HIXL READ/WRITE。日志 `path` 明确报告所走路径。
本地 copy 在返回时已经完成，即使请求类别为 ASYNC，也不会伪装成 HIXL 异步请求。
输出字段 `hixl_cases` 在旧模式中统计全部 CPU 访问用例，**包含这些本地路径**。

`acl` 模式中，CPU 只对自己的 DRAM 做实际 memmove；其他窗口统一使用 `aclrtMemcpyAsync` 提交 SDMA。
同步域请求通过 `aclrtSynchronizeStreamWithTimeout` 阻塞等待；异步请求先 `aclrtStreamQuery` 轮询，
再经流同步确认任务没有错误，之后才允许校验和交接。这里的 `ACL_SYNC` 指域请求的同步完成语义，
不代表对 Fabric VA 调用了同步 `aclrtMemcpy` 接口。
两端均为已映射且 NPU 可访问的 Fabric VA，显式使用 `ACL_MEMCPY_DEVICE_TO_DEVICE`，
其中包含物理位置为 Host DRAM 的窗口；不把自动 Host/Device 分类当作 Fabric 地址类型。
使用公开 `aclrtReserveMemAddressNoUCMemory` 预约 40 TiB 起始的 1 GiB **虚拟地址** arena，
将各 peer 的同一份 Fabric token 映射进去，并为自己的 DRAM 也导入一个设备 VA 别名。
该操作不分配 1 GiB 物理内存，也不复制数据；实际仍只有每节点一块 2 MiB DRAM、每卡一块 2 MiB HBM。
同一组导出和设备映射同时供 Host SDMA 与 NPU kernel 使用，不增加中转窗口。
该公开接口的地址范围和 1 GiB 粒度见 [CANN 文档](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_2134.html)。
指定起始 VA 属于 CANN 的试验特性，接口稳定性不能由本测试的通过代替保证；地址被占用或接口不支持会明确失败。
本测试实测版本允许为自己的导出 DRAM token 自导入设备别名；
[官方 ImportV2 文档](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/910/API/runtimeapi/aclcppdevg_03_2008.html)
对同进程 import/export 组合存在限制说明，不能把该次实测扩展成所有 SDK 版本的支持保证。
`acl_cases` 单独统计 CPU 访问（包含自身 DRAM 的 memmove），`hixl_cases=0`；不初始化 HIXL engine。
自身 DRAM 的 `ASYNC_REQUEST` 仍由 memmove 同步完成；其他窗口的 `ACL_ASYNC` 是真实异步提交与流完成等待。

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

只需 CANN 开发环境与本仓库源码。单机是三个 actor、三个窗口的对应子模型：

```bash
source /usr/local/Ascend/cann-9.0.0/set_env.sh
export HIXL_INCLUDE_DIR="$ASCEND_HOME_PATH/include"
export HIXL_LIB_DIR="$ASCEND_HOME_PATH/lib64"
bash scripts/build.sh
# 设备和 NUMA 按预约环境与现场拓扑替换。
bash scripts/rank.sh single --devices 0,1 --numa-ids 0,2 --layout node-shared --mode baseline
bash scripts/rank.sh single --devices 0,1 --numa-ids 0,2 --layout node-shared --mode kernel
bash scripts/rank.sh single --devices 0,1 --numa-ids 0,2 --layout node-shared --mode acl
```

共享机器须在预约任务中执行。README 的 task-submit 示例需添加 `--layout node-shared`，并将模式改为 `--mode acl`。
`--scheduled-devices`、NUMA、可见设备编号规则不变。

## 双机六参与者／六窗口：只用已安装 CANN 的 A、B 完整命令

推荐 `--mode acl --layout node-shared`：CPU 发起 ACL 拷贝，NPU 发起 AIV kernel，
共同使用同一组实际 DRAM/HBM。**不下载 GitCode HIXL，不编译适配库，不使用实验桥。**
测试 binary 链接 CANN 自带的 HIXL/ACL 库，但本模式不初始化 HIXL engine、不调用 HIXL 传输。
因此通过结果是 **CANN ACL Fabric + AIV** 的能力，不能标成原版 HIXL API 测试通过。

前提是两机已处于**同一 A3 HCCS/Fabric 超节点**，具有 Host DRAM 的 Fabric 访问权限。
普通以太网/RoCE 连通不能代替此前提；不需要公网，不要求安装 MPI。
所有硬件命令在两机各自已预约两卡的环境中执行；脚本不会远程预约或 SSH 拉起设备进程。
设备号使用预约环境内的 **ACL 逻辑 ID**，并检查 `DEVICE` 日志中的物理映射。
`node` 不接受 `--scheduled-devices`，不会替配置重编号。

### 1. A、B 分别准备相同源码并使用系统 CANN 编译

同一个新版源码包离线解包到两机，或两机 checkout 更新到相同提交。
必须包含 `src/node_shared.h` 且 `scripts/run.py` 支持 `acl` 模式，重新编译后再跑。
替换下面的仓库、CANN 路径；CANN 示例使用 `/usr/local/Ascend/cann-9.0.0`，不需要额外 HIXL 安装目录。
两机 CANN/驱动版本应匹配；保留各自终端，后面继续使用环境变量与结果父目录。

**机器 A：**

```bash
export TEST_ROOT=/path/to/HIXL-TEST
export CANN_ENV=/usr/local/Ascend/cann-9.0.0/set_env.sh
export PYTHONNOUSERSITE=1
cd "$TEST_ROOT"
source "$CANN_ENV"
export HIXL_INCLUDE_DIR="$ASCEND_HOME_PATH/include"
export HIXL_LIB_DIR="$ASCEND_HOME_PATH/lib64"
bash scripts/build.sh
# 输出的 libcann_hixl.so、libllm_datadist.so、libascendcl.so 应来自系统 CANN。
LD_LIBRARY_PATH="$HIXL_LIB_DIR:${LD_LIBRARY_PATH:-}" ldd build/matrix
mkdir -p results
export RUN_A=$(mktemp -d "$TEST_ROOT/results/node-shared-A-XXXXXXXX")
printf 'RUN_A=%s\n' "$RUN_A"
```

**机器 B：**

```bash
export TEST_ROOT=/path/to/HIXL-TEST
export CANN_ENV=/usr/local/Ascend/cann-9.0.0/set_env.sh
export PYTHONNOUSERSITE=1
cd "$TEST_ROOT"
source "$CANN_ENV"
export HIXL_INCLUDE_DIR="$ASCEND_HOME_PATH/include"
export HIXL_LIB_DIR="$ASCEND_HOME_PATH/lib64"
bash scripts/build.sh
LD_LIBRARY_PATH="$HIXL_LIB_DIR:${LD_LIBRARY_PATH:-}" ldd build/matrix
mkdir -p results
export RUN_B=$(mktemp -d "$TEST_ROOT/results/node-shared-B-XXXXXXXX")
printf 'RUN_B=%s\n' "$RUN_B"
```

`build.sh` 同时编译 Host 程序与 AIV kernel，不加 `--host-only`。
CANN 需要开发头文件、共享库、bisheng，另需本机 g++（C++17）和 Python ≥3.8。
本模式要求 `vmm` 分配器以及 Fabric retain/export/import、NoUCMemory VA 和流查询接口；版本边界沿用 README。

### 2. A 生成配置，B 使用完全相同的配置

优先复用上次双机通过的 `configs/site.json`，检查每台两卡、逻辑设备号和 NUMA 与本次预约一致。
在 A 执行：

```bash
cd "$TEST_ROOT"
cp configs/site.json configs/site-node-shared.json
```

没有旧配置时，才改用下面这段在 A 新建。**示例 IP 是文档保留地址，必须把 IP、设备号、NUMA 改为现场值。**
每台 `numa_ids` 第一项决定该机唯一 DRAM 的 NUMA；第二项保持与设备列表对应。
`acl` 模式只使用 A 的控制端口 `39000`；`engine_port_base` 保留配置格式，但本模式不建立 HIXL 连接。
控制端口需空闲且两机互通，Fabric 数据面由现场组网提供。

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
"${EDITOR:-vi}" configs/site-node-shared.json
```

以下假定节点名为 `node-a/node-b`；旧配置名称不同时，同步替换命令的 `--node`。
在 A 传配置给 B，替换 SSH 登录和 B 的仓库绝对路径：

```bash
export B_SSH=user@node-b-hostname
export B_TEST_ROOT=/path/to/HIXL-TEST
rsync -av --exclude '._*' --exclude '.DS_Store' \
  configs/site-node-shared.json "$B_SSH:$B_TEST_ROOT/configs/site-node-shared.json"
sha256sum configs/site-node-shared.json
```

**B** 校验摘要与 A 一致。现场无 SSH 时使用获准的离线通道传文件。

```bash
cd "$TEST_ROOT"
sha256sum configs/site-node-shared.json
```

### 3. 两边先 dry-run 检查六窗口模型

不访问 NPU、不建立通信。**A：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-a \
  --layout node-shared --allocator vmm --mode acl --dry-run
```

**B：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-b \
  --layout node-shared --allocator vmm --mode acl --dry-run
```

均应为 `world=4`、`cpu_owners=[0,2]`、`npu_actors=[0,1,2,3]`，六个窗口为：
`DRAM(owner0), HBM(owner0), HBM(owner1), DRAM(owner2), HBM(owner2), HBM(owner3)`。
预期计数：`hixl_cases=0, acl_cases=96, kernel_cases=96, handoff_cases=80, shared_cases=8`，合计 280。

### 4. 三轮：baseline → kernel → acl

**每轮先启动 A，再在 300 秒内启动 B；两边均退出且没有错误后，再进入下一轮。**
不要把 A 三轮一次性粘贴后才启动 B。每轮结果目录独立，预约时间也应覆盖完整运行。
`--control-timeout 300` 是控制等待时限，`--timeout 3600` 是本机 launcher 总时限。

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

**第 2 轮 kernel，A：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-a \
  --layout node-shared --allocator vmm --mode kernel \
  --control-timeout 300 --timeout 3600 --output "$RUN_A/kernel"
```

**第 2 轮 kernel，B：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-b \
  --layout node-shared --allocator vmm --mode kernel \
  --control-timeout 300 --timeout 3600 --output "$RUN_B/kernel"
```

**第 3 轮 acl（完整 CPU＋NPU 同窗口矩阵），A：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-a \
  --layout node-shared --allocator vmm --mode acl \
  --control-timeout 300 --timeout 3600 --output "$RUN_A/acl"
```

**第 3 轮 acl，B：**

```bash
bash scripts/rank.sh node --config configs/site-node-shared.json --node node-b \
  --layout node-shared --allocator vmm --mode acl \
  --control-timeout 300 --timeout 3600 --output "$RUN_B/acl"
```

每个 acl rank 应打印 `CANN_ACL_ONLY ... hixl_engine=DISABLED experimental_bridge=DISABLED`。
Host 路径记录 `CPU_MEMMOVE`、`ACL_SYNC`、`ACL_ASYNC`；异步路径必须实际等待 stream 完成。
检查 manifest 的 `selected_libraries` 中 HIXL/ACL 库确实来自系统 CANN。
任一轮失败，保留 A/B 目录并停止；重试使用新的输出目录，不覆盖旧证据。

### 5. B 打包结果，A 收齐三轮再汇总

两机三轮均结束后，**B：**

```bash
export B_RESULT_ARCHIVE="$RUN_B.tar.gz"
tar -C "$(dirname "$RUN_B")" -czf "$B_RESULT_ARCHIVE" "$(basename "$RUN_B")"
printf 'B_RESULT_ARCHIVE=%s\n' "$B_RESULT_ARCHIVE"
```

**A：** 把 `B_RESULT_ARCHIVE` 换成 B 刚打印的绝对路径；`B_SSH` 沿用配置传输时的登录。

```bash
export B_RESULT_ARCHIVE=/path/from/B/node-shared-B-XXXXXXXX.tar.gz
export COLLECT_ROOT=$(mktemp -d "$TEST_ROOT/results/node-shared-collect-XXXXXXXX")
rsync -avP --exclude '._*' --exclude '.DS_Store' \
  "$B_SSH:$B_RESULT_ARCHIVE" "$COLLECT_ROOT/"
tar -xzf "$COLLECT_ROOT/$(basename "$B_RESULT_ARCHIVE")" -C "$COLLECT_ROOT"
export B_RESULTS="$COLLECT_ROOT/$(basename "$B_RESULT_ARCHIVE" .tar.gz)"
(
  set -euo pipefail
  for mode in baseline kernel acl; do
    python3 scripts/collect.py "$RUN_A/$mode" "$B_RESULTS/$mode" \
      | tee "$COLLECT_ROOT/global-$mode.json"
  done
)
```

三轮均应输出 `GLOBAL_PASS`，且 `layout=node-shared`、`world=4`、`observed_hosts=2`。
acl 应包含六窗口模型及 `0/96/96/80/8` 五项计数；不是 HIXL 传输通过，`hixl_cases` 必须为零。
`LOCAL_RANKS_PASS` 只代表本机两 rank，必须收齐同一次运行的 A/B 结果才能判全局通过。
原始结果含现场路径、主机名、地址和句柄，按现场允许的通道保存，不提交到公开仓库。

MPI 也可使用 `mpi-rank ... --layout node-shared --mode acl`，仍是 **4 ranks**。
环境与 hostfile 规则见 [多机说明](multinode.md)；不要同时运行两终端入口与 MPI 入口。

### 可选：单独验证 HIXL API 或旧 combined 实验

旧 `hixl` 模式继续保留，用于测试 HIXL API；原版在某些现场存在初始化配置权限问题，不能保证开箱通过。
旧 `combined` 继续依赖 [实验桥](experimental.md)，用于比较 HIXL/kernel 交接，不属于上述“仅安装 CANN”的必跑流程。
这些旧模式分别保留结果；不能把 ACL 的通过计数写成 HIXL API 的通过计数。

### 预期全局计数

| 内容 | 单机两卡 | 双机各两卡 |
| --- | --- | --- |
| CPU 访问（acl 模式 `acl_cases`；旧模式 `hixl_cases`） | 24 | 96 |
| NPU kernel 访问（`kernel_cases`） | 24 | 96 |
| kernel 模式的 NPU 同址交接 | 4 | 48 |
| acl/combined 模式全部交接（`handoff_cases`） | 12 | 80 |
| 同轮共享 DRAM 访问组（`shared_cases`） | 4 | 8 |
| acl/combined 各自合计 | 64 | 280 |

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

2026-10-16 新增的系统 CANN `acl` 模式也已通过单机两卡实测：24 个 CPU 访问、24 个 NPU 访问、
12 个交接和 4 个共享访问组，共 64 项；实际加载系统 CANN 库，没有 HIXL 初始化、GitCode 构建库或实验桥。
新版 ACL 双机六窗口的 280 项仍待现场执行，不能沿用旧 HIXL/combined 双机通过反馈。
