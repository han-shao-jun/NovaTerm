# 持久录制模块基准

`RecordingBenchmark.cpp` 链接 `novaterm_history_recording`，覆盖独立持久录制链路。
它使用合成接收源，比较录制 OFF/ON 的接收量、丢弃量、CPU、内存及 GUI 定时器延迟。

构建目标为 `novaterm_history_recording_benchmark`，从仓库根目录运行：

```powershell
cmake --build build/Release --target novaterm_history_recording_benchmark
./build/Release/bin/novaterm_history_recording_benchmark.exe off build/recording-off.json
./build/Release/bin/novaterm_history_recording_benchmark.exe on build/recording-on.json
```

需要桌面环境；不启动 NovaTerm、不连接设备。合成录制基准不代表历史搜索、MCP 工具或真实设备验收。
实现与验收范围见 [P2 实施记录](../../../../docs/architecture/stages/P8_AI_MCP_Interface/records/P2_Implementation_and_Verification.md)，
历史样本说明见 [MCP 历史归档](../../archive/mcp-history/)。

源码与测试已按职责拆分，见 [目录重构记录](../../../../docs/architecture/History_Module_Layout_2026-10-25.md)。本基准沿用原目录与目标名，历史样本路径保持。
