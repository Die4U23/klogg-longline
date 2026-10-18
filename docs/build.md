# Windows 原生基线与候选构建

`tools/build.ps1` 使用相同的 x64 工具链构建固定基线或候选源码。完整操作见 [构建与运行](build-and-run.md)。需要 PowerShell、CMake 3.29.2、Ninja 1.12.0、Visual Studio C++ 工具、Windows SDK 和 Qt 6.7.3 MSVC 套件。

| 版本 | 源码目录 | 默认构建目录 |
| --- | --- | --- |
| 基线 `baseline` | `.local/baseline/klogg-25c7de6d8f6da2ce6a00882e5af70b4f331af4c5` | `.local/build-baseline` |
| 候选 `candidate` | `upstream/klogg` | `.local/build-candidate` |

Qt 默认位于 `.local/Qt/6.7.3/msvc2019_64`，须包含 `lib/cmake/Qt6/Qt6Config.cmake`。本仓库 Windows CI 安装 `win64_msvc2019_64` 和 `qt5compat`，使用 Qt 搜索后端。

在仓库根目录的本地 PowerShell 执行：

```powershell
./tools/build.ps1 -Variant baseline
./tools/build.ps1 -Variant candidate
./tools/build.ps1 -Variant baseline -ConfigureOnly
./tools/build.ps1 -Variant candidate -Targets klogg -Jobs 4
./tools/build.ps1 -Variant candidate -CleanFirst
./tools/build.ps1 -Variant candidate -QtRoot 'E:/Qt/6.7.3/msvc2019_64'
```

脚本先检查已知的 Visual Studio 2022 Preview 路径，缺失时通过 `vswhere` 寻找工具链，然后导入 `vcvarsall.bat` 的 x64 环境。Git 的 GitHub SSH 转 HTTPS 和 OpenSSL 设置仅作用于构建进程，并保留调用方已有配置，不修改全局 Git 配置。

配置和构建日志分别保存在 `.local/logs`。非零退出码会中止脚本并报告日志位置。`-ConfigureOnly` 只配置；`-Targets` 可接受多个目标，省略时构建全部默认目标；`-Jobs` 默认取处理器数量与 4 的较小值，限制 ExprTk 并行编译内存。

两种版本均采用 `RelWithDebInfo`、启用测试、关闭 LTO 和 Vectorscan，并共享依赖下载缓存。默认 `-Backend qt` 关闭 Hyperscan；可用 `-Backend hyperscan` 建立独立的 `.local/build-<variant>-hyperscan` 目录，但该模式未纳入本轮原生验收。有效对比要求两侧后端和构建参数一致。

脚本注入 `tests/native/inject.cmake`，并修复中文 MSVC `/showIncludes` 前缀的依赖捕获。更换依赖捕获配置后须干净重建，不能依赖可能缺失头文件关系的旧增量产物。

本脚本只构建，不执行测试。基线、候选、诊断日志须分别保存；构建成功不能代替 M0 故障复现或性能验收。正式证据与未覆盖范围见 [验证报告](validation-report.md)。
