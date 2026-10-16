# 同窗口联合测试与可选实验桥

`hixl` 和 `kernel` 模式可分别使用原版 SDK 能力，但只有 `combined` 才验证：
**同一块物理 DRAM/HBM 同时供 HIXL 和业务 AIV kernel 使用，并正确完成交接。**

如果目标是只用系统 CANN 验证 CPU/NPU 对同一组窗口的访问与交接，使用
[`--layout node-shared --mode acl`](node-shared.md)，不需要本节补丁。
该入口使用 ACL 拷贝＋AIV，不能用其结果替代本节的 HIXL API 联合验证。

## 为什么不是“装好 HIXL 就能跑 combined”

原单机环境使用 HIXL v9.1.0-beta.2。该版本没有公开取得内部已导出 Fabric token 的接口。
同一物理 allocation 被 kernel 提前 export，再交给原版 HIXL RegisterMem，会再次 export 而失败。
实验桥解决的是唯一导出和生命周期，不是替 HIXL 实现一种新的跨机传输协议。

它提供两个**非官方**入口：

```cpp
uint32_t HixlDomainWindowAcquire(void *addr, size_t len, aclrtMemFabricHandle *out);
uint32_t HixlDomainWindowRelease(void *addr);
```

Acquire 对完整、独立的原始 allocation retain/export 一次；重复调用返回相同 token。
HIXL RegisterMem 复用该 token 并计数；仍被 HIXL 注册时 Release 必须拒绝。
不支持子窗口、别名、并发释放或在同一物理 allocation 上释放后再次 export。
所有 importer 撤销后才允许 owner Release，最后释放物理内存。

补丁另含旧版本配置读取兼容：配置存在但不可读时走原有公开 hccn_tool 查询。
不修改系统配置权限、不伪造设备 IP、不提升执行身份。
这与共享窗口功能是两个独立问题；新版 SDK 可能已经修复，不应直接硬套旧补丁。

## 适用源码与许可证

- 上游：<https://gitcode.com/cann/hixl>
- tag：`v9.1.0-beta.2`
- commit：`2b7a38e6052ce92413141158453a7e3a9c929f7f`
- 补丁：`patches/hixl-v9.1.0-beta.2-domain.patch`
- 上游代码片段按 `patches/CANN-LICENSE` 分发，保留原版权声明。

补丁脚本会核对被修改原始文件的 SHA256；不匹配或已应用时直接失败。
HIXL 最新主线已经演进到新的 ubmem 后端/导出管理，不能用此 beta2 补丁证明新版适配完成。

## 离线准备

正常 hixl/kernel 测试不需要 HIXL 源码。以下步骤只给希望复现旧版实验桥的人使用。
不要对系统安装目录或正在使用的共享源码应用补丁；创建独立源码副本。

在允许联网的准备机获取指定版本源码，并准备好上游构建所需的已展开第三方依赖：

| 在 `CANN_3RD_LIB_PATH` 下的目录 | 版本/来源 |
| --- | --- |
| `cann-cmake/` | CANN cmake `master-006`，含 `function/prepare.cmake` |
| `pybind11/` | 2.13.6，含 `include/pybind11/pybind11.h` |
| `json/` | nlohmann json 3.11.3，含 `include/nlohmann/json.hpp` |
| `makeself/` | 2.5.0-patch1，含 `makeself.sh`、`makeself-header.sh` |

精确来源见该 tag 的 `cmake/fetch_cann_cmake.cmake` 与 `cmake/third_party/*.cmake`。
这些依赖不打包在本测试仓库；按各自许可证离线带入，测试服务器不执行下载。
还需本机 CMake、make、Python 开发头文件及已安装 CANN 开发包。

## 应用并构建隔离库

```bash
# HIXL_SRC 是指定 tag 的独立源码副本，依赖已在 third_party 下展开。
export HIXL_SRC=/path/to/isolated/hixl-v9.1.0-beta.2
python3 scripts/apply_domain_patch.py "$HIXL_SRC" --check
python3 scripts/apply_domain_patch.py "$HIXL_SRC"

source /path/to/cann/set_env.sh
# 也可将下面路径指向单独的离线依赖目录。
export CANN_3RD_LIB_PATH="$HIXL_SRC/third_party"
bash scripts/build_domain.sh "$HIXL_SRC"
```

脚本在 CMake 前检查展开的依赖，缺失就退出，不靠下载补齐。
只构建 `cann_hixl` / `llm_datadist` 两个 target，将库放在测试仓库的 `build/domain/lib`；
不会 `make install`，也不会运行 CANN 安装器或覆盖系统库。
若新 CMake/工具链或依赖组合导致失败，请保留构建日志，不能记为数据面验证失败。

## 运行

```bash
export HIXL_LIB_DIR="$PWD/build/domain/lib"
bash scripts/rank.sh single --devices 0,1 --mode combined
```

多机每台都准备相同实验库，以同样的 `HIXL_LIB_DIR` 启动 `node ... --mode combined`。
检查 manifest 的 `selected_libraries` 确实指向实验库；不要仅凭目录变量判断已生效。
程序会检查 Acquire/Release 符号、重复 Acquire、注册中释放拒绝和重复 Release 拒绝。

这个适配是验证代码，不是已集成进 simpler 的实现。
单机通过不能替代多机验证，也不覆盖故障恢复、权限隔离设计或驱动资源泄漏审计。
