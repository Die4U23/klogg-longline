# 有界长行 GUI 预览：设计与兼容性接口

日期：2026-10-17。源码快照：`upstream/klogg`，上游基线 `25c7de6d8f6da2ce6a00882e5af70b4f331af4c5`。C++17、Qt Widgets，目标 Qt 6.7.3。第 1–5 节保留原始源码分析与提案设计。下文实施状态区分之后获授权的候选版本与本提案。

## 候选版本实施状态

协调任务提供了 `.local/evidence/baseline/raw64/` 与 `.local/evidence/baseline/gui16/` 中的独立 M0 测量：真实 64 MiB 行读取会分配整行大小的原始缓冲区；真实 16 MiB GUI 自动换行操作则在分配失败前导致内存大幅增长。检查该自动换行路径发现了具体的乘法增长：每个换行行会按值保存一个 `WrappedString`，其 vector 又包含**所有**换行行。这是行元数据的二次方复制，并非每行都复制一整份 QString。候选版本共享不可变文本/行后备存储，并且只保存屏幕可见行的元数据。

候选版本生产代码现在增加了索引源快照、有上限的字节读取和显式 EOF 处理；异步 GUI 帧最多请求 128 KiB 源文本和 256 个逻辑行。每个行窗口最多含 16 KiB 源字节。解码字符串、上一帧、新的进行中帧、展开字符串和绘制缓存可能短暂共存：源文本上限不是总 RSS 上限。GUI 缓存诊断会报告保留的帧/行/字符串容量，并注明排除项；进程采样仍是更广泛的资源观测。过滤索引在 GUI 线程上解析，后台任务读取稳定的原始 LogData。数据销毁前，视图会先取消并等待任务结束。

长 UTF-8 行显示字节范围/窗口内列号，并提供前一窗口/后一窗口操作；主搜索结果会提供绝对字节锚点。远端窗口中的制表符间隔明确按窗口局部计算，不会错误显示成全局列号。长行模式/正则表达式高亮和旧 Quick Find 明确不可用；完整且受支持的字面量主搜索仍单独工作。拒绝复制整条长行；可以复制当前已就绪窗口中选中的部分。如果数据报告的最大行长超过预览上限，则拒绝使用现有按行分批的导出；尚未实现有界流式导出器。普通有界行仍保留现有解码/预过滤和复制行为。线性制表符展开保留了原有 NUL、CR、Unicode UTF-16 和初始列语义。

最终干净构建、行为测试和原生/离屏 GUI 测量记录在[验证报告](validation-report.md)中。诊断探针的硬进程内存上限退出时没有清理完成证据；原生剪贴板诊断被拒绝。设计和离屏结果都不能证明 macOS 覆盖，也不能关闭上游历史报告。

## 1. 实际调用链

以下路径均相对于 `upstream/klogg`；行号对应已检查的快照。

| 入口 | 后续操作 | 后果 / 迁移要求 |
| --- | --- | --- |
| `AbstractLogView::paintEvent`（`src/ui/src/abstractlogview.cpp:1064`）/滚动（`:1029`） | 使缓存文本区失效 → `drawTextArea`（`:2199`）→ `AbstractLogData::getLines(firstLine_, nbLines)`（`:2307`）→ `LogData::doGetLines`（`src/logdata/src/logdata.cpp:327`）→ `getLinesFromFile`（`:407`）→ `getLinesRaw`（`:349`）→ 分配/读取范围（`:386`）→ `RawLines::decodeLines`（`:453`） | 裁剪前已读取所有完整的可见逻辑行。新的绘制 API 必须传递有界行片段并限制帧总预算。 |
| 过滤视图 | `LogFilteredData::doGetLines`（`src/logdata/src/logfiltereddata.cpp:499`）→ 辅助工具（`:514`）→ `doGetLineString`（`:485`）→ `查找LogDataLine` → 源数据 `getLineString` | 请求片段前，必须将过滤后的可见索引映射为原始行号。现有 批次 辅助工具会逐条读取源行。 |
| 普通/展开内容访问 | `AbstractLogData::{getLineString,getExpandedLineString,getLines,getExpandedLines}`（`src/logdata/src/abstractlogdata.cpp:48-69`）→ 虚函数实现；`LogData::doGetLineString`（`:313`）获取完整解码行；`doGetExpandedLineString`（`:319`）随后调用 `untabify` | 不要悄悄让这些通用 API 返回截断字符串：搜索、选择和 CLI/导出调用方都预期取得完整内容。应新增仅用于预览的 API 并迁移 GUI 调用方。 |
| 绘制高亮 | `drawTextArea` 的原始文本 高亮器（`:2369` 起）→ 每个命中调用 `untabifyHighlight`（`:2396`）复制/展开整个前缀 → `untabify(logLine)`（`:2428`）→ 对展开文本执行 Quick Find 高亮（`:2432`）→ `WrappedString`（`:2446`）→ 最后才裁剪可见区域（`:2455`） | 最后的裁剪太晚。高亮器只能接收有界片段；原始 UTF-16 位置到展开列号的映射应对每个片段只计算一次，而不是对每个前缀重新展开。正则表达式语义另见下文。 |
| 自动换行与滚动条更新 | `updateScrollBars`（`:2168`）→ `getNbBottomWrappedVisibleLines`（`:2141`）→ 获取完整展开的底部行（`:2151`）→ `WrappedString`；水平范围使用 `getMaxLength` | 行实际绘制前，自动换行也可能触发完整读取。长行只对当前片段自动换行；保留逻辑行号，并显示续行/范围提示。不要为真实长行分配所有换行后的行。 |
| 行尾/右侧导航 | `jumpToEndOfLine`（`:1913`）/`jumpToRightOfScreen`（`:1921`）→ `maxLineLength`（`:1900`）→ 重复调用 `getLineLength` → `LogData::doGetLineLength`（`:271`）→ 完整展开字符串（`:277`） | 看似廉价的元数据查询会完整读取并展开长行，而且在 `max_element` 中重复执行。长行导航需要字节端点/异步计算的显示指标，不能继续用此方法。 |
| 坐标/词选取 | `convertCoordToFilePos`（`:1771`）读取缓存 `WrappedString` 并加上 `firstCol_`；`selectWordAtPosition`（`:1953`）获取完整展开行（`:1955`） | 为每个换行行保存片段起点，并将局部列映射回该起点。不要通过填充字符串来表示很大的绝对列。词选取在片段边界停止，并显式标明边界信息不完整。 |
| 选择状态、拖拽和上下文菜单 | `mouseMoveEvent`（`:651,664`）、`moveSelection`（`:1891`）、`selectAndDisplayRange`（`:2001`）调用 `getSelectedText().size()`；上下文菜单颜色标签（`:577`）获取选中文本 | 按元数据统计行/范围选择；部分选择长度从缓存片段坐标获取。选择变化信号不应为了计算数量或比较标签而物化所选内容。 |
| 剪贴板和基于选择的搜索 | `copy` / `copyWithLineNumbers`（`:1379,:1392`）、`updateGlobalSelection`（`:1982`）、添加/替换/排除 搜索和 查找 选中（`:1329-1374`）→ `Selection::getSelectedText`（`src/ui/src/selection.cpp:164`）→ 单行读取完整原始解码文本（`:199`）；片段读取完整展开文本后再 `mid`（`:204`）；范围读取所有选中行（`:209`）、构建 映射表，再拼接 | 所有调用方都需要相同的预检/截断选择契约。 |
| Quick Find | `searchForward`/`searchBackward`（`src/ui/src/quick查找.cpp:249,:265`）等待旧任务结束后启动 `QtConcurrent` → `doSearchForward`/`Backward`（`:284,:359`）→ `getExpandedLineString`（`:313,:324,:397,:409`）→ 正则表达式匹配器 → `Portion` → `setQuickFindResult`（`abstractlogview.cpp:1218`）→ `displayLine` | 这条路径搜索展开文本，与主搜索的原始文本 行为不同。它只在行之间检查取消。`stopSearch`（`quick查找.cpp:160`）和启动新搜索都会同步等待；仅把工作移出 GUI 线程不能保证等待有界。 |
| 保存/导出 / CLI | `saveLinesToFile`（`abstractlogview.cpp:1433`）按 5000 行分批，读取完整 `getLines`（`:1478`），追加换行符并经 TBB 图写入；`src/app/klogg_grep.cpp:63` 也调用 `getLines` | 现有导出不能作为超大剪贴板选择的安全流式替代品。安全替代方案必须以有界解码/输出块流式写入，并保留既定换行/编码语义。 |

## 2. 建议的最小 API 边界

保留现有索引和调度基础设施。在 `LogData` 中增加元数据/范围原语，通过 `AbstractLogData` 暴露预览适配层，并由 `LogFilteredData` 解析过滤行。下列名称是建议的契约，不代表这些声明已经存在。

```cpp
struct SourceStamp { /* stable file identity + session/index generation */ };
struct LineByteRange {
    SourceStamp stamp;
    LineNumber sourceLine;
    OffsetInFile begin;
    OffsetInFile contentEnd; // excludes actual LF/CRLF; clamps synthetic final LF to EOF
};
enum class PreviewState { Ready, Pending, Cancelled, Failed, SourceChanged, Unsupported };
struct PreviewRequest {
    LineNumber viewLine;
    uint64_t requestId;
    std::optional<OffsetInFile> byteAnchor; // initial request = line start
    size_t maxInputBytes;                  // enforced before buffer allocation
    qsizetype maxUtf16Units;               // decoded, unexpanded output cap
    qsizetype maxExpandedUnits;            // tab-expanded output cap
};
struct LinePreview {
    SourceStamp stamp;
    uint64_t requestId;
    LineNumber viewLine, sourceLine;
    OffsetInFile byteBegin, byteEnd; // exact consumed content interval
    QString rawText, displayText;   // both bounded; no UI marker embedded in source text
    bool hasBefore, hasAfter;
    std::optional<uint64_t> sourceUtf16Start, expandedColumnStart;
    PreviewState state;
    // compact bounded mapping: raw UTF-16 offsets <-> displayed offsets <-> source bytes
};
```

后端所需操作：

1. `getLineByteRange(viewLine)` 解析过滤行，并在不解码整行的情况下获取索引起止位置。要得到 `contentEnd`，需通过有界尾部读取检查实际支持编码的 LF/CRLF，或在索引端点字段之外由解码器检查尾部；不能因索引中的 合成 最终 LF 而剔除真实内容字节。现有 `getLineNumber()` 是显示行号（从 1 开始），不能简单相减后当作源索引映射。内部应使用 `查找LogDataLine`。
2. `readLineBytes(range, offset, maxBytes, expectedStamp, cancel)` 在分配前验证边界、算术溢出、文件代次，然后 定位读取 并最多读取 `maxBytes`。不得调用 `getLinesRaw`。短读表示失败/源变化，不能补齐后假报成功。不支持模式不得回退为整行读取。
3. 有界解码器/预览请求返回片段、已消费的字节范围和坐标映射。首先支持 UTF-8；起止位置应与解码器边界对齐，按现有编码器一致处理无效字节，并且只在文件开头处理 BOM。中间片段或无末尾换行的最后一行不能被错误去除 LF。只有在真实行尾才处理 CR。
4. 搜索/导航提供用于显示命中的**源字节锚点**。行命中位图没有命中位置。按需通过完整有界字面量 扫描器 解析一个命中；不要存储所有命中位置，也不要只搜索预览。每个有界单元之间检查取消和代次。

GUI 请求经由小型有界缓存/工作线程任务，并使用现有异步设施。绘制只使用已就绪片段，或显示 等待中/错误 占位，不等待工作线程。初始阶段每个视图只保留一个未完成预览代次；取消被替代的请求，并按 标记 + 请求 ID 丢弃过期结果。文件访问锁定区间应保持短暂；运行无界扫描/解码器时不要持有索引访问器。离屏分配测试不能证明原生 GUI 心跳行为。

## 3. 上限、显示坐标与制表符

初始候选版本上限待测量：每片段输入 64 KiB；原始解码和展开后输出分别限制为 16 Ki UTF-16 单元；每个视图/帧保留的预览数据载荷总计 2 MiB，另计一个小型 临时 缓冲区。这些是设计参数，不是实测资源数据。应按实际保留容量记账原始字节、两种字符串、坐标映射和缓存的 `WrappedString` 行，并包括释放前仍存活的处理中与上一帧对象。分配前先预留预算；开始新一帧不能重置仍被引用的对象预算。未服务的可见行标为 等待中，不要并行请求所有行。行/对象开销和活跃行数需要独立的小上限。预览总量记账应与 PRD 搜索数据路径预算、进程总 RSS 分开。

制表符间隔为 8（`src/logdata/include/linetypes.h:307`）。现有 `untabify` 会将 NUL 换为空格，按当前 UTF-16 列加 `initialPosition` 展开 制表符，并反复修改字符串。必须在制表符展开 **之前**限制输出；如果只限制输入，64 KiB 全 制表符 片段可膨胀到约 512 Ki UTF-16 单元。流式片段 展开器 应推进绝对展开列，并只输出允许的单元。如果一个 制表符 跨过输出上限，需保留它的续接状态或在该 制表符 前结束；不能丢掉剩余空格，也不能在下一页重复该 制表符。保留代理项对，并维护显示空格到源字节的映射。

索引保存行端点和一个全局 `maxLength_`，不会为每行保存精确的解码/展开长度。索引 解析 长度按 LF 字节数加 制表符 估值计算（`logdataworker.cpp:459-464`）；`getLineLength` 会解码并展开；搜索的 `getUntabifiedLength(std::string_view)` 则统计 UTF-8 字节。这些值不能互换。`LineLength`/`LineColumn` 底层类型为 `decltype(QString{}.size())`（`linetypes.h:123,148`）；目标 Qt 6 中它是 `qsizetype`，而 滚动条 仍是 `int`。不要假设是 32 位 Qt 5 类型，也不要移除显式 滚动条 限幅。

任意字节锚点不能给出绝对 UTF-16/制表符 列号。UTF-8 起始边界只需有界向前查看；但要得到精确的展开列起点，必须扫描此前内容或使用预先计算的解码器/制表符 检查点。第一版可以显示字节范围和**窗口相对**列号，并让长行选择绑定到片段身份。如果需要精确绝对列号，应在工作线程中从行首开始可取消地计算，并最多保留有界/LRU 检查点 作为优化。不能在绘制中同步扫描整个前缀，也不能从字节偏移臆造精确列号。有状态非 UTF-8 编码器通常不能从任意字节边界解码；应保留普通行路径，或在没有经过验证的检查点解码器时报告不支持长行窗口访问。

只对当前片段自动换行，并通过覆盖层/状态栏显示 `Long line — bytes X–Y of Z`，提供明确的前一范围/后一范围操作。标记必须与 `rawText`/`displayText` 分开，不能被选择/复制/匹配为文件内容，也不能改变逻辑行号。单一上限不能使任意正则表达式的运行时间有界。在经过验证的路径实现前，应禁用/报告不支持复杂长行正则表达式和自定义正则表达式高亮器；不能将片段边界悄悄当作真实行边界（`^`、`$`、后向断言等）。字面量片段高亮可使用有界前后查看并裁剪高亮，且独立于完整搜索正确性。

远端片段的水平 滚动条 控制片段局部列；显式行尾操作请求尾部字节窗口。普通行继续使用当前 滚动条 语义。这比把 int 滚动条 映射到无界虚拟展开行侵入更小。不要用完整 `getLineLength` 决定尾部请求的起点。启用远端窗口前，坐标转换、选择片段和 Quick Find `Portion` 都需要片段起点适配器；不能将原始字节位置传给 `LineColumn`。

## 4. 选择、Quick Find 与导出兼容性

增加有检查的选择 API：要么在调用方上限内返回完整文本，要么返回有类型的 `TooLarge/Unsupported/SourceChanged` 结果；不能成功返回部分剪贴板字符串。读取前依据索引字节跨度和经过检查的解码/换行/行号展开最坏情况，预检整行/范围大小。遍历范围，而不是为每个选中行分配 `Selection::getLines()`。生成文本时还应再次按实际输出记账，因为保守大小估算不一定精确。Windows 剪贴板文本最终需要一个有界 QString；Qt 剪贴板的副本也要单独记账。

可见片段的部分选择应使用缓存片段及其精确映射，并保留当前行为：整行复制原始解码 制表符，部分选择复制展开后的空格（`selection.cpp:161`）。鼠标移动时只更新元数据/状态计数。对于超大整行/范围复制，应说明选择范围和剪贴板上限，并提供明确的复制当前片段或有界流式导出操作。X11 主选区 选择、上下文标签和基于选择的搜索也要遵守上限。超长的选中搜索词必须在完整读取前明确拒绝；不能静默改用显示前缀。

Quick Find 目前只在 `QuickFindMatcher` 中保留 `QRegularExpression`，即使 转义 字面量模式也是如此，并且搜索的是**展开字符串**。原始-字节主搜索 KMP 结果不能直接替换：Quick Find 对空格的搜索可以匹配 制表符 展开文本，但无法匹配原始日志中的 制表符。普通行行为必须保留。在可取消地流式处理展开 UTF-16 之前，应明确拒绝不支持的长行模式，不能跳过长行并返回“未找到”。完整实现需要在匹配器中保留原始字面量/模式信息，用有界状态流式解码/制表符展开，携带字节锚点和显示坐标元数据，并在不保留所有匹配的情况下正确处理向后查找的 下一次 语义。还应将同步 `waitForFinished` 流程改为可异步完成代次 切换；仅限制数据量不能解决等待运行中正则表达式的问题。

导出需要通过现有任务机制增加单独的有界分块迭代器/写入器；保留过滤行顺序、真实源选择、所选输出编码和既有换行语义。`saveLinesToFile` 当前按行数分批，因此迁移前不能将其宣传为安全的复制替代方案。临时设置诚实的上限/拒绝状态，优于声称已实现流式导出。

## 5. 最小分阶段实现与验收

在预览 M0 确认分配/CPU 热点后，按以下顺序实现：

1. 后端精确字节范围元数据/读取 + 有界 UTF-8 解码器；小型边界测试使用真实 `LogData`，覆盖 LF/CRLF/无末尾换行、制表符 密集和多字节输入、过滤映射、源变化和短读。
2. 初始前缀绘制路径和帧/缓存预算，并显示明确的长行标记。同一切片中迁移 自动换行/底部滚动、行长导航和选择状态调用方，避免绕过上限。保留短行行为。
3. 安全的选择/复制门控与有界导出，或返回明确的不可用结果；验证选择、打开上下文菜单、复制远端小片段或使用基于选择的搜索时，都不会发生整行读取。
4. 通过源字节锚点按需请求命中/尾部窗口，并接入片段坐标；坐标映射测试通过后才启用。Quick Find 需要单独实现展开文本兼容/取消能力，或明确报告不支持。

父级/后端负责人需提供：(a) 固定的真实索引 64 位端点与最后一行/EOF 契约；(b) 代次/文件身份和读取失效信号；(c) 可取消、有上限的字节读取；(d) 带主搜索模式契约的单命中字节锚点解析器；(e) 预览数据载荷所有权/记账方案。GUI 负责人只能在明确分配文件后改动 `abstractlogview.{h,cpp}`、`selection.{h,cpp}` 和 Quick Find。通用 数据 API/过滤转发 应由一名明确的后端负责人负责，避免多个 代理 同时修改。

分别在普通短行、1/16/64 MiB 长行、制表符 密集/Unicode 案例和多条可见长行上测量基线与候选版本。记录读取/分配、临时/缓存/高亮/自动换行 保留字节、GUI 线程心跳、首片段/取消延迟和代次切换隔离。验证末尾附近的命中和跨片段边界；仅显示标记或截图成功不能证明完整搜索或资源验收。Windows 原生 Qt 6.7.3 结果不能证明 macOS 覆盖，也不能关闭上游历史问题。
