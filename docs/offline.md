# 无公网环境的准备与部署

本仓库没有自动下载、pip 安装或远程 clone 步骤。
测试机之间需要可用的**内网控制连接和 HCCS/Fabric**；不需要访问公网。
默认假设 HIXL/CANN 的开发环境、g++、Python3、必要驱动已装好。

## 有网机器准备源码包

```bash
git clone https://github.com/Leaf-Salix/HIXL-TEST.git
cd HIXL-TEST
bash scripts/package.sh
```

`dist/` 中生成提交版本对应的 `.tar.gz` 和 `.sha256`。
包中只有已提交源码和文档，不会包含结果日志、凭据、私有配置或编译产物。
如有本地修改，先提交再打包；打包 HEAD 不会悄悄包含未提交修改。
也可以直接从 GitHub 下载指定 commit 的源码归档。

## 传入隔离环境

使用现场允许的介质或文件传输通道。
可以从有网工作站直接传到目标内网入口，不要求目标服务器反向连接公网：

```bash
rsync -avP --exclude '._*' --exclude '.DS_Store' \
  dist/HIXL-TEST-<commit>.tar.gz \
  dist/HIXL-TEST-<commit>.tar.gz.sha256 \
  user@internal-entry:/path/to/incoming/
```

在接收侧校验并解包（文件名替换为实际值）：

```bash
cd /path/to/incoming
sha256sum -c HIXL-TEST-<commit>.tar.gz.sha256
tar -xzf HIXL-TEST-<commit>.tar.gz
cd HIXL-TEST
```

将同一个包传给每个计算节点，不要传入另一次修改后的源码。
每台机器单独编译可以避免工具链、动态库路径不同造成的问题。

## 在离线节点编译

```bash
source /path/to/cann/set_env.sh
# 若 HIXL 不在 CANN 默认 include/lib64 中，显式设置：
export HIXL_INCLUDE_DIR=/path/to/hixl/include
export HIXL_LIB_DIR=/path/to/hixl/lib64
bash scripts/build.sh
```

编译脚本只调用本机编译器、SDK、Python 标准库与 ldd/nm，不触发网络获取。
没有 AscendC 编译器时可先 `bash scripts/build.sh --host-only`，运行 baseline/hixl。
没有开发头文件但只有运行库时，需先由管理员离线补齐相同版本 SDK；“已装 HIXL”运行包不一定含开发文件。

不要求安装 MPI。多机用各节点 `node` 命令启动即可。
若现场已经配置 MPI，可用 [多机说明](multinode.md) 的 `mpi-rank` 入口。

`combined` 需要可选实验库，其离线源码依赖另见 [实验桥说明](experimental.md)。
不想改动原版 HIXL 时，先完成 hixl/kernel 两类独立测试，保留各自的能力结论。

## 结果带出

结果中包含现场地址、路径、主机名和内存句柄，仅通过现场允许的通道带出。
保留原始目录用于 `scripts/collect.py` 校验；对外分享另外制作脱敏副本，不能把修改后的日志当原始证据。
不要提交 `results/`、`configs/site.json`、凭据文件或环境变量转储。
