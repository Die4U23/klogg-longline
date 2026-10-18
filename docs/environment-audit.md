# Windows 原生构建环境初次调查

记录日期：2026-10-17（Asia/Shanghai）。**这是安装 Qt 之前的历史调查，不代表当前环境。** 同日后续已完成原生构建与候选验证，最终结果见 [验证报告](validation-report.md)。

## 当时确认的工具

- CMake 3.29.2：`C:/Strawberry/c/bin/cmake.exe`。
- Ninja 1.12.0：`C:/Strawberry/c/bin/ninja.exe`。
- Visual Studio 2022 Preview 的 `vcvarsall.bat x64` 可成功初始化环境。
- MSVC x64 编译器版本 19.44.35211，工具集目录 14.44.35207。`cl /Bv` 输出版本后因未提供源文件返回诊断，不能据此判定编译器不可用。
- Windows SDK 10.0.26100.0 的头文件、库、x64 `rc.exe` 与 `mt.exe` 存在。
- 当时可用磁盘空间：C 盘约 21.0 GiB、D 盘约 192.5 GiB、E 盘约 339.1 GiB。这些是历史数值。

## 当时缺失或未确认的条件

PATH 中没有 `qmake.exe` 或 `qtpaths.exe`；针对常见 Qt 安装路径、下载目录和仓库缓存的有限检查未发现兼容套件。该检查不是全磁盘扫描，不能证明其他位置不存在 Qt。

固定上游 Windows 配置指定 Qt 6.7.3 `win64_msvc2019_64`、`qt5compat`，共享安装流程还包含 `qtbase`、`qtimageformats`、`qtsvg`、`qttools`、`qttranslations` 和 `icu`。后续按固定套件补齐环境，源码依赖在配置阶段分别核验。

当时 `vswhere -all` 没有列出实例，但直接安装路径、编译器和环境初始化均可验证。现有 [构建脚本](build.md) 因此保留已知路径与自动查找两种方式。

WSL 的 Ubuntu 24.04 有 GCC 13.3.0，但当时未找到 CMake、Ninja 或 Qt 开发环境，不能用它代替 Windows 原生验证，也不能宣称 Linux 原生应用已通过。

## 证据边界

本次初查没有执行配置、编译、源码故障复现或性能测量。环境清单不能满足 PRD 的 M0 门槛。后续基线故障、分配量、耗时和候选对比均另行记录；构建输出和缓存置于被忽略的 `.local` 目录。
