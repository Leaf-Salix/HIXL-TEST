# 验证记录与证据边界

日期：2026-10-16（Asia/Shanghai）。

## CPU 原生 DRAM 能力探针：同机对照失败

2026-10-16，经 task-submit 预约单卡，运行两个同机进程的 [CPU 原生访问能力探针](cpu-direct.md#同机双进程硬件对照fail)。
两个进程的 Host SetAccess/GetAccess 均返回 0，查询权限为 READWRITE（flags=3），并进入 CPU 数据阶段。
首组 CPU_A READ 对端 DRAM 时，rank0 在目标区首字节（偏移 1048576）校验不一致，
随后 rank1 因控制连接 Broken pipe 退出。两 rank 均退出 1，零组 `CPU_CASE_PASS`，没有成功清理标记或成功 summary。
原始产物的 source/session/build 摘要一致，并与 `f12209a` 的探针源码匹配。

该结果记为 **FAIL，不是 UNSUPPORTED 或 CPU 直接访问通过**；根因尚未确定，双机 CPU 原生访问仍未实测。
它不改变下文已经通过的 NPU SDMA、AIV 或 HIXL 测试结果，也不能证明硬件没有 CPU-HCCS 连接。

## 独立 DRAM 路径探针

新增 [DRAM_A→DRAM_B 路径验证](dram-route.md)，记录实际拷贝 VA 的驱动查询，
并用 CANN profiling API 对 idle/N/2N 单独采集双方 PCIe/HCCS，排除初始化、预热和 CPU 校验。
2026-10-16 已经 task-submit 单机两卡验证：1 MiB × 4096/8192 次（4/8 GiB）数据与 guard、
正常释放全部通过，双方三个阶段的 PCIe/HCCS CSV 和 timeline 均导出成功。
实际查询 rc=0/raw=0/INNER；这不是逐跳 PCIe/HCCS 路径结论。
双机路径探针尚待用户运行；其结果与下文已有完整矩阵通过记录分别报告。

## 单机实测（仓库整理时执行）

环境为单台 Atlas A3、两张独立物理 NPU、CANN 9.0、26.0.rc1 驱动。
硬件执行通过现场 task-submit 预约两卡，未修改系统 SDK、配置权限或其他任务。
本仓库不依赖验证现场使用的 PyPTO/simpler 环境。

| 检查 | 结果 | 说明 |
| --- | --- | --- |
| C++ host 与真实 AIV kernel 编译 | 通过 | C++17；`dav-c220-vec` |
| baseline | 两 rank 均通过 | CPU 实际 DRAM↔本地 HBM，含完整窗口 guard |
| 独立 kernel | 48 用例通过 | 每个 AIV 操作四个 DRAM/HBM 窗口的所有不同源/目标组合 |
| 独立 HIXL | 64 用例通过 | 隔离 beta2 + 配置读取兼容修复；同步/异步 READ/WRITE |
| 同窗口 combined | 64 + 48 + 8 用例通过 | 隔离 beta2 + 配置兼容 + 实验窗口桥；同一组物理分配 |
| 源码/脚本检查 | 通过 | Python AST、shell 语法、格式 |
| 无硬件 UT | 18 项通过 | 三进程 TCP collective、异常退出/顺序错误、SIGTERM 子进程回收、配置/结果汇总 |
| 实验补丁 | 通过 | 校验原始文件摘要；应用结果与原实测补丁源码一致 |
| 实验桥离线构建脚本 | 通过 | 使用已有展开依赖，独立生成共享库，不安装系统库 |

使用本仓库 `build_domain.sh` 新生成的隔离库，再运行 combined 的 120 个用例，也全部通过。

数据校验使用非线性花纹，包含测试序号、窗口所属 rank、DRAM/HBM 类型和字节位置，
避免原线性 byte pattern 在较多 rank 时出现整段相同导致漏检。
联合测试中间不做 ACL 数据读回，只在完成交接后验证数据和 guard。
原始现场证据含机器信息，未公开提交；生成结果目录默认被 Git 忽略。

## 双机实测（用户反馈）

2026-10-16，用户反馈：在两台真机上按本仓库 A/B 多机示例执行，四种模式全部 `GLOBAL_PASS`。

| 模式 | 用户报告结果 | 按测试实现对应的能力 |
| --- | --- | --- |
| `baseline` | `GLOBAL_PASS` | 各 rank 的 CPU DRAM↔本地 HBM 及完整窗口校验 |
| `hixl` | `GLOBAL_PASS` | 所有 rank 对之间的 DRAM/HBM READ/WRITE，同步及异步完成 |
| `kernel` | `GLOBAL_PASS` | 真实 AIV kernel 主动访问所有映射窗口，包含跨机器 DRAM/HBM |
| `combined` | `GLOBAL_PASS` | 同一组物理窗口上的 HIXL/kernel 矩阵、跨后端交接和正常生命周期 |

证据来源是用户的真机实测反馈，尚未在仓库归档或独立复核该次原始 manifest/rank 日志。
用户未提供该次现场 CANN/HDK/HIXL 的精确版本、是否另有适配及启动方式，不能将单机现场版本直接套用到双机。
实际 rank 数与逐项计数以该次 manifest 为准；README 中的公式是测试预期，不当作已读取的现场计数。

该反馈补上了当前测试模型的跨机 DRAM/HBM 访问、真实 AIV 主动访问及同窗口交接验证依据。
它仍不等于原版 SDK 任意版本开箱通过，也不证明缓存一致性、原子操作或生产级故障恢复。

## node-shared 新布局单机实测

2026-10-16，通过 task-submit 预约两卡，执行每节点单一 DRAM 的新布局四模式，均正常通过。
实际只有一个 DRAM owner allocation 和两个 HBM allocation，两个 NPU 访问该唯一 DRAM 的同一份 Fabric 导出。

| 模式 | 实际全局计数 |
| --- | --- |
| baseline | 两 rank 均通过；非 owner 不申请第二块域内 DRAM |
| hixl | 24 个 CPU 域访问用例，包含本地 memmove/ACL 与 HIXL peer 读写 |
| kernel | 24 个 NPU 读写、4 个同址有序交接、4 个同轮共享访问组 |
| combined | 24 个 CPU 读写＋24 个 NPU 读写＋12 个交接＋4 个同轮共享访问组，共 64 项 |
| 默认 rank-private 回归 | 原 64＋48＋8，共 120 项通过 |
| 软件检查 | 原 18 UT＋新增 9 项拓扑/唯一分配/计数/汇总检查，共 27 项通过 |

新布局共同读同一 DRAM 区间、共同写不相交子区间，以及两个方向的同址有序写后读均有覆盖。
不同计算参与者对同一字节的无同步竞争写、原子操作及设备指令严格同时执行不在通过结论内。
旧默认布局的用户双机通过记录保留；新版 node-shared 双机六窗口、280 项矩阵尚未运行。
运行方法及计数见 [node-shared](node-shared.md)。

## 系统 CANN-only ACL 模式单机实测

2026-10-16，新增 `--layout node-shared --mode acl`，经 task-submit 预约单机两卡通过。
构建使用 CANN 9.0 安装中的头文件和库；运行时核对 `libcann_hixl.so`、`libllm_datadist.so`、
`libascendcl.so` 均解析到该系统 CANN 安装目录，没有加载 GitCode 构建的适配库。
不初始化 HIXL engine，不调用实验桥；这是 ACL Fabric＋SDMA＋AIV 的测试结论。

| 检查 | 实际结果 |
| --- | --- |
| node-shared baseline | 两 rank 通过，仍只有一块真实 DRAM 和两块 HBM |
| 独立 kernel 回归 | 24 个访问＋4 个 NPU 交接＋4 个共享访问组，通过 |
| 完整 acl | 24 个 CPU 访问＋24 个 NPU 访问＋12 个交接＋4 个共享访问组，共 64 项通过 |
| HIXL API 传输 | 不执行，`hixl_cases=0` |
| 软件检查 | 30 项 UT 通过，含 ACL 计数、后端/同步异步路径证据与全局汇总拒绝检查 |

同一 Fabric token 在非 UC 设备 VA arena 中建立映射，包含唯一 DRAM 的自导入别名；
Host SDMA 与 AIV 共同使用这些映射，不新增物理 DRAM/HBM 或中转数据副本。
1 GiB arena 只是虚拟地址预约；实际域内物理分配仍是三个 2 MiB 窗口。
同步域请求通过 `aclrtMemcpyAsync`＋流同步完成；异步请求先流状态轮询，再同步检查任务错误。
不是将 `aclrtMemcpy` 对所有导入地址的支持记成通过。自身 DRAM 的访问使用真实 CPU memmove。

调试中普通 VA 的远端 HBM 异步拷贝报过 507001，同步拷贝对另一种别名报过 207000，
小于 1 GiB 的 NoUCMemory 预约报过参数错误；这些失败保留为诊断证据，不计入成功结果。
最终实现按设备 Fabric VA、公开预约粒度与 SDMA 完成流程通过，无静默回退。
NoUCMemory 指定地址和同进程导出/自导入组合的版本边界见 [共享域说明](node-shared.md)，
当前结论不能推广到其他 SDK、所有拓扑或生产级稳定性。

**ACL-only 双机六窗口、280 项尚待现场真机运行**；旧的用户双机四模式反馈不替代该项。
完整 A/B 命令已更新为系统 CANN 的 baseline → kernel → acl 三轮。

## 单机原版与适配不能混淆

现场原版 HIXL 在设备配置文件存在但不可读时初始化失败。
因此不能把单机表中的 HIXL 通过描述为“原版 HIXL 在该现场开箱通过”。
该环境的 Host 分配也使用显式 ACL VMM；没有把失败的旧 Host 分配器记成成功。

- `hixl` 模式不依赖窗口实验桥，但现场使用了配置读取兼容修复。
- `kernel` 模式验证 ACL Fabric + AIV，不初始化 HIXL engine。
- `combined` 模式需要实验桥，不能用两个独立模式的通过结果替代。

## 尚未实测

- 超出用户此次双机示例的更大规模或其他拓扑；不从通过反馈推导任意规模支持。
- 实际 mpirun 启动：rank 环境选择已有 UT，用户此次反馈未说明具体启动方式。
- CANN ≥9.2：设备访问编号按版本分支适配，尚未在对应 SDK/硬件上运行。
- HDK25.5 的 ADXL Host 分配选项、纯 CPU 节点、普通跨超节点 RoCE 的等价访问。
- 并发无锁结构、原子操作、缓存一致性、故障恢复、容量极限和性能。

官方 HIXL 仓库中的双机 FabricMem 测试是额外参考，不与用户此次运行或本仓库单机验证混为一组证据。
