# 压缩索引专项探针

这个独立目标会编译固定版基线中真实的 `CompressedLinePositionStorage` 实现 `src/logdata/src/compressedlinestorage.cpp`，并与上游的 `cpu_info.cpp`、`logger.cpp` 一起构建。它链接真实上游头文件和库：Qt 6.7.3 Core、CPM 缓存中的 `type_safe`、StreamVByte 1.0.0，以及 mimalloc 2.1.7。它不会复制或替换索引算法。构建配置为 x64 MSVC `RelWithDebInfo`。

在 PowerShell 中构建并运行全部专项案例：

```powershell
.\tools\build_index_probe.ps1
```

脚本支持 `-Case`（一个或多个案例名）、`-Jobs`、`-ConfigureOnly` 和 `-RunId`。每次运行记录在 `.local/evidence/index-only-baseline/<RunId>/`；每个案例的标准输出、标准错误和运行器摘要分别保存。每份运行摘要都包含可执行文件的 SHA-256。构建日志位于 `.local/logs`。此隔离目标下载的依赖放在被忽略的 `.local/index-deps` 下。

## 固定基线结果

运行元数据和精简案例摘要位于 `.local/evidence/m0-index-baseline/results.json`；原始子进程输出和进程摘要位于其 `runs/` 子目录。测试源码为快照提交 `25c7de6d8f6da2ce6a00882e5af70b4f331af4c5`，使用 MSVC 19.44.35211.0、Qt 6.7.3、CMake 3.29.2、Ninja 1.12.0、StreamVByte 1.0.0 和 mimalloc 2.1.7。

- 绝对偏移较高（`2^32 + 17`）、块跨度为 127 字节时，`at` 和 `range` 均返回全部 128 个预期位置；报告的 `allocatedSize()` 为 576 字节，实测操作时间为 337,500 ns。
- 块跨度为 `UINT32_MAX`、起点为 1 时，也返回全部 128 个预期位置；最后一个绝对位置为 4,294,967,296，而每个存储增量仍在 32 位范围内。报告的 `allocatedSize()` 为 576 字节，操作时间为 372,400 ns。
- 块跨度为 `2^32` 时，追加操作触发上游 `type_safe::narrow_cast<uint32_t>` 的前置条件失败。子进程以 Windows 状态码 3221226505 终止，未输出最终探针 JSON，因此没有该案例最终的探针内操作时间或 `allocatedSize()`。捕获的标准错误指明了失败的转换。
- `pop_back()` 后重新追加并压缩，返回了预期位置；但 `allocatedSize()` 从弹出前的 576 字节升至重新压缩后的 736 字节。当前打包大小记账仍保留已移除块的字节。
- 移动一个已填充的块后，再追加具有另一种合法 32 位增量模式的第二块，返回了错误数据：范围中首个不匹配位于索引 1，预期 101，实际为 33,818,740。进程本身以 0 退出；这是数据错误，不是崩溃。
- 对空存储执行零计数范围时，进程以 Windows 状态码 3221225477（访问冲突）终止，未输出最终探针 JSON。因此没有最终的探针内耗时或分配结果。

进程运行器每 20 ms 采样 RSS/专用内存，并记录进程运行至终止的时长。这些采样属于进程级观测，不能补足崩溃案例缺失的最终探针内数据。这是压缩索引组件的证据，不代表应用层 M0 验收、长文件行为或完整平台覆盖。

## 候选版本组件结果

候选版本仅修改 `upstream/klogg` 下压缩索引的两个实现文件；固定的 `.local/baseline` 快照保持原样。跨度不超过 `UINT32_MAX` 的块仍使用 StreamVByte；只有更宽的跨度才使用 uint64 增量侧存储。候选版本同时处理了零计数范围，完整转移移动状态，并在弹出打包块时保留 StreamVByte 文档规定的 16 字节读取填充。现在 `allocatedSize()` 报告打包/宽增量数据、元数据、标记和当前块缓冲区的 vector 容量。与基线按 `.size()` 计数的口径不同，这些数值只能分别按标注的口径比较。

候选版本在 `.local/evidence/index-only-candidate/final-candidate-cases/` 中通过了高绝对偏移/小跨度、`UINT32_MAX`、`2^32`、打包块弹出/重新压缩、移动追加、空存储零计数范围，以及窄/宽/窄混合生命周期案例。混合案例跨窄到宽和宽到窄边界读取 144 项范围，分别穿过窄、宽块执行弹出，重新压缩，将数据移动赋值到已填充的目标对象，重新使用已移出的对象，测试自移动，并重复宽块弹出/追加 1,000 次。期间结果始终一致，重复循环中的已分配容量保持稳定。这些案例使用的候选版本可执行文件 SHA-256 为 `7eb351b8ab25a307239d9ce38694b73c8914e1a4fbcc71f36a44a82bb49a256b0`。

分别从固定版基线和候选版本源码构建的同一 `normal-performance` 探针，各运行三次，追加并验证 1,000,000 个正常间隔的位置。基线追加循环时间为 6,558,700、6,371,300、9,145,500 ns；候选版本为 7,475,900、8,192,700、8,066,800 ns。六次运行都匹配预期值。这些短时本地运行波动较大，不构成通用性能结论。基线按旧的大小口径报告 1,375,312 字节；候选版本按容量口径报告 2,001,050 字节，因此分配数值不能直接比较。逐次 JSON 和进程采样位于 `.local/evidence/index-only-baseline/final-performance-baseline/` 和 `.local/evidence/index-only-candidate/final-performance-candidate/`。
