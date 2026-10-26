# Windows 原生构建与验证操作手册

本文按仓库当前脚本说明 Windows 原生构建和验证流程。命令从仓库根目录的 PowerShell 执行。所有构建、缓存、fixture 和原始验证记录均放在忽略目录 `.local/`；示例使用全新的输出目录名，重复运行时请换新名称，不要覆盖失败记录。

## 已核实的工具链

本机可用的原生工具为 Visual Studio 2022 Preview x64 MSVC（`cl.exe` 19.44.35211，环境入口 `C:\Program Files\Microsoft Visual Studio\2022\Preview\VC\Auxiliary\Build\vcvarsall.bat`）、Windows SDK 10.0.26100.0、CMake 3.29.2 和 Ninja 1.12.0。`tools/build.ps1` 会调用 `vcvarsall.bat x64` 并导入其环境，不需要手工打开 VS Developer Prompt。Python 工具使用仓库运行时 `.local/python/Scripts/python.exe`；`tools/run_probe.py` 需要该环境中已有的 `psutil`。

项目固定使用 Qt 6.7.3 的 `win64_msvc2019_64` kit，默认根目录为 `.local/Qt/6.7.3/msvc2019_64`。本机使用 MSVC 2022 编译器配该 Qt kit。安装前可用以下命令核对 aqt 的参数和可用模块；这些是只读查询：

```powershell
& .\.local\python\Scripts\python.exe -m aqt install-qt --help
& .\.local\python\Scripts\python.exe -m aqt list-qt windows desktop --modules 6.7.3 win64_msvc2019_64
& .\.local\python\Scripts\python.exe -m aqt list-qt windows desktop --archives 6.7.3 win64_msvc2019_64
```

在空白 `.local/Qt` 下，项目所需 kit 可用如下 aqt 命令安装：

```powershell
& .\.local\python\Scripts\python.exe -m aqt install-qt `
  windows desktop 6.7.3 win64_msvc2019_64 `
  --outputdir .local/Qt `
  --modules qt5compat
```

`install-qt` 默认安装 base；6.7.3 base archive 列表包含 `qtbase`、`qttools`、`qttranslations`、`qtsvg` 等。本项目额外需要 `qt5compat`。安装前确认磁盘空间和目标目录；如果目标目录已经有 kit，不要直接覆盖，先用 `-QtRoot` 指向完整且匹配的现有安装。不要使用 aqt 的 `--UNSAFE-ignore-hash`。

## 准备源码和可选依赖

`tools/prepare_baseline.py` 下载并校验仓库固定的上游源码快照，不导入 Git 历史。当前快照已准备好时可重跑该命令做完整性校验：

```powershell
& .\.local\python\Scripts\python.exe .\tools\prepare_baseline.py
```

输出应指出 `.local/baseline/klogg-25c7de6d8f6da2ce6a00882e5af70b4f331af4c5` 和 archive SHA-256。若文件校验失败，停止使用该目录作为 baseline；不要手工修改或把 `upstream/klogg` 当 baseline。

`tools/fetch_dependencies.py` 是按需获取固定 GitHub CPM 源码包的可选工具。先列出当前白名单：

```powershell
& .\.local\python\Scripts\python.exe .\tools\fetch_dependencies.py list
```

只在 CMake 明确缺少且该包列于白名单时再取包，例如：

```powershell
& .\.local\python\Scripts\python.exe .\tools\fetch_dependencies.py fetch --name CRoaring
```

工具把源码、ZIP、哈希/commit 锁和本地 CMake overrides 放在 `.local/`，会拒绝替换已有包源码或被手工改动的锁文件。一次包下载成功不代表整个项目配置或构建成功。`type_safe` 等不在该 helper 白名单中的缓存依赖按 CMake 实际诊断处理，不要假设该 helper 会补齐所有依赖。

## baseline 与 candidate 构建

`tools/build.ps1` 的 `-Variant` 同时决定源码和独立构建目录：

| 参数 | 源码 | 默认构建目录 |
|---|---|---|
| `baseline` | `.local/baseline/klogg-25c7de6d8f6da2ce6a00882e5af70b4f331af4c5` | `.local/build-baseline` |
| `candidate` | `upstream/klogg` | `.local/build-candidate` |

请按参数指定的源构建，不要交换两个目录或复用对方的 CMake cache：

```powershell
.\tools\build.ps1 -Variant baseline -Backend qt -Jobs 4
.\tools\build.ps1 -Variant candidate -Backend qt -Jobs 4
```

CMake 的 MSVC `/showIncludes` 输出会受编译器语言和本地代码页影响。本机修复了 Ninja 依赖前缀的字节解码后，旧对象可能仍没有完整依赖记录。首次采用该修复重建既有 build tree 时，建议对每个 variant 做一次 clean-first 完整构建：

```powershell
.\tools\build.ps1 -Variant baseline -Backend qt -CleanFirst -Jobs 4
.\tools\build.ps1 -Variant candidate -Backend qt -CleanFirst -Jobs 4
```

`-CleanFirst` 会先清理该 variant 当前构建目录中的构建产物再重新构建，不删除源码、CPM 缓存或原始证据。后续无需每次都 clean-first；如果切换编译器、Qt kit、backend 或依赖配置，应使用与之对应的独立构建目录，或先核对是否需要重新 clean。

默认后端是 `qt`，构建配置为 `RelWithDebInfo`，测试开启，LTO 和 Vectorscan 关闭。日志写入 `.local/logs/` 并带时间戳；非零的 CMake 配置或构建退出码会使脚本报错。构建默认产物包括 `.local/build-<variant>/output/klogg.exe`、`klogg_tests.exe`、`klogg_itests.exe`；probe target 若在对应源码中启用，则位于 `reliability-probe/`。

可单独配置以先看依赖诊断，或只构建指定 target：

```powershell
.\tools\build.ps1 -Variant candidate -Backend qt -ConfigureOnly
.\tools\build.ps1 -Variant candidate -Backend qt -Targets @('klogg_reliability_probe', 'klogg_gui_probe', 'klogg_literal_tests', 'klogg_preview_tests', 'klogg_crawler_tests') -Jobs 4
```

`-Targets` 传入 CMake target 名。baseline 不包含候选源码新增的 literal/preview target；仅对 candidate 请求这些 target。若需另一套 Hyperscan 配置，使用隔离缓存：

```powershell
.\tools\build.ps1 -Variant baseline -Backend hyperscan -Jobs 4
.\tools\build.ps1 -Variant candidate -Backend hyperscan -Jobs 4
```

Hyperscan 配置能否成功取决于构建时依赖状态；它在本文所述验证中尚未验收。不要把 Qt backend 的结果当成 HScan 的结果。脚本构建不会运行测试。

## 测试

先设置本次测试进程的 Qt 和运行时 DLL 搜索路径，再由 CTest 调用测试。环境变量仅对当前 PowerShell 进程及子进程生效：

```powershell
$qtRoot = (Resolve-Path .\.local\Qt\6.7.3\msvc2019_64).Path
$build = (Resolve-Path .\.local\build-candidate).Path
$env:PATH = "$qtRoot\bin;$build\output;$env:PATH"
$env:QT_QPA_PLATFORM = 'offscreen'
ctest --test-dir $build --output-on-failure
```

当前 Windows candidate CTest 注册 9 项：`klogg_smoke`、`klogg_tests`、`klogg_itests`、`klogg_literal_tests`、`klogg_preview_tests`、`klogg_crawler_tests`、`klogg_efsw_lifecycle_tests`、`klogg_worker_lifecycle_tests` 和 `klogg_empty_search_probe`。空文件探针必须完成搜索并返回零匹配。IOCP 用例只在 Windows 注册；worker 用例覆盖索引任务立即关闭、连续提交和文件重新绑定。若只重跑 unit、integration 与 native regressions，可用：

```powershell
ctest --test-dir $build -R 'klogg_tests|klogg_itests|klogg_literal_tests|klogg_preview_tests|klogg_crawler_tests|klogg_efsw_lifecycle_tests|klogg_worker_lifecycle_tests|klogg_empty_search_probe' --output-on-failure
```

baseline 不包含 candidate-only native tests。失败时保留完整 CTest 输出和构建日志，不要只记录最后一行。

## Fixture、独立 oracle 和 probe

生成小型边界 fixture 和吞吐 fixture：

```powershell
& .\.local\python\Scripts\python.exe .\tools\fixtures.py generate --output .local/fixtures/boundaries --profile boundaries --seed 1
& .\.local\python\Scripts\python.exe .\tools\fixtures.py generate --output .local/fixtures/benchmark --profile benchmark --seed 1
```

生成器拒绝覆盖已存在文件；请用新目录保存新一轮 fixture。完整 profile 会真实写入两个 5 GiB 文件，需要至少约 10 GiB 可用空间且耗时较长：

```powershell
& .\.local\python\Scripts\python.exe .\tools\fixtures.py generate --output .local/fixtures/full --profile full --seed 17
```

`tools/fixtures.py search` 是按块读取文件的独立字节 oracle；示例将结果 JSONL 写入一个新文件：

```powershell
& .\.local\python\Scripts\python.exe .\tools\fixtures.py search .local/fixtures/boundaries/utf8_split_boundary.log --pattern '🙂雪' --chunk-bytes 8 --output .local/fixtures/boundaries/oracle-manual.jsonl
```

具体文件名和预期位置以该 profile 的 `manifest.json` 为准。oracle 字节搜索可检查无效 UTF-8 输入；这不表示产品接受无效 UTF-8。

`tools/run_probe.py` 可独立运行 native probe，输出 `stdout.txt`、`stderr.txt`、约每 20 ms 一个样本的 `samples.jsonl` 和 `summary.json`。用唯一输出目录；它拒绝覆盖非空证据目录：

```powershell
$qtRoot = (Resolve-Path .\.local\Qt\6.7.3\msvc2019_64).Path
$probe = (Resolve-Path .\.local\build-baseline\reliability-probe\klogg_reliability_probe.exe).Path
$fixture = (Resolve-Path .\.local\fixtures\full\compressed_span_5gib.log).Path
& .\.local\python\Scripts\python.exe .\tools\run_probe.py `
  --exe $probe --output .local/evidence/baseline/file-index-5gib-manual-01 `
  --timeout 90 --qpa offscreen --qt-root $qtRoot `
  --dll-dir .local/build-baseline/output --input $fixture `
  -- file-index $fixture 127 10 --timeout-ms 80000
```

`file-index` 的行号是从零开始，`127 10` 表示断言该行是单字节 LF。`--input` 会记录运行前后 SHA-256，若成功退出但输入改变，runner 返回非零。`--max-private-mib` 可启用 runner 的采样私有内存阈值；这不是 OS job-object 硬限制。采样可能漏掉两个采样点之间的峰值，RSS 也不等同于分配量。进程参数位于 `--` 后并原样传递给子进程；执行其他模式时按 `reliability_probe.cpp` 中的 `--help` / usage 说明替换。

## Fixture 对照与性能试验

`tools/compare_search.py` 会按 manifest 逐项调用 native search probe，并将完整行列表及 byte oracle 比较。输入应选小型、可审查的 manifest，不要拿完整 5 GiB fixture 跑矩阵：

```powershell
$qtRoot = (Resolve-Path .\.local\Qt\6.7.3\msvc2019_64).Path
& .\.local\python\Scripts\python.exe .\tools\compare_search.py `
  --probe-dir .local/build-baseline/reliability-probe `
  --manifest .local/fixtures/boundaries/manifest.json `
  --output .local/evidence/baseline/search-boundaries-run-01 `
  --qt-root $qtRoot --dll-dir .local/build-baseline/output
& .\.local\python\Scripts\python.exe .\tools\compare_search.py `
  --probe-dir .local/build-candidate/reliability-probe `
  --manifest .local/fixtures/boundaries/manifest.json `
  --output .local/evidence/candidate/search-boundaries-run-01 `
  --qt-root $qtRoot --dll-dir .local/build-candidate/output --candidate-status
```

baseline 与 candidate 的结果必须放在不同目录。candidate 的 `--candidate-status` 还会要求搜索完成状态和 first-hit byte offset 与完整结果列表对齐。失败记录保留，换新输出目录重跑。

`tools/benchmark_search.py` 面向固定 benchmark fixture，按配对轮次交错运行两种 probe，并先校验 manifest、fixture 哈希和 oracle。至少 3 轮：

```powershell
& .\.local\python\Scripts\python.exe .\tools\benchmark_search.py `
  --baseline-probe-dir .local/build-baseline/reliability-probe `
  --candidate-probe-dir .local/build-candidate/reliability-probe `
  --manifest .local/fixtures/benchmark/manifest.json `
  --output .local/evidence/benchmark-qt-run-01 `
  --backend qt --rounds 3 --timeout 600 --max-private-mib 512 `
  --qt-root .local/Qt/6.7.3/msvc2019_64 `
  --dll-dir .local/build-baseline/output --dll-dir .local/build-candidate/output
```

`--backend` 是记录用标签；必须与两个 probe 的实际构建设置一致。只把 oracle 验证成功的轮次计入比较。三轮仍属于本机诊断测量，不自动等价于 PRD 的正式性能验收。

## 实体大文件复测

最终 5 GiB 支持路径可按以下命令复测（每次使用新证据目录和 oracle 输出文件）。子进程工作目录是可执行文件所在目录，fixture 要传绝对路径：

```powershell
$fixture = (Resolve-Path .local/fixtures/full/full_5gib.log).Path
$probe = (Resolve-Path .local/build-candidate/reliability-probe/klogg_reliability_probe.exe).Path
& .local/python/Scripts/python.exe tools/fixtures.py search $fixture `
  --pattern KLOGG_FIXTURE_HIT_B --output .local/fixtures/full/oracle-end-rerun.jsonl
& .local/python/Scripts/python.exe tools/run_probe.py `
  --exe $probe --output .local/evidence/candidate/search5gib-rerun `
  --timeout 210 --max-private-mib 1024 --qt-root .local/Qt/6.7.3/msvc2019_64 `
  --dll-dir .local/build-candidate/output --input $fixture `
  -- search $fixture KLOGG_FIXTURE_HIT_B --timeout-ms 180000
& .local/python/Scripts/python.exe tools/check_evidence.py `
  .local/evidence/candidate/search5gib-rerun --completion complete `
  --oracle .local/fixtures/full/oracle-end-rerun.jsonl `
  --output .local/evidence/candidate/search5gib-rerun/results-verification.json
```

取消复测将 probe 参数替换为 `search $fixture missing --cancel-ms 100`；使用新输出目录，并将验证参数改为 `--completion cancelled`、去掉 `--oracle`。GUI probe 的参数为 `--file $fixture --action wrap --anchor-byte 5368708973 --expect-text KLOGG_FIXTURE_HIT_B --label candidate --timeout-ms 120000`，runner 使用 `--qpa windows`，验证器使用 `--gui`。最终报告记录了这些模式的实际执行结果及哈希。

## 索引组件 ASan

`tools/run_index_asan.ps1` 构建独立的 index-only 目标，并使用 MSVC `/fsanitize=address /Zi` 运行索引边界及生命周期用例。当前脚本固定选择 candidate 源码，没有 `-Variant` 参数：

```powershell
.\tools\run_index_asan.ps1 -Jobs 2 -RunId index-asan-candidate-01
```

该脚本要求本地已有 Qt、`type_safe`、streamvbyte 和 mimalloc 源码。它固定使用 `.local/build-index-asan`，并拒绝重用已存在的构建目录或同名证据目录；不要删除已有目录来强制重跑，需在全新 checkout/workspace 或由维护者安排新建隔离 build 路径。ASan 覆盖的是独立压缩索引组件对象，不是完整 klogg 应用；这是 Windows/MSVC、Qt 后端下的组件检查。当前验证只覆盖 Windows Qt backend；Hyperscan、macOS 和 Linux 尚未验证。

## 启动 GUI 与可分发目录

直接启动构建产物时，Windows 需要能找到 Qt DLL/平台插件及 oneTBB runtime。可在当前 PowerShell 临时补 PATH：

```powershell
$qtRoot = (Resolve-Path .\.local\Qt\6.7.3\msvc2019_64).Path
$build = (Resolve-Path .\.local\build-candidate).Path
$env:PATH = "$qtRoot\bin;$build\output;$env:PATH"
& "$build\output\klogg.exe"
```

需要检查便携目录时，可对一个新的本地 staging 目录运行 Qt 的 `windeployqt.exe`，并确保 TBB DLL 也进入该目录或在 PATH 可见。此步骤只是依赖部署准备；在没有完成并核验打包产物前，不要称其为发行版或可发布安装包。

打包工具每次都会先对当前 candidate 源码执行完整 clean build，不提供跳过构建的选项。每次运行使用新的安全 RunId；工具拒绝覆盖已存在的同名目录或 ZIP，并保留先前包：

```powershell
.\tools\package.ps1 -RunId 20261026-current-01 -Zip -Jobs 4
```

输出位于 `.local/dist/klogg-longline-windows-x64-20261026-current-01/`，`-Zip` 另生成同名 `.zip`。包内含当前完整源码补丁及 JSON、运行文件哈希清单，以及当前 Git HEAD/dirty 状态和生产源码指纹；工具会核对基线归档及完整文件集合，在 clean build 和补丁导出前后比较生产源码快照，并在隔离副本中实际应用完整补丁、核对全部文件字节；任何不一致都会拒绝打包。工具调用 Qt 6.7.3 `windeployqt --release --compiler-runtime --translations zh_CN`，额外复制 MSVC x64 CRT DLL、oneTBB 和许可/来源文件。默认查找本机 Visual Studio 2022 Preview；其他安装需设置 `VCINSTALLDIR` 为对应 `VC` 目录。输出是未签名的本地内部验证包，不是安装器或发行版；生成包本身不代表本轮长行、5 GiB 或性能验收已通过，须引用对应本轮证据。

## 验证范围

当前已记录的构建和测试使用 Windows 原生 MSVC 与 Qt backend。这里的命令不建立 Hyperscan、macOS 或 Linux 覆盖，也不代表完整 PRD 接受条件全部满足。保留 baseline、candidate、独立组件 ASan 和性能记录的边界；报告结论时应说明具体 source、backend、fixture 哈希、运行命令、退出码与证据目录。

