# UART 独立发送授权与只读发现

基于f9bbff6/0.2.93，继续06工作流中UART/ZMODEM下一项。本子批完整交付本机独立授权及MCP发现，新增 `novaterm_list_serial_file_targets`，旧二十五工具对象/Schema保持，完成后共二十六工具。上传/查询/取消的UART执行入口仍为下一子批，本批不打开串口、不发送文件或rz、不授予终端权限。

## 设计

选择单独的UART授权目录，复用现有本地文件读授权作为未来文件源。SSH远端目录授权具有路径与主机验证语义，不能当作UART发送许可；已有list_file_resources封闭输出保持，不塞入UART条目。授权的含义是允许未来的ZMODEM单文件发送，目录元数据不证明当前输入lease空闲或接收端已准备。

- 纯应用 `SerialFileTargets` 维护本次运行有效的授权。每客户端最多8项，全局32项；同一当前绑定更新保持targetId并轮换revision，撤回再共享产生新ID，旧revision不能撤回新授权。名称显式共享，UTF-8最多256字节，拒绝控制字符。
- `SerialFileBinding { SessionBinding connection; quint64 ioGeneration; QString configDigest; }` 绑定Session/attachment/epoch/Session generation、原Transport所对应attachment、真实SerialIoState generation与构造时端口/波特率/数据位/校验/停止位/流控摘要。UART允许缺少SSH式目标指纹，不能把端口名冒充SSH可信身份。
- `SerialFileTarget { QString id, title, revision; SerialFileBinding binding; }`。纯store提供share/list/remove/revoke/invalidateSession/invalidateBinding/bindings及sameBinding；所有入口校验独立client identityVersion，不依赖普通终端授权版本。
- `SerialFileTargetAdapter` 复核真实SerialTransport、Session Running、I/O connected/desiredOpen且非closePending/stopping、8数据位和有效配置；share在任何同步目录回调后复核原完整绑定和身份。失效/断开/换代/换绑及时回收旧授权额度，普通会话读权限变动保持；令牌轮换/全局停用/删除/服务停止回收。只读查询再次复核绑定与授权，旧UI快照不能给新连接授权。
- 本机文件授权窗口增加“串口 ZMODEM 发送”，明确勾选“允许ZMODEM发送”，路径和读取选项不适用；显示共享名称、当前连接，沿用固定客户端主体/单窗/旧revision选择保护。没有共享即无可发现条目。
- 新工具无参数，返回 `{instanceId, availability:"authorization_only", targets:[{targetId,title,revision,sessionId,epoch,protocol:"zmodem"}]}`，targets最多8项。无终端读取权限仍能发现本人授权；不披露端口、配置摘要、本机路径或凭据。客户端不能通过协议创建/修改授权。

## 实施计划

采用已有工作区及TDD；实现任务按subagent-driven-development分离文件责任并统一构建。用户仅要求先提交已完成工作，本子批保持工作区增量，不再自动提交/推送或改版本，不改机械风格，不连接真实设备。

1. [x] 应用授权store与Serial适配器：`application/SerialFileTargets.*`、`SerialFileTargetAdapter.*`、Budget及`tests/mcp/SerialFileTargetsTests.cpp`；身份/绑定每维变化、额度、更新/撤回/重建等真实RED/GREEN。
2. [x] 协议契约/路由/Service及能力登记：独立typed request、工具Schema/说明、Backend/Wire/HTTP路由，旧25项fixture保持，新增fixture；契约RED/GREEN与两代互操作。
3. [x] 本机UI与真实Session/内存串口状态HTTP回归：无终端权限发现、方向隔离、外客户端、旧I/O代际/Session换绑、撤权/令牌、旧UI与额度回收；不触发SerialTransport.connectToHost。
4. [x] Release默认全部目标构建，相关CTest/完整MCP业务/两代26工具互操作分别记录，独立只读复核及文档同步；不重复无改动的完整串口/Core/ConPTY或无关全套。

## 验证记录

### 实际RED/GREEN

| 验证 | 本轮结果 | 原始日志 |
| --- | --- | --- |
| 纯授权store RED | **2pass/10fail，3ms** | `build/b3b-uart-auth-store-red.txt` |
| 新工具契约RED | **2pass/1fail，2ms** | `build/b3b-uart-auth-contract-red.txt` |
| 适配器/新UI RED | **2pass/9fail，529ms** | `build/b3b-uart-auth-adapter-ui-red.txt` |
| 纯授权store完整GREEN | **12/12，4ms** | `build/b3b-uart-auth-store-green.txt` |
| 完整契约GREEN | **28/28，31ms** | `build/b3b-uart-auth-contract-green.txt` |
| 新typed Wire专项 | **3/3，7ms** | `build/b3b-uart-auth-wire-green.txt` |
| 新HTTP/UI及旧窗口保护联合专项 | **19/19，4.972秒** | `build/b3b-uart-auth-ui-green.txt` |

新API拒绝占位/未注册工具先取得真实RED，再实施。Wire专项没有单独取得RED，不与契约RED混称。首次HTTP接线构建因两个if初始化变量同名触发MSVC C4456 /WX（`build/b3b-uart-auth-http-red-build.txt`），局部改名后构建通过（`build/b3b-uart-auth-http-red-build-2.txt`），该编译问题不当作行为RED。新增fixture由契约测试程序读取实际ToolCodec.catalog导出，旧fixture未修改，兼容比较保持。

真实Session搭配未启动物理worker的SerialTransport/SerialIoState测试覆盖：无终端权限的两代发现、跨客户端、普通会话授权独立、真实I/O代际先于GUI通知变化、配置变化、待断开/实际关闭、Transport换绑、目录重加、7数据位拒绝、令牌/停止回收、旧UI及明确勾选。端口待写字节保持0，没有调用connectToHost或连接设备。

独立只读复核未见高可信Critical/Important新问题；检查身份/revision、I/O代际配置、同步回调后重读、旧绑定定向回收、UI选择及typed路由。审查者未修改代码、构建或运行测试，不以静态结论代替动态成绩。

### 固定源码最终验证

正常Windows MSVC BuildTools/Qt6.8.3 Release默认全部目标构建通过（`build/b3b-uart-auth-final-build.txt`）；相关CTest **28/28，60.09秒**（`build/b3b-uart-auth-final-ctest.txt`），完整MCP业务 **259pass/0fail/0skip，130.383秒**（`build/b3b-uart-auth-final-mcp.txt`），官方SDK2025/rawHTTP2026 **二十六工具互操作通过**（`build/b3b-uart-auth-final-interop.txt`）。退出码全部0（`build/b3b-uart-auth-final-results.json`），各批分别记录，不拼为全项目全套。

```text
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build/Release -j 4'
ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|sftp_panel|command_safety|resource_sharing|session|session_panel|ui_dialog_layout)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure
build/Release/bin/novaterm_mcp_tests.exe -o build/b3b-uart-auth-final-mcp.txt,txt
python tests/mcp/interop_check.py
```

Qt bin/plugins使用6.8.3，QT_QPA_PLATFORM=offscreen、NO_PROXY=*，TEMP/TMP为 `build/b3b-uart-auth-final-temp`。新目标带mcp/p8/security标签；未重复无改动的完整串口、纯协议、Core/ConPTY或无关全套，不套用上一子批181项串口成绩。旧MCP ACL等断言保持，本轮通过。互操作检查新目录空授权发现及参数拒绝；已授权非空目录由两代真实HTTP/内存串口专项覆盖，不冒充真机发送。

同步现行契约、使用说明、架构/路线图、阶段入口与A09实机清单，均保留未执行真机状态。diff空白及文档链接检查无新增缺失；AGENTS两条既有历史链接缺失单列（`build/b3b-uart-auth-doc-check.json`）。

本轮前置工作已先提交为f9bbff6（0.2.93），本授权目录子批为其后的未提交增量；不再改版本、未推送。当前UART上传执行、接收准备、按波特率总期限、原操作取消与ZSKIP/未知接收结果语义仍未开放，真实设备和B4未验收。

## 0.2.94 提交收口

按用户要求创建本地Git提交，版本0.2.93→0.2.94。实现源码保持上一轮已验证状态；上节28项相关CTest、259项业务及互操作仍保留原轮次。此次版本调整后正常Windows Release默认全部目标构建通过（`build/b3b-uart-auth-commit-0.2.94-build.txt`），协议相关CTest **4/4，10.81秒**（contracts/wire/http/endpoint，`build/b3b-uart-auth-commit-0.2.94-ctest.txt`），两代26工具互操作通过（`build/b3b-uart-auth-commit-0.2.94-interop.txt`）。退出码均0（同前缀results.json）。没有重复完整业务/串口或无关全套。

提交前qt-cpp-review按用户要求跳过机械风格，六领域只读复核未见高可信新行为问题。构建命令沿用上节，CTest筛选为 `^novaterm_mcp_(contracts|wire|http|endpoint)_tests$`，TEMP/TMP为 `build/b3b-uart-auth-commit-0.2.94-temp`，其余环境相同。不推送、不纳入build产物/日志，未连接设备。用户授权后续每阶段实现→行为审查→修复→本地提交，直至MCP软件范围收口，实际设备测试留给用户。
