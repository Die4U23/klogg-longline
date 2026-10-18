# 长行测试样例工具

`tools/fixtures.py` 为长行可靠性工作提供确定性输入，并提供独立的字面量搜索参考检查器。它仅使用 Python 标准库，不与应用匹配器共用代码。

生成普通 M0 `smoke` 测试集：

```powershell
python tools/fixtures.py generate --output .local/fixtures/smoke --profile smoke --seed 1
```

`smoke` 配置档包含 1、16、64 MiB 的单行文件、普通短行、短/长/短混合文件、UTF-8 文本、在默认 1 MiB 检查器边界处分开的 CRLF（CR 与 LF 分处两侧）、末尾没有 LF 的最后一行，以及密集命中。每个文件在清单中有一项，记录字节长度、SHA-256、生成说明和预期行号/字节位置。长行文件按有界数据块写入，并包含确定性的标记字符串。随机种子控制生成的普通行内容，并始终记录在清单中。如果输出目录中已有任何测试样例或清单，生成器拒绝替换；如需有意覆盖，请传入 `--force`。

运行独立检查器，并将其 JSON Lines 结果流保存到文件：

```powershell
python tools/fixtures.py search .local/fixtures/smoke/long_64mib.log --pattern KLOGG_FIXTURE_HIT_A --chunk-bytes 1048576 --output .local/fixtures/smoke/reference.jsonl
```

检查器针对每个匹配行输出一条命中记录（含该行第一个匹配的字节偏移），最后输出一条摘要。它接受非空、区分大小写的 UTF-8 字面量，并拒绝模式中的 CR 或 LF。LF 结束一个逻辑行；紧邻 LF 前的 CR 属于 CRLF 分隔符。空行和末尾没有 LF 的最后一行都会保留在行号中。参考工具按字节比较，不验证 UTF-8；无效字节在此参照工具中仍可被搜索。这不会扩大应用对有效 UTF-8 的生产契约：可靠性设计要求生产路径检测无效 UTF-8 并明确失败。UTF-8 测试样例用于测试多字节模式跨读取边界的情况。

检查器最多保留一个输入块加上 `pattern length - 1` 个重叠字节，以及最近输出行的一个整数。发现匹配时立即写入 JSONL 行，因此密集命中输出不会在内存中累积。输出所需磁盘空间与匹配行数成正比。

`full` 配置档额外生成两个真实写入的 5 GiB 文件：一个单行搜索文件，在 4 GiB 边界附近和文件尾部附近放置标记；另一个独立的压缩索引跨度案例，内容为 `LF + A*(5 GiB - 128) + LF*127`。第二个文件物理长度为 5 GiB，第 2 行长度为 `5 GiB - 128` 字节，包含 128 个 LF 字节，最后一个字节也是 LF。其清单记录行端点、后缀、末字节和 SHA-256。

```powershell
python tools/fixtures.py generate --output .local/fixtures/full --profile full --seed 1
```

此命令至少需要 10 GiB 可用磁盘空间，且可能耗时较长。它不会创建稀疏文件。大体积测试样例不会作为单元测试的一部分生成。

进行吞吐测试时，单独生成约 128 MiB 的短行数据集：

```powershell
python tools/fixtures.py generate --output .local/fixtures/benchmark --profile benchmark --seed 1
```

该工具流式写入约 128 MiB 外观普通的 ASCII 记录，每行含 LF 共 80 至 160 字节。固定标记 `KLOGG_BENCHMARK_HIT` 每 100 行出现一次，并且出现在最后一行。清单记录实际大小/哈希、首尾标记位置和命中总行数，不保存每个预期结果。`--seed` 控制行宽和填充内容。基准测试测试样例已在本地生成至 `.local/fixtures/benchmark/benchmark_normal_lines.log`，其 SHA-256 记录在清单中。

小型 `boundaries` 配置档覆盖从可配置的小数据块边界前开始和恰好从边界开始的字面量、跨读取拆分的 UTF-8 模式、含无效 UTF-8 的按字节搜索案例、长度超过读取数据块的模式、空文件、空行/无末尾 LF 的最后一行，以及跨读取边界的 CRLF：

```powershell
python tools/fixtures.py generate --output .local/fixtures/boundaries --profile boundaries --seed 1
```

该配置档刻意保持很小；默认数据块大小为 8 字节。无效 UTF-8 文件只能由字节参照工具搜索。生产契约仍要求有效 UTF-8，并在输入无效时明确失败。实际文件哈希和预期偏移记录在 `.local/fixtures/boundaries/manifest.json`。

运行专项单元测试：

```powershell
python -m unittest tests.test_fixtures
```

测试使用可配置的小型测试样例尺寸，同时验证相同写入器和检查器，包括单字节读取长度、长于数据块的模式、重叠匹配、跨读取的 UTF-8、CRLF 边界、空行/最后一行、可复现哈希，以及与逐行简单字节搜索参照工具的随机对比。测试也检查缩小后的 `full` 配置档文件和覆盖保护。测试通过只证明这些案例中的工具行为；它不能证明应用正确性、性能、5 GiB I/O 行为或平台覆盖。
