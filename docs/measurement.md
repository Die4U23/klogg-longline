# 原生进程测量工具

`tools/run_probe.py` 启动一个可执行文件，把输出直接写入文件，并采样进程资源。需要安装 `psutil`；缺失时会在启动子进程前失败。本地可使用 `.local/python/Scripts/python.exe`。

每次运行使用新的空输出目录。工具拒绝覆盖已有结果，生成 `stdout.txt`、`stderr.txt`、`samples.jsonl` 和 `summary.json`，不把全部子进程输出积累到内存。

以下示例在仓库根目录的本地 PowerShell 执行。先按 [测试样例说明](fixtures.md) 生成对应文件，并确保输出目录尚未使用：

```powershell
$fixture = (Resolve-Path .local/fixtures/full/long_16mib.log).Path
& ./.local/python/Scripts/python.exe ./tools/run_probe.py `
  --exe .local/build-candidate/reliability-probe/klogg_reliability_probe.exe `
  --qt-root .local/Qt/6.7.3/msvc2019_64 `
  --dll-dir .local/build-candidate/output `
  --output .local/runs/candidate-search-01 `
  --timeout 120 `
  --max-private-mib 512 `
  --qpa offscreen `
  --input $fixture `
  -- search $fixture KLOGG_FIXTURE_HIT_A
```

子进程的工作目录是可执行文件所在目录，因此输入文件应使用绝对路径。`--exe`、`--output` 和正的有限 `--timeout` 必填。`--qt-root` 把 Qt 的 `bin` 加入子进程 PATH，`--dll-dir` 可重复指定其他 DLL 目录。`--` 后的参数原样传给程序；`--input` 在运行前后记录文件大小和 SHA-256；`--compiler` 可记录编译器标签。

超时返回 124，超过私有内存阈值返回 125，无法取得所需私有内存指标返回 126；子进程成功但输入发生变化返回 3，其他子进程失败保留其退出码。报告分别记录子进程退出码和测量工具退出状态。终止时清理派生进程，但资源采样只统计直接启动的进程。

采样间隔约 20 ms，记录单调时间、RSS、虚拟内存、可用的私有内存和峰值工作集、CPU 时间及线程数。不可用字段保留为空，采样错误和短进程的采样缺口明确记录。汇总包含命令、程序哈希、时间、峰值、输入哈希，以及 QPA 和 PATH 的必要变更，不导出完整环境或凭据。

采样可能遗漏瞬时峰值，也会带来少量额外开销。采样峰值不等于精确分配量，测量成功不代表程序结果正确或验收完成。基线、候选和诊断输出分别保存，失败与不支持模式也必须保留。
