# mcp/P7 外部 Agent 与产品能力扩展实施计划

> For agentic workers：实施使用 superpowers:executing-plans；每个后端/能力族独立交付验收。

**Goal:** 复用P6聊天UI托管外部Agent，逐步扩展SFTP/Profile/片段/Skills等产品能力。
**Architecture:** 外部Driver→独立进程/SDK适配→统一事件；产品工具始终走P1授权。MCP保持现有HTTP，不照搬Netcatty的stdio/TCP产品桥。
**Tech Stack:** C++17、Qt QProcess/异步I/O、Qt Test；sidecar仅在真实需求下独立评估。
**Spec:** [总体架构](00_Overall_Architecture.md)；依赖[P1](P1_Capability_Runtime.md)、[P6](P6_Agent_Runtime_and_UI.md)。

## Global Constraints

不一次实现全部后端。外部原生本机文件/网络权限与产品scope分开，不能复制Netcatty App Server Auto→danger-full-access。不默认自动开主机/全局Agent/公共CLI，不将JS SDK放进Core。

所有模块必须遵守[总体架构](00_Overall_Architecture.md)§2.1～§2.4的低耦合、高内聚规则；下列专项结构验收是本阶段退出条件。

## Review Focus

- 裸thread ID跨runtime续接：T1绑定后端/二进制/runtime。
- 原生批准与产品批准互相错误授权：T2区分来源/有效期，不转永久grant。
- crash/提问挂起后永久等待：T2统一取消/进程退出。
- 本地远端路径混用/工具取密：T3真实资源和凭据边界。
- Skill或CLI绕过授权：T4仅指导模型，所有执行仍经P1。

## 文件与接口

新建 src/agent/drivers/ExternalAgentDriver.*、ExternalAgentSessionIdentity.*、ExternalAgentProcess.*、ExternalAgentEventAdapter.*；
后端代码放drivers/external/<backend>/，仅为实际后端建目录。
按实际服务新增 src/capabilities/adapters/SftpCapabilityAdapter.*、ProfileCapabilityAdapter.*、SnippetCapabilityAdapter.*；
可选src/agent/skills/SkillCatalog.*、SkillContextBuilder.*；扩展P6设置。
测试 tests/agent/ExternalAgentTests.cpp、ExternalSessionIdentityTests.cpp、ProductToolBoundaryTests.cpp、SkillContextTests.cpp；目标novaterm_external_agent_tests。

接口：
- ExternalSessionIdentity：backend、binaryIdentity、runtime、authMode、cliMode、externalThreadId、version；不兼容则新线程。
- ExternalAgentDriver复用TurnDriver，DriverCapabilities决定modelList/images/steer/resume/nativeInteraction可见性。
- NativeInteraction：来源、interaction/chat/turn ID、问题/选项或approval scope、deadline；答复回原生runtime，不成为产品永久授权。
- 产品Definition复用P1；本机文件用明确共享资源/下载目标意图，远端路径绑定Session和文件后端。
- SkillContext：显式选中身份/版本/文本，不产生授权。

## T1：一个后端生命周期

**Consumes:** P6 Driver/Event；**Produces:** 一个实测外部Driver。
- [ ] 测 differentRuntimeCannotResume、missingBinaryReportsUnavailable、processCrashEndsTurnOnce、cancelReclaimsPendingStreams。
- [ ] 开发前按已安装环境选定首个后端，冻结协议版本/脱敏fixture和认证模式，不占位宣称全部支持。
- [ ] QProcess stdio仅作外部runtime通信，不替换产品MCP；frame/读缓冲/输出/超时有界。
- [ ] 文本/工具/文件/计划/用量/warning映射事件，原生终态保真；resume不兼容明确新线程。
- [ ] 需要sidecar时先完成依赖/更新/打包/凭据/退出方案，再加入真实消费者。

## T2：原生提问、批准与steer

**Consumes:** NativeInteraction/能力声明；**Produces:** UI往返与取消收口。
- [ ] 测 nativeSessionApprovalNotPersistedAsProductGrant、unsupportedSteerDoesNotDropDraft、questionCancelledOnExit、retryableWarningDoesNotFinishTurn。
- [ ] 产品工具批准走P1，原生shell/文件批准由外部边界负责；不能混用ID、关掉必要确认或产生永久产品grant。
- [ ] 明确重复批准避免规则；steer仅真实支持后端显示，busy/review/compact/inactive给原因，失败保留草稿。
- [ ] 等待随turn/进程退出取消，迟到答复不影响新turn；升级runtime重跑fixture。

## T3：按真实服务扩产品工具

**Consumes:** SFTP/Profile/片段服务；**Produces:** 逐族capability和权限证据。
- [ ] 测 remoteDownloadUsesLocalDestination、serialHasNoImplicitSftp、profileListNeverReturnsSecrets、writeRequiresExplicitScope。
- [ ] SFTP先list/read，再单独授权写/传输/目录/删除/chmod；SSH账号权限不等于Agent权限。
- [ ] Profile元数据与凭据隔离；创建/导入先预览再写，格式失败不能擅自改存笔记。
- [ ] Snippet与NovaTerm脚本发布分别执行，不把Netcatty JavaScript nct.*脚本库当既有能力。
- [ ] 自动开/关主机、全局Agent需单独产品规格，不在sidebar隐式扩大scope。
- [ ] 每能力族验证本机/MCP一致授权，未实现不注册，回归九工具。

## T4：Skills、快捷消息和可选CLI

**Consumes:** P6输入器/P1目录；**Produces:** 显式指导上下文。
- [ ] 测 selectedSkillDoesNotGrantTools、skillVersionPreservedInHistory、slashStopUsesUnifiedCancel。
- [ ] Skills/快捷消息与系统/用户输入分层，保留版本选择；/stop、/compact调用Runtime，不从文本直接执行shell。
- [ ] CLI如确有需要另冻结身份/scope/输出/取消/discovery，不把Netcatty内部CLI当公共API。
- [ ] md/json/txt导出和分支/撤销复用消息模型，不导出秘密、不暗示回滚远端。

~~~bash
cmake --build build --config Debug --target novaterm_external_agent_tests
ctest --test-dir build -C Debug -R novaterm_external_agent_tests --output-on-failure
ctest --test-dir build -C Debug -L mcp --output-on-failure
~~~
默认fake进程/脱敏协议，不自动登录或消耗模型额度。退出按后端×功能×平台报告，模型目录存在不等于运行验收通过。

## 低耦合、高内聚验收

- 每个外部后端内聚其进程/协议/原生交互，产品工具适配内聚各自资源语义；两者通过公共Driver/能力契约协作，不相互include实现。
- 后端不可取得整个AgentManager/Service；产品工具不取得外部SDK原生会话来绕过P1。Skills仅构造上下文，不持权限和执行状态。
- [ ] 接入第二个fake后端时只新增后端模块/组合根注册/fixture，已有聊天、授权、产品工具无需增加后端条件分支。
- [ ] SFTP/Profile等适配测试仅链接对应窄端口和契约，不要求启动外部Agent；第三方SDK头只留在该后端适配实现中。

## 补充实施约束：外部后端的数据发送

外部Driver接收[P6模型发送策略](P6_Agent_Runtime_and_UI.md#补充契约模型发送与派生内容授权)过滤后的上下文，不能把外部进程当成绕过接收方授权的中转。backend/bin/runtime/authMode之外，resume还绑定接收方和上下文来源授权版本；后端不支持清除旧原生上下文时，撤权/接收方变更后禁用resume并创建干净会话。不能承诺删除外部已收到的内容。

- [ ] T1/T2增加revokedSourceDisablesOpaqueResume、externalRecipientChangeRequiresNewIntent；fake进程核对启动参数、上下文及发送次数。
- [ ] 无法约束外部后端接收方/原生上下文保留行为时明确unavailable，不宣称与内置Driver具备相同数据隔离保证。
