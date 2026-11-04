# Codex 确认信息必填化（2026-11-04，工作区增量）

## 用户选择与问题

用户再次反馈 Codex 确认框仍显示 actions/epoch/inputToken，非 MCP 开发者无法直观看懂。本次只读核对截图 epoch 对应的调用记录，发现这些请求没有携带旧的可选 `_action/_detail/_target`，仍采用旧参数布局；不能仅凭截图断定是缓存，或认为可选提示一定会被模型填写。

用户明确选择“只改 NovaTerm，稳定显示操作/目标/内容”。因此保持 Codex 客户端不变，把 NovaTerm 全部20工具的显示字段改为必填：

```text
1_操作: 操作目标终端
2_目标: ssh · <实际IP>
3_内容: 安装软件：apk add nikki luci-app-nikki …
```

数字前缀利用已核对的 Codex CLI 0.166.0 参数名排序规则，让这三项排在业务参数之前。`1_操作` 由各工具 schema 的 const 固定为工具 title；`2_目标` 使用实际 targetLabel，`3_内容` 说明实际命令、文件名/方向、查询关键词或读取范围。三项均为非空单行、最多60 Unicode码点，禁止控制/格式字符和不完整代理对。长内容由调用方仅缩短显示摘要，执行参数保持完整。

旧的三个可选英文字段不再接受。两个 codec 都检查新必填字段、格式及固定操作名；缺字段返回明确的中文说明，要求重新连接并刷新工具目录，且不会进入业务后端。字段仍不进入执行 DTO、授权或 operationId 指纹。直接输入省略 inputToken 的行为、显式旧令牌严格校验和业务安全规则不变。

## 展示与信任边界

- 这是输入契约变更，旧脚本/客户端必须更新。服务端不会为了兼容旧布局静默补字段或执行缺字段调用。
- 保证的是**服务端接受的请求包含三项信息**。Codex 在调用服务端之前展示确认框，旧/错误请求仍可能先出现一次旧框，再收到拒绝；服务器不能倒过来修改已经显示的框。
- 英文外框、Allow选项和客户端截断排版没有修改。重启新构建的NovaTerm后，还需重新连接/重启Codex以刷新工具目录及instructions。
- 固定操作名称由服务端核验，目标和内容仍由调用方填写，没有核验它们与全部真实参数一致，不能称为服务端可信授权摘要。终端实际执行仍由原业务参数和权限控制。

## 测试迁移与结果

旧业务测试的调用方工厂现在显式填写必填显示字段；辅助函数只补缺失字段，不覆盖显式错误值、不修正非对象参数。业务负例因此仍校验原来的未知字段/类型/预算规则。专门的原始请求测试绕过辅助函数，验证缺字段确实拒绝，而不是靠测试辅助函数掩盖问题。历史 fixture 文件保持原样，由已有比较投影明确排除单独测试的展示封装。

| 轮次 | 结果 | 说明 |
|---|---|---|
| 新原始契约 RED | QTest 2 pass / 2 fail，5ms | 旧字段排序不符，旧调用允许省略显示字段 |
| 新原始契约 GREEN | QTest 4/4，5ms | 全20工具必填、前三项排序、const及缺字段拒绝 |
| 本地协议初轮 | CTest 2/3，2.42s | wire中的旧动作校验夹具直接调用codec时漏显示字段；修正测试输入 |
| 完整相关初轮 | CTest 5/6，40.88s | 唯一MCP失败为直接构造HTTP请求的旧夹具漏字段；产品正确拒绝 |
| MCP复测 | CTest 1/1，33.19s | 修正直接HTTP夹具；其余五目标沿首轮通过记录，不冒称同轮6/6 |
| 官方SDK互操作首轮 | 未通过 | 独立客户端的直接 read_context 调用漏显示字段，被服务端拒绝 |
| SDK互操作复测 | 通过 | 官方SDK2025 + 原始2026 HTTP；20工具schema、中文字段/排序/const、原始缺字段拒绝及既有业务互操作通过 |

新wire用例在2025/2026两代分别逐项移除显示字段，断言INVALID_ARGUMENT、无pending调用、backend事件为零。长命令回归使用超过1000字符的执行参数配短摘要，确认解码后的完整动作原文未改变。原有中文/非BMP长度、换行/控制字符拒绝及幂等相关回归继续保留。Python脚本语法检查通过。

主程序及6个相关目标构建成功。只读代理复核生产校验与测试边界，未发现新增高可信问题。代理没有修改文件或重复执行测试。

## 本机证据与范围

临时日志：

- `/tmp/novaterm-required-preview-red.log`、`/tmp/novaterm-required-preview-green.log`
- `/tmp/novaterm-required-preview-local-tests.log`
- `/tmp/novaterm-required-preview-final-build.log`、`/tmp/novaterm-required-preview-final-tests.log`
- `/tmp/novaterm-required-preview-mcp-retest.log`
- `/tmp/novaterm-required-preview-interop.log`、`/tmp/novaterm-required-preview-interop-retest.log`

仅对本机软件夹具与回环HTTP进行验证，没有执行截图中的apk命令，没有连接/修改真实设备，也没有重启用户实例或修改Codex配置。原生Codex确认框的新版本实机截图尚未验收。未提交/推送，原有未提交改动保留；业务测试未写入B4工具台账。
