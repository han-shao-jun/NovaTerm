# 内存终端历史与 LZ4

本目录只负责当前终端 Scrollback 的块编码、可选 LZ4 压缩、后台预算与冷块读取，构建目标为 `novaterm_history`。压缩默认关闭；关闭压缩仍使用同一个历史后端。

`ChunkedScrollback`、快照、块和 B1 日志分页代码位于 `src/core/scrollback/`，与本目录共同编入该纯 C++ 库。独立入口为 `tests/history-noqt`，不查找或链接 Qt、MCP、录制、搜索应用或 UI。

- 持久接收流录制：[`../recording/`](../recording/README.md)。
- 离线历史授权、读取和搜索：[`../historysearch/`](../historysearch/README.md)。
- MCP 通过 Core/Session 窄端口读取当前缓存；读取不会自动开启压缩或录制。


## 0.2.125 历史块编码优化（2026-11-01）

perf采样mixed 20000行×80列、压缩ON显示字段编码与颜色写入为主要工作之一，LZ4本身占比较低。编码在索引/分配量验证后一次按精确字节数分配，逐字段小端写入，颜色5字节合并写入；用static_assert锁定CellBytes与字段数关系。不是复制结构体内存，不包含padding。保留所有尺寸、字段枚举和解码边界验证，压缩仍默认关闭。

曾尝试解码边界合并，局部仅约5%收益，已按用户高收益要求撤回。最终仅编码修改。固定语料优化前后编码payload逐字节相同，现有全字段、损坏输入、冷读、预算/取消/生命周期测试保留；新增精确编码上限与少1字节拒绝回归。

五轮交替完整流程A/B（taskset -c 0-15、mixed、20000行、256MiB预算、on、pace=0、64次读样本），各轮完整字段hash f45e6a8ff6ce647d一致且verified=true。摄入CPU ms旧78.205/79.104/78.704/79.417/77.856，新59.943/57.529/58.193/56.908/59.024，中位78.704→58.193（-26.06%）；摄入墙钟中位44.305549→33.653134ms（-24.04%）。scan中位24.733375→25.118342ms，cold_encoded_p95中位339.925→352.366us，不声称冷读收益，也不把压缩ON结果外推默认OFF路径。

0.2.125主程序构建及Core/scrollback/renderer/renderer_p5/history_no_qt_link_check CTest5/5（18.29秒）通过；完整纯历史后端及LZ4独立ASan/UBSan测试1/1（0.17秒）。临时证据/tmp/novaterm-history-ab-*.json、/tmp/novaterm-history-codec-*.bin、/tmp/novaterm-02125-{build,tests}.log、/tmp/novaterm-history-sanitized-tests.log不纳入Git。
