# B3b 第二子批：保存目标打开与查询

## 设计与实施计划

基线0.2.87、十八工具。按06的既有设计继续实施：新增 `novaterm_open_target(targetId,configVersion,operationId)` 和 `novaterm_query_target(targetId,configVersion,operationId)`。沿用Qt6/C++17与当前checkout，不改版本、不提交/推送、不连接真实设备。

目录共享和打开授权分开；打开必须同时具有当前客户端身份、目标配置版本的目录共享及独立打开权限。撤回共享同时撤回打开权限，重新共享不会自动恢复。新建会话不继承读取、输入、清空或文件权限。

应用层仅持有无秘密、有界操作索引，组合根提供窄打开/查询端口；可见TerminalView继续拥有Session。UI按完整保存配置匹配现有connecting/running/reconnecting会话，优先复用；串口被不同配置占用时拒绝，不关闭人类会话。凭据与主机密钥决策留在现有本机通路。查询只返回会话绑定与连接事实，不返回底层错误正文。

operationId精确重试不再次调用工厂；同ID异参拒绝。记录先占位再调用工厂，防止同步重入；每客户端最多32、全局256，运行期间不淘汰，满额拒绝新操作但保留已有查询/重试。失败也保留事实，不换ID自动重试。重启不恢复记录，不自动重跑。查询核验原attachment，重连返回实际新epoch，标签换目标返回invalidated。

本批选择先交付打开/查询，因为安全关闭还需要人工接管、命令/传输忙碌事实的联合边界；不注册关闭工具。单文件传输和B4仍待后续。目录原Schema不扩张；只修正其工具说明中过期的“不可打开”一句，原十七工具对象保持。

### 任务1：应用授权与幂等记录

- [x] 新增工具真实契约RED，确认基线不支持打开。
- [x] SavedTargets增加版本绑定的独立打开选择；新增TargetConnections纯Qt Core用例，端口仅传无秘密配置标识和连接事实。
- [x] 覆盖未授权零创建、精确重试/异参、跨客户端/身份、配置变更/撤权、重入、失败保留、记录上限和查询不创建。

### 任务2：MCP与可见UI接线

- [x] 独立DTO/BackendEvent接入ToolCodec、WireSession、HTTP路由、Service、能力注册，不扩张旧九项ToolRequest。
- [x] 本机设置增加独立打开勾选；SessionPanel复用配置/凭据解析，TerminalPage按完整配置复用/创建可见标签。
- [x] 真实HTTP与本机共享UI回归：目录共享不允许打开、明确勾选后只创建一次，查询/换绑/撤权不创建。

### 任务3：验证与文档

- [x] 正常Windows Release默认目标构建，相关MCP/SessionPanel/UI测试；完整业务文件单独记录，不运行无关全套/Core/ConPTY。
- [x] 两代标准协议完整工具Schema互操作；冻结新增fixture并同步当前契约/进度/使用说明。
- [x] 审查本批差异、diff空白和文档链接，保留真实失败与未验收范围。

## 执行记录

Ruling：采用既有06设计并在本记录具体化，不再要求用户重复批准已授权的继续实施；关闭与传输不混入本批。当前checkout适合已有Windows构建复用。

### 实现与审查

新增TargetConnections纯Qt Core索引及独立TargetConnectionRequest/BackendTargetConnectionCall，不扩张旧九项终端联合。SavedTargets保存版本绑定的openAllowed；撤回目录删除对应打开权限，打开选择本身不改变目录Schema。Service使用现有准入、排队和HTTP响应路由，最终复核采用原租约validation和共享取消状态，不持有PendingRequest裸指针。

SessionPanel将原SSH/UART配置解码提取为共用本机方法，先核对无秘密configurationKey再解析凭据。MainWindow通过QPointer持有UI入口，凭据返回后再核验Guard。TerminalPage比较实际SSH完整配置（含environment/认证），串口仅在实际占用、释放中或自动重连预留时拒绝；断开且无自动重连的Failed标签不永久锁住端口。

独立只读审查确认并修复：Linux密钥环解锁会运行嵌套事件循环，原回调后复核晚于连接副作用，因此将Guard传到凭据返回后、标签注册/切换后和Session.start之前；SSH投影未覆盖environment，现对实际SshTransport::sessionConfig逐字段核对。再次复核还发现未启动视图可能已由close变为Closed，不能用Created状态判断回收；现以显式onStarting共享标记为准，未启动标签一律回收，已准入会话不因RPC断流自动关闭。最后只读复核未见>80置信的新生命周期问题；复核者未运行测试。

Service用原Transport对象上的随机运行期UUID追踪连接，避开SessionDirectory首次取得目标指纹时的attachment换代。同对象重连返回实际epoch，换Transport失效。TerminalView仅在本机主机密钥模态窗口期间设置无秘密布尔pending，query返回host_key_confirmation_required，不把认证决策交给Agent。

### RED与中间结果

- `build/b3b-connections-contract-red.txt`：新工具未实现时isKnownTool(novaterm_open_target)真实失败，2pass/1fail；首次协议GREEN为3/3（`b3b-connections-contract-green.txt`）。最终兼容对象和独立新fixture由相关CTest复验。
- 纯应用首次运行出现0xc0000005，`b3b-connections-app-crash.txt`的v2断言位置停在首次获准打开。根因为空QMap已保存cend后，非const范围遍历触发隐式分离，随后比较/使用旧迭代器。改用std::as_const遍历，`b3b-connections-app-green-2.txt`为10/10、6ms；最终增加重入守卫与全局256条预算断言并由CTest通过。原崩溃日志保留，不归因环境。
- `build/b3b-connections-http-red-3.txt`：真实Session/内存Transport首次连接后query误报invalidated，4pass/1fail，1.433秒。原Transport身份修复后`b3b-connections-http-green.txt`为5/5、1.526秒，含两代HTTP。本轮较早两份http-red日志仅为MOC更新晚于源编译、测试函数未进入旧可执行文件，不能当作产品RED；随后重新构建取得上述真实RED。
- 首轮固定源码相关CTest为21/22、39.34秒（`b3b-connections-final-ctest.txt`），唯一失败是HistoryContractsTests仍硬编码18及legacy+9；原始文件另存`b3b-connections-history-count-red.txt`（5pass/2fail）。只把新增工具总数改为20及legacy+11，旧九终端/三个历史对象逐项比较保持，未放宽或跳过原断言。
- 最初沙箱构建停于CMake目录检查，停止本次cmake/ninja后在正常Windows环境构建；不将此环境停滞算作产品RED。首轮完整默认Release构建与审查后构建均退出0；最后构建见下节。

### 固定源码验证

正常Windows环境通过BuildTools vcvarsall进入MSVC，命令：

```text
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build/Release -j 4'
ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|command_safety|resource_sharing|session|session_panel|ui_dialog_layout)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure
build/Release/bin/novaterm_mcp_tests.exe -o build/b3b-connections-final-mcp.txt,txt
python tests/mcp/interop_check.py
```

Qt6.8.3 bin进PATH，QT_PLUGIN_PATH指向同前缀plugins，TEMP/TMP为工作区build/b3b-connections-temp；不全局强制offscreen，沿用各目标现有入口。没有增加应用Python依赖，互操作沿用build/mcp-test-deps。

最终默认Release全部目标构建退出0，`build/b3b-connections-final-build-2.txt`；相关CTest **22/22，39.76秒**，`build/b3b-connections-final-ctest-2.txt`，包含MCP其余19目标、Session和两项UI，不包含完整业务文件。完整业务与互操作成绩另行记录，不拼成51项全套。

- 完整MCP业务文件 **158pass/0fail/0skip，97.975秒**，`build/b3b-connections-final-mcp.txt`，进程退出0；原ACL与全部已有业务断言保留。本批新增两代HTTP打开/查询、首次连接/重连查询及嵌套撤权零连接断言均包含其中。
- 官方SDK2025 / rawHTTP2026 **二十工具输入/输出Schema、目标目录/打开/查询/精确重试及原历史/终端/等待/任务互操作通过**，`build/b3b-connections-final-interop.txt`，退出0。协议版本分别为2025-11-25和2026-07-28，不代表两代NovaTerm工具接口或完整合规认证。
- 三项退出码另存 `build/b3b-connections-final-results.json`，均为0。文档新增/修改链接32项、0缺失（`build/b3b-connections-doc-links.txt`），git diff --check通过。

上述是固定源码的独立验证，不拼接首轮失败或历史成绩；未把相关22目标和业务文件写成全项目51项目标全绿。

### 保留范围和下一起点

本批版本保持0.2.87，未提交或推送。原十七工具对象保持；目录工具Schema未扩张，仅更正说明中的打开能力，新增两工具独立冻结fixture。README、MCP使用说明、架构索引/路线和00～06同步当前二十工具状态，较早记录保留当时事实。

HTTP/互操作使用注入的内存连接工厂，验证协议/授权/幂等和实际Session代际，不连接服务器或串口。可见TerminalView创建/回收接线通过正常Windows编译和独立静态复核，尚未做真实GUI连接、密钥环/主机密钥交互或实际设备验收，不能用替身成绩关闭B4。未跑无关全套/Core/ConPTY、跨平台、消毒器或性能验收。

下一批为安全关闭（本客户端本次创建、未人工接管、无活动执行/传输且身份仍匹配）和独立SSH/SFTP单文件传输；新会话预授权规则亦待后续，当前默认不授予终端权限。不自动部署、重启、烧录或连接设备。

## 本轮审查修复：手动断开串口的错误预留

用户要求修复qt-cpp-review确认的P2。保存配置中的reconnectSeconds不能代表运行期是否仍有重连计划：手动断开会设置_manualDisconnect并停止定时器，后续Failed也不再安排重连。

- [x] 增加真实Session＋内存Transport回归，覆盖自动重连开启后的手动断开、远端断开、禁用及不支持重连。为共用生产判断入口，先把原配置判断抽到Session查询并保持旧行为，记录真实RED。
- [x] Session查询改读实际重连定时器状态；UI端口预留消费该事实，继续保留连接中、实际连接或待释放端口的保护，不修改自动重连调度、工具Schema、版本或机械风格。
- [x] 固定源码Release构建，相关Session/UI/MCP检查并记录本轮独立成绩；不运行无关全套/Core/ConPTY，不连接真实设备，不提交或推送。

本轮真实RED：`build/b3b-serial-reservation-red.txt`为3pass/3fail、12ms，原配置判断在手动断开、不支持重连及关闭后的事实检查失败，禁用重连组通过。修复后同一回归文件不改断言，`build/b3b-serial-reservation-green.txt`为6/6、99ms（含setup/cleanup）。四组均使用真实TerminalSession和内存FakeTransport，不打开串口。

新增`hasPendingAutomaticReconnect()`只读Session的QChronoTimer::isActive()，本机UI在线程内消费；没有修改_manualDisconnect、定时器启停或重连策略。原审查者只读复核确认错误预留已消除，连接中/已连接/待释放和实际已安排重连仍受保护，未发现高可信新问题；复核者未构建/运行测试。

本轮正常Windows Release默认全部目标构建通过，`build/b3b-serial-reservation-build.txt`，退出0。固定源码相关CTest **3/3，19.47秒**（Session完整目标、SessionPanel及MCP访问/交互），`build/b3b-serial-reservation-ctest.txt`，退出0；它与前述新增回归6/6是独立两次运行，不拼接计数。本轮未重跑完整MCP业务文件/两代互操作、无关全套/Core/ConPTY或真实设备，不引用此前158/158等成绩代替本轮结果。工具Schema、二十工具数量和版本0.2.87保持，未修改机械风格、未提交或推送。

相关命令：

```text
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build/Release -j 4'
build/Release/bin/novaterm_session_tests.exe savedSerialPortReservationUsesPendingReconnect -o build/b3b-serial-reservation-green.txt,txt
ctest --test-dir build/Release -C Release -R '^novaterm_(session|session_panel|mcp_access_interaction)_tests$' --output-on-failure
```

Qt bin和QT_PLUGIN_PATH沿用前述6.8.3前缀，最终CTest的TEMP/TMP为工作区build/b3b-serial-reservation-temp。当前契约/使用说明同步“实际已安排重连”的判断，git diff --check与新增策略头空白检查通过。

## 0.2.88 提交前收口

2026-10-27用户明确要求创建本地Git提交。本次包含B3b第二子批保存目标独立打开授权、可见终端打开/复用、查询和审查修复，以及手动断开串口后的错误预留修复。按AGENTS.md当前0.2.x每提交PATCH递增约定将根版本0.2.87→0.2.88，不采用commit_check.md旧0.1.<提交数>规则。README、架构索引/路线、现行契约和使用说明均已同步二十工具状态；关闭、文件传输、新会话预授权规则及B4保持待办。

提交前按0.2.88固定源码重建并运行相关目标；本节单独记录本轮成绩，不引用此前成绩替代。不运行无关全套/Core/ConPTY，不连接真实设备，不推送，不纳入build产物或工作辅助文件，不修改机械风格。

本轮0.2.88固定源码的正常Windows Release默认全部目标构建通过（退出0，`build/b3b-connections-commit-0.2.88-build.txt`）。Qt前缀同前，TEMP/TMP为工作区build/b3b-commit-0.2.88-temp。验证分别为：

- 相关CTest **22/22，37.61秒**，`build/b3b-connections-commit-0.2.88-ctest.txt`，包含MCP其余19目标、Session及两项UI；选择命令为本记录前述22项目标正则，并排除完整业务文件。
- 完整MCP业务文件 **158pass/0fail/0skip，94.701秒**，`build/b3b-connections-commit-0.2.88-mcp.txt`，退出0。原ACL与已有行为断言保持，没有放宽或跳过。
- 官方SDK2025/rawHTTP2026二十工具Schema及保存目标目录/打开/查询/重试、历史/终端/等待/任务互操作通过，`build/b3b-connections-commit-0.2.88-interop.txt`，退出0。仅内存和本机回环夹具，不连接真实设备。

三项退出码另存`build/b3b-connections-commit-0.2.88-results.json`，全部为0。上述各批独立记录，不宣称全项目全套通过。提交范围只含本批53个文件（含源码、测试、文档和根版本），build产物、运行日志和辅助文件不入库。

提交前文档新增/修改本地链接34项、0缺失（`build/b3b-connections-commit-0.2.88-links.txt`）；git diff --check通过。
