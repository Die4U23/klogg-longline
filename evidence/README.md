# 可复核证据

`windows-2026-10-17/manifest.json` 记录每份本机原始记录与导出副本的路径、大小和 SHA-256。高频采样仍在忽略的 `.local/evidence/` 中；导出不是重新运行测试。

- `candidate/`：最终 clean 构建的测试、5 GiB 搜索/索引/取消、GUI 和验证器结果。
- `benchmark/`：最终 5 对交错测量；所有轮次均先核验完整结果集合。
- `baseline/`：相同环境的 clean 基线结果；集成测试超时被明确保留。
- `baseline-m0-*`、`baseline-file-index-5gib/`、`baseline-raw64/`、`baseline-gui16/`：原始同类缺陷复现。
- `baseline-integration-earlier/`：较早的一次通过记录，不覆盖最终基线超时。
- `diagnostic-*`：旧 ABI 混合构建失败或存在其他本任务进程时的测量，不用于最终通过/性能结论。
- `index-asan/`：独立索引组件 ASan；不代表完整应用 sanitizer。
- `tool-tests/`、`build-dependencies/`、`reproducibility/`、`package/`：测试工具、头文件依赖修复、源码/构建身份及包检查。

结论、限制和 AC01—AC12 对照见 [验证报告](../docs/validation-report.md)。不要仅凭日志中的 `passed`、退出码或哈希将未覆盖场景标为已验收。
