# mcp/P3 实施与验收记录

日期：2026-10-22～23。本轮从既有 P3 计划继续实施，不改写 P2 的断电、真实终端性能和跨平台未验收结论。

## 实施边界与决策记录

- T1：HistoryAuthorizationStore 独立拥有 capture 授权；HistoryReadService 拥有固定高水位、字节偏移与有界读取游标。协议入口创建可信身份，不接受模型指定 Principal 或本机路径。
- T2：HistorySearchService 拥有后台扫描、UTF-8/ANSI/CR 投影、原始多片段定位、续页和取消。只使用异步记录读取端口，不链接存储实现、Widgets、HTTP 或 Session。
- T3：novaterm_evidence 仅依赖 Qt Core，通过来源读取和授权端口验证 hash、代际、权限及期限。诊断来源目前以假来源验证；生产诊断适配随 P4 交付，不宣称已有诊断包。持久证据写入尚未实现，显式请求持久化会被拒绝。
- T4：增加三个历史工具，旧九工具目录对象逐项与原 fixture 核对。所有读取通过 P1 可信 Principal 和 Dispatcher 准入、结果交付复核；不继承 Session 读取权限。

Ruling: 按现有阶段计划在当前工作区续作，不创建分支/worktree、不自动提交；保留既有未跟踪 `.dide/`。依据阶段划分文档的工作区约束。

Ruling: 历史身份的 tokenEpoch 取已认证客户端令牌摘要的 64 位标记，而不复用会因 Session 授权变化推进的客户端版本。代价：依赖服务器生成的随机令牌摘要标记；历史自身授权仍另有单调 accessVersion。关闭标签、修改 Session grant 不应撤销用户明确选择的离线共享；轮换令牌则拒绝旧 grant/cursor。

Ruling: 持久共享由本机表单单独选择，默认只到退出。私有授权文件有独占锁；第二实例无法取得锁时历史授权入口拒绝服务，避免用旧磁盘快照恢复已撤销的授权。变更先保存再发布，失败时明确保持原授权。

Ruling: 首版协议页可小于请求上限：目录最多 40 项、搜索最多 16 命中，读取正文预算约 24 KiB 后再计入协议的文本/结构化双份编码；最终工具封装不超过 64 KiB。代价是大日志需要更多续页；不会以裁掉命中却推进游标来隐藏结果。

Ruling: 一个搜索页的多条命中可引用同一证据，EvidenceRef 保留该页按序的原始多片段和总 hash；不会将 CR/ANSI 投影误作连续原文。默认期限内存证据，不自动开启录制或持久化任务结果。

Ruling: 搜索 cursor 使用每页随机不透明句柄，由服务端保存下一页/上一页精确重试关系，而不在客户端令牌里放 decoder 状态或可编辑页序号。代价：进程重启后不能恢复旧搜索；这是 SEARCH_CURSOR_EXPIRED 的明确语义。

## 本轮复现与修复

1. 新工具契约 RED：旧目录只有 9 项且不识别历史请求；实现后旧对象比较、三个新请求/非法参数和原契约目标通过。
2. 离线接线 RED：缺少显式 capture 共享实现；修复后原始 NUL/非法 UTF-8 字节可经 base64 往返，Session 关闭后可读，撤权后拒绝，搜索返回证据 ID。
3. 分页投影 RED：消费投影缓冲后把空的内部调度消息当磁盘缺口；修复后文本/原字节分页与连续扫描一致。
4. 读取超过 128 页 RED：旧实现按页积累游标；改为有界读取流，仅保留当前位置与一页精确重试缓存，180 页可继续推进。同游标并发返回 Busy。
5. 合并 gap 的部分授权 RED：原 gap [1,20]、授权 [5,25] 时会跳过丢失区间；现明确 HISTORY_RANGE_MISSING，不泄露范围外元数据或伪装连续。
6. 协议缓存 RED：消费两次续页后仍能取回更旧页；协议缓存复用前必须经过 HistorySearchService 校验，不绕过页序、空闲或绝对期限。
7. 拒绝不完整 UTF-8 查询；初始取消不读磁盘；期限到期不从头重扫。投影内存记账由逐字符扫描当前行改为增量计数。
8. 证据解析并发 RED：连续 33 次未完成解析没有拒绝；统一 create/resolve 在途上限为 32 后第 33 次明确拒绝，迟到/移除/销毁不交付正文。
9. 导入采用本机选文件后异步读取/hash，读取/导入共享 2 个在途名额，排队导入也预留最多 32 个资源槽位；销毁取消回调，不把 QObject/界面指针交给 worker。
10. 过期授权名额 RED：预置 128 个已删除 capture 的持久授权后，新的显式共享被容量拒绝；修复为先构建有效快照，成功保存后再回收删除/身份失效条目，失败不发布授权变化。

## 验证状态

环境：Windows 本机、Qt 6.8.3、MSVC 19.51、Ninja Release。`cmake --build build/Release -j 6` 最终完整增量重建成功；没有启动可能恢复真实设备连接的主程序。

| 验证 | 本轮结果 | 原始记录 |
| --- | --- | --- |
| 注册目标 | `ctest -N` 实核 47 项 | 当前生成的 CTestTestfile.cmake |
| 跨模块回归（最后授权名额修复前），证据目标另验 | 45/46，308.97 秒 | `build/p3-full-regression-except-evidence.txt` |
| 最终证据目标 | 1/1，0.45 秒；QTest 17/17 | `build/p3-evidence-final-ctest.txt`、`build/Release/tests/evidence/EvidenceTests.ctest.txt` |
| 合并覆盖范围 | 46/47；唯一失败目标为 novaterm_conpty_tests | 上述两个独立运行的组合，不冒充单次全套通过 |
| 该轮八项相关目标 | 8/8，71.82 秒 | `build/p3-final-targeted.txt` |
| 最后授权回收修复后 | 完整重建成功，三个业务函数 QTest 5/5（含 init/cleanup）；MCP 业务目标 1/1，65.52 秒 | `build/p3-grant-capacity-green.txt`、`build/p3-final-mcp-business.txt` |
| 新模块 QTest | 授权 6/6、读取/搜索 23/23、证据 17/17、历史契约 6/6；录制 UI 9/9 | 各 CTest 指定的文本输出文件 |
| 协议互操作 | 官方 MCP SDK 2025 + raw HTTP 2026，12 个 input/output Schema 校验通过 | `build/p3-interop.txt` |
| UI 预览 | 独立假录制/本机意图，已检查共享表单 | `build/p3-history-sharing.png` |

ConPTY 是此前本机失败清单中的目标，本次未改动该生产路径。此次 CTest 该项 stdout 为空，不能据此声称某条具体断言或后续检查通过；本轮未重新定位这个已记录目标，也未放宽其断言。

构建和详细日志保留在 `build/p3-*.txt`、`build/history-p3-smoke/`；Windows QTest 详情从 CTest 指定的文本输出文件读取。独立审查确认两项重要问题（部分授权 gap、协议缓存绕过搜索页序/期限），均有复现与修复用例；最后资源回收及异步导入由实现者复核并验证。

已确认环境限制：沙箱临时目录导致历史私有存储不可用和文件夹写入失败；主程序 `/Zi` 编译在沙箱内出现 C1902 PDB 管理器错误。对应步骤在沙箱外重试，不能把这些沙箱失败当成功或产品回归。

## 待验收与暂缓问题

- Windows 本机以外平台、真实 UART/SSH、真实终端性能、断电及不可中断 I/O 的硬退出上限未验收。
- 全局/多 Principal 饱和与清理竞争的完整压力矩阵仍留 P8；现有专项覆盖单 Principal 名额、期限、取消、重试、输入/响应/映射上限，不把它们扩大为全资源压力验收。
- 生产 DiagnosticResult 来源、跨客户端证据共享、证据磁盘持久化不在本次首版交付内；默认拒绝越权/持久化请求。
- 独立审查的一项次要问题暂缓：投影上限的 projection_gap 已明确 `complete=false`、`PROJECTION_LIMIT`，当前范围是触发位置的零长度标记，尚不能定位整个被省略的缓冲行。不得用它宣称完整无故障；可改用 raw_bytes。
