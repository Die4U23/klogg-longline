# 索引任务退出死锁的复核记录

原始版本为 `d1c1df6349a0e518a68dfd6c73c9b40347861caf`。只新增测试目标、未修改生产代码时，四个公开 API 场景分别在 15 秒超时；独立抓栈确认调用线程持锁等待线程池，而工作线程等待同一把锁。

- `original-validation.json`：原始二进制哈希、四次运行耗时、超时与进程 RSS 采样。退出码来自超时后的终止，不是应用返回的失败断言。
- `original-stacks.json`：另一次运行约 3 秒时抓取的两个相关线程栈摘要，源码行号对应原始版本。
- 回归源码位于 `tests/native/worker_lifecycle_tests.cpp`，使用真实临时文件及公开 API，没有替换调度器或强制线程执行顺序。

复现原始问题时，在上述原始提交上加入本轮的测试文件和 `tests/native/CMakeLists.txt` 中对应的目标定义，保持生产源码不变，按构建手册准备依赖并构建：

```powershell
./tools/build.ps1 -Variant candidate -Targets klogg_worker_lifecycle_tests
ctest --test-dir .local/build-candidate -R '^klogg_worker_lifecycle_tests$' --output-on-failure
```

修复后的相同目标应在 CTest 的 60 秒超时内完成。首轮原始诊断仅包含前四个用例，每个最多重复 100 次；后来增加的公开 `LogData` 立即关闭和双文件重新绑定用例不属于那四次原始测量。调度具有平台差异，原始四次超时不是对所有机器故障概率的估计。

完整原始日志、原始 EXE/PDB、抓栈脚本及构建输出保留在忽略目录 `.local/lifecycle-audit/`。本目录只保存精简可核验记录；旧 IOCP 修复和 2026-10-17 性能证据保持原始归属。

## 同轮 UI 诊断与最终集成

`ui-original-validation.json`、`ui-original-stack.json` 保留原始 UI 配合独立 worker 修复的诊断组合。`intermediate-preview-test-failure.json` 保留测试前置条件失效及定位过程。`candidate-validation.json` 记录最终八项 CTest、Crawler 连续二十轮、worker 与原有集成各十轮、39 项 Python 测试、二进制及测试源码哈希。最终生产源码经过 455 步干净构建，之后仅重建修正过前置条件的 Crawler 测试目标。详细根因见两份生命周期报告，发布 CI 结果以对应 PR 为准。
