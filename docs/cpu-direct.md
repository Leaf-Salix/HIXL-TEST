# CPU 原生访问远端 DRAM：源码结论与能力探针

截至 2026-10-19，**同机 CPU 原生共享 DRAM 已通过：原探针错用了 Device-target import，改成显式 Host-target import 后，双方四组 READ/WRITE、完整窗口及清理通过。**
普通 VA 预约和追加 HOST 权限，并不会把 Device-target handle 自动变成正确的 Host-target import。
跨机是另一项能力：已核查的旧 V2 驱动实现明确拒绝跨服务器的 Host-target import，目前没有通过的双机 CPU 原生访问结果。
CPU 接入 HCCS Fabric、远端 DRAM 可由 NPU SDMA 访问，并不等于 Host 用户态能直接解引用远端地址。

本页新增的是**能力探针**，用于核对现场实际安装的 SDK/驱动；不是已经证明支持的跨机传输例程。
只有两端 Host-target import 成功并查询到 `HOST READWRITE` 权限后，才执行真正的 CPU 原生读写。
不支持则报告 `UNSUPPORTED`，不回退 SDMA、MTE、RDMA 或 TCP 搬运。

## 已核查的实现

| 层次 | 证据 | 对本问题的含义 |
| --- | --- | --- |
| HIXL FabricMem | 已核查版本 `FabricMemTransferService::ProcessCopyWithAsync()` 调用 D2D `aclrtMemcpyAsync` | Host API 发起不等于 Host CPU 执行数据搬运 |
| Runtime VMM | `aclrtMemSetAccessImpl` → `rtMemSetAccess`；`NpuDriver::MemSetAccess` → `halMemSetAccess` | 仅拥有 Fabric token 或 Device VA，不能代替 Host 访问权限 |
| ACL V2 import | 获取当前 Device，再将该 Device ID 传给 HAL import | 导出的数据来自 DRAM，不等于导入目标是 Host；CPU 探针必须显式选择 Host 目标 |
| driver V2 同机 Host import | `devmm_master_mem_import` → 同 OS 共享 pages → Host map | 存在独立 Host-target 路径；本次同机修复已实测通过 |
| driver V2 跨机 Host import | `devmm_import_para_check` 对远端 SDID＋Host 导入目标返回 `-EOPNOTSUPP` | 当前核查源码没有提供跨服务器 Host-target import 能力 |
| driver V3 VMM | `vmm_dir_check()` 在访问方为本地 Host、源物理内存的 server_id 属于另一台服务器时返回 `DRV_ERROR_NOT_SUPPORT` | 明确禁止该公开实现中的 Host→远端 Host/Device 映射 |
| driver V3 import / set-access | `vmm_import()` 与 `vmm_set_access_check()` 都会调用上面的方向检查 | 先按 Device 导入、再追加 Host 权限也受相同限制 |
| MemFabric Hybrid | SDMA 后端的 global Host↔global Host 调用 `CopyG2G`；其任务最终提交给设备队列 | DRAM→DRAM 不代表 CPU load/store |
| MemFabric `HOST_SHM` | 枚举注明 same-node；实现打开本地 `/dev/shm` 或 hugetlbfs 文件再 `mmap` | 是同机共享内存，不是 CPU 跨机 HCCS 接口 |
| Host RDMA / Host URMA | 是另一类网络传输后端 | 不拿其成功替代本题要求的 CPU 原生 HCCS 访问 |

驱动里的方向表示“访问方页表中的 VA → 被访问方 PA”，不是 memcpy 的源/目的参数顺序。
该实现允许 Device→远端 Host，拒绝 Host→远端 Host，因此现有 SDMA 拷贝成功与上述限制可以同时成立。
`halHostRegister` 的头文件还明确不支持 VMM VA；不能把导入的 Fabric VA 再交给它强行变成 CPU 指针。

固定参考版本与可复核位置：

- CANN driver 公开源码快照 `9b9ad87cb8099fd741c93c93419a904ff05a60ce`：
  [GitHub 镜像：vmm_dir_check / import / set-access](https://github.com/hicann/driver/blob/9b9ad87cb8099fd741c93c93419a904ff05a60ce/src/ascend_hal/svm/v3/api/master/svm_vmm.c#L1933)，
  [上游对应文件](https://gitcode.com/cann/driver/blob/master/src/ascend_hal/svm/v3/api/master/svm_vmm.c)。
- [驱动公开接口声明与约束](https://gitcode.com/cann/driver/blob/master/pkg_inc/ascend_hal_base.h)：
  `halMemSetAccess` 的 H2H 跨 Host 备注，以及 `halHostRegister` 的 VMM VA 限制。
- MemFabric Hybrid 快照 `004d9317289fe99bd6bf13def0500b3fa3795ccc`：
  [数据操作类型](https://github.com/Ascend/memfabric_hybrid/blob/004d9317289fe99bd6bf13def0500b3fa3795ccc/src/hybm/include/hybm_def.h#L67)、
  [Host SHM 映射](https://github.com/Ascend/memfabric_hybrid/blob/004d9317289fe99bd6bf13def0500b3fa3795ccc/src/hybm/csrc/mm/hybm_host_shm_segment.cpp#L453)、
  [SDMA 数据操作](https://github.com/Ascend/memfabric_hybrid/blob/004d9317289fe99bd6bf13def0500b3fa3795ccc/src/hybm/csrc/data_operation/host/hybm_data_op_sdma.cpp#L515)。
- Runtime 快照 `501d254c79f868cab740e40593b7715bfae27851`：
  [公开 VMM API 说明](https://github.com/hicann/runtime/blob/501d254c79f868cab740e40593b7715bfae27851/docs/zh/api_ref/11-04_virtual_memory_management.md)、
  [跨服务器官方示例](https://github.com/hicann/runtime/blob/501d254c79f868cab740e40593b7715bfae27851/example/1_basic_features/memory/12_cross_server_physical_memory_sharing_withoutpid/client.cpp#L104)，后者仅设置 Device 权限，没有 CPU 数据访问。

**版本边界：**公开 driver v3 源码不等于现场 CANN 9.0/26.0.rc1 驱动二进制的逐字实现。
现场驱动导出了相关公开 API，但现有二进制显示旧 VMM 实现特征；不能用新源码替代双机实测，
也不能据此证明硬件理论上绝无这种能力。因此保留下述运行期能力探测。

## 探针到底验证什么

每机分配一个 2 MiB P2P Host DRAM allocation，交换 Fabric handle。
CPU 探针通过系统 HAL 的 `halGetHostID()` 动态取得 Host 目标，调用
`halMemImportFromShareableHandleV2(MEM_HANDLE_TYPE_FABRIC, ..., host_id, ...)` 导入对端 allocation，
再使用普通 `aclrtReserveMemAddress` / `aclrtMapMem`，随后显式调用：

```text
aclrtMemSetAccess(peer_va, ..., location=HOST, id=0, flags=READWRITE)
aclrtMemGetAccess(peer_va, location=HOST)
```

VA 预约沿用本仓库 CANN 9.0 现场的大页参数 `flags=1`，没有使用 NoUCMemory arena。
较新的公开 Runtime 文档把 flags 标为预留、建议 0，因此这个非默认约定也属于 SDK 兼容性边界，
不能承诺所有版本均接受。reserve/map 失败记为 `FAIL`，不据此宣布 CPU 跨机能力不存在。

双方都成功且查询到 READWRITE 才打开数据阶段；否则所有 rank 都不碰 peer 指针。
数据阶段分别由 CPU_A、CPU_B 主动执行 READ、WRITE，共四组 64 KiB 用例。
搬运是 `volatile uint8_t` 的逐字节 CPU load/store；每组更换花纹，双方检查完整 2 MiB 和 guard，
并使用 CPU 内存屏障及元数据完成握手。不是吞吐 benchmark，也不验证跨机原子操作或无同步并发写。

- 源码不调用 `aclrtMemcpy*`、不创建或提交 stream、不启动 kernel。
- 仍需 `aclInit`、一个 Device context 来建立 Fabric 元数据和导入句柄；**不等于支持无 NPU 的纯 CPU 服务器**。
- 普通 TCP 只交换句柄、配置摘要和阶段状态，不传校验内容或数据载荷。
- 每个 rank 是 launcher 的独立子进程；SIGBUS/SIGSEGV 会使其失败，父进程有超时，不做 ACL 初始化后的 fork。
- 正常 PASS/UNSUPPORTED 都先撤销两端进口映射、同步后再释放 owner。故障退出会保留失败日志。
- 本探针不采样交换机/CPU 端口。即使双机 CPU 读写成功，输出仍是 `hccs_route=NOT_MEASURED`；
  物理 HCCS 归因还需现场 CPU/Fabric 端口拓扑及计数，不能用所选 NPU 的计数器代替。

## 两机完整命令

两端位于同一 A3 HCCS/Fabric 超节点，各预约一张 NPU，CANN/驱动和权限满足原先 Fabric 测试条件。
源码、run-ID、coordinator 完全一致；每次运行换新 run-ID，不复用输出目录。
以下保留地址、路径、设备和 NUMA 都要按现场替换。

### 1. A、B 分别编译

**A：**

```bash
export TEST_ROOT=/path/to/HIXL-TEST
export CANN_ENV=/usr/local/Ascend/cann-9.0.0/set_env.sh
export PYTHON_BIN=python3
export PYTHONNOUSERSITE=1
cd "$TEST_ROOT"
source "$CANN_ENV"
bash scripts/build_cpu_direct.sh
```

**B：**

```bash
export TEST_ROOT=/path/to/HIXL-TEST
export CANN_ENV=/usr/local/Ascend/cann-9.0.0/set_env.sh
export PYTHON_BIN=python3
export PYTHONNOUSERSITE=1
cd "$TEST_ROOT"
source "$CANN_ENV"
bash scripts/build_cpu_direct.sh
```

不需要 HIXL GitCode 源码、bisheng、MPI 或 Python 第三方包。显式链接系统 `libascendcl`、`libdl`，
从已安装驱动动态加载 `libascend_hal.so`；使用 CANN 安装目录的 `include/driver` 头文件。
该原生 HAL handle 经 ACL Map/Free 直传的组合已在本次安装栈验证，其他 SDK 版本仍需验证。
launcher 校验 binary 和编译时源码摘要；更改 C++/头文件后必须重新编译。

### 2. A 配置，复制给 B

**A：**

```bash
cd "$TEST_ROOT"
cat > configs/cpu-direct.env <<'ENV'
export COORDINATOR=198.51.100.10:39501
export RUN_ID=cpu-direct-20261016-run01
ENV
"${EDITOR:-vi}" configs/cpu-direct.env
export B_SSH=user@node-b-hostname
export B_TEST_ROOT=/path/to/HIXL-TEST
rsync -av --exclude '._*' --exclude '.DS_Store' \
  configs/cpu-direct.env "$B_SSH:$B_TEST_ROOT/configs/cpu-direct.env"
```

把 coordinator 改为 B 可访问的 A 地址和空闲端口。无 SSH 时用获准离线渠道复制该文件。
`configs/*.env` 及 `results/` 不进入 Git。

### 3. 两端提交预约任务

**A：**

```bash
cd "$TEST_ROOT"
task-submit --device auto --max-time 450 \
  --env PYTHONNOUSERSITE=1 --env "CANN_ENV=$CANN_ENV" --env "PYTHON_BIN=$PYTHON_BIN" \
  "bash '$TEST_ROOT/scripts/cpu_direct_node.sh' 0 {} 0"
```

**B：**

```bash
cd "$TEST_ROOT"
task-submit --device auto --max-time 450 \
  --env PYTHONNOUSERSITE=1 --env "CANN_ENV=$CANN_ENV" --env "PYTHON_BIN=$PYTHON_BIN" \
  "bash '$TEST_ROOT/scripts/cpu_direct_node.sh' 1 {} 0"
```

两端各记下任务 ID，用 `task-submit --wait <任务ID>` 读结果。
两端实际启动应相隔少于 120 秒；排队时间过长导致连接超时，换新 run-ID 后重新预约。
`{}` 由调度器填充预约设备，必须符合现场 ACL 逻辑 ID 规则；有可见设备重映射时按其规则修改。
末尾 `0` 为 DRAM NUMA，应按现场替换。此任务不用 PyPTO/simpler。

没有 task-submit 的现场，在各自已获准的预约环境中执行以下直接入口即可：

**A：**

```bash
cd "$TEST_ROOT"
source "$CANN_ENV"
bash scripts/cpu_direct_node.sh 0 0 0
```

**B：**

```bash
cd "$TEST_ROOT"
source "$CANN_ENV"
bash scripts/cpu_direct_node.sh 1 0 0
```

### 4. 在 A 汇总两个结果目录

**A：**

```bash
cd "$TEST_ROOT"
source configs/cpu-direct.env
export B_SSH=user@node-b-hostname
export B_TEST_ROOT=/path/to/HIXL-TEST
export B_COPY="$TEST_ROOT/results/$RUN_ID-B-collected"
mkdir -p "$B_COPY"
rsync -av --exclude '._*' --exclude '.DS_Store' \
  "$B_SSH:$B_TEST_ROOT/results/$RUN_ID-B/" "$B_COPY/"
collect_rc=0
"$PYTHON_BIN" scripts/cpu_direct.py collect "$TEST_ROOT/results/$RUN_ID-A" "$B_COPY" \
  --output "$TEST_ROOT/results/$RUN_ID-summary.json" || collect_rc=$?
printf 'collect exit=%s\n' "$collect_rc"
cat "$TEST_ROOT/results/$RUN_ID-summary.json"
tar -czf "$TEST_ROOT/results/$RUN_ID-evidence.tar.gz" -C "$TEST_ROOT/results" \
  "$RUN_ID-A" "$RUN_ID-B-collected" "$RUN_ID-summary.json"
```

证据文件包含 hostname 和现场路径，不直接提交公开仓库。

## 结果含义

| 输出 / 退出码 | 含义 |
| --- | --- |
| `TWO_HOST_CPU_DIRECT_PASS` / 0 | 两机 CPU 原生 READ/WRITE、完整窗口和释放通过；没有提交 NPU 搬运；HCCS 逐跳路径尚未采样 |
| `SINGLE_HOST_CPU_PASS` / 0 | 同机对照通过，必须显式 `collect --allow-single-host`；不是跨机证明 |
| `UNSUPPORTED` / 3 | 至少一端 Host-target import 明确返回 DRV_ERROR_NOT_SUPPORT，或 SetAccess/GetAccess 返回 FEATURE_NOT_SUPPORT；双方不进入 CPU 数据阶段并正常清理；不是测试通过 |
| `FAIL` 或其它非零 | 分配/import/map、参数/权限、数据/guard、CPU fault、清理、超时或配置不一致；不能简单当成接口不支持 |

`UNSUPPORTED` 会让 task-submit 显示非零退出（3），这是能力探测的预期负结果。
HAL 的明确 NOT_SUPPORT 在 import 证据中归一化为 207000；其余 HAL 错误仍记 FAIL，不静默回退 Device-target。
同一轮不支持时双方都应产生 `CPU_DIRECT_RESULT ... status=UNSUPPORTED cleanup=PASS fallback=NONE`。
如果已有导入映射，但新增 HOST 权限被拒，仍不对该地址执行 CPU 读写。

## 实测边界

2026-10-16 已使用系统 CANN 9.0 编译本探针，并核对 binary 未引用 memcpy、stream 创建或 kernel 启动 API。
源码/格式检查、47 项仓库 UT 和独立实现审查通过。

### 2026-10-16 旧 Device-target 探针：FAIL

2026-10-16，经 task-submit 预约一张 NPU，两个进程各自初始化 context 并绑定同一预约设备，
执行同机 Fabric Host 映射与 CPU 原生读写对照。原始日志与 manifest 已复核；
两端 source/session/build 摘要一致，并与提交 `f12209a` 的探针源码一致。

| 阶段 | 实际结果 |
| --- | --- |
| 环境及独立 ACL/profiler 初始化基线 | 通过；这一步不是 CPU 数据搬运验证 |
| 两端 Host 访问权限 | `set_rc=0 get_rc=0 flags=3 ready=1`；均成功设置并查询到 READWRITE |
| CPU 数据阶段 | 两端进入 `CPU_IO_BEGIN ... fallback=NONE`；没有提交 NPU 搬运 |
| 首组用例 | 按当前代码顺序为 CPU_A READ 对端 DRAM；rank0 在偏移 `1048576`（目标区首字节）报告 `CPU data/guard mismatch` |
| 对端退出 | rank1 随后报告控制连接 `Broken pipe`；这是 rank0 失败退出后的次生错误，不是独立的第二个数据错误证据 |
| 完成与汇总 | 两个 rank 均退出 1；没有任何 `CPU_CASE_PASS` 或 `cleanup=PASS`，收集器拒绝失败结果，没有生成成功 summary |

**本次结论是数据校验失败，不是 `UNSUPPORTED`，也不是 `SINGLE_HOST_CPU_PASS`。**
API 返回成功与实际 CPU 数据访问通过是两项独立条件；前者在本次满足，后者没有满足。
全窗口校验在首个目标字节中止，未完成四组 READ/WRITE 和 guard 验收，正常清理路径也未走到完成标记。
首轮日志没有记录该字节的实际值；后续增加诊断和匹配正对照，结果见下节。

以上为修复前旧探针的失败记录；2026-10-19 的 Host-target 修复及同机通过结果见下节。
上面的双机命令保留为实验排障入口，不能当作已验证可用流程。
双机 CPU 原生访问尚未实测；公开实现的拒绝分支不替代你现场安装版本的运行结果。
本次同机失败也不能证明 CPU 没有物理接入 HCCS，或证明硬件不可能提供跨机访问能力。

### 后续排查：导入 Host 视图与 Device 视图的行为不同

这一节保留 2026-10-16 对旧导入方式的阶段性排查；不能用它宣布驱动没有实现同机 Host-target 共享。
2026-10-19 已通过目标选择的配对对照找到原探针的错误，见下一节。

2026-10-16，继续在同一系统 CANN 9.0 环境经 task-submit 预约单卡，两个同机进程绑定同一设备。
诊断保留原花纹、逐字节 CPU 拷贝和全窗口校验，控制 socket 不传测试载荷；失败后累计错误并完成正常清理。
以下为独立诊断程序的实测结果，不是原能力探针已经通过。

| 对照 | 实际结果 |
| --- | --- |
| 纯 CPU POSIX 共享映射 | 两进程四组 READ/WRITE、完整 2 MiB 窗口与 guard、清理通过；不初始化 ACL/NPU |
| 同进程同一物理 allocation 的第二个普通 VA | owner→alias 与 alias→owner 的 CPU 写后读均正确 |
| 原 Fabric 导入流程 | owner Fill 自检通过；首组 peer READ 实际读到 0，期望为 246；本地目标确实被写成 0 |
| peer WRITE | 导入视图中可本地读回新值，owner 的目标区仍保持原花纹；未形成预期共享语义 |
| reserve flags=0、owner 显式 HOST 权限、retained handle 导出 | 分别单因素修改，均未修复 CPU 校验 |
| 导出/导入角色分离 | exporter-only 与 importer-only，交换角色后，DEFAULT（uint64 token）与 FABRIC（128 字节 token）均失败 |
| 显式 PID 白名单 | 改用 export flags=0，并设置对端 bare PID 白名单，DEFAULT/FABRIC 均未修复 CPU 校验 |
| CPU cache 维护 | 按 CTR_EL0 的缓存行大小执行 `dc civac` 与 `dsb sy`，没有指令异常，仍出现相同错误 |

为确认共享句柄本身有效，最后原样复用本仓库 DRAM 路径探针的关键映射流程：
owner Device 权限→retain/export→自身及 peer import→NoUC Device VA。
**先用 NoUC 视图进行 SDMA 正对照，再为同一个已导入 handle 增加普通 VA 与 HOST READWRITE，
执行 CPU 四组 READ/WRITE，最后重新初始化花纹并复测原 NoUC SDMA 路径。** 没有为 CPU 视图重复 import token。

| 最终匹配对照 | 无 cache 维护 | 有 cache 维护 |
| --- | --- | --- |
| SDMA 前测，64 KiB 搬运及双方完整 4 MiB 校验 | PASS | PASS |
| CPU 四组 READ/WRITE，双方完整 4 MiB 校验 | FAIL；四组均完成，各自接收方校验失败 | 相同 FAIL |
| 换新花纹后的 SDMA 后测及双方完整窗口校验 | PASS | PASS |
| 两端正常卸载、释放及退出码 | cleanup=PASS；均退出 1 | cleanup=PASS；均退出 1 |

CPU 与 SDMA 使用不同花纹和独立校验阶段；SDMA 不补写 CPU 测试结果，也不作为 CPU 成功的回退。
早期简化 SDMA 参照在同步时返回过 507001，未计入正对照；上表使用完整映射流程后取得的前后通过结果。

**当时故障点收敛到：在旧 Device-target import 流程下，导入 handle 的普通 Host VA
未表现出与 owner DRAM 共享的 CPU 读写语义；同一 handle 的 NoUC Device 视图仍经 SDMA 前后测独立验证有效。**
已有对照不支持将问题解释成花纹/偏移错误、句柄完全失效，或仅缺少上述 CPU cache 维护。
SetAccess/GetAccess 的成功也不足以证明真实 CPU 共享访问已建立。
这仍没有定位到现场驱动的具体缺陷或受支持语义：驱动管理的 IO VMA 未提供可用于本次比较的 pagemap PFN，
不能据此断言两边物理地址不同。也不能把单机结果写成双机结论，或推断 CPU 未接入 HCCS、硬件不支持。

### 2026-10-19 根因确认与 Host-target 修复：同机 PASS

**原探针的映射选择有误，而不是现场驱动没有实现同机 CPU 共享 DRAM。**
`aclrtMemImportFromShareableHandleV2` 内部先获取当前 Device ID，再向 HAL 导入；因此得到的是面向当前 NPU 的导入。
给这个 handle 预约普通 VA、追加 HOST 权限，不能代替显式 Host-target import。
SDMA 使用 Device 视图可以通过，与 CPU 使用这条旧流程失败并不矛盾。

通过仅记录参数、原样调用真实函数的 HAL 透传日志确认：旧流程的 import 目标确实是预约 NPU，
映射后该 Device 权限已为 3，而 HOST 权限在 SetAccess 前为 0、之后为 3。
因此此前怀疑的“已有权限导致 SetAccess 跳过”不适用于本次失败的 peer。
GetAllocationProperties 返回 HOST 属性也不能作为导入目标的证据；实际调用参数才说明导入到了谁。

配对对照仅把 import 目标改为 `halGetHostID()` 的返回值，保留分配、token、普通映射、花纹、CPU 操作及同步：

| 同机 exporter-only / importer-only 对照 | DEFAULT | FABRIC |
| --- | --- | --- |
| ACL 默认 Device-target | READ/WRITE 校验失败 | READ/WRITE 校验失败 |
| HAL 显式 Host-target | READ、写回、完整 2 MiB 与 guard、清理通过；双端退出 0 | 相同通过 |

随后修复正式探针，在**没有透传插件**的任务中完整回归：两个同机进程、同一预约 NPU，
双方 `HOST_IMPORT target=HOST rc=0 ready=1`，HOST Set/Get 均为 READWRITE，
四组 64 KiB CPU READ/WRITE 全部通过，每组双方检查完整 2 MiB 及 guard，正常清理、退出码 0/0。
正式收集器生成 **`SINGLE_HOST_CPU_PASS`**，`hccs_route=NOT_MEASURED`；没有 stream、DMA、kernel 或数据回退。
收集器现要求 Host-target import 证据，不能把旧 Device-target 日志当成修复后的验收。

对应源码快照：

- Runtime `da5ed60f1deaa20f4905cc811b53db0960378cf7`：
  [ACL V2 import 读取当前 Device ID](https://github.com/hicann/runtime/blob/da5ed60f1deaa20f4905cc811b53db0960378cf7/src/acl/aclrt_impl/memory.cpp#L1132)，
  [Runtime 将目标 ID 传给驱动](https://github.com/hicann/runtime/blob/da5ed60f1deaa20f4905cc811b53db0960378cf7/src/runtime/feature/src/api_impl/api_impl.cc#L7161)。
- Driver `9fc724866dc6dd602236b56bd3130ae82f68e493`：
  [HAL import 显式接收目标并允许动态 Host ID](https://github.com/hicann/driver/blob/9fc724866dc6dd602236b56bd3130ae82f68e493/src/ascend_hal/svm/v2/devmm/devmm_svm.c#L5998)，
  [同机 Host import 复用共享 pages](https://github.com/hicann/driver/blob/9fc724866dc6dd602236b56bd3130ae82f68e493/src/sdk_driver/svm/v2/master/comm/svm_master_mem_share.c#L1049)，
  [跨服务器 Host-target import 的拒绝分支](https://github.com/hicann/driver/blob/9fc724866dc6dd602236b56bd3130ae82f68e493/src/sdk_driver/svm/v2/master/comm/svm_master_mem_share.c#L1412)。

现场 HAL 的函数布局、日志行号和访问调用链与旧 V2 源码对应；没有把公开 V3 直接当作现场实现。
公开快照不作为现场整个内核驱动逐字版本的替代；Host-target 成功以实测为依据。
原 Device-target 后追加 HOST 的内部映射失败细节没有逐条确认，但修正 import 目标已使同机能力通过。
**跨机 Host-target 仍未实测**；上述 V2 源码在远端 SDID＋Host 目标时明确拒绝。
不能从同机 PASS 推导 CPU 经 HCCS 跨机 load/store 已可用，也不能从该驱动限制推导硬件物理上不可能。
