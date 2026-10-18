# 可复现的 CPM 源码归档

`tools/fetch_dependencies.py` 从 GitHub codeload ZIP 归档中准备所选 CPM 依赖。它会在 `.local/dependencies/dependency-lock.json` 中记录每个精确源码引用、解析出的提交、归档 URL、SHA-256、解压路径和 CPM 源码覆盖项；生成的 `.local/dependency-overrides.cmake` 会为对应的 `CPM_<exact package name>_SOURCE` 设置缓存路径。

当前基线构建需要的依赖可按以下步骤处理：

```powershell
python tools/fetch_dependencies.py list
python tools/fetch_dependencies.py fetch --name CRoaring
.\tools\build.ps1 -Variant baseline -ConfigureOnly
.\tools\build.ps1 -Variant candidate -ConfigureOnly
```

辅助工具使用 CPM 推断出的精确名称 `CRoaring`、仓库 `RoaringBitmap/CRoaring` 和标签 `v4.2.1`。对于标签版本，它会通过 GitHub API 解析标签并记录提交。即使标签和提交不变，归档 SHA-256 也能检测下载 ZIP 的内容变化。GitHub 生成的 ZIP 本身不是签名源码制品；复现构建时应将生成的锁定文件与归档一起保留。

注册表还支持上游 CPM 文件中声明的部分 GitHub 包。一次调用可请求多个包：

```powershell
python tools/fetch_dependencies.py fetch --name CRoaring --name streamvbyte
```

只请求当前构建所需的包。GitHub 归档不包含子模块内容。特别要保留现有的 `type_safe` CPM 克隆：其归档不含所需的 `debug_assert` 子模块。`simdutf` 和 `type_safe` 已在本地 CPM 缓存中，因此不会加入此归档注册表。Uchardet 仍使用 CPM 的上游 GitLab 源和版本 `0.0.8`，没有替换版本。CPM 已成功克隆至 `.local/cpm-cache/uchardet/451be2a1085534e378d9cb25e91b2d83acb83b31`，其 `HEAD` 为 `ae6302a016088ad07177f86d417b20010053632b`（标签 `v0.0.8` 对应的标签最终指向的提交）。GitLab 网页/API 归档端点目前向直接 HTTP 客户端返回 Anubis 浏览器挑战，因此没有为 Uchardet 记录生成归档哈希。通过 CPM 声明的仓库和标签获取 Git 源码仍可复现。

工具会解压至 `.local/dependencies/<name>`，在解压前检查 ZIP 路径，限制归档和展开后的大小；除非包被声明为 `DOWNLOAD_ONLY`，否则还会检查根目录是否存在 `CMakeLists.txt`。如果目标目录已存在，工具拒绝替换，因为其中可能含有本地修改。如果生成的锁定文件或覆盖文件与上次生成的内容不同，工具也会拒绝覆盖。解压失败后请使用新的本地依赖目录重试；不要删除或覆盖未经检查、可能含有修改的现有源码树。

这些路径、归档和元数据属于 `.local` 下的本地构建输入，不是上游 Git 历史，也不应提交。准备一个包只能证明归档已下载、哈希与提交已记录，且源码布局通过声明的结构检查；这不能证明整个项目已成功配置或构建。
