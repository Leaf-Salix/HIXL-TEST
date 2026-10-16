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

## 双机六参与者／六窗口

继续用相同的 `configs/site.json`，每节点 devices 配两张已预约 NPU。
每台编译相同新版代码，两个节点均加 `--layout node-shared`；模式和配置必须一致。

```bash
# A 机器预约环境：
bash scripts/rank.sh node --config configs/site.json --node node-a --layout node-shared --mode combined
# B 机器预约环境：
bash scripts/rank.sh node --config configs/site.json --node node-b --layout node-shared --mode combined
```

也分别运行 baseline、hixl、kernel。仅 combined 使用实验桥；不同模式要分别保留结果目录。
MPI 入口同样支持该选项；hostfile、rank 顺序和现场环境规则见 [多机说明](multinode.md)。

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
