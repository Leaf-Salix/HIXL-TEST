# 多机多 CPU、多 NPU 验证

## 硬件前提

本测试面向**同一 A3 HCCS/Fabric 超节点内的多台服务器**。
管理网 IP 互通不代表 Fabric 已连通。请管理员先确认超节点拓扑、设备组网、
Host DRAM 访问权限及 CANN/HDK/灵衢版本。普通 RoCE 多机不在该测试的等价能力范围内。

每个 rank 对应一个 CPU worker、一块 DRAM 和一张 NPU 的 HBM。
同机两个 rank 的 Host 内存仍是两个独立窗口；可以放在相同或不同 NUMA 节点。
这验证多个 CPU worker 的参与，不验证 CPU socket 之间的硬件缓存一致性。

## 1. 在每个节点准备相同源码和配置

目标节点不必联网。提前传入仓库源码包，在每台机器上使用已安装 SDK 编译。
源码版本、配置 JSON、模式、allocator 必须相同；构建路径可以不同。
建议先两机各一张卡验证，再扩展为两机各两张卡。

复制 `configs/two-node.json` 为 `configs/site.json`。其中 `198.51.100.*` 是文档保留地址，必须替换。

```json
{
  "coordinator": "198.51.100.10:39000",
  "nodes": [
    {"name": "node-a", "ip": "198.51.100.10", "devices": [0, 1], "numa_ids": [0, 2], "engine_port_base": 39100},
    {"name": "node-b", "ip": "198.51.100.11", "devices": [0, 1], "numa_ids": [0, 2], "engine_port_base": 39100}
  ]
}
```

- `ip` 是进程所在主机可绑定、其他主机可访问的 IPv4，用于控制面/HIXL engine 标识；不是随便填的 NPU 网口地址。
- rank 顺序严格按 nodes 顺序及每个 devices 顺序：A0、A1、B0、B1。
- `devices` 是**实际执行进程环境**中的 ACL 逻辑 ID；调度器重新编号后应填编号后的值。
- `numa_ids` 按本机拓扑替换，长度与 devices 相同。上面数字只是配置格式示例。
- coordinator 必须属于 nodes[0] 所在机器。只需节点间内网；不需要公网。
- 每个 rank 的 HIXL 端口是 `engine_port_base + 本节点内索引`，两个节点可用相同 base。
- 控制端口不能与任一 HIXL 端口重合。端口均需空闲，并按现场管理要求放通。
- 此外 HIXL/驱动的数据面连接需要现场配置；开放这些控制端口不能代替 Fabric 组网。
- 控制面只用于受信任的隔离测试网络，没有认证或加密。Fabric token 禁用了 PID 白名单校验，与原实验一致；不要暴露给不受信任客户端。

脚本不会远程预约资源，也不会自动通过 SSH 启动其他节点。
请使用集群调度器同时取得两台机器的卡，再在两个预约环境中分别运行命令。

## 2. 各节点先检查本地 baseline

```bash
source /path/to/cann/set_env.sh
bash scripts/build.sh
bash scripts/rank.sh single --devices 0,1 --numa-ids 0,2 --mode baseline
```

确认每个 rank 的 `DEVICE` 逻辑/物理映射和 `RAW_VMM` NUMA 符合预约与拓扑。
同一台机器上不能把同一物理设备交给两个 rank。

## 3. 运行多机 HIXL 传输矩阵

在 A 的预约终端：

```bash
bash scripts/rank.sh node --config configs/site.json --node node-a --mode hixl
```

在 B 的预约终端（默认 60 秒内启动；也可设置更长控制超时）：

```bash
bash scripts/rank.sh node --config configs/site.json --node node-b --mode hixl
```

各 rank 创建本地 DRAM/HBM、注册、交换描述符、连接全部对端。
测试覆盖同机与跨机的所有 rank 对，两个发起方向和所有 DRAM/HBM 组合。
HIXL `TransferSync` 返回成功、`TransferAsync` 轮询到 `COMPLETED` 后，才通知其他 rank 校验。

## 4. 分开验证 NPU kernel 主动访问

等待上一步全部结束、端口释放后，在 A/B 重新执行各自命令，仅把模式改成 `--mode kernel`。
这是全新的分配生命周期：每块物理窗口只 export 一次，所有 NPU 导入其他 rank 的窗口，
每个 NPU 执行所有源/目标组合，包括“源和目标都在远端”的组合。
使用 MTE 完成事件、pipeline barrier 和实际 stream synchronize，再由各 owner 检查完整窗口。

`kernel` 通过表示 ACL Fabric 映射 + 真实 AIV 访问通过，不能记成 HIXL API 传输通过。
多机上尚无本仓库实测结果；可能暴露单机没有的拓扑/权限/指令访问限制，应保留失败日志。

## 5. 验证同窗口跨后端交接（可选）

准备 [实验桥](experimental.md)，确认所有节点加载一致的适配库，再把模式改成 `--mode combined`。
测试包含 hixl/kernel 全部矩阵，额外对每个有序 rank 对验证：

1. producer CPU DRAM → HIXL 同步 WRITE → consumer HBM → consumer AIV → producer DRAM → CPU 校验。
2. consumer AIV 从 producer DRAM 读取 → consumer HBM → producer HIXL 异步 READ → producer DRAM → CPU 校验。

交接中间没有 ACL 数据读回；只依据上一后端的完成状态与控制通知。
最终检查中间 HBM、最终 DRAM，以及所有其他窗口的 guard。

正常释放顺序：所有操作完成 → 所有 HIXL 断链/注销 → 全局同步 → 所有 kernel import 撤销 →
全局同步 → owner 释放唯一 export 持有 → owner 释放物理内存。

## 6. 可选 mpirun 启动

MPI 仅提供启动和 rank 编号；测试仍使用同一个元数据控制连接，数据不经过 MPI。
需要现场已有 Open MPI/MPICH。该入口的 rank 选择做过 UT，尚未做真实 MPI/多机硬件运行。

所有节点使用相同绝对仓库路径，先保证远程 shell 能加载 CANN/HIXL。
Open MPI hostfile 例子：

```text
node-a-hostname slots=2
node-b-hostname slots=2
```

Open MPI 示例（hostfile 顺序和 JSON nodes 顺序必须一致；以下映射为每节点两 rank）：

```bash
export CANN_ENV=/path/to/cann/set_env.sh
export HIXL_LIB_DIR=/path/to/installed/hixl/lib64
export TEST_ROOT=/path/to/HIXL-TEST
mpirun -np 4 --hostfile hosts --map-by ppr:2:node --bind-to none \
  -x CANN_ENV -x HIXL_LIB_DIR \
  bash "$TEST_ROOT/scripts/rank.sh" mpi-rank \
  --config "$TEST_ROOT/configs/site.json" --mode hixl
```

不要让四个 MPI rank 再各自使用 `node` 子命令，否则会重复启动本机设备。
`mpi-rank` 每个 MPI rank 只启动一份 matrix；每份有独立结果目录。
MPICH 使用其对应 hostfile/映射/环境传递参数；程序读取 `PMI_RANK` / `PMI_SIZE`。
若每个 MPI rank 有独立可见设备掩码，应按该 rank 的实际可见逻辑 ID 配置 JSON，
不要直接套用“整节点共享同一掩码”的 `[0,1]` 示例。

## 7. 汇总

复制同次运行的所有结果目录，运行：

```bash
python3 scripts/collect.py results-from-a results-from-b
# MPI 模式要传四个 launcher 结果目录。
```

汇总拒绝缺 rank、重复 rank、失败日志、不同配置/模式/源码，以及同配置的不同运行结果混合。
全局随机 `RUN_ID` 来自本次实际控制组，用于区分重复运行。
配置不同节点必须在不同 hostname 的实际机器上执行；共享 hostname 的环境先由管理员区分。
结果不包括跨机性能、原子操作或缓存一致性结论。
