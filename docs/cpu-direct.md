# CPU 原生访问远端 DRAM：源码结论与能力探针

截至 2026-10-16，**没有在已核查的公开实现中找到受支持的 CPU 原生 load/store 经 HCCS 访问另一台服务器 DRAM 的路径**。
公开驱动实现还明确拒绝 Host 页表映射远端服务器的物理内存。
CPU 接入 HCCS Fabric、远端 DRAM 可由 NPU SDMA 访问，并不等于 Host 用户态能直接解引用远端地址。

本页新增的是**能力探针**，用于核对现场实际安装的 SDK/驱动；不是已经证明支持的跨机传输例程。
只有两端成功建立并查询到 `HOST READWRITE` 权限后，才执行真正的 CPU 原生读写。
不支持则报告 `UNSUPPORTED`，不回退 SDMA、MTE、RDMA 或 TCP 搬运。

## 已核查的实现

| 层次 | 证据 | 对本问题的含义 |
| --- | --- | --- |
| HIXL FabricMem | 已核查版本 `FabricMemTransferService::ProcessCopyWithAsync()` 调用 D2D `aclrtMemcpyAsync` | Host API 发起不等于 Host CPU 执行数据搬运 |
| Runtime VMM | `aclrtMemSetAccessImpl` → `rtMemSetAccess`；`NpuDriver::MemSetAccess` → `halMemSetAccess` | 仅拥有 Fabric token 或 Device VA，不能代替 Host 访问权限 |
| driver VMM | `vmm_dir_check()` 在访问方为本地 Host、源物理内存的 server_id 属于另一台服务器时返回 `DRV_ERROR_NOT_SUPPORT` | 明确禁止该公开实现中的 Host→远端 Host/Device 映射 |
| driver import / set-access | `vmm_import()` 与 `vmm_set_access_check()` 都会调用上面的方向检查 | 先按 Device 导入、再追加 Host 权限也受相同限制 |
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

每机分配一个 2 MiB P2P Host DRAM allocation，交换 Fabric handle 并导入对端 allocation。
使用普通 `aclrtReserveMemAddress` / `aclrtMapMem`，随后显式调用：

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

不需要 HIXL GitCode 源码、bisheng、MPI 或 Python 第三方包。只显式链接系统 `libascendcl`。
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
| `UNSUPPORTED` / 3 | 至少一端 SetAccess 或 GetAccess 明确返回 FEATURE_NOT_SUPPORT；双方都未进入 CPU 数据阶段，正常清理；不是测试通过 |
| `FAIL` 或其它非零 | 分配/import/map、参数/权限、数据/guard、CPU fault、清理、超时或配置不一致；不能简单当成接口不支持 |

`UNSUPPORTED` 会让 task-submit 显示非零退出（3），这是能力探测的预期负结果。
同一轮不支持时双方都应产生 `CPU_DIRECT_RESULT ... status=UNSUPPORTED cleanup=PASS fallback=NONE`。
如果已有导入映射，但新增 HOST 权限被拒，仍不对该地址执行 CPU 读写。

## 实测边界

2026-10-16 已使用系统 CANN 9.0 编译本探针，并核对 binary 未引用 memcpy、stream 创建或 kernel 启动 API。
源码/格式检查、47 项仓库 UT 和独立实现审查通过。

### 同机双进程硬件对照：FAIL

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
日志没有记录该字节的实际值，尚不能据此确定映射、缓存可见性或探针实现中的具体根因。

本版本尚无通过的同机 CPU 数据访问验收结果；上面的双机命令保留为实验排障入口，不能当作已验证可用流程。
双机 CPU 原生访问尚未实测；公开实现的拒绝分支不替代你现场安装版本的运行结果。
本次同机失败也不能证明 CPU 没有物理接入 HCCS，或证明硬件不可能提供跨机访问能力。
