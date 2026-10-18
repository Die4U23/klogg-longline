# 项目状态

更新日期：2026-10-17。M0—M4 的 Windows 本地交付已完成，结论限定于实际覆盖的场景，见 [验证报告](validation-report.md)。

- 固定上游快照的同类索引/长行分配故障已复现，保留原始失败和哈希。
- 已实现宽跨度索引、完整流式字面搜索、普通路径字节预算、异步有界预览，以及任务切换、缓存、follow 与关闭回收。
- 最终候选完整重编译；应用启动、42 个原生测试用例（399,622 assertions）、9 项独立搜索对照、33 项 Python 工具测试通过。索引组件另外通过 7 项 ASan 用例。
- 实体 5 GiB 索引、末尾搜索、取消及原生 Windows 预览通过。本机取消反馈约 17.6 ms，5 GiB GUI 最大心跳间隔约 270 ms。
- 普通日志 5 对重复测量完整结果均正确，总耗时中位数约 +0.05%，未超出 10% 回归门槛。
- 已导出源码补丁、精简证据、验证报告和包含运行库的未签名 Windows 本地包。

基线最终集成复跑出现一次 120 秒挂起，已保留超时，候选对应整套集成通过。旧增量构建的头文件依赖编码错误已修复，最终结果来自 clean build。

尚未覆盖 Linux、macOS、Hyperscan/VectorScan、网络文件系统、完整应用 sanitizer 和严格故障注入。长行模式及内存预算边界见 [实现契约](implementation-contract.md)。

2026-10-18 发布流程：通过 [问题 #1](https://github.com/Die4U23/klogg-longline/issues/1) 与功能分支提交首版，并以 PR 当前提交的 CI 结果作为合并门槛。没有 Co-authored-by，没有导入上游 Git 历史，保留 GPL 和源码版权。实际合并状态以 GitHub 记录为准。2026-10-17 本地验证 ZIP 位于 `.local/dist/klogg-longline-windows-x64.zip`，它是忽略目录中的本机产物，不是已上传的发行资产。构建与复测步骤见 [构建手册](build-and-run.md)。
