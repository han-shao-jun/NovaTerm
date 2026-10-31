# MCP工程验证记录 2026年10月31日

这里保存代码审查、实现说明、构建、代码级单元/集成测试、消毒器和提交验证。
使用真实MCP工具的操作、响应、副作用与复测结论仅在[B4工具测试台账](../B4_Test_Report.html)维护。
原混合段落按发生时内容保留用于追溯，其中工具上下文不作为第二份现行台账。

<a id="b4-01-code-fix"></a>
<a id="fix-detail"></a>


### B4-01 代码修复与分轮验证（2026-10-31）

用户要求根据测试结果改代码。根因是libvterm在宽字符放不进行尾时留下未打印空格位，
历史入库将该空Cell编码为空格。新增内部setwrappadding回调与Cell的wrap_padding位，
只在真实宽字符提前换行时标记空单元；标记随移动/resize保留，写入/擦除清除。
VTAdapter只过滤明确标记的空单元（resize后可能在行中），不按零码点猜测删除。
没有修改MCP分页协议、输入策略或文件/提权功能。

- 原始自动化RED：wideWrapLogPreservesText的195/196/197列×4/512/65536字节预算及空格对照，
  QTest **3通过/10失败**，/tmp/novaterm-b4-01-red.txt。
- 初版仅裁软行末尾零格，专项14/14；独立审查发现光标跳格经resize移到行尾会误删。
  新增softWrapPreservesCursorGapAfterResize真实RED **2通过/1失败**，
  /tmp/novaterm-b4-01-review-red.txt。该初版已被显式标记替换，不作为最终实现。
- 标记方案专项15/15；追加resize/覆盖四场景QTest6/6（含初始化与清理），
  /tmp/novaterm-b4-01-marker-green.txt、/tmp/novaterm-b4-01-lifecycle.txt。
- 最终Release主程序及相关目标构建成功，/tmp/novaterm-b4-01-final-build.log。
  Core/terminal_ops/scrollback CTest **3/3，8.04秒**，分别QTest **118/58/31通过**，
  /tmp/novaterm-b4-01-final-ctest.log。
- MCP日志两代SSH/UART、capture展开与采样时间专项 **8/8，0.752秒**，
  /tmp/novaterm-b4-01-final-mcp.txt；不是完整MCP重测。
- 独立只读复审未见高可信新缺陷。使用本轮构建的Core在196×56下回放原expected.txt，
  LF转CRLF并追加100行filler：512字节预算10页、65536字节预算2页，返回的样本前缀
  均与完整 **4464字节**预期逐字节一致。驱动/tmp/novaterm-wrap-replay-review.cpp，
  根代理复执行证据/tmp/novaterm-b4-01-original-replay.log。

原RUN-04的8通过/1失败保持历史事实；本节是修复后的软件与原样本回放成绩，
未重启用户运行实例，不冒充更新后的实际MCP会话验收。未连接设备、未跑无关全套、
未测其他平台/GPU长稳、未提交/推送，版本保持0.2.107。B4总体验收仍未完成。

<a id="font-detail"></a>
<a id="font-fix"></a>

### 提权凭据窗口字号调整（2026-10-31）

用户截图显示提权凭据子窗口说明、状态和字段标题异常放大。原因是这些ElaText
沿用默认28px；主MCP设置页已显式设置字号。McpSettingsDialog现将该子窗口说明及
存储状态设为12px，sudo/su字段标题设为13px，沿用既有设置页层级；不改凭据逻辑。
Release主程序和MCP测试目标重建成功（/tmp/novaterm-elevation-font-build.log）。
实际弹窗存储状态、提权可用性和模型保持相关QTest **9/9，0.605秒**，
/tmp/novaterm-elevation-font-tests.txt；保留offscreen不支持propagateSizeHints警告。
没有新增仅断言字号常量的测试；本轮未做高DPI真屏截图复验，用户旧运行实例待重启。




<a id="linux-writer"></a>
<a id="linux-writer-detail"></a>

## Linux 安全本地写入后端

用户要求实现Linux Writer。复用LocalFileWriter及现有SFTP owner/worker/下载接口，20工具不变。
逐级openat固定无链接目录；UUID临时名O_CREAT/O_EXCL/NOFOLLOW/CLOEXEC、0600、独占协作flock。
写入循环处理短写/EINTR，64MiB文件与64KiB块上限；长度/IO/安全失败后poison禁止发布。
发布前fsync及回读SHA256比对已收到的字节流；显式覆盖还固定原目标可读fd并保存/复核摘要。
初始缺失永远RENAME_NOREPLACE，初始已有且明确overwrite=true才原子替换；不会先删目标。
目录/临时名/原目标身份在发布前重新复核，已发布结果不因取消删除；临时名被替换时cancel
返回false保留清理不确定事实，恢复原归属后可再次清理。析构/move沿RAII释放。

### 支持范围与边界

- 支持ext2/3/4、XFS、Btrfs、tmpfs、overlayfs类型且要求renameat2原子能力；
  其他类型（含NFS/CIFS/FUSE）和能力缺失明确拒绝，无降级。实际本机测试statfs为ext系列。
- 覆盖原目标必须可读、普通单硬链、有写权限位、≤64MiB；读取可能更新atime。
  新目标0600，不继承旧执行位/ACL。元数据摘要不把协议contentVerified变true。
- Linux固定fd不阻止目录重命名，采用发布前名称/身份链复核；最终检查到rename/unlink
  仍非CAS，目录写入者必须可信。flock只是协作锁；不检查目录ACL/owner。
- 成功表示发布，不承诺断电持久性或同步I/O硬截止。macOS继续明确拒绝。

### 真实分轮验证

1. 新增Linux回归并启用原可移植用例。原始本地I/O **37pass/28fail**，
   /tmp/novaterm-linux-writer-red.txt；其中一项是既有Reader把根目录/误判非法的旧夹具。
   SFTP下载复制专项 **5pass/17fail**，/tmp/novaterm-linux-writer-sftp-red.txt。
2. 初版Writer打通复制链路 **22/22**（含初始化/清理）；本地临时内容同长度改写曾漏检，
   首轮夹具std::get触发bad_variant_access，/tmp/novaterm-linux-writer-green.txt。
   改为明确断言后单次复测3/3，说明元数据是否变化受时序影响，不改写首轮失败。
   独立stat实测同长度改写可保持相同mtime/ctime，增加暂存内容摘要复核。
3. 本地I/O随后64pass/1fail，唯一是旧Reader根目录断言，保留源变化拒绝，
   改为验证合法/实际读取和非法//拒绝；该夹具失败在初始RED已存在，生产Reader不改。
4. 独立审查发现原覆盖目标同长度改写也可保持时间戳；新增真实RED **2pass/1fail**，
   /tmp/novaterm-linux-writer-target-red.txt。增加固定原目标有界摘要，联合临时篡改
   GREEN **4/4**，/tmp/novaterm-linux-writer-target-green.txt。独立复审100次拒绝复现通过；
   审查者另一次10000轮运行主动中止，不计通过。
5. 最终Release主程序和相关目标构建通过，/tmp/novaterm-linux-writer-release-build.log。
   相关CTest **3/3，2.87秒**，/tmp/novaterm-linux-writer-final-ctest.log；对应QTest报告
   本地I/O68、传输索引31、SFTP安全92通过。原测试文件中Windows-only条件分支未在Linux执行，
   不把总数解释为Windows后端也测过。新增测试含64MiB真实写入、原目标摘要上限和新权限0600。
6. 两代HTTP下载生命周期专项 **4/4，0.243秒**，/tmp/novaterm-linux-writer-http.txt。
   独立ASan/UBSan构建执行本地I/O **68/68，1.012秒**，退出0、stderr空，
   /tmp/novaterm-linux-writer-sanitizer.txt及同前缀sanitizer-stderr.log。

未连接板卡、未向SSH/UART发输入或文件，未重启用户运行实例，未跑无关全套/其他平台，
未提交/推送、版本保持0.2.107。Linux Writer从“未实现”改为“软件已验证、实机待验收”，
B4总体验收保持未通过。源码与全部平台差异的边界见[现行契约](02_Current_MCP_Contract.md)。

同步novaterm_download_file描述：Writer支持Windows和受支持的Linux本地文件系统，
Linux需renameat2，覆盖原目标须可读且≤64MiB，macOS不可用。Schema和工具数量不变。
主程序/协议契约/MCP目标重建成功，/tmp/novaterm-linux-writer-catalog-build.log；
接口契约CTest1/1通过，/tmp/novaterm-linux-writer-contracts.log；两代HTTP下载专项再次4/4，
/tmp/novaterm-linux-writer-http-final.txt。与此前3目标/消毒器成绩分轮保存，不冒充全套。

<a id="s15-fix-detail"></a>

## 用户范围与最终结论

用户要求review测试fail相关代码并修复，随后要求复测，最终明确“只复测失败项”。
本节保留原RED、期间已结束的扩大检查及最后定向结果，不将旧成绩改写成同一轮全通过。
S15原失败已在当前运行实例复验通过；UART停止时无控制TTY的环境限制单列，未将Ctrl+C
提交改成确认终止，也未修改板卡TTY/getty配置。产品版本保持0.2.107，无提交/推送。

## 审查范围与发现

qt-cpp-review确定性扫描后，三位独立只读代理分别覆盖模型/API、生命周期/线程、安全/性能
共六领域，未宣称六个独立代理同时运行。范围是ObservationState、InputCapabilities、
TerminalInteraction、TerminalTasks、SessionTerminalController及对应测试；现有机械风格
提示不作为本轮行为问题修改。lint：/tmp/novaterm-s15-review-lint.txt。

1. S15（置信度100）：ObservationState每次受理覆盖唯一Replay，后续回复/按键使旧operationId
   不再被识别；Task索引只知道任务，旧普通输入可被重新当作任务提交。真实新增8数据行
   全部失败（QTest2pass/8fail），/tmp/novaterm-s15-red.txt。
2. 同步销毁链：onChanged回调可能销毁TerminalInteraction，原watch继续scheduleFlush，
   输入端口还可能继续访问已销毁state/port。去掉保护的独立ASan对照真实触发
   heap-use-after-free，/tmp/novaterm-s15-asan/unguarded-stderr.log；保护版通过。
3. UART stop：代码只请求Ctrl+C，完成证据不足时保持unknown/terminationConfirmed=false，
   与设备TTY_NR=0/TPGID=-1下脚本继续运行的观察一致；未确认存在“误报已终止”的代码缺陷。

## 实现

- 在共享ObservationState维护固定600000ms的已受理ID历史，client/session/epoch/grant/
  cacheGeneration/operation为键，不含control/user/prompt。后续输入或release/reacquire
  不解除旧ID保护；cache/epoch/grant新绑定及撤权沿已有失效规则，不升级旧票据。
- 最近操作精确Replay继续兼容；更早ID同参数/异参数均冲突，不把Controller当前另一个操作
  的事实当作旧结果。已有任务自己的query/精确重试仍优先返回原任务记录。
- 历史每client/session4096条、全局16384条；满额拒绝新输入，不驱逐未到期记录。
  当前owner另有release余量1/作用域、全局32；已经没有控制权的空release不能使用该余量。
- 输入前先预留ID和额度，未受理仅回滚自身预留；submit=true即提交，之后交付失败不回滚。
  pending不会被TTL误清；首次受理起算，重试不续期；serial阻止外层迟到回调覆盖较新Replay。
- 回调复制后用QPointer检查owner；输入端口各可重入边界核对存活，预留回滚用weak历史引用。
  owner销毁或revoke移除预留后，不会复活状态或访问已析构对象。
- 原有缓存换代规则保留；记录有期限且应用重启不恢复，客户端仍不可盲重放未知结果。

## 分轮软件证据（不要相加为一次全套）

- 初始RED：8数据行失败，2pass/8fail；包含旧ID相同/不同参数、普通/任务入口、释放前后。
- 期间编译有两项夹具/类型修正：Revision没有qHash，改散列其value；测试SessionId需显式
  NovaTerm::Mcp限定。第一次调用未完成构建的新测试名得到Function not found，保留
  /tmp/novaterm-s15-green.txt；不得将该次当作功能RED。
- 新容量测试最初复用了最新release的ID来测试“新意图”，引发bad_variant_access；
  最新事实仍可判冲突，改用真正新ID验证期限释放容量，未放宽生产判重。
  /tmp/novaterm-s15-state-green.txt保留原失败。
- 完成后的相关CTest3/3（1.94秒），状态/任务/能力入口分别QTest13/29/5通过，
  /tmp/novaterm-s15-final-ctest.log；之后又补了任务→普通输入反向数据行。
- 两代协议SSH/UART四数据行：首轮夹具未开输入权限2pass/4fail，修正为显式授予测试输入
  后6/6（0.529秒），/tmp/novaterm-s15-http.txt及http-green.txt；生产权限检查未放宽。
- 独立ASan/UBSan：改动的ObservationState/InputCapabilities/TerminalInteraction及任务测试
  使用消毒器编译，其他依赖链接原Release库。去保护对照发现UAF；保护版任务29/29，
  /tmp/novaterm-s15-asan/guarded-tests.txt，stderr空。不是全应用/全部依赖消毒器验收。
- 用户收窄范围前已启动并结束的完整MCP：160pass、0fail、3个Windows-only skip，
  41.737秒，/tmp/novaterm-s15-full-mcp.txt；收到“只复测失败项”时已经结束，没有继续扩大。

## 最后仅失败项复测

- S15定向olderInputIdCannotBeResubmitted：14/14（12个数据行加初始化/清理），
  0.617秒，/tmp/novaterm-s15-failed-only.txt。
- 同步销毁ASan定向inputOwnerMayBeDestroyedDuringAcquisition：3/3（含初始化/清理），
  0.054秒，/tmp/novaterm-s15-asan/failed-only.txt，stderr空。

实际MCP工具复测详见[原场景与反向核验](../B4_Test_Report.html#s15-fix-detail)。

<a id="failed-retest-02-detail"></a>

## RETEST2代码级定向复测

S15定向QTest14/14（12个数据行加初始化/清理），615ms；同步销毁ASan/UBSan保护版
定向3/3（含初始化/清理），48ms，stderr为空。
实际工具调用记录见[S15工具复测与UART中断](../B4_Test_Report.html#failed-retest-02-detail)。

### 软件环境与命令


应用0.2.107，实例b8351a9a-246c-48b9-b71c-3524d68fd290，PID93005；
/proc/93005/exe和build/Release/bin/novaterm的设备号66308、inode15117040一致。
本轮复用现有Release及已构建ASan保护版，没有重新构建；不是全应用/全部依赖消毒器验证。
ASan保护版来自上一轮独立构建；之后TerminalTaskTests新增过反向ID数据行，本轮ASan仅运行原销毁函数，
不把该旧ASan二进制当作新增反向测试的验证。源码与二进制摘要、mtime保存在binary-source-manifest.json。

```sh
QT_QPA_PLATFORM=offscreen build/Release/bin/novaterm_mcp_terminal_tasks_tests olderInputIdCannotBeResubmitted -o /tmp/novaterm-failed-retest-kqt_ruyu/s15-qtest.txt,txt
QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 /tmp/novaterm-s15-asan/tasks-guarded inputOwnerMayBeDestroyedDuringAcquisition -o /tmp/novaterm-failed-retest-kqt_ruyu/lifetime-asan.txt,txt 2> /tmp/novaterm-failed-retest-kqt_ruyu/lifetime-asan-stderr.log
```

<a id="commit-02113-detail"></a>

用户要求创建Git提交并推送Gitee。按AGENTS.md每次提交递增PATCH，第三方源码单独提交：

| 版本 | 提交 | 范围 |
| --- | --- | --- |
| 0.2.108 | edc27bb | libvterm显式wrap_padding及回调 |
| 0.2.109 | 598b558 | VTAdapter宽字符日志填充过滤与Core回归 |
| 0.2.110 | 998302d | Linux安全LocalFileWriter、目录说明及下载回归 |
| 0.2.111 | b53d5e3 | S15操作ID历史、回调销毁保护及回归 |
| 0.2.112 | c7f48cc | MCP提权凭据窗口字号 |
| 0.2.113 | 本批文档收口提交 | HTML唯一台账、正式契约、已删除过程文档的Git历史链接及导出脚本 |

0.2.108工作区主程序与相关测试目标构建通过。此时工作区已含本批全部修复，
不把工作区测试冒充各拆分提交独立检出验证。相关CTest10/10（11.77秒）：
core、terminal_ops、scrollback、mcp_local_file_io、sftp_session_safety、mcp_file_transfers、
mcp_observation_state、mcp_terminal_tasks、capability_entry、mcp_contracts。
另轮MCP定向18/18（1.892秒）：两代SSH/UART历史输入ID、两代HTTP下载、精确日志及两组提权设置。

最终0.2.113主程序、MCP业务/契约目标再次构建成功；契约CTest1/1（0.02秒），
两代SSH/UART判重与下载协议定向8/8（0.701秒）通过。不是完整MCP或全套CTest重跑。
HTML确定性再生成一致，87用例/20工具映射保留，内部锚点、历史记录保留及git diff --check通过。
待删除历史Markdown的活动引用改指向Gitee提交411ec15中的原文件；历史验证事实不重写。

日志均为本机临时证据：/tmp/novaterm-commit-02108-build.log、
/tmp/novaterm-commit-02108-related-ctest.log、/tmp/novaterm-commit-02108-mcp-focused.txt，
以及/tmp/novaterm-commit-02113-build.log、/tmp/novaterm-commit-02113-contracts.log、
/tmp/novaterm-commit-02113-protocol.txt。构建产物、日志、临时凭据目录均不纳入Git。

未重启用户实例；前面的真实MCP证据仍属于0.2.107运行实例，不能写成0.2.113实机重测。
本轮提交阶段未再连接设备，不改板卡环境；UARTRE3停止未达期望、高DPI真屏、其他平台和B4剩余矩阵仍未验收。

<a id="mcp-error-handling-review"></a>

## 范围与总体判断

用户要求review MCP错误处理并修复问题。本轮扫描src/mcp下98个C++文件，三位独立只读代理
分别覆盖模型/契约与API、生命周期与线程、错误校验与质量共六领域。确定性lint输出171条
（PAT117、DEP42、HDR11、TMO1），没有将这些机械提示批量改写；完整输出保存在
/tmp/novaterm-mcp-error-review-lint.txt。深审定位到以下六类实际问题。

Result<T>/Error、统一错误码与unknown事实的总体结构合理；本轮收敛错误转换并修复可达失败路径，
没有替换为异常驱动拒绝、增加工具、改变正常响应Schema或放宽身份/权限检查。

| 问题 | 等级 | 复现与修复 |
| --- | --- | --- |
| 观察完成、替换、撤权回调销毁owner后继续访问 | P1 | 三条独立ASan UAF；在invoke/deliver/release边界核对QPointer |
| 任务撤权/期限释放控制后的owner销毁链 | P1 | 任务撤权ASan UAF；保护refresh/sweep/result/query/stop/revoke/capabilities外层，期限先记录unknown再释放 |
| 能力Cancelled/DeadlineExceeded/Busy被吞成SessionNotAvailable | P2 | read/input四行真实RED；CapabilityAdmission统一转换，额度Busy保留可重试/250ms，安全类拒绝保持隐藏 |
| SFTP内部失败被投影为取消，外层总期限丢失原因 | P2 | worker timeout/path_changed两行RED，外层期限另1行RED；保存首个内部abortError，失败不伪装取消，published/unknown仍优先 |
| 上传确认锁竞争不可重试，存储错误误报繁忙 | P2 | 两行RED；真实锁竞争可重试250ms，非普通锁路径/符号链接及其他存储错误为UPLOAD_MEMORY_UNAVAILABLE；额度等待到最早保留项到期 |
| 历史搜索wire预算2秒早于业务12秒 | P2 | 两代协议4行RED；共享业务预算12000ms，wire保留5000ms余量，真实到期仍取消且明确DEADLINE_EXCEEDED |

上述缺陷均有真实回归或ASan复现，非仅静态推断。重复代码只在错误转换和搜索预算处收敛，
没有顺带处理退役命令名称、机械风格或未证实的异常路径。

## 分轮RED与中间失败（保留原成绩）

- 上传确认：2pass/2fail，锁竞争retryable错误与目录占锁路径错误分类分别失败。
- SFTP终态：5pass/2fail，超时/路径变化误为cancelled；用户取消、已发布、unknown对照通过。
- 外层总期限：注入单调时钟后2pass/1fail，实际把失败误作cancelled；没有等待真实120秒或改生产期限。
- 拒绝转换：2pass/4fail，read/input各自deadline/cancelled均误报SESSION_NOT_AVAILABLE。
- Wire首轮2pass/4fail，其中2026两行是夹具缺clientCapabilities；补齐协议元数据后
  再次2pass/4fail，四行均在2001ms错误超时，生产参数校验未放宽。
- 独立ASan/UBSan：观察完成、观察替换、观察撤权、任务撤权四个独立进程均触发heap-use-after-free。
  仅TerminalInteraction、TerminalTasks、ObservationCapabilities与任务测试使用消毒器编译，
  其他库为Release依赖，不称全应用或全依赖消毒器验证。
- 首轮扩大相关CTest2/4：锁目录测试仍失败（Qt将目录也报告LockFailedError），补查路径类型；
  SFTP两条旧连接失效断言仍期待cancelled，改为明确failed/PERMISSION_DENIED，保留其他事实断言。
- 第二轮Wire/上传确认/SFTP相关CTest3/3（4.26秒）。额外期限销毁与能力Busy专项5/5（185ms）。
- 主程序与25个相关测试目标构建成功。首轮相关CTest24/25（64.56秒）；完整MCP在该批
  158pass/2fail/3平台skip（39.263秒），两代HTTP下载夹具同样将SSH断连期待为cancelled。
  追到真实断连触发后改为精确failed/PERMISSION_DENIED，并保留未发布、进度和不重复发送保护。

## 最终验证

- 仅重建并复测首轮失败的完整MCP目标：CTest1/1（39.04秒）；QTest 160 passed, 0 failed, 3 skipped, 0 blacklisted, 39018ms。
  其他24个已通过目标未重复执行，不拼成一次25/25。
- 最终ASan定向9/9（591ms），stderr为空；含新四个销毁场景、query/capabilities期限销毁两行
  及此前输入获取销毁回归。覆盖范围与初次四条RED分开记，不伪称每个补充案例都有独立RED。
- 三位只读代理复审修复及补充修改，未见新增高可信问题；正式MCP契约与使用说明同步。

本机证据统一前缀/tmp/novaterm-mcp-errors-：lock-red.txt、transfer-red.txt、timeout-red.txt、
mapping-red.txt、wire-red.txt、wire-red-corrected.txt、first-green-ctest.log、second-green-ctest.log、
extra-green.txt、final-build.log、final-ctest.log、mcp-retest.log、first-final-lasttest.log。
/tmp/novaterm-mcp-errors-asan/保留各*-red-stderr.log、最终final-green.txt、final-green-stderr.log及构建脚本。

没有连接真实设备、重启用户实例、修改板卡环境、运行无关全套或其他平台；没有提交/推送或改版本。
以前UART停止失败及B4剩余验收不因本轮软件修复变成通过，87条有效用例状态保持。

## 原工程验证元数据

以下为迁出时的历史元数据，不再作为工具测试成绩。

<a id="run-linux-writer"></a>

### Linux Writer软件验证

```json
{
  "key": "LINUX",
  "anchor": "run-linux-writer",
  "date": "10.31",
  "title": "Linux Writer软件验证",
  "score": "3 / 3",
  "unit": "相关CTest",
  "scope": "本地安全写入 + SFTP下载复制链路",
  "note": "两代下载专项4/4；ASan/UBSan通过。未连接真实设备，不替代B4实机验收。",
  "detailAnchor": "linux-writer-detail",
  "className": ""
}
```

<a id="run-mcp-errors-review"></a>

### MCP错误处理审查修复

```json
{
  "key": "ERR1",
  "anchor": "run-mcp-errors-review",
  "date": "2026-10-31",
  "title": "MCP错误处理审查修复",
  "score": "6类问题已修复",
  "unit": "软件回归；B4验收状态保持",
  "scope": "错误转换/终态原因/锁与期限/回调销毁",
  "note": "相关CTest首轮24/25，失败MCP目标复测1/1；ASan定向9/9。原RED和中间夹具失败分别保留。",
  "detailAnchor": "mcp-error-handling-review",
  "className": ""
}
```

<a id="evidence-fix"></a>

### 修复后的软件验证

```json
{
  "anchor": "evidence-fix",
  "key": "FIX",
  "title": "修复后的软件验证",
  "path": "/tmp/novaterm-b4-01-final-ctest.log",
  "note": "另见 /tmp/novaterm-b4-01-final-mcp.txt、/tmp/novaterm-b4-01-original-replay.log"
}
```

<a id="evidence-ui"></a>

### 提权窗口字号验证

```json
{
  "anchor": "evidence-ui",
  "key": "UI",
  "title": "提权窗口字号验证",
  "path": "/tmp/novaterm-elevation-font-tests.txt",
  "note": "构建：/tmp/novaterm-elevation-font-build.log；实际弹窗专项9/9"
}
```

<a id="evidence-linux-writer"></a>

### Linux Writer验证

```json
{
  "anchor": "evidence-linux-writer",
  "key": "LINUX",
  "title": "Linux Writer验证",
  "path": "/tmp/novaterm-linux-writer-final-ctest.log",
  "note": "另见 /tmp/novaterm-linux-writer-http.txt、/tmp/novaterm-linux-writer-sanitizer.txt；原RED及审查修复分轮保留。"
}
```

<a id="evidence-mcp-errors-review"></a>

### 错误处理RED/GREEN与ASan证据

```json
{
  "key": "ERR1",
  "anchor": "evidence-mcp-errors-review",
  "title": "错误处理RED/GREEN与ASan证据",
  "path": "/tmp/novaterm-mcp-errors-*",
  "note": "分轮日志和四条真实UAF见详细记录；最终ASan9/9。原始lint位于/tmp/novaterm-mcp-error-review-lint.txt。"
}
```

<a id="issue-font"></a>

### 提权凭据窗口字号

```json
{
  "anchor": "issue-font",
  "title": "提权凭据窗口字号",
  "status": "软件验证通过",
  "state": "partial",
  "description": "说明与状态 12px，字段标题 13px。修正默认 28px，凭据行为保持不变。",
  "stats": [
    {
      "value": "9 / 9",
      "label": "设置相关 QTest"
    },
    {
      "value": "Release",
      "label": "构建成功"
    }
  ],
  "note": "高 DPI 真屏截图尚待复验。",
  "noteClass": "muted",
  "links": [
    {
      "target": "font-detail",
      "label": "详细记录"
    },
    {
      "target": "evidence-ui",
      "label": "验证证据"
    }
  ]
}
```

<a id="issue-input-owner-lifetime"></a>

### 输入回调销毁后的悬空访问

```json
{
  "anchor": "issue-input-owner-lifetime",
  "title": "输入回调销毁后的悬空访问",
  "status": "已修复 · ASan定向通过",
  "state": "partial",
  "description": "同步onChanged回调可销毁TerminalInteraction，原路径继续访问已释放对象。复制回调、QPointer/端口存活检查和弱引用回滚保护已补齐。",
  "stats": [
    {
      "value": "UAF",
      "label": "去保护对照真实复现"
    },
    {
      "value": "3 / 3",
      "label": "失败项ASan定向复测"
    }
  ],
  "note": "未在用户运行实例中故意销毁服务；使用独立ASan/UBSan对照验证，其他依赖为原Release库。 RETEST2复用已构建保护版，定向3/3（48ms），stderr空；本轮未重新编译。",
  "noteClass": "muted",
  "links": [
    {
      "target": "s15-fix-detail",
      "label": "审查与修复详情"
    },
    {
      "target": "evidence-s15-fix",
      "label": "ASan证据"
    },
    {
      "target": "failed-retest-02-detail",
      "label": "再次定向复测"
    }
  ]
}
```

<a id="issue-mcp-error-handling"></a>

### MCP错误处理与销毁收口

```json
{
  "anchor": "issue-mcp-error-handling",
  "title": "MCP错误处理与销毁收口",
  "status": "已修复 · 软件回归通过",
  "state": "partial",
  "description": "保留拒绝/超时/取消原因，纠正锁错误与搜索预算，保护观察和任务的同步销毁边界。",
  "stats": [
    {
      "value": "6类",
      "label": "确证问题修复"
    },
    {
      "value": "24/25 + 1/1",
      "label": "相关CTest两轮，非单轮25/25"
    },
    {
      "value": "9/9",
      "label": "ASan定向"
    }
  ],
  "note": "没有实机复测；原UART环境问题和B4剩余判据保持。",
  "noteClass": "muted",
  "links": [
    {
      "target": "mcp-error-handling-review",
      "label": "审查与分轮验证"
    },
    {
      "target": "evidence-mcp-errors-review",
      "label": "证据"
    }
  ]
}
```

## 原混合判据摘要

以下状态是拆分前混入软件证据的历史口径；当前工具测试状态以B4台账为准。

| 用例 | 原状态 | 原范围 | 原证据 | 原缺口 |
| --- | --- | --- | --- | --- |
| A04 | 部分验证 | 本机 / SSH | 新构建真实MCP回放242行Unicode样本，21页348片段与4472字节预期完全一致；SSH日志5页与158行下载报告一致。 | UART全样本及其他输出/缓存组合待补，不能把已存源内容复测算作所有平台通过。 |
| P02 | 部分验证 | 本机 | Linux本机子集及Reader/Writer软件验证已完成；本轮没有Windows/macOS成绩。 | 各平台独立运行与真实设备验收；macOS Reader/Writer仍未实现。 |
| DF01 | 部分验证 | Linux / 真实SSH | 已确认运行当前构建；MCP下载到Linux新目标完成published=true，Writer实机通路可用。 | 非法目标/旧epoch等仅软件回归，本轮不重跑；其他支持文件系统组合另验。 |
| DF02 | 部分验证 | Linux / 真实SSH | 上传后下载1MiB随机二进制，本机源、板卡文件、下载文件SHA-256一致且本机逐字节相等。 开发流程两轮日志通过SFTP下载，与远端实际运行结果一致。 | 本轮随机文件正向通过；空文件及其他平台/设备组合仍待验收。 |
| DF03 | 部分验证 | Linux软件回归 | 默认不覆盖、精确事实索引和初始缺失竞争创建的软件回归通过。 日志下载精确重试返回原transferId和published事实。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| DF04 | 部分验证 | Linux软件回归 | SFTP复制worker的短读、提前EOF、增长、读错和源守卫拒绝场景通过。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| DF05 | 部分验证 | Linux软件回归 | 读取中取消、发布前撤销、关闭失败及发布后取消保留目标的软件回归通过。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| DF06 | 部分验证 | Linux软件回归 | 远端源属性守卫及本地非普通文件拒绝有软件回归，未连接真实SFTP设备。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| DF07 | 部分验证 | Linux软件回归 | Linux父目录/祖先被替换时拒绝发布；默认不覆盖竞争目标，临时归属不明不误删。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| OF01 | 部分验证 | Linux软件回归 | Linux本地覆盖true/false及同名目标软件回归通过，接口仍20工具。 用户一次确认overwrite=true后，初版首次发布与修正版替换均完成；同ID改overwrite=false返回TARGET_OPERATION_CONFLICT。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| OF02 | 部分验证 | Linux软件回归 | 初始缺失即使overwrite=true仍RENAME_NOREPLACE，竞争创建目标保留。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| OF03 | 部分验证 | Linux软件回归 | 原目标identity/size/mtime/mode/link变化拒绝；同大小同时间戳改写亦由摘要复核拒绝。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| OF04 | 部分验证 | Linux软件回归 | Linux覆盖目录/符号链接/多硬链/FIFO/只读目标明确拒绝，未测Windows句柄场景。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| OF05 | 部分验证 | Linux软件回归 | 本地发布前取消保留原文件，已发布后cancel不删除新文件；真实网络竞争待验收。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| OF06 | 未执行 | 远端SFTP | Linux Writer开发缺口已解除；本项远端故障/扩展组合仍缺完整实机证据。 | 在受控服务端执行丢响应或扩展能力组合，并分别核对上传/下载事实。 |
| OF07 | 未执行 | 远端SFTP | Linux Writer开发缺口已解除；本项远端故障/扩展组合仍缺完整实机证据。 | 在受控服务端执行丢响应或扩展能力组合，并分别核对上传/下载事实。 |
| OF08 | 部分验证 | Linux软件回归 | Linux替换新文件权限0600，不继承原0700执行位；ACL组合未验收。 覆盖后的脚本实际ls -li为-rw-------，通过显式sh执行；没有自动chmod或自动执行。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| F01 | 部分验证 | Linux软件回归 | Linux本地下载后端与原SFTP复制链路已打通；没有新增协议工具或连接owner。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| F02 | 部分验证 | Linux软件回归 | Linux非法路径、链接、非普通文件、临时替换、目录变化和同大小内容变更回归通过。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| F03 | 部分验证 | Linux软件回归 | Linux取消/析构/move清理与64MiB最大文件通过；磁盘满/真实断连/总期限尚未全覆盖。 同时上传/下载的同会话竞争返回FILE_TRANSFER_BUSY；先查询原下载完成，再原ID重试上传成功。 | 软件回归通过；重启新构建后在明确授权的真实SSH目标执行该用例全部判据。macOS与其他文件系统组合不据此通过。 |
| F04 | 部分验证 | 真实SSH / 上传与下载 | 传输字节数、两端published事实、板卡wc/sha256sum与本机逐字节比较分别核对。 脚本两版本机/远端SHA匹配；日志下载后对比结果，发布、内容与脚本退出状态分别记录。 | 仅本次SFTP单文件闭环；UART交付及覆盖后属性组合未验收。 |
| A14 | 部分验证 | 本机 / 软件回归 | 原宽字符多空格问题已在当前真实MCP实例复验：196列、含300双宽字长行的样本精确一致。 | 组合字符/emoji与其他列宽真实实例矩阵仍未全覆盖。 |
| S15 | 部分验证 | 当前构建 / 本机实机 + 两代协议软件回归 | 原跨工具重复ID失败已修复：本机原场景7/7、反向任务→普通输入2/2，均无额外副作用；最近精确重试及新ID正常，原始失败仍保留。 再次定向复测：原场景7/7、反向2/2，QTest14/14；本轮独立证据见RETEST2。 | 原失败项已通过；其他平台/真实SSH与UART的完整组合不据此全部验收。 |

## 原迁入摘要快照

# B4 测试结果与证据索引

[打开测试工作台](../B4_Test_Report.html#overview) · [用例结论](../B4_Test_Report.html#cases) · [执行记录](../B4_Test_Report.html#runs) · [问题与修复](../B4_Test_Report.html#issues) · [证据](../B4_Test_Report.html#evidence) · [历史原文](../B4_Test_Report.html#archives)

更新：2026-10-31。**B4未通过**。本页按用例整理当前结论，历史记录仅用于追溯。
当前主要验证Linux本机交互和UART只读；SSH/UART输入、文件交付、录制正向流程、
多平台与性能仍待验收。B4-01代码已修复，软件回归和原样本回放通过，
新构建的真实MCP会话复验尚未执行。

## 阅读方式

- 用例状态是完整判据的当前摘要。“部分验证”表示确有子证据，但并非整项通过。
- “待复验”表示已有代码修复证据，还缺真实运行实例复验；“环境受限”仍在验收范围中。
- 清单、当前结论、执行轮次和证据互相链接。每轮成绩独立，不能把子检查次数相加为覆盖率。
- 判据键：清单版本 / 用例ID / 子判据 / transport / 平台 / RUN_ID。
- 来源为v2清单87条有效用例；A07–A09、T01–T07等10个旧编号退役，原始成绩只存归档。



## 2. 执行记录



### R01 · 首轮安全子集
- RUN_ID：RUN-20261030-MCP-01；0.2.107 / Linux，本机无害交互、UART只读。
- 原始口径：70条中通过10、阻塞39、不适用18、未执行3；没有判为失败项。
- 当前解释：本机成绩不能算作SSH通过；旧授权与退役工具的判据不可直接继承。
- 主要事实：精确重试副作用一行；UART既有1082行；401/404认证边界。
- [原始报告](../B4_Test_Report.html#archive-01) · [证据位置](../B4_Test_Report.html#evidence-r01)



### R02 · 只读复测
- RUN_ID：RUN-20261031-MCP-02；0.2.107 / Linux。
- 8个只读子检查通过、0失败；UART10页1086行，UTF-8位置连续。
- 屏幕搜索、any/all和超时已核对。输入受工具审批策略阻塞，不归为产品失败。
- [原始报告](../B4_Test_Report.html#archive-02-03) · [证据位置](../B4_Test_Report.html#evidence-r02)



### R03 · 本机交互复测
- RUN_ID：RUN-20261031-MCP-03；环境调整后本机输入可受理。
- 12个交互子检查通过、0失败：输入、精确重试、旧条件拒绝、中文回复、按键、粘贴、
  35秒任务、停止重试和释放控制。完成任务退出0，停止样本退出130。
- SSH/UART输入、清空、人工接管、断流和文件传输没有在此轮验证。
- [原始报告](../B4_Test_Report.html#archive-02-03) · [证据位置](../B4_Test_Report.html#evidence-r03)



### R04 · 边界补测
- RUN_ID：RUN-20261031-MCP-04；仅本机终端。
- **8个子检查通过、1失败**。通过项：非零退出、等待超时后继续、release不终止、
  控制丢失保留unknown、分页位置、capture展开、搜索位置、65动作拒绝。
- 失败项：240行精确日志样本多1空格；历史失败保留，不能用后续修复改写此轮成绩。
- [问题与修复](../B4_Test_Report.html#issue-b4-01) · [详细响应](../B4_Test_Report.html#run04-detail) · [证据位置](../B4_Test_Report.html#evidence-r04)



## 3. 问题与修复



### B4-01 · 宽字符软换行多空格
- 关联用例：[A04](../B4_Test_Report.html#case-A04)、[A14](../B4_Test_Report.html#case-A14)。
- 发现：196列输出LONG-加300个双宽字符，行尾人工填充被写入历史；小/大预算结果一致。
- 当前：代码修复及软件回归通过；用户旧实例尚未重启，真实MCP复验待执行。
- [修复经过及全部验证](#b4-01-code-fix) · [原始失败](../B4_Test_Report.html#run04-detail) · [修复证据](#evidence-fix)
- 下一步：重启新构建，复跑原样本；继续A14组合字符/emoji边界，逐平台、transport记账。



### UI · 提权凭据窗口字号
- 原因：子窗口ElaText沿用28px默认字号。
- 调整：说明/存储状态12px，sudo/su标题13px，不改凭据行为。
- 主程序构建成功，设置相关QTest9/9。高DPI实际截图尚未复验。
- [详细验证](#font-fix) · [证据位置](#evidence-ui)



## 4. 证据索引

以下为本机临时文件路径，迁移或清理后可能不可用；结论和边界仍保留在本文件中。

| 锚点 | 记录 | 路径与内容 |
| --- | --- | --- |
| R01 | 首轮 | /tmp/novaterm-mcp-run-20261030/；副作用计数、认证边界；部分早期响应仅存结论性摘要 |
| R02 | 只读 | /tmp/novaterm-mcp-run-20261031-02/；responses.redacted.json、verify.py、verification.json |
| R03 | 交互 | /tmp/novaterm-mcp-run-20261031-03/；41条响应及副作用文件、verify.py、verification.json |
| R04 | 边界 | /tmp/novaterm-mcp-run-20261031-04-xDlxLX/；42条响应+large-control.redacted.json、expected.txt、actual.txt、verify.py |
| FIX | 日志修复 | /tmp/novaterm-b4-01-final-ctest.log；/tmp/novaterm-b4-01-final-mcp.txt；/tmp/novaterm-b4-01-original-replay.log |
| UI | 字号 | /tmp/novaterm-elevation-font-build.log；/tmp/novaterm-elevation-font-tests.txt |



## 5. 判据审查修正

- 现行20工具、统一主体与直接路径，不能沿用双客户端身份、目录预共享及保存目标流程。
- 首轮S项属于local_shell，不转记SSH/UART。A06旧展开缺证、C02伪token不能证明有效token换代。
- 实时capture不依赖录制；本次实时正向已测，持久录制H组仍缺正向证据。
- Linux上传可以验收；下载侧Writer为开发缺口，不能一并阻塞所有上传。
- UART同主体输入控制由工具自动衔接，不要求旧式手动release。
- 原文中过时说法只用于追溯；当前执行以清单v2和本页用例结论为准。






<a id="commit-02115-detail"></a>

## 0.2.114与0.2.115提交验证

用户要求创建提交并推送Gitee，按主题分为错误处理修复0.2.114（6595170）和工具台账范围整理0.2.115。
每个提交递增PATCH，临时凭据目录、构建产物和日志不纳入Git。

0.2.114相关25个测试目标重新构建成功，提交前同一轮CTest **25/25（64.72秒）**；
其中完整MCP QTest为 **160 passed, 0 failed, 3 skipped, 0 blacklisted, 39818ms**。覆盖MCP契约、授权、读取、观察、HTTP/wire、
等待/任务、文件/串口传输及直接相关能力/资源共享；没有运行无关Core/GPU/PTY全套。
这是本次提交前的新一轮证据，不改写前面24/25与失败目标1/1的历史成绩。

最终0.2.115主程序 `cmake --build build/Release --target novaterm -j 6` 构建成功。
0.2.115只整理文档、导出呈现和版本，未重新运行上述业务目标，也没有重跑ASan或真实设备场景。
用户运行实例未被重启；未修改板卡配置。原UART停止环境问题和B4剩余判据仍未验收。

台账结构复核：87条用例、20工具映射、11轮实际MCP工具测试；49条部分验证、38条未执行。
代码级测试、模拟后端夹具、审查、构建、消毒器和提交验证均存于工程文档；
工具台账内的执行记录与证据不含这些成绩。确定性再生成一致，内部锚点、迁移后的外部引用、
签名token检查及 `git diff --check` 通过。本次提交验证没有向工具台账新增测试轮次或更改其运行版本。

本机临时日志：`/tmp/novaterm-02114-commit-build.log`、`/tmp/novaterm-02114-commit-ctest.log`、
`/tmp/novaterm-02114-commit-lasttest.log`、`/tmp/novaterm-02115-commit-main-build.log`、
`/tmp/novaterm-02115-commit-doc-check.log`。
