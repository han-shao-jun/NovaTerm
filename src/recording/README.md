# 持久接收流录制

构建目标 `novaterm_history_recording`，负责 HistoryRecorder、块文件存储、SQLite 索引、保留/配额及异步 HistoryService。依赖 Qt Core、私有 Qt Sql、OpenSSL Crypto 和线程库，不依赖 MCP 或 LZ4 Scrollback。

Windows 静态 OpenSSL Crypto 的证书和网络依赖由录制库向链接使用者传递（crypt32、ws2_32），避免录制测试及其他使用者出现未解析符号。

`HistoryTypes.h` 是记录、范围、完整性和捕获描述的值类型契约；离线读取可以引用这些类型而不链接具体录制后端。既有 `NovaTerm::Recording` 类型名、磁盘格式、配置键和构建目标保持。

本目录不负责授权、搜索、终端屏幕或 Agent 编排。录制源与离线 HistoryReadPort 的具体接线位于 [`../resources/RecordingHistoryReadAdapter.h`](../resources/RecordingHistoryReadAdapter.h)，由应用/MCP 组合根使用。

行为测试位于 `tests/recording/`，CTest 标签为 recording；QTest 文件输出在 `build/Release/tests/recording/`。


## 0.2.122 大记录摘要计算优化（2026-11-01）

perf录制合成负载中，HistoryChunkStore::append累计约47%，SHA-256路径约46%。项目已要求OpenSSL；对≥4096字节数据改用EVP SHA-256运行时加速，元数据小块及provider失败时沿用Qt SHA-256。摘要算法、32字节格式、链校验、持久化、取消和丢失语义不变。没有自实现密码算法或缓存完整性结果。

同机64KiB×1024哈希对照：Qt约248ms、OpenSSL约25ms，每次摘要均与Qt一致。整链三轮交替64MiB合成录制（taskset -c 0-15、X11 QWidget基准、固定约3.9MiB/s）CPU秒：旧3.393505/3.298547/3.294301，新2.211204/2.166988/2.208638；中位3.298547→2.208638，降低33.04%。全部67108864字节接受、零丢失。该指标含基准GUI重绘，是进程CPU成本；输入节拍固定，不宣称吞吐或真实终端帧率提升。

新增落盘frame摘要与Qt SHA-256比较，覆盖空/1/4095/4096/65536字节载荷及读回。初始测试默认64KiB读取预算导致4/5记录，真实失败保留；显式128KiB测试预算后通过，生产预算未改。0.2.122主程序构建通过；recorder/store/service/recording_ui/history_search/resource_sharing相关CTest6/6（11.46秒）。HistoryChunkStore局部ASan/UBSan，依赖仍Release，定向3/3（176ms），不扩大为全工程插桩或其他平台验收。

基准入口保持tests/benchmarks/history/mcp/RecordingBenchmark.cpp。临时证据/tmp/novaterm-recording-ab-*.json、/tmp/novaterm-recording-profile.data、/tmp/novaterm-02122-{build,tests}.log及/tmp/novaterm-store-sanitized.log不纳入Git。未读取用户录制、未连接设备；测试使用自动清理的独立临时目录。
