# B3b 第四子批：新会话预授权

## 0.2.90 提交前收口

用户要求先创建本地Git提交，再继续MCP模块。将本批新会话预授权、测试与文档一并收口，按AGENTS.md每提交PATCH递增约定将根版本0.2.89→0.2.90。不推送，不纳入build产物/日志和本机辅助文件，不改变机械风格，不连接真实设备。

提交前在0.2.90固定源码重新执行正常Windows Release默认全部目标构建，退出0（`build/b3b-sharing-commit-0.2.90-build.txt`）。相关CTest **22/22，43.28秒**（`build/b3b-sharing-commit-0.2.90-ctest.txt`），完整MCP业务文件 **234pass/0fail/0skip，125.336秒**（`build/b3b-sharing-commit-0.2.90-mcp.txt`），官方SDK2025/rawHTTP2026 **二十一工具互操作通过**（`build/b3b-sharing-commit-0.2.90-interop.txt`），各自退出0。退出码见 `build/b3b-sharing-commit-0.2.90-results.json`，22项与完整业务分开运行，不拼为一次全套；下面0.2.89成绩保持原事实，不代替本轮验证。

Qt bin/plugins沿用6.8.3前缀，TEMP/TMP为工作区 `build/b3b-sharing-commit-0.2.90-temp`；验证命令与下方相同，仅日志前缀改为本节0.2.90。没有运行无关全套/Core/ConPTY或连接真实设备。

用户随后要求单独列实机测试清单，新增[独立清单](../B4_Test_Report.html#checklist)并同步入口。清单全部项目初始未执行，分开当前可测与文件传输后续补验；纳入本次本地提交，不据此宣称实机通过。提交后继续文件传输子批，未实施代码不混入本提交。

用户随后显式调用qt-cpp-review：完整确定性扫描17个C++文件，74条提示均位于未改动旧行，改动行无提示；六领域独立只读复核未发现高可信新增问题或待核查项。该审查没有运行构建/测试，不代替提交前验证。

## 设计与实施计划

> 执行：superpowers:executing-plans；按任务进行 RED/GREEN 和最终独立审查。

**目标：** 本机逐客户端、逐保存目标显式选择新会话的读取、完整终端输入和清空权限，连接就绪后绑定一次。

**架构：** SavedTargets 保存本次运行有效的规则及规则修订；TargetConnections 仅在首次成功创建后发布窄通知。Session 适配器以弱引用等待原 Transport/连接代际就绪，经现有 AuthorizationService 原子初始化空授权行；不新增 Session owner。

**技术：** Qt 6.8.3 / C++17，现有 Windows Release checkout。

**依据：** [06 第5节](../06_Terminal_Workflow_Extensions.md#5-已保存目标连接管理)及[实施顺序](../01_Phases_and_Acceptance.md#b3b文件传输和保存目标连接)。

约束：默认全关；复用、精确重试、重连不得重新授权；不覆盖已有授权行，包括用户已经撤销的行。输入/清空需要读取；关闭读取级联关闭两项。撤回允许打开清除规则，重新开启不复活。普通会话授权不改变保存目标规则。

方案：采用已有共享规则和授权服务；在打开时直接授权无法正确绑定首次 SSH 指纹，在查询时授权会让精确重试覆盖人工撤权，故使用首次创建通知及有界一次性等待。脚本、旧命令、托管提权、文件权限不纳入本批。

等待最多120秒，全局最多256项（不超过原连接操作总预算）；无规则不占名额。规则修改/目录撤回/配置重存/身份轮换/停用、连接失败/重连/换绑、人工输入或显式连接动作均丢弃待授权。SSH主机密钥仍由本机处理；首次指纹发现可更新绑定，就绪后核验实际当前绑定。已绑定权限随后按现有会话授权规则管理，修改预授权只影响尚未生效及未来创建操作。

Ruling：用户已要求按现有文档继续实施，沿用前批的设计与记录位置，直接完成可审查实现；不重复设计批准，不提交/推送、不改版本、不连接设备。代价：本批只完成连接预授权，受限单文件传输和B4继续保留。

## 任务与验证

### 任务1：规则、首次创建通知及原子授权

- [x] 测试：本机设置中选择预授权后，新建运行中连接得到对应权限；先记录旧实现真实失败。
- [x] SavedTargets 增加独立读/输入/清空选择和不复用的规则修订，校验客户端/配置版本，清除/撤回不复活。
- [x] TargetConnections 追加一次性 Created 通知；只在原创建成功且最终准入仍有效时调用，重试/复用/拒绝均零通知。
- [x] AuthorizationService/AccessStore 提供原子初始化空行入口，拒绝旧行、非就绪与不支持的权限，单次推进访问版本和通知。

### 任务2：Session 接线及本机设置

- [x] 新增 TargetSessionSharing 适配器，有界弱引用等待；在原请求完成后排队应用，避免授权版本变化使打开响应自失效。
- [x] MCP设置增加三个明确列，保留既有四列语义；读取级联、输入/清空独立，陈旧界面仍按身份/配置拒绝。
- [x] 两代HTTP和真实Session/内存Transport覆盖：默认拒绝、独立选择、首次连接、重试不恢复、复用不授权、撤回/重存/轮换/人工接管/重连/换绑及已有行。

### 任务3：验证、审查和文档

- [x] Release默认构建；相关MCP、Session、SessionPanel/UI测试；完整MCP业务及两代互操作分别记录，不运行无关全套/Core/ConPTY。
- [x] 更新现行契约/说明/进度及AGENTS导航，二十一工具Schema保持。
- [x] 独立只读复核所有权、同步重入、撤回再授权、首次指纹与回调生命周期；diff及文档链接检查。
- [x] 完整业务新增40项场景后超过原CTest的120秒期限；仅将该目标TIMEOUT调整为180秒，经正常默认构建重新配置后，复验注册的CTest入口。

## 审查重点

1. 首次指纹发布和目录同步重入不会把旧 Entry 或容器引用带过回调。
2. 预授权不增加会话 owner，不在 HTTP 线程访问 Session，不保存明文凭据。
3. 用户在等待期间操作或修改授权后，不被迟到回调覆盖。
4. 撤回再开启、同连接重连及相同操作重试均不恢复一次性授权。
5. 已授权会话的正常权限变更不撤掉无关保存目标共享。

## 执行记录

本页不引用此前成绩作为本轮结果。基线HEAD为 `c2b74f7`（0.2.89）；本轮版本保持，不提交/推送、不连接真实设备。

### RED/GREEN及中间结果

- 本机界面选择后的两代HTTP：`build/b3b-sharing-red.txt` **6pass/6fail，2.687秒**，原实现未授予已选权限；默认全关及复用拒绝场景通过。首次接线后连同原打开/查询回归 **14/14，4.235秒**（`build/b3b-sharing-green.txt`）。
- 扩展等待矩阵首轮 **26pass/6fail，11.364秒**（`build/b3b-sharing-pending-red.txt`）：其中4项是两个夹具不满足Session状态机（Connecting直接重连、换绑后错误启动）；按真实生命周期修正，原产品断言保持。另2项为目录移除/重加错误复活待授权。单独只跑该行为复现 **2pass/2fail，769ms**，退出2（`build/b3b-sharing-directory-red.txt`）。
- 修复为稳定共享票据和同步失效回收：票据只共享等待元数据，Session/Transport仍为QPointer；目录移除、换绑/代际变化同步移除，首次合法指纹保留。process快照只复制票据，目录find及授权回调后再次按成员资格复核，不跨回调保存容器引用。
- 扩展后两代HTTP专项 **44/44，15.934秒**（`build/b3b-sharing-pending-green.txt`，退出0），含首次连接/指纹/主机密钥待决、规则取消再启、打开撤回、配置重存、令牌轮换、人工输入、重连/换绑、手工授权行、目录移除重加、失败和全局停用。重试不恢复人工撤权，重连不恢复输入/清空。
- 纯应用授权事务与保存目标两个CTest目标 **2/2，0.18秒**，验证原子初始化禁止旧行覆盖、拒绝不支持权限和非就绪事实，规则隔离/级联/修订，以及首次创建通知仅一次。最终22项验证另行记录。
- 首次沙箱构建停在CMake目录检查，已中止并使用正常Windows构建，不作为产品RED。首次GREEN构建出现LNK2019：构建期间将规则参数由int换为具名枚举，造成两侧编译签名不一致；固定源码重建成功。后续没有在编译时修改源码。原日志分别为 `b3b-sharing-red-build.txt`、`b3b-sharing-green-build.txt` 和 `b3b-sharing-green-build-2.txt`。

### 独立只读复核

按requesting-code-review安排独立复核，无源码修改、无构建/测试；未发现高可信Critical/Important/Minor。检查弱引用、稳定票据、授权初始化拒绝已有行、规则修订、身份变化、人工操作、目录移除、首次指纹和普通Session授权独立性。

审查另考虑created通知未直接比较facts代际/Transport标识；当前生产同步调用链无可达换绑窗口，目录返回Entry值拷贝，故不列确证问题、不作防御性扩张。Ruling：保留既有同步窄端口信任边界；如果未来允许通知异步化，必须在通知处重新核验原创建标识，否则可能绑定错误连接。真实SSH/UART、跨平台及性能留B4，不将源码复核当作这些验收。

### 固定源码最终验证

正常Windows MSVC BuildTools、Qt6.8.3，Release默认全部目标构建通过，退出0（`build/b3b-sharing-final-build.txt`）。

相关CTest **22/22，40.79秒**（`build/b3b-sharing-final-ctest.txt`），包括本轮相关MCP应用/协议/HTTP、授权、Session及SessionPanel/UI；完整MCP业务排除在这22项之外。没有运行无关全套、Core、ConPTY或SFTP面板专项。

完整MCP业务文件 **234pass/0fail/0skip，120.756秒**（`build/b3b-sharing-final-mcp.txt`），进程退出0；保持原ACL及全部已有断言，新增40项两代预授权场景均包含其中。完整文件单独执行，不拼为全项目全套。该成绩暴露出原CTest单目标120秒已不足；本轮仅将 `novaterm_mcp_tests` 的 TIMEOUT 调至180秒，保留完整注册入口和原断言，不拆分或跳过场景。

官方SDK2025/rawHTTP2026 **二十一工具互操作通过**（`build/b3b-sharing-final-interop.txt`），退出0，原Schema/目录/打开/查询/重试/关闭复用拒绝、历史/终端和等待/任务检查保持。该互操作夹具仍是复用会话，不将其成绩表述为新增预授权成功场景；新增成功/失效场景由上述真实Session+内存Transport的两代HTTP覆盖。

期限调整后重新配置及Release默认全部目标构建通过，退出0（`build/b3b-sharing-final-build-2.txt`）。构建、22项CTest、完整业务与互操作退出码分别为0，保存在 `build/b3b-sharing-final-results.json`，分批记录，不套用0.2.89提交前成绩或拼成一次全套。注册业务目标的额外复验单列于下方。

注册入口额外复验 **1/1，目标120.99秒，总121.00秒**（`build/b3b-sharing-final-registered-mcp.txt`），退出0；日志确认生成的TIMEOUT为180，使用CTest实际工作目录。此项与前面的22项分开运行，不拼成一次23/23；业务的234项精确计数来自前述独立QTest文件输出。

命令：

```text
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build/Release -j 4'
ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|command_safety|resource_sharing|session|session_panel|ui_dialog_layout)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure
build/Release/bin/novaterm_mcp_tests.exe -o build/b3b-sharing-final-mcp.txt,txt
python tests/mcp/interop_check.py
ctest --test-dir build/Release -C Release -R '^novaterm_mcp_tests$' -V
```

Qt bin为 `C:/Programs/Qt/6.8.3/msvc2022_64/bin`，QT_PLUGIN_PATH为同前缀plugins；TEMP/TMP为工作区 `build/b3b-sharing-final-temp`，不全局设offscreen。SDK依赖沿用 `build/mcp-test-deps`，应用不增加Python运行依赖。

最终git diff --check通过。修改文档本地链接233项检查无新增缺失；AGENTS两条既有历史Merge_Gitee_2026-10-22/23链接目标不存在，HEAD也有同一引用，不改写历史。打开工具fixture仅description变化，Schema/annotations保持；结果见 `build/b3b-sharing-doc-check.json`。

### 保留范围与下一起点

本轮版本0.2.89、二十一工具，不提交/推送、不纳入build产物/日志或本机辅助文件，不改机械风格。新会话预授权本批源码/替身与协议路径已实现；真实SSH主机密钥/认证、真实UART、本机交互视觉、其他平台、期限边缘/大并发时延及完整BSP闭环仍待B4。本轮不连接真实设备，不以正常构建或替身回归代替这些验收。

下一功能批次是06第4节受限SSH/SFTP单文件传输：本机资源引用和远端路径读写独立授权、默认不覆盖、进度/取消与查询、路径/链接/发布竞争复核、字节/发布/校验事实分离及未知结果不可自动覆盖重试。现有预授权不提前授予文件权限。
