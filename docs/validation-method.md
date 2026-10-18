# 验证证据的导出与解释

本页说明如何把本机 `.local/evidence/` 中已生成的验证记录整理成可审查、可提交的 `evidence/` 目录。导出器只复制材料，不重新运行测试，也不改写原始证据。原始输出和高频采样仍保留在忽略目录 `.local/evidence/`。

## 导出

使用仓库的 Python 运行时，在仓库根目录执行：

```powershell
& .\.local\python\Scripts\python.exe .\tools\export_evidence.py `
  --output .\evidence\validation-2026-10-17
```

目标目录必须尚不存在；导出器拒绝覆盖已存在的目录。新完成的证据通过显式 `--add LABEL=SOURCE` 添加，然后指定一个新的输出目录重新导出，例如：

```powershell
& .\.local\python\Scripts\python.exe .\tools\export_evidence.py `
  --output .\evidence\validation-2026-10-18 `
  --add candidate-search-followup=.local/evidence/candidate/search-followup
```

所有来源必须位于仓库 `.local/evidence/` 内。`manifest.json` 为每个文件记录仓库相对源路径、源 SHA-256 和大小，以及导出文件路径、SHA-256、大小和状态。这样可以核对导出内容与原始记录的关系。源文件若在读取期间发生变化，会标记 `source_changed_during_export`；该份导出不应当作为稳定证据使用。导出过程中若失败，部分目标会保留下来供审计；选择新目录重新执行，不要手工改写清单来掩盖失败。

如果只需要指定来源而不导出内置的十组默认来源，使用 `--no-defaults`。这时至少传入一个 `--add`；例如：

```powershell
& .\.local\python\Scripts\python.exe .\tools\export_evidence.py `
  --no-defaults `
  --output .\evidence\literal-followup-only `
  --add candidate-literal-followup=.local/evidence/candidate/literal-followup
```

## 默认包含的记录

默认选择覆盖这些已记录的验证组：

| 标签 | 本机来源 | 记录类型 |
|---|---|---|
| baseline-m0-index | `.local/evidence/m0-index-baseline` | 独立压缩行索引边界复现 |
| baseline-raw64 | `.local/evidence/baseline/raw64` | 原始长行 GUI 试验 |
| baseline-gui16 | `.local/evidence/baseline/gui16` | GUI 资源边界诊断 |
| baseline-file-index-5gib | `.local/evidence/baseline/file-index-5gib` | 5 GiB 文件索引试验 |
| baseline-search-matrix-unicode | `.local/evidence/baseline/search-matrix-unicode` | Unicode 搜索比较矩阵 |
| candidate-upstream-unit | `.local/evidence/candidate/upstream-unit` | 候选单元测试 |
| candidate-upstream-integration | `.local/evidence/candidate/upstream-integration` | 候选集成测试 |
| candidate-literal-tests-second | `.local/evidence/candidate/literal-tests-second` | 候选字面量搜索回归复跑 |
| candidate-preview-tests | `.local/evidence/candidate/preview-tests` | 候选预览测试 |
| candidate-index-only-asan | `.local/evidence/index-only-asan/asan-check-20261017b` | Windows/MSVC ASan 索引组件检查 |

仅导出命名为 `summary.json`、`results.json`、`comparisons.json`、`benchmarksummary*.json` / `benchmark-summary*.json` 的结构化 JSON，以及 JSONL、`stdout.txt`、`stderr.txt` 和 `compiler-version.txt`。`samples.jsonl` 和其他文件名含 `sample` 的高频采样会跳过。日志中的原始 fixture（例如大型 `.log` 文件）、二进制和其他未列出的文件不会复制。

stdout、stderr 和编译器版本文本各自最多导出 2 MiB。超限文本保留前缀并附截断标记，标记含源文件原始字节数与 SHA-256；清单另存源文件和导出文件各自的哈希。JSON/JSONL 默认上限为 16 MiB，超限文件会被清单标为 `skipped_too_large`，不会输出无效的截断 JSON。可用 `--text-limit-bytes` 和 `--json-limit-bytes` 改上限；不要把文本上限设到 2 MiB 以上，以免扩大公开输出。

## 证据边界

- `m0-index` 是直接编译上游真实压缩行索引源码的独立、同类组件复现。它支持对索引边界问题的判断，但不是完整 klogg 应用的复现，也不代表 PRD 全部验收已经通过。
- `.local/evidence/` 是本机原始记录；导出的 `evidence/` 是经白名单筛选的副本。导出不是重新测量或重新验证。导出清单中的哈希用于完整性核对，不代表结果正确。
- ASan 记录只覆盖独立索引对象/组件；它是在 Windows/MSVC 环境使用 Qt 后端的检查，不等价于整个 GUI 或产品运行时的 ASan 覆盖，也不能证明其他操作系统上的行为。
- RSS 是外部进程采样值，采样间隔可能漏掉短暂峰值。RSS 不等于精确分配量；单次资源记录只说明该次运行观测到的数值。具体预算和失败边界应以对应的原始 summary 为准，不从不同运行推断统一预算。
- 单次诊断记录不是性能验收。候选与基线的局部耗时可能受机器负载、缓存、编译选项和运行次数影响；最终重复对照及项目门槛判断单列于 [验证报告](validation-report.md)，不将历史诊断混入最终结果。
- `stdout` 若被截断，应查看原始 `.local/evidence/` 文件并按清单哈希核对；高频 `samples.jsonl` 被排除是为了避免导出膨胀，不应把导出副本当作完整原始采样包。
