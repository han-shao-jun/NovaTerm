# MCP 架构评审三项修复

基线：0eec9e9 + 2026-10-23 五项修复的工作区。保留已有改动，不自动提交、推送或重新合并。

## 设计与边界

- HistoryReadService 接收取消状态并在提交分页状态前复核；取消、断连、deadline 均由同一领域操作标记传递，实际在途 I/O 额度直到完成才释放。
- 新 `src/resources/ResourceSharingService` 是显式资源共享应用用例：持有历史授权/查询及跨 History/Evidence 的证据发布流程；仅依赖 Qt Core、领域库与最小身份/授权文件端口，不依赖 MCP/HTTP/Session/Widgets。独立文件导入和证据不随 History 来源替换清除。
- `src/resources/HistoryCapabilities` 提供 list/read/search 三个类型化端口，共享现有 Dispatcher，准入不能由 MCP 调用者跳过。
- HistoryCapabilityAdapter 只做 wire cursor、请求/结果转换，注入共享应用服务。默认本地私有文件适配仍复用现有安全文件边界；应用服务本身可用纯值假端口独立测试。
- Application 显式持有资源应用实例，UI 共享意图调用该应用实例；MCP Service 接收/暴露同一实例供组合根装配，不以 MCP enabled 控制应用资源生命周期。
- 旧 ToolRequest 冻结九工具；History wire 请求移入 protocol 专用头。WireSession/BackendEvents 在协议层分别路由 terminal/history，终端应用与 LegacyTerminalMapping 不再包含 History 请求。

## 执行

1. 新增读取取消的 RED 回归（取消前准入零 I/O、迟到完成无游标、超过 64 次取消后其他 Principal 仍可读取）；修复并 GREEN。
2. 新增应用边界/来源切换回归；抽资源应用服务与最小持久化/身份端口，迁移已有授权/导入/证据逻辑，不复制第二套状态。
3. 接入领域能力端口、MCP codec/BackendHistoryCall，旧九工具 fixture 不变；历史 fixture 与 HTTP 回归验证十二工具兼容。
4. 验证独立应用目标无需 MCP/HTTP/Widgets，两个可信入口可复用同一资源实例；来源移除不撤销独立导入资源。
5. 构建、相关回归和必要跨模块全套（此次新增构建边界/跨模块重构）；已知 ConPTY 断言不放宽。独立复核后同步架构与验收记录。

未实施 P4/P5/P6 产品功能、真实设备、持久证据或跨客户端共享；I-001 停机延迟仍为待测风险，不把它当已确认缺陷或本轮性能验收。

## 执行记录

三项主要修复完成。取消回归先 RED 后 GREEN；无录制来源导入先 RED；新增纯应用服务最终 8/8，包含双入口、70 次入口关闭、来源移除、持久授权 epoch 和 generation=3。独立资源复核发现搜索重放/证据后半段准入缺口，新增两条 RED 回归后修复；协议复核未见新的明确阻断项。

Release 默认目标构建成功；DLL 依赖实核 Qt Core/Test。全量首次 47/49 后修复搜索校验优先级回归；相关 22 目标先 21/22（Wire 新夹具缺元数据），修正夹具后只重跑 Wire 1/1，最终分段覆盖 22 目标均通过。ConPTY 完整文件复跑仍只有既有两条句柄断言失败。完整证据与未验收范围见 P3_Architecture_Fixes_2026-10-24.md，不混记不同轮次。
