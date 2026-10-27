# B3b 第一子批：保存目标授权目录

## 设计和实施范围

用户要求查看 P8 进度并继续实施。基线为 0.2.86；B0/B1、B2、B3a 已实现，B3b 尚未开始，B4 需要指定真实设备。本批选择先交付可实际使用的只读保存目标目录，为后续打开/查询/关闭提供显式目标和版本边界。

直接暴露 SessionStore 会泄露未共享端点及凭据引用；将其并入活动 Session 目录会混淆配置与连接身份。因此采用独立、纯 Qt Core 的应用目录，UI 从现有保存列表提供经过裁剪的描述，协议仅列出当前客户端明确获准的描述。目录不创建/拥有 Session。

新增 `novaterm_list_targets(limit=25,cursor?)`，limit 1..50。目标仅 SSH/Serial，返回 targetId、title、transport、endpoint、configVersion 和 availability（saved_configuration，表示保存配置存在，不代表在线）。不读取凭据、不返回认证方法/私钥路径/credentialRef，不推断可复用活动会话。打开、输入、清空和文件权限均不随目录共享授予。

共享只在本次应用运行有效，绑定客户端当前身份版本及目标配置版本；配置变更、删除、客户端撤销/轮换后失效，重启默认关闭。源目录最多 256 项，共享记录最多 4096 条，title/endpoint 各最多 256 UTF-8 字节。越界源更新拒绝并清空旧目录，避免继续暴露已失效描述。分页游标签名并绑定客户端、身份版本和目录修订；变更后明确拒绝续页，不混合两个快照。沿用现有请求调度/短期限，不新增采样、文件 I/O 或设备操作。

## 实施计划

- [x] 协议 RED：新工具未注册时，真实契约断言失败；旧 17 工具对象冻结保持。
- [x] 应用目录：配置版本、独立共享、有限预算、变更失效、稳定分页及客户端隔离专项。
- [x] MCP 接线：独立 DTO/事件，不修改旧九项终端请求联合；沿用鉴权、调度和响应封装。
- [x] 本机 UI：保存列表发布无秘密摘要；MCP 设置按客户端勾选目录共享，过期行不能授权新配置。
- [x] GREEN 和验证：默认 Release 构建，相关 MCP/SessionPanel 专项；按实际输出记录，不运行无关全套或连接设备。
- [x] 同步 00～06/README、架构索引、使用说明与本记录，保留后续打开/关闭和文件传输待办，不提交/推送。

## 决策记录

本批先冻结并实现目录这个有独立消费者的边界；不会将完成目录写成完成整个 B3b。UI 和领域实现留在当前 checkout。Git 只在命令中使用 safe.directory，不修改全局配置。不会自行创建提交或更改版本。

## 实际验证

### RED、独立审查与修复

日志均为本轮 `build/b3b-*`；不把历史全套成绩代入本批。

- `b3b-contracts-red.txt`：新工具尚未注册，isKnownTool 真实失败（2pass/1fail）；新独立 fixture 生成后 `b3b-contracts-green.txt` 为3/3。旧B3a夹具取固定13..16项比较，不重写其对象。
- 独立审查发现全局撤权传空client时只删除空键，可能保留其他客户端旧共享。`b3b-revoke-red.txt` 双客户端用例真实失败（2pass/1fail）；空client现明确清全部共享并换修订，完整目录文件 `b3b-app-green.txt` **7/7，15ms**。
- 首轮完整业务文件 `b3b-mcp-detail.txt` **152pass/1fail，94.720秒**，唯一失败是新增真实HTTP目录用例；旧外来属主ACL夹具本轮通过，不套用历史1307解释新失败。根因是HttpMcpEndpoint未为BackendSavedTargetsCall登记jobs路由，业务完成时无法回送，响应为空；`b3b-http-red.txt` 增强诊断后再次复现（2pass/1fail，152ms）。补齐同类事件登记，首个空目录断言也检查ok，避免把空响应当合法空目录。
- 第二次独立只读复核已核对ToolCodec/WireSession/HttpMcpEndpoint/Service/事件映射，没有发现其余高可信阻断项；复核者未运行测试。GUI刷新使用排队连接，不在itemChanged栈里删除当前行。

### 构建与首轮专项

正常环境以BuildTools vcvarsall进入MSVC；沙箱首次重生成停滞及Qt DLL初始化失败单列为环境问题。首个正常默认构建触发新增局部名data遮蔽QWidget::data的/WX错误，已仅更名为configurationBytes；未改编译选项。默认并发导致响应明显变慢，停止本次Ninja进程树后用 `cmake --build build/Release -j 4` 续建成功，日志 `b3b-build-j4.txt`（退出0）。这不是性能验收。

首轮相关CTest `b3b-ctest.txt` **21/21，23.17秒**，包含MCP其余19目标与SessionPanel/UI布局两项，不包含业务目标。此成绩早于HTTP路由修复，最终源码须另行复验，不能代替最后结果。

复现环境：Qt6.8.3 bin进PATH，QT_PLUGIN_PATH指向同前缀plugins，TEMP/TMP为工作区 `build/b3b-temp`；不全局强制offscreen。业务自行采用原有offscreen入口。当前 `ctest -N` 实核51目标，MCP标签20目标；注册数不代表全套通过。

相关命令：

```text
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build/Release -j 4'
ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|command_safety|resource_sharing|session_panel|ui_dialog_layout)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure
build/Release/bin/novaterm_mcp_saved_targets_tests.exe -o build/b3b-app-green.txt,txt
build/Release/bin/novaterm_mcp_tests.exe -o build/b3b-mcp-detail.txt,txt
```

### 最终源码复验

HTTP路由修复后再次执行默认Release全部目标构建，`b3b-final-build.txt` 退出0。后续只更新文档和Python互操作验证；C++源码固定。

- 新HTTP目录/本机共享窗口两条专项（含setup/cleanup）**4/4，404ms**，`b3b-http-green.txt`。覆盖真实HTTP目录分页、未共享隐藏、跨客户端/篡改游标拒绝、配置失效，以及UI逐客户端勾选、切换和令牌轮换。
- 最终相关CTest **21/21，22.04秒**，`b3b-final-ctest.txt`，命令同上。包含旧兼容契约/两代Wire/HTTP/endpoint/授权/执行/等待/任务/资源安全，以及SessionPanel和UI布局；不包含业务文件。
- 完整业务文件 **153pass/0fail，97.169秒**，`b3b-final-mcp-detail.txt`，退出0；原外来属主ACL用例本轮通过，未放宽或跳过断言。它与前项是两批验证，不拼成一次全套。较早152pass/1fail的路由遗漏成绩原样保留。
- 文档新/改链接检查30项、0缺失；`git diff --check`通过。全项目注册51项、MCP标签20项，仅表示当前注册。没有重跑无关全套或Core/ConPTY。

独立标准库rawHTTP检查已通过2025-11-25和2026-07-28：两代均列18项工具，新目录工具对象等于独立冻结fixture，实际目录响应ok且structuredContent与content.text相等；日志 `b3b-raw-interop.txt`，脚本 `build/b3b-raw-interop.py`。仅启动内存SSH夹具，不连接真实服务器。

官方SDK2025/rawHTTP2026完整互操作已通过：**18项输入/输出Schema、新保存目标目录响应、历史/终端兼容及B3a等待/启动/查询/停止/精确重试**，日志 `b3b-interop.txt`，退出0；命令 `python tests/mcp/interop_check.py`，同上Qt/TEMP环境。标准库raw检查与SDK完整脚本是独立两次运行，不合并成绩。

原Python环境缺包，按项目锁定mcp==1.30.0/jsonschema==4.26.0及其依赖安装到工作区build/mcp-test-deps；下载/安装完成，日志 `b3b-python-deps.txt`。只用于互操作夹具，不改全局Python或应用运行依赖。C++产物在互操作前已固定，没有连接真实设备。

## 下一起点和保留范围

### 本轮两项审查修复计划

用户要求修复审查发现的两项行为问题，并明确不修改代码风格。

- [x] 先新增SSH/UART标签及无标签回退、用户名不外泄的真实RED，再仅修改目录名称投影。
- [x] 新增真实GUI共享→修改Session读取/输入授权→目录及分页仍有效的RED；客户端身份代际与访问权限版本分离，原RPC准入继续使用访问版本。
- [x] 保留并验证令牌轮换、旧GUI行、客户端删除、全局停用及外部配置重载的身份失效行为，不修改接入文件格式、工具Schema或权限边界。
- [x] 正常Release构建、相关CTest、完整MCP文件和两代SDK互操作，记录本轮独立成绩；不改机械风格，不提交/推送、不连接设备。

### 两项审查修复实现和RED/GREEN

名称投影仅更改savedTargetSources()：优先label.trimmed()，空标签回退SSH host/UART portName。原保存列表/连接详情函数不做风格或行为重构；目录不再把SSH username拼进title。

客户端注册表新增独立运行期identityVersion和分配高水位；普通Session权限仍按原access.version推进并复核RPC，目录grant、GUI旧行和签名游标使用独立身份代际。轮换令牌、客户端删除/重建、全局停用及原全局撤销继续失效。clear/replaceAll不归零身份高水位；保存失败恢复旧记录，保留已分配高水位。外部接入文件reload对未变化令牌保留身份，token变化时分配新身份；私有接入JSON格式和十八项工具Schema保持。

仅补必要实现、回归和契约；没有处理PAT-2、PAT-7、PAT-3等lint机械风格。

- 名称RED：`b3b-review-title-red.txt`，SSH标签/空标签、UART标签/空标签四组全部真实失败（2pass/4fail，34ms），可见实际SSH标题包含private-auth-user。修复后 `b3b-review-title-green.txt` **6/6，44ms**，包含用户名/私钥路径不进入对外名称断言。
- 身份RED：`b3b-review-identity-red.txt` **2pass/1fail，248ms**，真实本机勾选目录后修改Session权限，HTTP目标从2项变0。修复后 `b3b-review-identity-green.txt` **3/3，3.045秒**：读取撤回/授予、完整输入授予/撤回后两项目录、原分页和UI勾选保持；原访问版本仍递增。另核验轮换token/旧GUI行拒绝、全局停用/重新启用和客户端删除。
- 身份注册表专项 `b3b-review-registry-green.txt` **3/3，3ms**，核验普通权限版本推进独立、同ID删后重建、清空后不复用、全局身份变化不替代访问版本。
- 跨实例专项 `b3b-review-reload-green.txt` **3/3，156ms**，使用现有RetainedCredentials内存夹具：无关配置reload保持、token替换/全局停用/删除身份失效、重新启用不复活旧身份。没有连接真实设备或操作系统密码。
- 独立只读复核已核对名称投影、身份分配/重载/回滚、旧行拒绝和原RPC准入；未发现>80置信的新行为缺陷，复核者未运行测试。

日志目录为工作区build/，本轮TEMP/TMP为build/b3b-review-temp，Qt环境同前。默认Release构建 `b3b-review-build.txt` 退出0。最后固定源码验证分别为：

- 相关CTest **21/21，19.08秒**，`b3b-review-ctest.txt`，选择命令同前（MCP其余19目标及两项UI），退出0。
- 完整MCP业务文件 **154pass/0fail，96.394秒**，`b3b-review-mcp-detail.txt`，退出0，未放宽或跳过原断言。
- 官方SDK2025/rawHTTP2026完整互操作通过，`b3b-review-interop.txt`，退出0，仍为18项工具Schema及目录/历史/终端/等待/任务/精确重试检查，工具Schema未改。
- 文档新/改链接31项0缺失，`git diff --check`通过。各专项计数含setup/cleanup，不与上轮成绩拼接；未跑无关全套/Core/ConPTY，未连接真实设备。

本记录的“两代互操作”指MCP标准协议版本2025-11-25和2026-07-28的通信检查；原NovaTerm工具名称/参数/响应的兼容另由冻结fixture和业务回归检查。互操作通过不等同完整标准合规认证。

下一批实施保存目标打开/复用/查询/关闭，须新增独立打开权限，不把本批目录共享自动升级；沿用可见TerminalView/Session创建通路，并核验配置版本、operationId幂等、人工接管与忙碌关闭。单文件传输仍需补后端取消、有限期限和发布事实，不能直接把现有普通SFTP接口开放给MCP。

以上实施/审查阶段未连接真实SSH/UART，不执行部署/重启，不做跨平台、消毒器或性能验收，不运行无关全套/Core/ConPTY。目录的saved_configuration只说明已保存，不表示设备在线或认证有效；未声称B3b/B4整体完成。此前版本保持0.2.86，未提交或推送；用户随后要求的本地提交收口见下节。

## 0.2.87 提交前收口

2026-10-27用户明确要求创建Git提交。本次包含B3b第一子批保存目标目录、HTTP响应路由/全局撤权修复、两项行为审查修复、回归及契约/实施文档。按AGENTS的0.2.x每提交递增约定将根版本0.2.86→0.2.87，不采用commit_check.md旧0.1.<提交数>规则；同步README和Development_Roadmap中过期的十七工具状态。没有修改代码风格，不纳入build产物或工作辅助文件，不推送。

同版本固定源码的默认Release构建通过（退出0，`build/b3b-commit-0.2.87-build.txt`）。本轮Qt环境同前，TEMP/TMP为工作区build/b3b-commit-0.2.87-temp，验证分别为：

- 相关CTest **21/21，20.28秒**，`build/b3b-commit-0.2.87-ctest.txt`，包含MCP标签其余19目标和novaterm_ui_dialog_layout_tests、novaterm_session_panel_tests。选择命令为本记录前述-R正则，并-E排除业务文件。
- 完整 `novaterm_mcp_tests.exe` QTest另存文件 **154pass/0fail，95.338秒**，`build/b3b-commit-0.2.87-mcp-detail.txt`，退出0；未放宽、跳过或删除原断言。
- `python tests/mcp/interop_check.py` 官方SDK2025/rawHTTP2026十八工具Schema及目录/历史/终端/等待/任务/精确重试互操作通过，`build/b3b-commit-0.2.87-interop.txt`，退出0。这里指MCP标准协议版本2025-11-25和2026-07-28，不是两代NovaTerm工具接口。

文档新/改链接34项0缺失，git diff --check通过。上述是0.2.87同版本的新一轮验证，各批独立，不据此宣称51项目标全套通过。没有重跑无关全套/Core/ConPTY、跨平台/消毒器/性能或真实设备；B3b目标打开/复用/查询/关闭、单文件传输及B4仍待后续。
