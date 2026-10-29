# MCP统一接入与已连接会话（工作区增量）

## 用户确认范围

取消AI客户端管理，所有AI共用一个令牌和一套授权；会话读取、完整输入、提权选择及清空默认全开；删除保存目标表及其四个MCP工具，仅暴露已连接会话。本机手动保存/打开SSH与串口连接保持。此前表格前两列布局调整被整表删除替代。

新存储为version=2单access记录。旧多客户端令牌不继续认证，不合并旧授权；显式启用创建统一令牌。普通状态读取不访问系统凭据库。令牌重置保留同一统一主体的终端选择，但使旧身份绑定及旧令牌失效；停用撤销运行期授权。文件与历史授权仍独立，所有使用统一令牌的AI共享。

内部主体ID/身份代际、请求重试、连接绑定、文件发布和人工接管校验保留；不是额外一层用户管理。历史和文件窗口不显示客户端选择。新就绪连接默认四项权限，但同一连接的人工取消不会被普通刷新覆盖。

## 分轮证据

- 初始统一接入RED：AccessStore 2pass/2fail（旧v1令牌仍接受、启用不自动建接入）；UI 3pass/2fail（仍有客户端选择器、无自动接入）；历史UI 2pass/1fail（仍有选择器）。对应build/shared-access-{store,ui,history}-red.txt。
- 保存目标退役RED：6pass/4fail，四个保存目标工具仍被识别；build/shared-access-retirement-red.txt。
- 第一次整批编译发现ResourceComposition仍调用旧clients接口，修为统一主体和持久token epoch读取；不降编译警告。
- 下一次指定脚本源码编译通过，主程序链接因用户重新打开开发版而LNK1104；用户已再次退出，继续构建。
- 单接入存储初轮20pass/1fail，失败为旧两客户端循环改成同一身份后在第一次循环重授导致第二次撤销断言失败；改为先检查两共享视角均撤销、只重授一次，再检查共享恢复与再次撤销。未改变产品撤销行为。
- 历史UI初轮14/14通过（3.108秒），含文件选择期间重置/替换身份/停用拒绝。仅最初无选择器断言有观察到RED，不称后增场景均已跑过RED。

所有build日志和测试输出仅本机临时证据，不纳入提交。未连接设备、未提交/推送，版本仍0.2.102。

## 收口验证

- 默认权限/离线边界真实RED 2pass/3fail；修复后统一接入、默认权限、旧工具拒绝与离线拒绝专项10/10（2.893秒）。
- 凭据操作重入真实RED 2pass/3fail；增加保护及停用竞争回归后完整接入存储28/28（1.939秒）。对应build/shared-access-{defaults,reentry}-red.txt及store-green2.txt、ui-green2.txt。
- 指定脚本Release默认构建通过（build/shared-access-final-build.log）；MCP/录制相关CTest27/27（15.95秒，单独排除完整MCP业务以使用QTest文件输出），没有排除Windows串口文件发送目标。
- 会话相关CTest首轮1/2：Session通过（20.79秒），面板失败。面板完整QTest 6pass/1fail暴露HEAD已存在的interactiveShellKind未保存；最小一行补存，原断言保留，完整面板CTest复验1/1（0.28秒）。不是本次重构引入的字段丢失。
- 完整MCP业务初轮144pass/5fail（89.141秒），唯一场景为五种连接的新绑定默认授权改变。夹具明确撤销新连接后再操作旧行，保留权限/修订号/访问版本均不变的断言；最终完整149/149（87.724秒）。日志build/shared-access-full-mcp{,-final}.txt。
- 官方MCP SDK2025与raw HTTP2026验证22工具Schema、8个已退役工具拒绝和保留工作流通过（build/shared-access-interop.log）。测试依赖仅安装build/mcp-test-deps；首次pip以GBK读取中文注释失败，设置PYTHONUTF8后成功，没有修改系统包。
- 确定性扫描已执行（build/shared-access-lint.log）；独立只读行为审查及末轮增量复核未见高可信新问题。没有机械格式化。
- 离屏截图发现会话名称列仍过窄，最后仅将名称列改为填充剩余宽度、当前操作列按内容布局。上述完整业务成绩属于布局微调前，最终布局构建/专项与截图另记，不冒充重新运行全部业务。
- 最终列宽微调后指定脚本构建通过（build/shared-access-layout-build.log），接入/默认权限/离线拒绝/两代退役拒绝/布局专项11/11（3.211秒，build/shared-access-layout-final.txt），离屏深浅主题截图确认会话名与状态不再挤在一起（build/shared-access-preview-final-local-input-{dark,light}.png）。

未连接设备、未运行无关全套/Core/ConPTY、其他平台或消毒器；未打包、提交、推送。旧凭据库条目不做破坏性清理，但旧v1配置不认证；历史录制与已保存的本机SSH/UART配置未删除。

## 0.2.103 审查修复与提交收口

用户随后要求修复新审查确认的两项P2，完成后创建Git提交并推送Gitee；前文“未提交/推送”为原实施批次状态，不作为本轮限制。

1. 默认授权初始化不再推进共享主体的全局访问版本。当前连接的行修订和绑定仍更新，并发出对应会话的撤权事件；重连旧请求仍由绑定及代际检查拒绝，显式授权变更、令牌重置和全局停用仍保持原全局失效。另一会话新建/重连不再使原任务、活动等待及保留续扫失效。
2. 提权凭据存储方式改为未知/持久/仅本次运行三态。未确认后端时界面明确显示“尚未确认”，状态查询不访问凭据库；实际同步/worker操作完成后显示真实后端，加载已有持久引用时可从非秘密元数据恢复持久状态。

新增两代协议×任务/活动等待/续扫×新建/重连12行回归，并保留显式撤权拒绝。初始RED为2pass/12fail，其中两行受多会话夹具按排序取首项影响；改单独定位原Session ID+epoch。单独重跑task-reconnect复现TASK_NOT_AVAILABLE，原始与时序输出分别为build/review-fixes-session-red{,-timing}.txt，不将所有初轮失败都写成目标缺陷。存储三态新增回归RED为2pass/5fail（build/review-fixes-persistence-red.txt）。

按提交约定根版本0.2.102→0.2.103，修复后同版本验证如下：

| 批次 | 结果与本机日志 |
| --- | --- |
| 指定脚本Release默认构建 | 成功，build/review-fixes-green-build.log |
| 跨会话及实际未知存储弹窗专项 | 15/15，5.023秒，build/review-fixes-session-green.txt |
| 完整接入存储 | 33/33，2.422秒，build/review-fixes-persistence-green.txt |
| 提交前相关MCP/录制CTest | 27/27，18.32秒，build/novaterm-02103-related-ctest.log；仅将完整MCP业务移到下一独立QTest批次，未排除Windows串口文件发送目标 |
| 提交前完整MCP业务 | 162/162，95.612秒，build/novaterm-02103-full-mcp.txt |
| 提交前Session/SessionPanel CTest | 2/2，23.31秒，build/novaterm-02103-session-ctest.log |
| 官方SDK2025/raw HTTP2026 | 22工具Schema与8退役工具拒绝、保留工作流通过，build/novaterm-02103-interop.log |
| 独立只读修复复核 | 版本失效边界、绑定/撤权保持、三态传播及无凭据状态访问未见高可信新问题 |

上述分别保留各自批次，不拼作一次全套。此次纳入完整统一接入主题及两项修复，未连接设备、未跑无关全套/Core/ConPTY、未测其他平台或消毒器、未重新打包。产物、测试依赖、日志和本机构建脚本不纳入提交。用户已明确授权本地提交并正常推送Gitee，不改写旧轮次的“未提交”状态。
