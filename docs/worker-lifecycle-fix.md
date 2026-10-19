# LogDataWorker 析构等待死锁定位

## 结论

在基线提交 `d1c1df6349a0e518a68dfd6c73c9b40347861caf` 上，四种索引操作生命周期场景都在约 15 秒后超时。抓取到的主线程栈停在 `QThreadPoolPrivate::waitForDone()`，索引工作线程同时停在 `RunnableWrapper` 执行路径内等待 SRW 锁。现有候选差异移除了三个 worker lambda 对 `operationsMutex_` 的获取；调用侧仍用该锁串行化请求，并在等待池任务完成后才安排下一项操作。`attachFile()` 新增 `waitForDone()`，以保证旧任务结束后再更换文件名。

索引修复单独验证时，454 步干净构建、八项 CTest、六项 worker 用例连续十轮、原有集成连续十轮和 39 项 Python 测试全部通过；最终与 UI 回调修复集成后的结果另行记录。

## 基线证据

原始运行记录位于 `.local/lifecycle-audit/original/summary.json`。二进制 SHA-256 为 `39f59215c062a0f869b11aa8b217b793731a0201430afb26bb4fa9a75211ed1d`。四个场景均在 15.031 秒触发超时并以记录器状态码 1 结束：

| 场景 | 峰值 RSS |
| --- | ---: |
| `destroy` | 20,516,864 bytes（约 19.56 MiB） |
| `partial` | 18,751,488 bytes（约 17.88 MiB） |
| `check` | 18,771,968 bytes（约 17.90 MiB） |
| `next` | 31,096,832 bytes（约 29.66 MiB） |

线程栈记录 `.local/lifecycle-audit/original-stack/threads.json` 显示，主线程从 `destroyImmediatelyAfterFullIndex` 进入 `LogDataWorker::~LogDataWorker()`，随后阻塞于 `QThreadPoolPrivate::waitForDone()`（析构函数第 196 行）。另一工作线程处于 `RunnableWrapper<LogDataWorker::indexAll...>::run`，并等待 SRW 锁。捕获时约 3 秒。该证据支持“析构等待任务结束，而任务无法越过锁等待”的死锁路径；它不是对所有运行环境或所有场景的概率估计。

## 锁与修复语义

`operationsPool_` 的最大线程数设为 1，池本身已保证任务按序执行。原实现中 `indexAll()`、`indexAdditionalLines()` 和 `checkFileChanges()` 的调用方在持有 `operationsMutex_` 时调用 `waitForDone()`；对应 worker lambda 又尝试获取同一把锁。若调用方正在等待前一个任务，任务却在获取调用方持有的锁，双方就无法前进。

当前候选差异在三个 worker lambda 中移除 `operationsMutex_` 获取，保留调用侧锁和各调用侧既有的 `waitForDone()`。这样锁只串行化调用者，池负责串行执行任务；worker 不再等待被调用方持有的锁。头文件注释也说明了这项约束。

`attachFile()` 同样持有调用侧互斥锁，并新增 `operationsPool_.waitForDone()`，再清除中断标记和更换 `fileName_`。这使切换文件仍等待正在运行的旧任务结束，避免任务读到不一致的文件状态。析构函数保留设置中断标记、持锁并等待池完成的流程；修复点是 worker 不再依赖析构等待期间由调用方持有的互斥锁。

相关候选位置：`upstream/klogg/src/logdata/src/logdataworker.cpp` 的析构、`attachFile()`、`indexAll()`、`indexAdditionalLines()` 和 `checkFileChanges()`；锁用途说明位于 `upstream/klogg/src/logdata/include/logdataworker.h` 的 `operationsPool_` 成员附近。

## 已排除的相邻疑点

- **IOCP 初始化失败**：公开 `efsw::FileWatcher` 的非 Generic 构造在 Win32 backend 初始化失败时会销毁该 backend 并回退到 Generic。因此直接构造一个初始化失败的 Win32 backend 后调用 `watch()`，不代表产品公开路径可达的回归场景。
- **文件监控包装层锁反转**：klogg 的 `EfswFileWatcher` 在持有自己的递归互斥锁时调用监控 API；反向的 Win32 回调在 IOCP 分发过程中排队到 Qt 线程，包装层之后才在 `findChangedFilename()` 中获取该锁。静态路径检查未发现可达的 `mutex_` 与 `mWatchesLock` ABBA。此结论是源码检查，不是运行时并发证明。
- **过滤对象/UI 生命周期**：本轮原始证据的阻塞栈指向 `LogDataWorker` 池任务与 `operationsMutex_`，没有显示过滤对象或 UI 生命周期路径；因此本报告不把这些相邻问题列为该死锁的根因，也不宣称它们已被全面排除。

## 验证边界与后续记录

上表和栈均属于指定基线二进制的原始诊断证据。索引修复阶段的八项 CTest 耗时 21.531 秒，六项 worker 用例连续十轮耗时 54.703 秒，原有集成连续十轮耗时 124.782 秒，均成功。六项 worker 用例包含五类各 100 次操作，以及核验两次成功通知、第二文件三行和精确字节数的正向用例。日志位于 `.local/lifecycle-audit/final-validation/`，它们早于本轮 UI 回调修复，不作为 UI 修复后的证据。

同轮另行复现并修复了窗口排队回调的释放后访问，见[UI 回调生命周期报告](ui-callback-lifecycle-fix.md)。两种根因分别记入 Issue #5 和 #6；最终集成验收见本轮证据目录和 PR 发布记录。通过这些测试不代表其他平台或全应用 sanitizer 验收。
