# 本次证据的归属

- `original-delayed-write.json`：原始 efsw 1.4.1 静态库的独立诊断，5 次均观察到删除后的取消状态写入。`exit_code=1` 是诊断主动报告观察到了缺陷。
- `original-reproducer.cpp`：对应诊断源码。它仅对目标监控结构拦截 `delete`、保留存储直到完成包出队，避免靠随机堆复用推断迟到写入。它不是产品功能，也不是修复后的通过测试。
- `original-integration-repeat.json`：未修改的原始集成程序前 8 轮通过、第 9 轮访问冲突的摘要。记录里的 stdout/stderr 文件名对应本地原始运行目录，不表示这些完整日志随本目录发布。
- `candidate-validation.json`：修补后完整重编译的本地检查结果及二进制哈希；与上述原始失败分开。

完整日志、构建产物和诊断内存转储留在 `.local/ci-followup/`。主分支原始失败的公开记录为 [Actions 39092237442](https://github.com/Die4U23/klogg-longline/actions/runs/39092237442)。GitHub CI 结果以各提交对应的 Actions 记录为准。

## 独立复现原始缺陷

在本地 Windows x64 的 **Visual Studio Developer PowerShell** 中，先运行仓库的依赖准备步骤，取得未修改的 `.local/dependencies/efsw`（1.4.1 / `62f785c56b7a34f035193d4cb831921347b586b8`）。以下使用新的诊断构建目录，不链接候选修补库：

```powershell
cmake -S .local/dependencies/efsw -B .local/efsw-original-repro -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_SHARED_LIBS=OFF -DBUILD_TEST_APP=OFF
cmake --build .local/efsw-original-repro
# efsw 的静态库路径以该构建目录中的实际输出为准。
$efswLib = (Get-ChildItem .local/efsw-original-repro -Recurse -Filter efsw.lib | Select-Object -First 1).FullName
cl /nologo /EHsc /MD /O2 /std:c++17 /DEFSW_STATIC /I.local/dependencies/efsw/include /I.local/dependencies/efsw/src evidence/windows-2026-10-18-iocp/original-reproducer.cpp $efswLib /Fo.local/efsw-original-repro/repro.obj /Fe.local/efsw-original-repro/repro.exe
Push-Location .local/efsw-original-repro
.\repro.exe
Pop-Location
```

这里的关键观察是 `status_at_delete=0x103`、`status_after_delete=0xc0000120`。附带测量使用首版原始静态库；上面的独立构建命令提供复现路径，不声称其新生成二进制与记录中的二进制哈希相同。
