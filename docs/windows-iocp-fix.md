# Windows IOCP 生命周期修复

本次修复跟踪 [问题 #3](https://github.com/Die4U23/klogg-longline/issues/3)，起点是首版合并后的 [主分支失败记录](https://github.com/Die4U23/klogg-longline/actions/runs/39092237442)。首版 PR 的检查通过并未排除间歇性故障；后续检查暴露了真实内存破坏。

## 现象与独立复现

未修改的集成测试程序曾在 `QThreadPool::reset → QThread::isRunning` 崩溃；后续不挂调试器的完整测试前 8 轮通过，第 9 轮在搜索测试回收对象期间以 `0xC0000005` 退出。诊断进程还在其他 Qt 对象中遇到同类失效指针，其中低 32 位为 `0xC0000120`。

依赖 efsw 1.4.1 的 `DestroyWatch()` 调用 `CancelIoEx()` 后立即释放 `OVERLAPPED`。但取消请求并不表示 I/O 已完成，且 IOCP 状态块的更新与完成包出队有关，存储必须保留到完成处理结束。[取消 API 约定](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)、[IOCP 约定](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)。

独立诊断只链接原始 efsw 静态库，拦截目标监控结构的 `delete` 并暂留其 40 字节存储，以便安全观察迟到写入。5 次独立进程均在第一次迭代复现：删除时 `Internal=0x103`，取出取消完成包后变为 `0xC0000120`。每次进程耗时约 8—14 ms。这里 `exit 1` 表示诊断成功观察到了缺陷，不是候选测试通过。

复现源码、输出和原始集成失败汇总保存在 [本次证据目录](../evidence/windows-2026-10-18-iocp/)。完整内存转储仅留在忽略目录 `.local/`，不进入仓库。

## 修补范围

- 取消时把监控项移入等待回收集合；保留目录句柄、通知 buffer 和 `OVERLAPPED`，直到唯一完成包被消费。取消包即使令 `GetQueuedCompletionStatus()` 返回 `FALSE`，只要 `OVERLAPPED` 非空，仍需处理和回收。
- 整体析构按取消、排空、等待工作线程退出、关闭 IOCP 的顺序执行。未调用 `watch()` 的实例在析构时同步排空。
- 每个监控项最多有一次待完成请求；零字节完成只重挂一次，不解析旧 buffer。回调内移除自身时，存储保留到回调返回。
- 修复同一 Windows 实现中创建失败时删除错误指针导致的 buffer 泄漏。

修改只覆盖 efsw 的四个 Windows 内部文件。CMake 校验原始版本和文件哈希后替换目标源文件；固定依赖缓存、历史基线及其他平台实现未修改。版权和 MIT 许可随覆盖源码保留。审查用差异在 [补丁](../patches/efsw-win32-iocp.patch)，来源记录在 [清单](../patches/efsw-win32-iocp.json)。

## 回归与证据边界

`klogg_efsw_lifecycle_tests` 使用真实 Windows 目录通知和 IOCP，覆盖文件变化与取消竞态、出队后尚未分发时删除、取消完成包、零字节分发、未启动线程析构、回调内自移除、运行中析构和连续添加/删除。零字节场景是在消费真实完成包后向生产分发函数传入零字节，验证该分支；不声称制造了操作系统通知缓冲区溢出。

Windows CI 要求七项 CTest 全部通过，再对原有整套集成测试连续执行 10 轮，任一轮失败即失败。PR 的当前提交和合并后的 `main` 都必须通过这套检查。

2026-10-17 的 5 GiB、取消延迟、GUI 心跳和普通日志性能记录仍是历史源码的测量，本次不把它们改写成新版本重新测得的结果。旧源码、补丁和证据哈希保持不变，当前源码通过 `patches/followups.json` 的显式前后哈希链核验。诊断构建的 ASan 未覆盖完整 Qt/操作系统 I/O 写入，不能据其未报错否认这次缺陷。

本地最终结果：完整重编译后七项 CTest 通过；新增 8 个 IOCP 用例、216 条断言通过，连续 100 轮通过；原有 8 个集成场景、959 条断言连续 20 轮通过（约 254.4 秒）；39 项 Python 测试与来源校验通过。摘要和二进制哈希见 `evidence/windows-2026-10-18-iocp/candidate-validation.json`。
