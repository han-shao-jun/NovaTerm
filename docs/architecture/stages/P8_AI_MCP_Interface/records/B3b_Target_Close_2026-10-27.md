# B3b 第三子批：保存目标安全关闭

## 0.2.89 提交前收口

用户明确要求创建本地Git提交。本次合并收口B3b安全关闭与两项审查修复，按AGENTS.md现行0.2.x每提交PATCH递增约定将根版本0.2.88→0.2.89，不采用commit_check.md旧0.1.<提交数>规则。当前二十一工具，单文件传输、新会话预授权及B4保留待办；不推送，不纳入build日志/产物和本机辅助文件，不改机械风格，不连接真实设备。

提交前以0.2.89固定源码重建并验证相关目标；下面较早0.2.88的分轮成绩保持原事实，不代替本轮验证。

本轮正常Windows Release默认全部目标构建通过，退出0，`build/b3b-close-commit-0.2.89-build.txt`。相关CTest **24/24，68.82秒**（`build/b3b-close-commit-0.2.89-ctest.txt`），包含MCP其余19项、Core、Session、SessionPanel、SFTP面板与UI布局。完整MCP业务文件排除在该24项之外，成绩单独记录，不称为全项目全套。

完整MCP业务文件 **194pass/0fail/0skip，105.314秒**（`build/b3b-close-commit-0.2.89-mcp.txt`），进程退出0；原ACL及全部已有断言保持，没有放宽/跳过。官方SDK2025/rawHTTP2026 **二十一工具互操作通过**（`build/b3b-close-commit-0.2.89-interop.txt`），退出0。四项退出码见`build/b3b-close-commit-0.2.89-results.json`，均为0，三批测试分别记录，不拼成一次全项目全绿。

Qt bin/plugins沿用6.8.3前缀，TEMP/TMP为工作区`build/b3b-close-commit-0.2.89-temp`；命令使用下文本轮修复的同一24项CTest正则、默认Release构建、完整MCP业务文件及`python tests/mcp/interop_check.py`，本节单独列明0.2.89日志，不能套用较早0.2.88成绩。没有运行无关全套/ConPTY或真实设备。

提交范围为本批55个文件（源码、测试、文档及根版本），没有build产物、日志或本机辅助文件。提交前git diff --check通过；新增/修改文档链接无缺失（`build/b3b-close-commit-0.2.89-links.json`），AGENTS两条既有历史链接缺失单独记录，未扩入本批修改。

## 设计与实施计划

基线0.2.88、二十工具。继续执行06第5节已有设计：新增 `novaterm_close_target(targetId,configVersion,operationId,epoch)`，operationId引用本客户端原打开操作。复用现有Qt6/C++17、纯应用有界索引、Service组合和Session关闭通路；不创建第二个连接owner。成功只关闭连接，保留可见终端和缓存供人工查看，不删除标签。

关闭须保有目录共享和独立打开授权；只允许原操作创建的Session，禁止关闭复用会话。核验原Transport身份/连接generation、调用方预期epoch、人工输入代际及本机手动重连/断开事实。当前执行、串口传输、受限命令/脚本保护和SSH面板后台操作均需空闲；运行终端须有可信的就绪证据，否则busy/unknown拒绝。连接/认证处理中不猜测安全。最终副作用前重查请求/权限；已接受的关闭保持事实，精确重试不再次关闭，也不追踪换绑后的新连接。

方案选择：沿用既有Session.close并保留终端；删除UI标签会额外销毁缓存并引入异步删除/换绑边界，本批不采用。文件传输、新会话预授权和B4另行实施。

Ruling：用户已授权按本目录继续实施，采用06现有边界并在此具体化；不重复要求设计批准。复用指定checkout及Windows Release构建，不提交/推送、不改版本、不连接真实设备，不作机械风格修改。执行使用writing-plans、executing-plans与test-driven-development流程；项目改哪测哪规则优先于技能的通用全套/提交步骤。

### 任务1：契约与应用受理

- [x] 新工具契约先RED，检查必需标识/epoch、拒绝force/任意连接参数；原二十工具Schema保持；目录/打开仅更新关闭说明。
- [x] TargetConnections增加类型化Close动作及窄关闭端口，按原记录拒绝跨客户端、复用、缺失/变化身份、旧epoch；同步重入和成功精确重试零重复关闭。
- [x] 应用回归覆盖授权撤回、取消、busy可重查及关闭后换绑。

### 任务2：Session/本机接线

- [x] Service组合关闭守卫，读现有Controller/Arbiter/命令保护，不创建新执行器；本机提供SSH面板后台操作事实。
- [x] 真实Session+内存Transport及两代HTTP回归，覆盖人工输入/手动连接动作、执行/传输、unknown、撤权、成功与精确重试。

### 任务3：验证与文档

- [x] 正常Windows Release默认构建，相关MCP/Session/UI测试，完整业务文件与两代互操作分别记录；不运行无关Core/ConPTY/全套。
- [x] 冻结新增工具fixture，更新现行契约、进度、使用说明和记录；diff/链接检查。
- [x] 完整差异复核所有权、同步重入、忙碌/未知事实及失败关闭语义。

## 执行记录

### 实现与独立审查

TargetConnections使用类型化Open/Query/Close动作，保留旧九项ToolRequest联合。关闭复用原有打开记录的32/客户端、256/全局预算，不新增无界记录；在任何query/close回调前占位，回调按值复制，不跨回调保存Qt容器迭代器或引用。成功保存首次关闭事实及其epoch，精确重试不再次调用关闭端口。查询仍面向原Transport身份，首次主机指纹发现可改变epoch但不改变connectionGeneration；重连后的旧创建操作不关闭新连接。

Service只组合现有Session、Controller、Arbiter与CommandRuntime事实；用户输入代际、显式断开/重连的单调事实、后台SFTP状态和最终授权/取消/期限均复核。执行/确认/上传由原CommandRuntime查询忙碌，未知目标保护仍阻止关闭；没有第二执行器或Session owner。关闭仅调用Session.close，保留可见终端和缓存。

独立只读审查发现SFTP UI的_busy可能被旧directoryListed/terminalPathLookupFailed清空。已取得真实RED并改为联合worker真实队列与活动命令：出队和commandActive=true同锁，整个命令作用域RAII在阻塞I/O和所有continue/退出后清位，读取tryLock失败保守busy，不阻塞GUI。旧worker持有独立control，新连接不被旧清位影响。再次独立只读复核未见高可信新增问题，复核者未运行测试。新增关闭fixture已生成，目录和打开的旧fixture仅更新关闭说明，Schema未扩张。

### RED与中间结果

- 新工具契约：`build/b3b-close-contract-red.txt` **2pass/1fail**，失败为基线不认识novaterm_close_target；首次GREEN `build/b3b-close-contract-green.txt` **3/3**。
- 两代HTTP、真实Session/内存Transport：`build/b3b-close-http-red.txt` **10pass/8fail**，未接入关闭副作用时成功与busy断言失败；首次GREEN **18/18，3.985秒**（`b3b-close-http-green-initial.txt`）。扩展手动连接、嵌套撤权/人工输入、UI忙碌、换绑后第二轮 **30/30，7.069秒**（`b3b-close-http-green-2.txt`），各次独立记录，不拼成一套。
- 可靠就绪复核：显式allowUnverifiedPrompt配置即使已有prompt代际也不得关闭；`build/b3b-close-untrusted-red.txt` **2pass/2fail，551ms**，证明只查readyForCommand不够。Service补hasTrustedProfile检查，保留现有输入/执行行为；最终GREEN另列。
- SFTP审查回归：`build/b3b-close-sftp-red.txt` **2pass/1fail，39ms**，旧目录回调清除UI_busy但真实队列仍有工作；保留原断言后补活动无排队及取锁失败检查。offscreen字体目录提示不作为产品RED原因。
- 首轮默认构建在新增CommandRuntimeTests引用不存在的ScriptPublicationResult.errorCode字段失败（C2039，`build/b3b-close-final-build.txt`）；按现有DTO改为optional ErrorCode的error字段，没有修改产品接口。随后默认构建通过（`build/b3b-close-final-build-2.txt`），但该轮早于SFTP审查修复，不能充当最终源码验证。
- 最初沙箱构建停于CMake目录检查，已停止该次构建，在正常Windows MSVC环境重建。沙箱无法运行测试可执行文件与真实契约RED分开记录。

### 固定源码最终验证

正常Windows MSVC BuildTools环境、Qt6.8.3，最终固定源码Release默认全部目标构建退出0，`build/b3b-close-final-build-5.txt`。最终相关CTest **23/23，57.83秒**，`build/b3b-close-final-ctest.txt`，包括原22项相关目标及SFTP面板；不含完整MCP业务文件。SFTP面板完整QTest **20/20，15.868秒**，原RED场景连同活动位/锁争用断言均通过，输出在`build/Release/tests/sftp-panel.ctest.txt`。本轮未放宽或跳过已有断言。

完整MCP业务文件 **188pass/0fail/0skip，103.754秒**，`build/b3b-close-final-mcp.txt`，进程退出0。包含两代HTTP的15类关闭场景、关闭后重置/换绑精确重试不误关新连接、明确非可信Profile拒绝等；原授权、外来owner ACL及终端/历史/等待/任务断言保持。

官方SDK2025 / rawHTTP2026 **二十一工具互操作通过**，`build/b3b-close-final-interop.txt`，退出0。新增关闭输出Schema校验、拒绝关闭复用会话及拒绝后原连接仍可查询，原目录/打开/查询/重试、历史/终端/等待/任务保持。成功关闭由真实Session+内存Transport两代HTTP覆盖；SDK fixture本身是复用终端，不能把关闭拒绝当成SDK成功关闭验收。

退出码另存`build/b3b-close-final-results.json`；上述23项CTest、完整业务与互操作分别运行，不拼成全项目全套。源码与本机回环/内存替身成绩不代替B4。

命令：

```text
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build/Release -j 4'
ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|command_safety|resource_sharing|session|session_panel|sftp_panel|ui_dialog_layout)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure
build/Release/bin/novaterm_mcp_tests.exe -o build/b3b-close-final-mcp.txt,txt
python tests/mcp/interop_check.py
```

Qt bin为`C:/Programs/Qt/6.8.3/msvc2022_64/bin`，QT_PLUGIN_PATH为同前缀plugins；TEMP/TMP用工作区`build/b3b-close-temp`。不全局设offscreen，UI专项沿用CTest配置；SDK依赖沿用build/mcp-test-deps，不增加应用Python依赖。

### 剩余范围

版本保持0.2.88，未提交/推送、未连接真实设备。MCP单文件传输、新会话预授权规则、B4真实SSH/UART/客户端/跨平台/性能仍待后续。新建连接默认不授予终端权限；没有可靠提示符的UART及认证处理中只能由本机用户收尾。可见终端保留不是删除标签；源码/内存替身、正常编译和只读审查不代替真实GUI连接与密钥环/主机密钥验收。不运行无关全套/Core/ConPTY。

## 两项行为审查修复（2026-10-27）

用户要求修复qt-cpp-review确认的两项问题，不改机械风格、版本或工具Schema，不提交/推送、不连接设备。

- [x] 人工按键/文本/粘贴成功入Core命令队列即推进独立单调事实，关闭联合核对该事实与原Arbiter写入代际；不改变既有输入抢占/写入语义，不把Agent编码、resize/focus/协议输出当人工接管。两代HTTP在安全回调中入队、返回前Arbiter仍为零，确定性验证旧窗口。
- [x] ConfirmationRegistry按Session查询未消费/未过期活动确认并纳入CommandRuntime.busy；MRTR命令和脚本覆盖待确认、到期未sweep、撤权、断流、拒绝/取消消费，保留其他Session隔离与完成后不忙。
- [x] 两项先真实RED再GREEN；正常Windows Release默认构建、相关MCP/Session/UI及因本轮Core改动增加Core专项，完整业务/两代互操作分别记录，不运行无关全套/ConPTY。最终差异只读复核并同步契约/记录。

本轮前述成绩只代表审查修复前的源码，不替代以下验证。

### 本轮RED/GREEN

- 人工输入窗口：`build/b3b-close-review-input-red.txt` **2pass/6fail，1.553秒**。真实Session+内存Transport，两代HTTP分别对粘贴、按键和IME文本入队，在本机安全回调返回前确认Arbiter写入代际仍为0。原实现全部误关；新增Core入队代际后同一断言全部拒绝。扩展后的完整关闭专项 **38/38，9.156秒**（`build/b3b-close-review-input-green.txt`）。
- MRTR活动确认：`build/b3b-close-review-confirmation-red.txt` **2pass/10fail，4ms**；覆盖命令/脚本，真实CommandRuntime返回detached提示后、零上传/执行/目标预留时误判不忙。修复后 **12/12，3ms**（`build/b3b-close-review-confirmation-green.txt`），包括到期未sweep、撤权、断流及拒绝/取消消费，其他Session不受影响。
- Core准入边界专项 **4/4，24ms**（`build/b3b-close-review-core-green.txt`）：成功入队在GUI输出交付前即有单调事实，连接reset不清除；空输入、被8MiB命令预算拒绝的粘贴、Agent编码、焦点/尺寸变化及协议响应均不计入。新计数为独立atomic事实，不触发新的QObject回调，不提前改变原Arbiter抢占/输入token/写入语义。

独立只读复核未发现>80置信的新增问题：入队计数在成功push之后、命令锁释放之前发布；拒绝入队不增加，正常新Core为零，连接reset不清除；关闭两次safety联合读取。确认作用域同时核对活动、未消费和期限，既有撤权/断流清理保持。复核者未修改代码、未构建或运行测试。

### 本轮固定源码验证

正常Windows MSVC BuildTools环境Release默认全部目标构建通过，退出0，`build/b3b-close-review-final-build.txt`。相关CTest **24/24，68.79秒**（`build/b3b-close-review-final-ctest.txt`），包含前轮23项及因Core本轮改动增加的Core完整目标；Core目标本次16.19秒通过，没有运行无关全套/ConPTY。

完整MCP业务文件 **194pass/0fail/0skip，104.137秒**（`build/b3b-close-review-final-mcp.txt`），进程退出0；原有授权/隔离、ACL、历史/终端/任务等断言保持，本轮新增两代人工输入入队场景包含其中。

官方SDK2025/rawHTTP2026 **二十一工具互操作通过**（`build/b3b-close-review-final-interop.txt`），退出0，保持目录/打开/查询/关闭拒绝复用、等待/任务/重试与历史/终端契约。MRTR忙碌生命周期由真实CommandRuntime专项覆盖，不把SDK原互操作成绩表述成新增MRTR关闭场景的HTTP验收。

构建、相关24项CTest、完整业务与互操作均单独运行且退出0，退出码见`build/b3b-close-review-final-results.json`，不与前轮188/188或23/23拼为一次全套。两项修复后的git diff --check通过；当前文档新增/修改链接无缺失（`build/b3b-close-review-doc-links.json`），既有AGENTS两条历史链接缺失保持前述单独记录。

```text
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build/Release -j 4'
ctest --test-dir build/Release -C Release -R '^novaterm_(core|history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|command_safety|resource_sharing|session|session_panel|sftp_panel|ui_dialog_layout)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure
build/Release/bin/novaterm_mcp_tests.exe -o build/b3b-close-review-final-mcp.txt,txt
python tests/mcp/interop_check.py
```

Qt bin/plugins沿用上述6.8.3前缀，TEMP/TMP为工作区`build/b3b-close-review-temp`。版本0.2.88、二十一工具Schema和原输入仲裁语义保持；未提交/推送、未连接真实设备，不改机械风格。


本轮git diff --check通过；目录/打开两个旧fixture对比确认仅description变化，Schema/annotations保持。已改文档本地链接共核对249项，本轮新增/修改链接无缺失；AGENTS.md既有两条Merge_Gitee_2026-10-22/23历史链接的目标不存在，原HEAD也有同一引用，本轮未改写历史记录。结果见`build/b3b-close-doc-links.json`。
