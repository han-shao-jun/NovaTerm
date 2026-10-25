# P2 Windows 录制基线（2026-10-22）

实现、默认值依据、审查及未验收范围见 [P2 实施记录](../../../../../docs/architecture/stages/P8_AI_MCP_Interface/records/P2_Implementation_and_Verification.md)。

- `off.json` / `on.json`：同一 64 MiB、64 KiB/16 ms 合成源，开启录制零丢失，正常封口。进程级独立运行。
- `regression.txt`：既有 39 项 38/39；唯一失败 ConPTY。
- `conpty-known-failures.txt`：两条已知句柄断言的文件输出；不放宽断言。
- `store.txt` / `service.txt` / `session-observation.txt`：私有 ACL、恢复/配额和实际背压/代际等行为验证。
- `recording.txt` / `ui.txt` / `ui.png`：最终四项录制 CTest 和独立 offscreen 管理面板预览。

```powershell
# 已在 AGENTS.md 的 MSVC 环境构建，Qt bin/plugin 环境与该文件一致。
$env:QT_QPA_PLATFORM='offscreen'
build/Release/bin/novaterm_history_recording_benchmark.exe off build/off.json
build/Release/bin/novaterm_history_recording_benchmark.exe on build/on.json
```

该夹具不启动主程序或真实连接。事件循环间隔并非实际终端 GUI P95；CPU/内存与实际终端发布门槛仍待验证，不能据零丢失样本宣称性能达标。JSON 的目录占用包括尚打开的索引 WAL/SHM。
