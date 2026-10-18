# Windows 文件监控取消与回收修补

这四个文件复制自 [SpartanJ/efsw 1.4.1](https://github.com/SpartanJ/efsw/tree/62f785c56b7a34f035193d4cb831921347b586b8/src/efsw)，随后仅修改 Windows 异步 I/O 生命周期。上游 MIT 许可保留在 `LICENSE`；原始依赖源码和其他平台实现保持独立。

`overlay.cmake` 校验原始四个文件的 SHA-256，只替换 `efsw` 目标的两个 Windows 实现文件，并优先使用配套内部头文件。依赖版本不匹配时停止配置，不静默套用修补。

原实现取消后立即删除 `OVERLAPPED`。Windows IOCP 的迟到完成包仍会写入该地址，污染复用它的 Qt 对象。修补将监控项从活动集合转入等待回收集合，消费成功或失败完成包后才释放；整体析构先取消、排空，再等待线程退出。零字节完成只重新提交一次，回调内移除自身时延迟到回调返回后回收。

来源差异见仓库 `patches/efsw-win32-iocp.patch`，复现与验证见 `docs/windows-iocp-fix.md`。没有导入上游 Git 历史。
