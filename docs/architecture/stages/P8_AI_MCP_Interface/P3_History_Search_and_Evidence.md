# mcp/P3 历史搜索、证据与离线授权实施计划

> For agentic workers：实施使用 superpowers:executing-plans；按任务先验证失败边界再实现。

**Goal:** 授权后分页读原文、跨块搜索和生成可验证证据，支持 Session 关闭后的显式离线访问。
**Architecture:** 资源授权→只读提交快照→有界后台扫描→原始区间 EvidenceRef；观察 capture 与持久 HistoryCapture 使用不同 ID/游标。
**Tech Stack:** C++17、Qt Core、P2 ChunkStore/Index、Qt Test、现有 MCP adapter。
**Spec:** [总体架构](00_Overall_Architecture.md)；依赖 [P2](P2_History_Recording.md)，接口供 P4/P5/P6 使用。

> 2026-10-23：首版原文/搜索、离线授权、期限内存证据和三个历史工具已实现。下列复选项只表示已核对的实现与专项范围；生产诊断来源、组合报告、持久证据、完整压力矩阵和其他平台仍单列。命令与当前成绩见 [实施记录](P3_Implementation_and_Verification.md)。

## Global Constraints

Capture 元数据仅记录来源，不继承 Session grant。关闭标签不撤销显式持久授权；客户端移除/令牌轮换/撤权/缩范围使旧 cursor 失效。EvidenceRef 与 cursor 都不是授权凭证。导入仅经本机文件选择/明确共享资源，不接受任意本机路径。

所有模块必须遵守[总体架构](00_Overall_Architecture.md)§2.1～§2.4的低耦合、高内聚规则；下列专项结构验收是本阶段退出条件。

## Review Focus

- 跨块 UTF-8/ANSI/CR 投影导致证据偏移错误：T2 原始 record/byte 映射。
- 扫描预算耗尽却声称无命中：T2 complete=false + 高水位 cursor。
- 原始日志删除、授权缩范围、客户端轮换：T1/T3 全部解析路径重验，不泄露。
- 未录制时在线诊断证据被偷偷持久化：T3 默认仅期限内存并标 expiresAt。
- 组合报告带入其他资源：T3/T4 每来源授权，不能凭 taskId/captureId 猜测访问。

## 文件与契约

2026-10-24 当前接线：`src/resources/ResourceSharingService.*` 组合授权事务、历史查询与证据发布，`HistoryCapabilities.*` 提供三领域端口；应用服务独立链接 Qt Core/领域库。`HistoryCapabilityAdapter` 保留协议投影和具体录制适配，UI 经 Application 的应用实例发出共享意图。History/Evidence 核心之间不建立反向实现依赖。具体 owner、身份/持久化端口与各轮回归见 [架构修复记录](P3_Architecture_Fixes_2026-10-24.md)。

新增 src/history/HistoryReadService.*、HistorySearchService.*、HistoryProjectionDecoder.*、HistoryAuthorizationStore.*；History仅持有记录读取/搜索与capture授权。
新增 src/evidence/EvidenceTypes.h、EvidenceStore.*、ImportedResourceStore.*、EvidenceSourceReader.h、EvidenceAuthorization.h、CMakeLists.txt；来源适配器在集成层注册，不让Evidence核心链接History或诊断实现。
修改 P1 Registry/McpCapabilityAdapter、src/mcp/protocol/ToolCodec.*（仅追加新工具）、McpAccess.* 或独立资源授权适配、HistoryCaptureModel/Panel。
新增 tests/history/HistorySearchTests.cpp、HistoryAuthorizationTests.cpp，目标 novaterm_history_search_tests；Evidence测试置 tests/evidence/EvidenceTests.cpp，独立目标 novaterm_evidence_tests；MCP 新工具兼容用例加入 tests/mcp/ContractsTests.cpp/McpTests.cpp。

接口：
- readAsync(HistoryReadRequest, ResourceContext, ReadCallback)：原文字节片段、byteCount、records/gap/truncated/nextCursor；仅协议边界转 base64，核心不依赖 JSON。
- searchAsync(HistorySearchRequest, ResourceContext, SearchCallback)：命中原始范围、evidenceIds、nextCursor、complete。
- HistorySearchRequest.mode：raw_bytes 或 text；text 首版明确 UTF-8，非法序列替换并保留原始偏移，不自动猜编码。
- EvidenceRef：source(kind)、resourceId、原始范围、contentHash、采集时间、权限归属、来源版本/状态、expiresAt/persistence。
- EvidenceStore::createAsync/resolveAsync(ref, ResourceContext) → 已授权有界 bytes/元数据或 EVIDENCE_EXPIRED/权限拒绝；来源适配器异步读取。
- ResourceGrant：Principal、资源代际、允许 record/时间范围、独立 accessVersion。
- ImportedResource：显式共享后分配 ID、hash/大小/代际；解析无任意文件系统能力。

已新增三个 MCP 工具：novaterm_list_captures、novaterm_read_history、novaterm_search_history。新增 input/output Schema 已冻结为 history-tool-catalog-2026-10-23.json；旧九工具 Schema 不改。cursor 绑定资源/代际/范围/授权/高水位/期限。

## T1：离线授权和读取

**Consumes:** P2 committed range、P1 trusted principal；**Produces:** 资源 grant 和分页原文。
- [x] 测 closedSessionExplicitCaptureGrantWorks、sessionGrantDoesNotAuthorizeHistory、rotatedTokenInvalidatesCursor、rangeShrinkRejectsOldCursor。
- [x] 实现独立授权存储与版本推进；列举/读/搜索/证据解析共用资源验证，未来 Analyzer 复用该端口。
- [x] 原文字节返回 base64 和明确 byteCount，单响应默认≤64KiB，按最终编码预算裁剪，不把任意二进制直接塞 JSON UTF-8。
- [x] 页面保持固定已提交高水位；缺失/清理范围明确报错或 gap，不推进 cursor 隐藏缺口。
- [x] UI 增加离线共享范围/撤销入口，重启仅保留用户选择的持久授权，不重新继承临时 Session 权限。

## T2：有状态投影和有界搜索

**Consumes:** immutable chunks/high-water mark；**Produces:** 带原文定位命中和可取消游标。
- [x] 覆盖跨记录 literal、splitUtf8AndAnsi、crProjectionMapsToRaw、matchDoesNotCrossGapOrBoot、预算内续扫和初始取消零 I/O；真实终端并行输入性能按 P8 验收。
- [x] 在跨 record/chunk decoder 保留 UTF-8/控制状态，CR 覆盖/ANSI 去除规则版本化；匹配区间映射回原 recordId/byteOffset。
- [x] raw_bytes 做精确字节匹配；text 做 UTF-8 投影 literal 匹配，首期不开放任意正则。
- [x] 全局扫描并发2/Principal1，单页扫描2秒或16MiB；预算先到返回 complete=false 和同高水位续扫位置。
- [x] query、命中数、返回文本、队列和取消检查均有上限；不一次复制几GB为 QString。

## T3：证据与导入资源

**Consumes:** 历史命中、在线任务结果或用户导入；**Produces:** 可解析 EvidenceRef/小摘要。
- [x] 测 expiredSourceDoesNotClaimVerifiable、memoryEvidenceGoneAfterRestart、diagnosticEvidenceRequiresOwner、importedPathNotAcceptedFromModel。
- [ ] 三类来源的通用契约及 fake 均已验证，历史/导入已接线；生产 diagnostic_result 适配与 Recipe 结果发布随 P4。
- [x] shared_terminal 来源标记及 Recipe/目标/环境/截断元数据已由核心与 fake 验证，不假定隔离 stdout；生产消费者随 P4。
- [x] 仅存期限内存证据；显式返回 persistence=memory/expiresAt，不自动开启录制。
- [ ] 首版默认拒绝跨客户端共享与持久证据请求；后续显式权限和持久化实现另验收。已共享离线资源按自身版本控制。
- [x] 本机显式导入校验大小/hash，解析只接受资源 ID；删除联动引用失效，报告 EVIDENCE_EXPIRED。Analyzer 属 P5。

## T4：MCP 与工具目录集成

**Consumes:** T1～T3；**Produces:** 三个历史工具和兼容证据。
- [ ] 新目录兼容和仅列授权 capture 已由 fixture/HTTP 测试验证；组合报告尚未实现，reportCannotMixUnauthorizedEvidence 随报告消费者验收。
- [x] 经 P1 adapter 注册已实现工具，稳定 tools/list，不按活动标签动态删工具。
- [x] 保持 HTTP/认证/双协议/取消；后台扫描不运行 GUI 同步磁盘I/O。
- [x] 跑新搜索目标、MCP contracts/业务/wire；记录未覆盖平台和资源上界。

~~~bash
cmake --build build --config Debug --target novaterm_history_search_tests novaterm_evidence_tests
ctest --test-dir build -C Debug -R "novaterm_(history_search|evidence)_tests" --output-on-failure
ctest --test-dir build -C Debug -L mcp --output-on-failure
~~~
退出：原文可追溯、授权独立、缺口/不完整语义真实，P4/P5 不必读取整个日志来生成报告。

## 低耦合、高内聚验收

- History读/投影/搜索围绕原始记录与偏移内聚；Evidence/导入是独立应用模块，不能继续作为History容纳所有来源的工具集合。
- Evidence核心通过资源授权与只读来源解析端口工作；HistoryCapture、DiagnosticResult、ImportedFile适配器由组合根注册。核心不includeHistoryChunkStore、诊断任务或Agent消息。
- [x] 独立链接History搜索和Evidence测试，三个fake来源验证统一hash/权限/到期契约；Evidence核心不依赖History reader具体实现。Analyzer 随 P5。
- [x] 所有资源回收/引用失效仍由所属owner产生事实；不靠共享可变缓存或“证据ID可猜即查”跨模块访问。
- [x] novaterm_evidence 只暴露EvidenceTypes、来源读取/授权端口及服务；来源适配在集成目标装配，不反向链接History/诊断实现。

## 补充契约：搜索续页与有界状态

首版选择服务端SearchSession，避免把decoder内部状态放进可篡改cursor或每页从日志头重扫。SearchSession由HistorySearchService拥有：Principal、资源/授权版本、固定提交高水位、query/mode、投影版本、原始位置、UTF-8/ANSI/CR状态、匹配尾部及映射和单调期限。实现采用每页随机不透明句柄，服务端仅接纳下一页或上一页精确重试，页序和完整状态均不接受客户端重写；每次读取仍重新授权。

cursor绑定query及投影版本；更换query/mode返回CURSOR_MISMATCH，状态到期/重启/淘汰返回SEARCH_CURSOR_EXPIRED，资源删除返回EVIDENCE_EXPIRED。授权撤销按权限拒绝，不把游标有效当授权有效。这些新能力错误已实现，不修改旧九工具错误语义。

设计初值：搜索状态全局16个/Principal2个，单状态最多1MiB、空闲TTL60秒、绝对TTL5分钟；达到容量拒绝新搜索，不淘汰正在执行的页。扫描worker并发仍为全局2/Principal1。单query最多4KiB；每页最多128命中且最终编码≤64KiB，扫描仍为2秒/16MiB先到。1MiB包含decoder、映射、匹配尾部及上一页缓存；所有租约受绝对TTL限制，取消/撤权/完成释放扫描资源。

完成后释放decoder/扫描租约，仍可在原TTL内保留最后一页缓存供精确重试；该终态缓存仍计入状态数量和内存额度，不续租。

同一页只运行一次，精确重试返回有界上一页缓存；重试必须保持生成该页时的有效命中数和响应预算，否则返回 CURSOR_MISMATCH，合法推进下一页仍可调整预算。初始即终结且没有重试句柄的搜索立即释放状态。相同SearchSession并发续页返回Busy，旧于缓存的页序号拒绝。不因网络重试推进两次。budget耗尽在UTF-8、ANSI序列或匹配中间时保留状态；EOF、gap、boot边界各有明确flush/reset，命中不跨gap/boot。原始范围可为多个片段，不假定CR覆盖后的文本对应单个连续字节区间。

ANSI 去除包含 ESC intermediate/final、CSI、OSC 与 DCS/SOS/PM/APC 控制字符串；状态跨 record 保存，控制字符串至 ST 结束，CAN/SUB 可取消序列，仍受原 4 KiB 序列上限限制，不模拟完整终端光标定位。

text投影首版按行缓冲，单行及单控制序列分别限制64KiB和4KiB。遇超限不无界累积：返回projection_gap及原始区间、停止该次搜索并释放状态，complete=false、nextCursor=null、reason=PROJECTION_LIMIT，提示可用raw_bytes新建搜索。complete只在固定高水位全部扫描且无投影遗漏时为true；尾部unknown_tail另由sourceCompleteness返回，不能被complete覆盖。

- [x] T2增加pagedMatchesEqualContinuousScan、budgetInsideUtf8AnsiAndLiteral、changedQueryRejectsCursor、cursorExpiryDoesNotRestartScan、duplicatePageDoesNotAdvance、longLineProjectionLimitIsExplicit。
- [ ] 覆盖状态总内存/Principal限额、租约到期与清理并发；查询上下文不在GUI同步重建。
- [x] T3将sourceCompleteness、projectionVersion和多片段原始范围带入EvidenceRef；未来报告消费者须保留不完整状态，不能解释为“没有故障”。
