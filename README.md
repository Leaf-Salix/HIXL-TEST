# HIXL-TEST

验证 Atlas A3 上 CPU DRAM 与多张 NPU HBM 能否组成混合通信内存域。
覆盖单机多卡，以及**同一 HCCS/Fabric 超节点内**的多机多卡。
目标机器已安装 HIXL/CANN；测试运行与编译均不访问公网，不依赖 PyPTO、simpler、pip 或 MPI。
可选择 `mpirun` 启动，MPI 不承载测试数据。

> 状态：单机两卡做过实机验证；2026-10-16 用户反馈，按仓库 A/B 双机示例运行的 baseline、hixl、kernel、combined 四种模式全部 `GLOBAL_PASS`。双机结果及证据来源见 [验证记录](docs/validation.md)。
> 普通以太网/RoCE 多机不能直接视为 FabricMem 环境。本仓库固定启用 FabricMem，不做协议回退。
> `combined` 是实验模式，需要额外桥接补丁；已安装原版 HIXL 不代表该模式可以直接运行。

**只使用已安装 CANN/驱动验证完整 CPU＋NPU 共享域时，使用 `--layout node-shared --mode acl`。**
CPU 使用公开 ACL 同步/异步拷贝，NPU 使用真实 AIV kernel，访问同一组唯一 DRAM/HBM；
不需要下载 GitCode HIXL、编译适配库或调用实验桥。所链接的 HIXL/ACL 库直接来自 CANN 安装目录，
例如 `/usr/local/Ascend/cann-9.0.0/lib64`；本模式不初始化 HIXL，不能记成 HIXL API 传输通过。
A/B 两机完整命令见 [系统 CANN 共享域测试](docs/node-shared.md)。

本仓库已执行的检查、SDK 适配及未验证项目见 [验证记录](docs/validation.md)。

**严格验证同机两个 NPU 共用一块 DRAM 时，使用 `--layout node-shared`，见 [共享 DRAM 六窗口测试](docs/node-shared.md)。**
该选项在双机各两卡时只分配两块 DRAM 和四块 HBM，显式覆盖六个计算参与者对六个位置的读写，
并增加同址有序交接、同轮读同一区域和写不同子区间。新布局单机已通过，双机新矩阵尚待实测。

## 测什么

默认 `--layout rank-private`：每个进程绑定一张 NPU，并拥有两个 2 MiB 窗口：真实 Host DRAM 和本地 HBM。
CPU 直接初始化/校验自己的 DRAM；访问其他空间通过 HIXL 域操作。
NPU kernel 使用真实 AIV MTE（GM→UB→GM）主动访问映射后的所有窗口。

| 模式 | 验证内容 | 对已安装 HIXL 的要求 |
| --- | --- | --- |
| `baseline` | 每个 rank 的 CPU DRAM↔本地 HBM；检查分配、映射和环境 | 不初始化 HIXL；需要 SDK 供链接 |
| `hixl`（默认） | 所有 rank 对之间 DRAM/HBM 的 READ/WRITE，同步和异步完成 | 原版支持 FabricMem 的 HIXL；不要求实验接口 |
| `kernel` | 每个 NPU 主动搬运任意两个不同窗口之间的数据 | ACL VMM Fabric 导入/导出；不初始化 HIXL，不能算 HIXL 传输通过 |
| `combined` | 同一组物理窗口的 HIXL、kernel、跨后端交接与生命周期 | 本仓库的可选 beta2 实验桥，见 [实验说明](docs/experimental.md) |
| **`acl`**（仅 node-shared） | CPU ACL＋NPU kernel 完整矩阵、共享 DRAM 访问与交接 | 仅已安装 CANN 的 ACL/Fabric 能力，不初始化 HIXL、不需要补丁 |

每次操作前更换数据花纹，操作后逐字节检查所有 owner 的完整窗口，包括源和目标区间外 guard。
测试长度为 4 KiB、64 KiB；默认 rank-private 布局的源偏移为 256 B、目标偏移为 512 B。
NPU stream、HIXL completion 和 CPU 校验都有实际返回状态检查。

默认 rank-private 布局中，设总 rank 数为 `P`，全局用例数为（node-shared 使用独立计数）：

| 内容 | 公式 | 2 ranks | 4 ranks（两机各两卡） |
| --- | --- | --- | --- |
| HIXL | `32 × P × (P−1)` | 64 | 384 |
| NPU kernel | `2 × P × (2P) × (2P−1)` | 48 | 448 |
| 跨后端交接 | `4 × P × (P−1)` | 8 | 48 |

各 rank 都打印全局计数；不要把这些计数再乘以 rank 数。
从两卡开始，全窗口 kernel 矩阵随规模快速增长；这不是性能 benchmark。

## 环境

- Linux、Atlas A3；多机必须由管理员配置在同一 HCCS/Fabric 超节点内。
- FabricMem 官方基线：HDK ≥25.5、灵衢 ≥1.5.0、CANN ≥9.0。版本号本身不保证硬件组网满足条件。
- 已安装的 HIXL/CANN **开发头文件和共享库**、g++（C++17）、Python ≥3.8。
- `kernel` / `combined` / `acl` 另需 CANN 的 bisheng 与 AscendC 头文件；当前 kernel 目标为 `dav-c220-vec`。
- 本机与各远端环境使用匹配的 CANN、驱动、HIXL，运行账号具有设备和 Fabric 内存权限。
- 实测基线为 CANN 9.0、26.0.rc1 驱动、HIXL v9.1.0-beta.2 及隔离适配；新版 SDK 尚不能据此宣称验证通过。

CPU 参与是“CPU worker 管理真实 DRAM 并发起域操作”。
本仓库每个 rank 都有 NPU，不验证纯 CPU、无 NPU 服务器的加入，也不声称 CPU 能直接解引用远端 HBM 指针。

## 编译与单机快速开始

推荐的系统 CANN 流程（设备与 NUMA 值按预约环境、现场拓扑替换）：

```bash
source /usr/local/Ascend/cann-9.0.0/set_env.sh
export HIXL_INCLUDE_DIR="$ASCEND_HOME_PATH/include"
export HIXL_LIB_DIR="$ASCEND_HOME_PATH/lib64"
bash scripts/build.sh
bash scripts/rank.sh single --devices 0,1 --numa-ids 0,2 --layout node-shared --mode baseline
bash scripts/rank.sh single --devices 0,1 --numa-ids 0,2 --layout node-shared --mode kernel
bash scripts/rank.sh single --devices 0,1 --numa-ids 0,2 --layout node-shared --mode acl
```

以下旧入口继续用于单独验证 HIXL API 与默认 rank-private 布局：

```bash
# 将此路径换成实际安装位置。
source /usr/local/Ascend/ascend-toolkit/latest/set_env.sh
bash scripts/build.sh

# 使用当前进程可见的 ACL 逻辑设备 ID；仅在已预约/独占的卡上执行。
bash scripts/rank.sh single --devices 0,1 --mode baseline
bash scripts/rank.sh single --devices 0,1 --mode hixl
bash scripts/rank.sh single --devices 0,1 --mode kernel
```

默认 `vmm` 分配器与原单机实验一致：Host P2P Huge DRAM、设备 Huge HBM。
它需要 HDK 26.0 的 retain 支持。**新机器应核对 NUMA 拓扑并显式指定**：

```bash
# 两个 NUMA 值按对应 NPU 的拓扑替换，不要照抄。
bash scripts/rank.sh single --devices 0,1 --numa-ids 0,2 --mode baseline
```

不指定 NUMA 时使用历史测试机的启发式 `2 × floor(physical_device_id/2)`，会在日志中打印；
此规则不能推广到所有 A3 机型。错误 NUMA 值可能导致分配失败或远端 NUMA 访问。
HDK 25.5 可先尝试 `--allocator adxl --mode hixl`，但其分配器和本机组合未验证；
不要把 HDK25.5 下需要 retain 的 kernel/combined 模式当作已支持。

编译只需 HIXL 测试时可用 `bash scripts/build.sh --host-only`。
非默认安装位置可设置 `HIXL_INCLUDE_DIR`、`HIXL_LIB_DIR`；C++ 字符串 ABI 默认 `0`，
若本地 SDK 要求不同，编译时设置 `HIXL_CXX11_ABI=1`。
`HIXL_LIB_DIR` 也控制运行库优先级，实际解析路径记录在结果 manifest 中。

共享机器的 `task-submit` 示例：

```bash
# 参数按本地队列规范调整；{} 由队列替换为获批设备列表。
# 仅在该任务明确获批两卡时使用 --device-num 2。
CANN_ENV=/path/to/cann/set_env.sh
export CANN_ENV
TASK_ROOT="$PWD"
export TASK_ROOT

task-submit --device auto --device-num 2 --max-time 600 \
  --env CANN_ENV="$CANN_ENV" --env HIXL_LIB_DIR="${HIXL_LIB_DIR:-}" \
  --env PYTHONNOUSERSITE=1 \
  "bash '$TASK_ROOT/scripts/rank.sh' single --scheduled-devices {} --mode hixl"
```

`--devices` 始终是 ACL 逻辑 ID，不会自动改写。
只有 `--scheduled-devices` 会在存在 `ASCEND_RT_VISIBLE_DEVICES` 时转换成 `0..N−1`；
务必检查 rank 日志的 `DEVICE` 物理卡号。每张已预约卡只能分给一个 rank。
队列若不支持 `{}` 或采用别的屏蔽规则，按现场规则传 `--devices`。

## 多机与离线运行

完整步骤见 [多机运行](docs/multinode.md) 和 [离线部署](docs/offline.md)。
下面两条命令分别在两台机器、同一次预约期间启动；不要求机器访问 GitHub：

```bash
# 先将 configs/two-node.json 中示例 IP、逻辑设备、NUMA 和端口替换为真实配置。
# 两边必须使用完全相同的配置内容和测试模式。
# 机器 A：
bash scripts/rank.sh node --config configs/site.json --node node-a --mode hixl
# 机器 B：
bash scripts/rank.sh node --config configs/site.json --node node-b --mode hixl
```

然后以同样方式运行 `kernel`；`combined` 先准备实验桥。
原版 SDK 的 hixl/kernel 独立测试使用不同进程生命周期和不同分配，不能替代同窗口交接证明。

## 读取结果

每个本地 launcher 输出 `RESULT_DIR=...`，目录包含 `manifest.json` 和 `rank-N.log`。
非零退出、超时、数据/guard 错误、cleanup 错误、少用例都会失败，不会被标成跳过或通过。
所有节点完成后，将各自**同一次运行、同一模式**的目录复制到一个位置：

```bash
python3 scripts/collect.py /path/to/node-a-result /path/to/node-b-result
```

只有所有 rank 的返回码、用例数、全局随机 run ID、源码摘要和日志都匹配，才输出 `GLOBAL_PASS`。
`LOCAL_RANKS_PASS` 只是本地 launcher 成功；多机需收齐结果。
多机请使用不同 hostname，汇总器会拒绝把同一 hostname 下的多个配置节点认作跨机结果。
原始 manifest/日志可能含本地路径、IP、主机名和运行时句柄，不应直接公开提交。

## 已知边界与故障排查

- 验证显式完成后的可见性，不验证全局缓存一致性、远端原子操作、并发无锁队列、故障恢复或峰值带宽。
- 用户反馈双机 `kernel` 与 `combined` 均通过，覆盖真实 AIV 跨机访问及同窗口跨后端交接；不能只用 Host/AICPU 展开 SDMA 通过代替这些验证。
- HIXL 可使用 TCP 建链；本测试也有独立 TCP 控制面。数据矩阵不通过 TCP 控制连接传输。
- `coordinator bind/listen failed`：控制 IP 不在 rank0 本机、端口占用或权限错误。单机默认自动选控制端口；可用 `--port` 和 `--engine-port-base` 覆盖端口。
- `Hixl::Initialize` 失败：检查 HIXL 环境、设备 IP/配置可读性。旧 beta2 在配置存在但不可读时可能失败，实验补丁含兼容修复；不要靠伪造 IP 或改共享系统权限绕过。
- `MallocPhysical` / `MapMem` / `SetAccess` 失败：先检查 baseline、NUMA、驱动、Huge/P2P Host 内存权限和设备 ID 映射。
- 缺少 `HixlDomainWindowAcquire`：加载的是原版库；仅 `combined` 需要实验桥。
- Fabric 重复 export 失败：同一物理窗口只能导出一次，需统一句柄管理，不能给 HIXL 和 kernel 各导出一次。
- 控制超时：检查每台机器是否启动、配置/模式是否相同、内部端口连通和各 rank 日志。调试可增大 `--control-timeout` / `--timeout`。
- 失败时是 fail-stop：停止本 launcher 创建的进程，不保证分布式优雅清理；跨机另一端会因 EOF/超时失败。SIGKILL/机器失联场景仍由现场调度器回收。

## 源码检查与许可

```bash
python3 scripts/check.py
python3 -m unittest discover -s tests -v
```

检查不访问 NPU/公网；控制面 UT 使用本机 loopback 端口和 C++ 编译器。
本仓库原始测试代码采用 MIT；`patches/` 中上游衍生补丁单独适用 CANN 许可证，见 [许可说明](NOTICE)。

参考：[官方 HIXL](https://gitcode.com/cann/hixl)、
[FabricMem 文档](https://gitcode.com/cann/hixl/blob/master/docs/zh/FabricMem.md)、
[官方双机性能数据](https://gitcode.com/cann/hixl/blob/master/benchmarks/performance.md)。
官方双机传输数据是设计依据，不是本仓库已完成多机验证的证据。
