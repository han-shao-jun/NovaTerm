# B3b 文件传输第一子批：独立授权与资源目录

> 执行：writing-plans / executing-plans / test-driven-development，沿用现有checkout。

**目标：** 为后续受限单文件传输提供本机显式的本地目录资源引用和当前SSH连接的远端路径读写授权，Agent只能查询已获准元数据。

**基线：** `469fa73`，0.2.90、二十一工具。用户已要求创建本地提交并继续MCP，本地提交已完成；本批作为其后工作区增量，不自动再次提交或推送。

**依据：** [06第4节](../06_Terminal_Workflow_Extensions.md#4-sshsftp-单文件传输)及[实施顺序](../01_Phases_and_Acceptance.md#b3b文件传输和保存目标连接)。

## 设计与边界

采用Qt6/C++17，新增纯Qt Core的FileResources授权目录、Session适配器与独立本机设置窗口。MCP新增只读 `novaterm_list_file_resources`，可选sessionId/epoch成对筛选，不扩张旧九项终端请求联合；原二十一工具对象保持。

本地目录按可信客户端身份版本授权，远端目录另绑定完整Session/Transport attachment/epoch/generation/目标指纹；终端读取/输入/清空、打开目标和文件授权互不推导。上传将需要本地读+远端写，下载将需要远端读+本地写；本批只展示这些独立选择。

本地返回随机resourceId、用户选定名称、revision与读写布尔值，不返回绝对本机路径、目录内容或文件字节。远端仅披露用户明确共享的绝对目录及scopeId/revision和实际sessionId/epoch。响应标记 `availability=authorization_only`；筛选时无可见当前远端scope或目标不可用统一拒绝，无参数只列本人已共享的元数据，不发现未授权终端。

本机界面提供“文件资源授权”入口；用户选择本地目录、填写远端目录并分别选择读写；远端仅当前已连接SSH可授权。每次提交复核启用状态、身份版本与界面原连接快照；陈旧行不能授权新连接。本地规则不因普通终端授权变化失效，远端连接失效即回收；令牌轮换、删除和全局撤销回收对应全部规则，不持久化。

预算：本地8项/客户端、全局128项；远端8项/客户端、全局256项；目录路径最多1024 UTF-8字节、名称最多256字节。重复同一客户端/绑定/路径更新原资源但旋转revision；撤回后再共享产生新ID。列表固定最多8+8项，无分页、无文件系统I/O、无无限默认值。

Ruling：目录允许无参数发现，并仅返回已授权远端scope的sessionId/epoch；否则只有文件权限的客户端无法取得筛选所需标识，被迫额外申请终端读取。为保持8+8固定响应预算，将远端限额约束在整个客户端，跨多Session也共用8项；代价是同时需要更多目录时须先撤回旧授权。真正传输仍须显式当前Session/epoch。

路径为本机显式选择的授权意图，需规范绝对路径且拒绝控制字符、上级跳转、UNC/设备路径、远端根目录及反斜线；这不宣称完成文件系统身份验证。实际打开文件时仍须由后续工作线程用安全句柄/远端能力复核链接、替换、普通文件与范围；不能直接把本批词法校验当作沙箱。

现有SFTP接口面向人工面板，数据阶段阻塞、取消/发布事实及路径边界需要独立接入。Ruling：先完成可单独验收的四向权限与只读目录，再实施上传/下载、查询/取消和安全发布事务；不注册尚无后端的数据传输工具。代价是本批不能交付文件，文件交付与B4继续保留。既有SFTP面板和脚本上传不扩权、不机械改造。

## 任务

### 1. 契约与纯授权目录

- [x] 新工具契约先RED：无参仅发现已授权项，可选sessionId/epoch必须成对，拒绝host/localPath/clientId/读写等注入参数，旧二十一对象保持。
- [x] contracts/FileResourceRequest.h与application/FileResources.*定义独立请求、元数据、四向权限、身份与完整绑定、预算及撤回；方法只供可信本机组合调用，不解码本机路径。
- [x] 新纯应用FileResourcesTests：跨客户端、版本、完整绑定、路径词法、预算、更新/撤回不复活和普通授权独立性。

### 2. Session适配、本机界面与协议接线

- [x] FileResourceAdapter只组合AccessStore/SessionDirectory和纯目录，当前SSH身份及旧UI快照复核；目录/身份变化回收、无磁盘/网络I/O，不创建Session/SFTP owner。
- [x] FileResourceSharingDialog提供本地目录选择、远端当前SSH选择及显式读写；身份变化禁用旧窗口，连接变化清除旧选择，显示本机路径但不回传协议。
- [x] ToolCodec/ToolCall/BackendEvents/Wire/HTTP路由/Service/CapabilityRegistry完整接入，两代HTTP验证文件授权独立于终端权限、拒绝旧epoch/身份及不泄漏本机路径。
- [x] 二十二工具fixture和两代互操作扩展，原对象及原断言保留；授权目录不被写成已支持上传/下载。

### 3. 验证、复核与文档

- [x] 正常Windows Release默认全部目标构建；相关MCP/Session/UI及新纯应用目标，完整业务和两代互操作分别记录；不跑无关全套/Core/ConPTY或真实设备。
- [x] 独立只读复核回调重入、客户端身份、目录绑定、旧窗口和路径披露；diff/链接检查。
- [x] 更新02现行契约、01进度、06设计状态、入口/架构三处及AGENTS；实机清单增补仅授权目录可测，数据传输保持后续。

## 审查重点

1. 同步SessionDirectory.find可能刷新/发失效信号，回调后重新找授权/绑定，不保留容器引用。
2. 远端授权不被重连/首次指纹/换绑转交新目标，UI陈旧快照必须拒绝。
3. 单独文件授权不要求或隐含终端读取/输入；身份轮换和普通Session授权变化分别处理。
4. 对外局部投影不含本机绝对路径或凭据，错误不回显隐藏路径。
5. 全局/客户端限额与撤回后再共享不复活旧资源；词法路径准入不替代后续句柄安全。

## 执行记录

本批基于469fa73，版本0.2.90保持；不引用此前预授权提交的234项成绩替代本批验证。

### RED/GREEN与中间结果

- 新工具契约 **2pass/1fail，3ms**（`build/b3b-file-resources-contract-red.txt`）：基线不认识新工具。首次工具接入后契约 **3/3，2ms**（`b3b-file-resources-contract-green.txt`）。
- 文件权限独立发现契约 **2pass/1fail，3ms**（`b3b-file-resources-discovery-red.txt`）：原拟定必填Session会阻碍无终端读权限客户端发现。调整为可选成对筛选后 **3/3，4ms**（`b3b-file-resources-discovery-green.txt`）；随后添加真实HTTP目录生成的新fixture，最终契约目标另记录。
- 纯应用首轮 **5pass/1fail，3ms**（`b3b-file-resources-application.txt`）：Windows设备前缀被Qt路径规范化剥离，导致原始扩展路径未按约定拒绝。改为规范化前校验原始前缀，补设备/UNC变体；**6/6，66ms**（`b3b-file-resources-application-green.txt`）。另补客户端跨Session共用八项预算的回归，最终完整目标另记录。
- 两代HTTP与真实授权窗口初步专项 **5/5，1.718秒**（`b3b-file-resources-http.txt`），覆盖仅文件授权成功发现、四向选择、无本机路径/文件I/O、跨客户端、旧epoch、配置绑定变化、普通终端授权独立和旧窗口拒绝。
- 首次UI构建因全局SessionId与MCP强类型同名产生C2872/C2440（`b3b-file-resources-http-build.txt`），明确使用MCP具名类型后构建通过；没有修改全局类型或产品接口。最初冻结fixture辅助请求遗漏2026要求的Mcp-Method头，HTTP400；补齐后从真实目录生成fixture，不作产品RED。

### 独立复核与修复

独立只读复核找到一项P2：适配器仅监听目录invalidated；真实SSH断开到Failed保留Transport/Session generation时只发布changed，隐藏远端规则仍占八项限额，界面只列Running连接又无法撤回。

初轮回归 **2pass/2fail，97ms**（`b3b-file-resources-disconnect-red.txt`），其中手动断开为确证行为失败；另一夹具仅发errorOccurred而未真正断开，运行期仍Running，按既有Session语义修正为实际Transport.disconnect。单独手动断开真实RED **2pass/1fail，51ms**（`b3b-file-resources-disconnect-manual-red.txt`）：旧SSH共享八项→断开保留终端→另一运行SSH无法获得新授权。

修复后在SessionDirectory.changed中按最多256项的目录索引检查存活连接，回收非就绪远端授权；QScopedValueRollback防止find同步刷新重入，快照不跨回调持有容器引用。本地授权保留。手动与传输层断开均确认没有invalidated、Session generation未变时仍立即释放，另一SSH可使用额度。完整文件专项 **7/7，3.716秒**（`b3b-file-resources-review-green.txt`），含原两代HTTP和旧窗口回归，退出0。

审查无其他高可信Critical/Important问题；审查者未修改代码、构建或测试。修复按真实RED→GREEN验证，没有将初轮静态复核写成修复后的再次独立复核。真实设备、跨平台、实际文件打开/传输/发布及性能不在本次静态复核结论内。

### 固定源码验证

正常Windows MSVC BuildTools、Qt6.8.3，Release默认全部目标初次构建通过，退出0（`build/b3b-file-resources-final-build.txt`）；随后各轮测试暴露的夹具修正及最终成绩如下分开记录。

首轮相关CTest **22/23，39.34秒**（`build/b3b-file-resources-initial-ctest.txt`）：旧关闭工具契约用catalog.last定位，追加目录后取错对象。完整契约文件输出 **23pass/1fail，8ms**（`b3b-file-resources-contracts-full-failure.txt`）。改为原冻结位置20，并同步旧关闭fixture写出辅助入口，保留原fixture、完整对象及位置断言；完整契约 **24/24，8ms**（`b3b-file-resources-contracts-full-green.txt`），写出结果与原关闭fixture完全相同。默认构建再次通过，日志为 `b3b-file-resources-final-build-2/3.txt`。没有通过改写旧fixture放宽兼容检查。

第二轮相关CTest **22/23，37.59秒**（`b3b-file-resources-second-ctest.txt`），契约已通过，Session目标失败。根目录直接完整QTest **137pass/0fail/4skip，17.018秒**（`b3b-file-resources-session-full.txt`）；按CTest实际工作目录build/Release再跑 **136pass/1fail/4skip，17.620秒**（`b3b-file-resources-session-ctest-cwd.txt`），复现 `agentContextFiltersProgressWrapDuplicatesAndAlternate` 的单次trySnapshot为空断言。

源码定位：waitForIdle只等待已提交字节/命令完成，Parser下一轮processHistory仍可能占modelMutex；tryTerminalState是try-lock，且发布fallback未必已可用。旧测试对200轮进度/备用屏采样逐次要求非阻塞调用立即成功，与既有API语义不符。本轮只将该测试改为最多1000ms等待**当前revision**的快照，保留所有重复行计数、进度过滤、备用屏清空/范围、标题及12字节/2行预算断言；生产Core/Provider/Session源码不改。实际CTest工作目录连续10个独立进程专项均通过（`b3b-file-resources-session-capture-green-1..10.txt`）。四项skip均是既有Linux raw PTY两项、Linux lrzsz手工矩阵和Unix Secret Service平台条件，没有新增skip。

该测试时序修正后正常Release默认构建通过，退出0（`b3b-file-resources-final-build-4.txt`）。前两轮脚本在CTest失败后停止，未继续业务/互操作，不套用前轮成绩；最终完整验证另列。

最终固定源码相关CTest **23/23，39.94秒**（`build/b3b-file-resources-final-ctest.txt`），包含新增纯应用资源目录及相关MCP/授权/Session/UI等目标；不含完整MCP业务文件。没有运行无关全套、Core或ConPTY。

完整MCP业务文件 **239pass/0fail/0skip，132.678秒**（`build/b3b-file-resources-final-mcp.txt`），退出0，包含本轮两代HTTP、真实授权窗口及两条断开额度回收回归；原ACL与已有断言保持。

官方SDK2025/rawHTTP2026 **二十二工具互操作通过**（`build/b3b-file-resources-final-interop.txt`），退出0，新增目录空授权发现成功及未授权绑定拒绝均按实际outputSchema验证，原目标/终端/历史/等待/任务/精确重试检查保持。互操作夹具未授文件目录，已授权非空结果由真实Session+内存SSH两代HTTP专项覆盖，不混称SDK真机传输验证。

四项退出码保存在 `build/b3b-file-resources-final-results.json`，均为0。23项CTest、239项完整业务及互操作分别运行，不拼为全项目全套；旧二十一工具fixture无修改，新fixture从实际HTTP目录冻结。

```text
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build/Release -j 4'
ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|command_safety|resource_sharing|session|session_panel|ui_dialog_layout)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure
build/Release/bin/novaterm_mcp_tests.exe -o build/b3b-file-resources-final-mcp.txt,txt
python tests/mcp/interop_check.py
```

Qt bin/plugins沿用6.8.3前缀，TEMP/TMP为工作区 `build/b3b-file-resources-final-temp`，SDK依赖沿用build/mcp-test-deps，不增加应用Python依赖。git diff --check及新增文件空白检查通过；修改文档本地链接281项无新增缺失，AGENTS两条既有历史链接仍缺失、HEAD也有同一引用（`build/b3b-file-resources-doc-check.json`）。冻结辅助夹具遗留的两个私有目录已按明确绝对路径清理，没有将其或build日志加入源码。

## 当前保留范围

版本0.2.90保持，基于469fa73的本批增量尚未提交/推送；实机清单单独保留并已加入授权目录检查项，所有设备项目仍为未执行。本轮未连接SSH服务器或真实串口，UI真实视觉、其他平台与性能仍待B4，不以offscreen窗口测试代替。

下一子批仍是受限单文件上传/下载、查询/取消、有界缓冲/并发/文件大小/期限、路径/链接/替换复核和发布事务；与现有脚本发布协调，同一操作精确重试不重复覆盖。当前资源引用不证明文件存在或安全，不能直接接既有人工作业路径而省略这些边界；UART文件协议入口不在本批。

## 本轮行为审查修复（2026-10-27）

用户要求修复qt-cpp-review确认的两项P2，并核实目录树刷新丢选择的待验证项；不查机械风格。本轮沿用receiving-code-review、systematic-debugging、TDD及effective-cpp流程，不改版本/工具Schema、不提交/推送、不连接设备。

- [x] 客户端主体：先验证名称/稳定ID可见、窗口模态、父客户端切换收回旧窗口及同客户端只开一窗；再固定显示主体，阻止旧窗口在选择变化后继续操作。不会随父选择静默改绑授权对象。
- [x] 目录选择：先复现无关Service.changed后撤回选择丢失；只按同一ID+revision恢复，已更新、撤回或换绑的条目不自动选中，避免旧撤回意图作用于新规则。
- [x] 采样锁存：提取原测试采样步骤，使用实际QTRY宏和受控“成功一次后忙碌”读源取得确定性RED；按本轮目标revision锁存成功结果，覆盖Snapshot和有界TerminalState两条路径，保留原内容断言，不改生产采样。
- [x] 正常Release默认构建、相关23项CTest、完整MCP业务及两代二十二工具互操作分别记录；结果不套用前批，不跑无关全套/Core/ConPTY。
- [x] 同步当前契约/使用说明/实机清单中的窗口行为与本轮记录，diff和链接复核。

授权窗口采用WindowModal阻止父窗口层级交互、明确显示名称和稳定客户端ID，并在程序侧父选择变化时关闭旧窗口；单个MCP设置窗口复用同客户端已打开的授权窗口。后台终端连接和事件处理继续，关闭授权窗口后恢复父窗口交互。采样支持代码仅位于测试文件，以便受控重现Qt宏的重复求值，不向产品加入测试钩子。

### 本轮真实RED/GREEN与独立复核

- UI真实RED **5pass/4fail/0skip，19.213秒**（`build/b3b-file-review-ui-red.txt`，退出4）：客户端身份不可见、父选择改变后旧窗口仍开、重复窗口及无关刷新丢选择；规则更新/删除/连接替换时不误选的既有保护通过。修复后连同原两代HTTP、陈旧身份/连接和断开额度回归 **14/14，4.322秒**（`build/b3b-file-review-ui-green.txt`，退出0）。
- 采样确定性RED **2pass/2fail/0skip，621ms**（`build/b3b-file-review-capture-red.txt`，退出2）：实际QTRY宏重复求值，使Snapshot与TerminalState两条路径都把首次成功覆成空。按每轮目标revision锁存成功结果，feed前重置，修复后含原完整内容回归 **5/5，14ms**（`build/b3b-file-review-capture-green.txt`，退出0）。原重复行、进度、备用屏、标题和预算断言保留。
- 修复后独立只读行为复核未见高可信Critical/Important新问题，核对了固定客户端主体、QPointer延迟销毁、ID+revision恢复及每轮revision采样；审查者未写代码、构建或运行测试。不重新执行机械风格检查。

### 本轮固定源码完整验证

正常Windows Release默认全部目标构建通过，退出0（`build/b3b-file-review-final-build.txt`）。同一源码相关CTest **23/23，39.12秒**（`build/b3b-file-review-final-ctest.txt`，含完整Session目标），完整MCP业务 **246pass/0fail/0skip，129.454秒**（`build/b3b-file-review-final-mcp.txt`），官方SDK2025/rawHTTP2026 **二十二工具互操作通过**（`build/b3b-file-review-final-interop.txt`）。三批分别运行，退出码全部为0（`build/b3b-file-review-final-results.json`），不拼为全项目全套或套用前批成绩。

构建、CTest筛选和互操作命令与上节一致，业务输出参数改为 `-o build/b3b-file-review-final-mcp.txt,txt`；Qt环境沿用6.8.3，TEMP/TMP独立使用 `build/b3b-file-review-final-temp`。没有重跑无关全套、Core或ConPTY，没有连接真实SSH/UART设备。版本0.2.90、二十二工具Schema保持，未提交/推送。

同步现行契约、使用说明及独立实机清单A08；设备与视觉验收仍标为未执行。diff空白检查通过，修改文档本地链接无新增缺失；AGENTS两条HEAD已有历史缺失链接不计为本轮引入（`build/b3b-file-review-final-doc-check.json`）。日志及本机验证脚本均留在忽略的build目录。

## 0.2.91 提交前收口

用户要求创建本地Git提交。本次按PATCH递增约定将0.2.90→0.2.91，包含独立文件授权目录、`novaterm_list_file_resources`、断开额度回收及本轮授权窗口/选择/测试采样修复；当前二十二工具，原二十一工具fixture保持。相关说明、架构状态及独立实机测试清单随代码纳入提交；build产物、验证脚本、日志及本机辅助文件不纳入。不推送，不改机械风格，不连接真实设备；上传/下载/进度/取消、安全发布与B4范围仍待后续。

0.2.91正常Windows Release默认全部目标构建通过（`build/b3b-file-commit-0.2.91-build.txt`）；同版本相关CTest **23/23，44.04秒**（`build/b3b-file-commit-0.2.91-ctest.txt`），完整MCP业务 **246pass/0fail/0skip，129.648秒**（`build/b3b-file-commit-0.2.91-mcp.txt`），官方SDK2025/rawHTTP2026 **二十二工具互操作通过**（`build/b3b-file-commit-0.2.91-interop.txt`）。构建/三批验证退出码均0（`build/b3b-file-commit-0.2.91-results.json`），不拼成全项目全套，不以0.2.90成绩代替本次提交前验证。

命令沿用上节，构建日志前缀与业务输出改为 `build/b3b-file-commit-0.2.91-*`，TEMP/TMP为 `build/b3b-file-commit-0.2.91-temp`；CTest仍明确排除业务目标并另跑完整业务文件。没有运行无关全套/Core/ConPTY或连接真实设备。提交前diff空白检查与文档链接检查通过，无新增缺失，AGENTS两条HEAD已有历史缺失链接单列（`build/b3b-file-commit-0.2.91-doc-check.json`）。
