# 窗口销毁后的排队回调

真实 `CrawlerWidget` 在样式变化或自动搜索请求排队后立即关闭，排空事件队列时均复现 `0xC0000005`。保持窗口存活的自动搜索用例通过。独立调试栈显示，已销毁窗口的 `changeEvent` 回调仍经 `QEventDispatcherWin32` 执行，随后在 `loadIcons → IconLoader::shouldInvert → QObject::parent` 访问失效对象。

这组原始 UI 复现保持 `d1c1df6` 的 UI 源码不变，但使用已修复索引死锁的 worker，以避免两种故障互相遮蔽；它不是完全未修改的整仓基线。两次普通运行分别在 7.453 秒、6.922 秒退出，峰值进程 RSS 采样为 50,900,992 和 50,958,336 bytes；耗时包含进程退出处理，不能视为回调执行时间。调试器在另一进程约 0.61 秒捕获首次访问冲突。精简记录见 `evidence/windows-2026-10-18-worker/ui-original-validation.json` 和 `ui-original-stack.json`，完整日志在 `.local/lifecycle-audit/`。

## 修复

`dispatchToMainThread` 把函数投递给全局事件分发器，无法随捕获对象销毁而取消。现有 `dispatchToObject` 使用相同的 `Qt::QueuedConnection`，但接收对象由调用者指定。Qt 在对象销毁时移除发给该对象的待处理调用。

本轮将六个 UI 实现文件中的 20 处此类回调绑定到实际被访问的对象：

- CrawlerWidget 和 TabbedCrawlerWidget 的样式更新、构造图标及自动搜索。
- HighlightersDialog、HighlighterSetEdit 和 PredefinedFiltersDialog 的构造图标、删除、排序和选择更新。
- MainWindow 的样式/托盘更新，以及捕获 crawler、编辑框的两处子对象操作；后两者分别使用子对象作为接收者。

这些对象均属于 GUI 线程，仍按队列执行，不改为同步调用。其他不捕获窗口对象的后台错误提示和进程级文件监控分发不在本次替换范围。

## 回归与边界

`tests/native/crawler_tests.cpp` 新增两种真实窗口立即关闭及一种存活自动搜索用例；`queued_ui_lifecycle_tests.cpp` 覆盖四类轻量 UI 对象的构造后销毁、标签页样式变化，以及存活编辑器的排队图标初始化。测试在销毁后处理事件队列，使用 QPointer 验证对象已删除；正向测试同时确认功能没有被简单禁用。

MainWindow 托盘路径、所有排序组合和 macOS 选择行为没有独立运行时覆盖；相同接收对象修复经过源码审查并参与 Windows 整体编译。本轮不声称完成全应用 sanitizer 或其他平台验收。最终候选结果与 PR / 主分支 CI 链接由对应发布记录给出。

连续复跑发现一个已有关闭测试的前置条件缺失：第十轮 `REQUIRE_FALSE(isPreviewReady())` 失败，且仅加入线程池占用仍然失败。追加诊断确认 `linePreview(..., 0, 1)` 返回不支持当前编码，`showByteWindow()` 在清理旧预览前返回；旧预览仍就绪，并非已证明新预览提前完成。纯 ASCII 样本的自动检测不保证显示编码为 UTF-8。测试现显式选择 UTF-8 并断言元数据没有错误，同时占用真实全局线程池以保证有效请求排队，然后释放占用并立即关闭窗口。RAII 在断言失败时也释放占用并恢复线程数。中间失败保留于 `intermediate-preview-test-failure.json`。

最终本地验证：455 步生产源码干净构建通过；测试前置条件修正后重建 Crawler 目标，八项 CTest 通过，Crawler 全套 11 个用例 / 131 个断言连续二十轮通过；worker 六用例连续十轮、原有集成连续十轮、39 项 Python 测试均通过。精简记录见 `candidate-validation.json`。
