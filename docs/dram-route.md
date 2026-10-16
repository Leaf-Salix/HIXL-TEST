# DRAM_A → DRAM_B：驱动路径查询与 PCIe/HCCS 采样

这个独立探针只测 **A 的 CPU DRAM → B 的 CPU DRAM**。
A 的 Host 调用 ACL，由 A 选定 NPU 的 SDMA 发起传输；B 被动持有目标 DRAM。
它复用现有 `acl` 模式的 Fabric 设备 VA 映射方法，但不运行完整矩阵、AIV kernel、HIXL engine，
也不分配或初始化 HBM，不经过应用层 HBM staging。只依赖系统 CANN、驱动和 Python 标准库。

每端只有一块 4 MiB P2P Host DRAM；传输默认 1 MiB，A 源偏移 256 B，B 目标偏移 2 MiB。
原始 CPU 映射、导出 token 和 SDMA 设备 VA 都引用同一实际 allocation。
使用 `aclrtReserveMemAddressNoUCMemory` 预约的 1 GiB 是 **VA**，不是另分配的物理内存。
自导入别名与指定 VA 的 SDK 版本限制沿用 [node-shared 说明](node-shared.md)。

本探针回答“这次 ACL/Fabric DRAM 拷贝对应哪些驱动分类、哪些端口流量”。
不能直接把其结果写成 HIXL API 路径实测，也不验证 CPU 原生 load/store 解引用远端指针。
两机仍需位于同一已配置的 HCCS/Fabric 超节点；普通以太网/RoCE 连通不够。

## 采样边界与证据

| 步骤 | 采样开关 | 操作 |
| --- | --- | --- |
| 初始化 | OFF | ACL/device、分配、export/import/map、stream；`npu-smi info`、`ldd` |
| 驱动查询 | OFF | 在 A 的实际拷贝上下文，向 `drvDeviceGetTransWay(src,dst,&way)` 传入实际源/目标设备 VA，记录返回码、原始值 |
| 预热 | OFF | 32 次相同 DRAM 拷贝，等待完成，CPU 校验两端完整窗口 |
| 每轮准备 | OFF | 两端填充本轮独立花纹；准备 profiler、配置 50Hz 互连采样 |
| idle | ON | 两端 start 后握手，前后各静默 1 秒；中间 A 等待 `idle-ms`，不提交拷贝 |
| n | ON | 两端 start 后握手，静默 1 秒；A 提交 N 次拷贝，每 32 次同步，最终同步完成；静默 1 秒，双方 stop |
| 2n | ON | 同上，提交 2N 次 |
| 每轮校验 | OFF | **双方 profiler 都已 stop/finalize 后**，CPU 逐字节检查两端完整 4 MiB，包括 guard |
| 导出、释放 | OFF | 先撤销所有进口映射，再释放 owner；整个 run 结束后才执行 msprof 导出 |

`aclprofSetConfig(ACL_PROF_SYS_INTERCONNECTION_FREQ,"50",2)` 配合 `aclprofStart/Stop`，
采集所选设备的 PCIe/HCCS（平台还可能同时导出 SIO 等）计数器。
初始化、预热、花纹填充、CPU 校验不进入任何采样窗口。
窗口内只留 SDMA 提交/完成、固定静默时间和少量元数据 TCP 握手，TCP 不传测试内容。
控制面和其他作业的端口背景仍可能被计数，因此必须保留 idle 对照。
计数器是设备/端口聚合值，不是进程独占归因；尽量在双方预约的卡及相关链路都空闲时运行。

每轮记录实际完成的逻辑拷贝次数和字节数，以及本机 monotonic 时间。
时间戳用于检查边界，**不能直接与另一台机器或 msprof timeline 的时间轴相减**。
同轮重复拷贝使用同一个源和目标区间，最后验证数据；不声称每一次重复都换花纹或逐次回读。
N→2N 增加的是总提交字节数，**平均带宽不应被要求翻倍**；看持续时间、timeline 与背景一起判断。
这不是无 profiler 开销的性能 benchmark。

## A、B 两台完整命令

每端只需预约 **一张 NPU**。以下命令在两端各自获准的设备环境内执行；不要在共享服务器裸跑。
文档里的路径/IP、设备逻辑 ID、NUMA 必须按现场替换。
在 hwServer 用 `task-submit` 包裹 `run` 和 `export`；不要把后台进程跑出预约任务。
`export` 在某些部署上也需要队列权限才能初始化驱动。
若调度器重映射可见设备，传预约环境内的 ACL 逻辑 ID，核对日志 `DEVICE` 的 physical ID。

### 1. A、B 分别使用同一源码包编译

**A：**

```bash
export TEST_ROOT=/path/to/HIXL-TEST
export CANN_ENV=/usr/local/Ascend/cann-9.0.0/set_env.sh
export PYTHONNOUSERSITE=1
export PYTHON_BIN=python3  # 现场使用指定 venv 时改为它的绝对路径
cd "$TEST_ROOT"
source "$CANN_ENV"
bash scripts/build_route.sh
```

**B：**

```bash
export TEST_ROOT=/path/to/HIXL-TEST
export CANN_ENV=/usr/local/Ascend/cann-9.0.0/set_env.sh
export PYTHONNOUSERSITE=1
export PYTHON_BIN=python3
cd "$TEST_ROOT"
source "$CANN_ENV"
bash scripts/build_route.sh
```

只编译这个探针即可，不用先 `build.sh`、不用 bisheng、GitCode HIXL、MPI 或 pip install。
`ldd build/dram_route` 中 ACL、profiler 应来自系统 CANN；驱动库由系统 CANN 引入。
编译阶段保存源码与 binary SHA256；启动会拒绝过期 binary。两端源码/测试参数不一致也会拒绝。

### 2. 在 A 确定本轮参数，原样传给 B

**A：** IP 是文档保留地址；把 `COORDINATOR` 改为 B 能访问的 A 地址和空闲 TCP 端口。
RUN_ID 每次重跑换新；两端使用完全相同的 RUN_ID、COORDINATOR、BYTES、ITERATIONS、IDLE_MS。
以下命令可在无公网环境执行。

```bash
cd "$TEST_ROOT"
cat > configs/route.env <<'ENV'
export COORDINATOR=198.51.100.10:39500
export RUN_ID=dram-route-20261016-run01
export BYTES=1048576
export ITERATIONS=32768
export IDLE_MS=3000
ENV
"${EDITOR:-vi}" configs/route.env
export B_SSH=user@node-b-hostname
export B_TEST_ROOT=/path/to/HIXL-TEST
rsync -av --exclude '._*' --exclude '.DS_Store' \
  configs/route.env "$B_SSH:$B_TEST_ROOT/configs/route.env"
source configs/route.env
```

**B：**

```bash
cd "$TEST_ROOT"
source configs/route.env
```

没有 SSH 时用现场允许的离线渠道复制这个小文件。`.env` 和结果目录均被 Git 忽略。
默认 N=32768、1 MiB，即 N 轮逻辑 32 GiB、2N 轮 64 GiB；只重复操作小窗口，不存储大张量。
若 payload 很短、采样点太少，可两端一起增大 N，必要时增加 `--timeout` 和预约时限。

### 3. 在两个终端启动，A 等待 B 加入

以下两组 `run` 要在两端预约任务内并行启动，启动间隔小于 300 秒。
选择与原先通过的 Fabric DRAM 测试一致的设备和 NUMA。
每个输出子目录必须不存在；不要复用旧结果目录。

**A：**

```bash
cd "$TEST_ROOT"
source "$CANN_ENV"
source configs/route.env
export RUN_A="$TEST_ROOT/results/$RUN_ID-A"
"$PYTHON_BIN" scripts/route.py run \
  --rank 0 --device 0 --numa 0 \
  --coordinator "$COORDINATOR" --run-id "$RUN_ID" \
  --bytes "$BYTES" --iterations "$ITERATIONS" --idle-ms "$IDLE_MS" \
  --timeout 900 --output "$RUN_A"
```

**B：**

```bash
cd "$TEST_ROOT"
source "$CANN_ENV"
source configs/route.env
export RUN_B="$TEST_ROOT/results/$RUN_ID-B"
"$PYTHON_BIN" scripts/route.py run \
  --rank 1 --device 0 --numa 0 \
  --coordinator "$COORDINATOR" --run-id "$RUN_ID" \
  --bytes "$BYTES" --iterations "$ITERATIONS" --idle-ms "$IDLE_MS" \
  --timeout 900 --output "$RUN_B"
```

脚本结束后打印 `WARMUP_PASS`、三个 `PHASE_PASS`、`ROUTE_RANK_PASS ... cleanup=PASS`。
任意数据、guard、同步、释放错误或进程超时都会使 `run` 非零退出；保留失败日志。
驱动查询返回不支持会记录 `UNAVAILABLE`，不妨碍独立验证数据，但路径证据不完整。

**hwServer 的 task-submit 入口（代替上面的直接 run 和下一节的 export）：**
两端分别执行下面命令；脚本在同一个预约任务内运行该 rank，双方全部采样停止后自动导出。
`{}` 由调度器替换为预约设备 ID；下面假定现场此 ID 可直接作为 ACL 逻辑 ID，
有可见设备重映射的部署须按其规则修改。末尾 `0` 是 DRAM NUMA ID，应按现场替换。
两端都须完成前述编译和 `configs/route.env` 配置，源码包位于各自 `TEST_ROOT`。

**A：**

```bash
cd "$TEST_ROOT"
task-submit --device auto --max-time 1800 \
  --env PYTHONNOUSERSITE=1 --env "CANN_ENV=$CANN_ENV" --env "PYTHON_BIN=$PYTHON_BIN" \
  "bash '$TEST_ROOT/scripts/route_node.sh' 0 {} 0"
# 记录返回的任务 ID；用 task-submit --wait <任务ID> 查看结果。
```

**B：**

```bash
cd "$TEST_ROOT"
task-submit --device auto --max-time 1800 \
  --env PYTHONNOUSERSITE=1 --env "CANN_ENV=$CANN_ENV" --env "PYTHON_BIN=$PYTHON_BIN" \
  "bash '$TEST_ROOT/scripts/route_node.sh' 1 {} 0"
```

两端均应等待任务成功，再执行第 5 节汇总。此探针不导入 PyPTO/simpler，不需要其 Python 包或编译变量。

### 4. 双方 run 都结束后再导出

不要在另一端还在采样时执行 `msprof --export`。下面同样在允许 profiler 使用设备的预约环境中运行。
`export` 对 idle/n/2n 分别调用系统 `msprof --export=on --output=...`；不重新运行测试。

**A：**

```bash
cd "$TEST_ROOT"
source "$CANN_ENV"
source configs/route.env
export RUN_A="$TEST_ROOT/results/$RUN_ID-A"
"$PYTHON_BIN" scripts/route.py export --output "$RUN_A"
```

**B：**

```bash
cd "$TEST_ROOT"
source "$CANN_ENV"
source configs/route.env
export RUN_B="$TEST_ROOT/results/$RUN_ID-B"
"$PYTHON_BIN" scripts/route.py export --output "$RUN_B"
```

### 5. 在 A 汇总双方完整原始结果

**A：**

```bash
cd "$TEST_ROOT"
source configs/route.env
export RUN_A="$TEST_ROOT/results/$RUN_ID-A"
export B_SSH=user@node-b-hostname
export B_TEST_ROOT=/path/to/HIXL-TEST
export B_COPY="$TEST_ROOT/results/$RUN_ID-B-collected"
mkdir -p "$B_COPY"
rsync -av --exclude '._*' --exclude '.DS_Store' \
  "$B_SSH:$B_TEST_ROOT/results/$RUN_ID-B/" "$B_COPY/"
"$PYTHON_BIN" scripts/route.py collect "$RUN_A" "$B_COPY" \
  --output "$TEST_ROOT/results/$RUN_ID-summary.json"
cat "$TEST_ROOT/results/$RUN_ID-summary.json"
tar -czf "$TEST_ROOT/results/$RUN_ID-evidence.tar.gz" -C "$TEST_ROOT/results" \
  "$RUN_ID-A" "$RUN_ID-B-collected" "$RUN_ID-summary.json"
```

需要分析时保留整个 evidence 包。原始 Fabric handle 不打印；日志包含 hostname、地址和现场库路径，
属于现场证据，不应直接提交到公开仓库。两台同 hostname 会保守拒绝双机通过，先修正现场命名。

## 如何判读

- `transfer_status=TWO_HOST_PASS`：两台不同 hostname 的本次匹配运行完成拷贝、guard 和释放。
  单机开发验证必须加 `collect --allow-single-host`，只会报告 `SINGLE_HOST_PASS`。
- `counter_status=AVAILABLE`：两端三个阶段都导出了可识别的 PCIe/HCCS 带宽 CSV。
  缺文件、导出失败、未知列、缺 Tx/Rx 类别、NaN 都是 `INCONCLUSIVE`；**不是零流量**。
  这个状态不代表计数器已完成因果归因或证明物理路径。
- `route_conclusion=REQUIRES_ANALYSIS` 始终保留：对照 A/B 的驱动分类与 idle/n/2n 的原始 timeline。
  汇总文件列出带宽 min/max/average、单位 MB/s 及减去 idle average 的差值；它不是累计字节计数。
  不把负差值截成零，不把 PCIe completion、posted、non-posted 或 Tx/Rx 重复相加冒充有效载荷。
- A 的 `DRIVER_QUERY` 针对实际 `aclrtMemcpyAsync` 源/目标设备 VA。
  原始值按实测 CANN 9.0 的 runtime enum 解读：0=INNER、1=PCIe、2=HCCs、3=UB。
  查询是驱动对该地址对的传输分类，**不是完整逐跳链路**；INNER/SDMA 不能推导为“没有 HCCS”。
  不支持或未知值保留原样，换 SDK 时应重新核对 enum。
- PCIe 与 HCCS 可能同时是某条端到端路径的不同段；一端某计数器没有增量不能单独排除那种链路。
  稳定的载荷相关增量、持续时间变化、两端对应关系和驱动分类共同构成证据。
  空闲时计数器已有大流量、速率明显异常或背景变化大时，结论仍应保持不确定。

每端 `idle/`、`n/`、`2n/` 下保留原始 `PROF_*`，导出后有
`mindstudio_profiler_output/{hccs_*.csv,pcie_*.csv,msprof_*.json}`。
可以用本地 timeline 查看器查看 `msprof_*.json` 的 HCCS/PCIe 层级，核对采样覆盖与有效样本数。
当前自动解析器针对实测 CANN 9.0 的 CSV schema；新版本格式变化会明确报证据不足，原始数据仍保留。

接口与数据格式参考：[CANN profiling API 采集说明](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/850alpha002/devaids/Profiling/atlasprofiling_16_0101.html)、
[CANN 互连数据导出字段](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/83RC1alpha001/devaids/Profiling/atlasprofiling_16_1144.html)、
[MindStudio PCIe/HCCS 数据格式](https://gitcode.com/Ascend/msprof/blob/1f7d504101bc2eca627409507115a6f86d44b306/docs/zh/profile_data_file_references.md)。

## 验证记录

2026-10-16，在单台 Atlas A3、两张独立物理 NPU、系统 CANN 9.0 上，经 task-submit 实测通过：

| 检查 | 结果 |
| --- | --- |
| DRAM-only 编译/运行 | 仅系统 ACL、profiler、驱动；没有 HBM allocation 或 HIXL engine |
| 预热 | 32 次后，两端完整窗口和 guard 通过 |
| idle / N / 2N | N=4096、1 MiB，分别为 0 / 4 GiB / 8 GiB；两端数据、guard、清理通过 |
| 两端各三个 profile | PCIe/HCCS CSV 与 timeline 成功导出；汇总 `SINGLE_HOST_PASS counter_status=AVAILABLE` |
| 实际地址驱动查询 | rc=0、raw=0、INNER；没有据此宣布 PCIe/HCCS 逐跳路径 |

原始采样包含空闲阶段已有的 HCCS 背景值，因此没有把单机端口计数当作跨机路径结论。
本探针的双机采样尚待用户运行；之前完整矩阵的 `GLOBAL_PASS` 不替代本次路径验证。
