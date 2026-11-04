# 新输入令牌在提交前过期：定位与验证（2026-11-04）

## 结论及边界

已定位并实机复现本轮报错的主要延迟位置：**Codex 创建交互调用之后、进入获准 MCP 调用之前**。新 read_context 在数毫秒内返回，随即发起 interact，但审批/客户端队列阶段持续数分钟；获准后的 MCP 调用仅约 4～5ms，服务端正确拒绝已经过期的令牌。不能单凭该计时再把审批 UI、自动审批、客户端队列分别归因，也不外推所有潜在故障。

对照 Codex 0.166.0 官方 [mcp_tool_call.rs](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/core/src/mcp_tool_call.rs)：工具 item started 先于 maybe_request_mcp_tool_approval；result 的 duration 从 handle_approved_mcp_tool_call 内 Instant::now 起算，包含后续调用和响应处理。`item_completed.started_at_ms/completed_at_ms` 与 `item.duration` 是不同边界。这里的差值是获准调用前的时间，**不是服务端调度耗时**。

用户已提供的 120000ms 候选有效期保持，与单次等待 30000ms 分离；没有再次延长或取消过期检查。该补丁只能缓解有限排队，不可能保证任意长的审批等待后仍可执行。服务端不能未经重新观察、核对和必要审批，自动把旧令牌替换成新令牌。

## 可追溯时间证据

[脱敏时间数据](Input_Token_Timing_2026-11-04.json) 保留每次 read 开始/返回、interact/start_task 开始/返回、获准调用时长、令牌内 expires（服务端单调时钟）、operationId。没有保存完整令牌或凭据。

历史 8 次：创建调用到结束 70.773～338.199s，获准调用实际 1.282～5.994ms；全部对应刚完成的 read_context，签发旧格式中没有 issued 时间。它们证实此前同类报错也主要发生在客户端调用获准前。

本轮在线实例 `c2c63077-dcf9-4a8c-b2c9-cea9f811f57f`，ttyUSB0 会话 epoch `551a6411-c640-47ce-ae54-a7b9e8ca45a6`：

| 操作 | 调用总耗时 | 获准 MCP 调用 | 获准前时间 | 结果 |
|---|---:|---:|---:|---|
| 初次只读设备查询 | 1.379s | 5.576ms | 1.373s | submitted |
| HDMI 节点/格式查询 | 152.484s | 4.318ms | 152.480s | INPUT_TOKEN_EXPIRED，零新增输入 |
| 原查询同 ID，重新观察后续调 | 1.480s | 5.319ms | 1.475s | submitted |
| 一次 start_task 采集 | 54.106s | 6.050ms | 54.100s | accepted，随后 running |
| 采集后新令牌立即发起无害输入 | 324.912s | 4.582ms | 324.907s | INPUT_TOKEN_EXPIRED，零新增输入 |
| 程序先等待40s，再发起无害输入 | 305.193s（不含主动等待） | 4.859ms | 305.188s | INPUT_TOKEN_EXPIRED，零新增输入 |

最后一行使用 setTimeout(40000)，记录的墙钟等待约39.24s；墙钟不等同单调时钟，未将其硬写成精确40.000s。该对照因额外305s审批/排队等待失败，**不能声称“等待40秒后提交通过”**，也没有改写用户此前被中断的40秒测试。

旧在线进程没有 issued/准入详细诊断，不能精确恢复其服务端签发和准入单调时刻。已有 expires 不与 Unix 时间直接相减；获准调用区间只能限定服务端处理的墙钟范围，不能冒充精确服务端准入时间。本轮新增诊断构建后尚未重启在线实例，实际验证仍属旧诊断格式。

## 实际串口流程

1. 先列会话并观察 root@ema:/# 提示符；确认 ttyUSB0 为 RK3588。只读列设备产生了其他未接传感器的驱动噪声，保留原输出，没有加载模块、修改驱动/串口配置。
2. 观察期间出现人工输入导致 preempted，当时停止后续输入。之后两次观察提示符稳定、无新的活动命令，才继续原已授权的只读查询。首次查询明确过期后只以原 operationId 和重新取得的令牌续调，没有重发结果未知的操作。
3. HDMI RX 确认为 `/dev/video80` / `stream_hdmirx`，3840×2160 NV12；驱动输入时序为296692000Hz / 29.97帧每秒；进程查询未显示其他 v4l2-ctl。
4. 只调用一次 start_task，operationId `2ab89984-0e34-4852-a02a-581642953003`，taskId `e4fdcc39-704d-4974-9886-c367fbb02893`。远端命令：

```sh
printf '\nNT_EXPIRY_CAPTURE_BEGIN_1104\n'
timeout -s TERM -k 3 45 v4l2-ctl -d /dev/video80 --stream-mmap=4 --stream-count=1800 --stream-to=/dev/null
nt_rc=$?
printf '\nNT_EXPIRY_CAPTURE_END_1104 rc=%s\n' "$nt_rc"
```

服务端任务期限55000ms，远端45s期限/3s结束宽限。之后只 query_task；状态由running到unknown/reason=deadline，terminationConfirmed=false，未请求stop或重复启动。观察到采集进度约59.97fps、结束标记rc=0和shell提示符；这些是终端文本证据，不改变UART任务的unknown边界，也不证明原生4K60独立画面、完整帧序号或精确采集墙钟时长。

5. 采集后和延迟对照均得到明确过期拒绝，未把无害命令的预期标记当成已执行。最终屏幕仍为采集结束标记和提示符，控制已expired；无活动输入控制或本轮未结束采集。

## 实现与回归

保留所有原有未提交提示信息及候选120s补丁，不修改Codex安装、审批策略或设备配置。新增：

- 新 token 增加签名的 issued 单调时间（字符串），expires与issued同一次取时；旧token缺issued仍沿原校验接受，不从旧expiry反推签发时间。
- INPUT_TOKEN_EXPIRED 说明 issuedMs/expiresMs/admissionMs，明确本次没有新增输入。仍不可自动重试；鉴权、cache、人工/控制/提示符/执行代际、输入序号及operationId判重顺序保持。
- TerminalInteraction 默认仍用原QElapsedTimer；可注入状态时钟用于确定性时间测试，不用真实睡眠两分钟。
- RequestScheduler→ObservationState组合覆盖排队45s受理、恰好120s过期、人工接管、控制改变、权限撤销、会话换代、缓存失效；精确重试过期后只返回原事实，改动作拒绝。
- 长任务期限unknown后query不续输入令牌；新观察后可接受一条新输入；再次精确重试不增加字节，冲突动作拒绝。签名新token的issued编码/解码在该集成中校验；旧issued缺省由状态测试和解析分支静态检查覆盖，不冒称旧签名样本实机回放。

新增诊断回归真实RED：9 passed / 1 failed（缺签发/准入错误说明）；候选120s已有RED证据沿用户记录保留，本轮没有重新运行旧30s产品版本或覆盖其他修改。

首轮相关CTest2/3，terminal_tasks在AccessStore初始化处abort。沙箱不允许ptrace；获准在沙箱外运行同一新定向测试3/3（93ms）。沙箱外strace可见QSaveFile通过O_TMPFILE及`linkat(/proc/self/fd/...)`发布access.json；沙箱内同类最小文件发布探针返回ENOENT，沙箱外5目标整体通过。这支持当前测试失败与环境中的临时文件发布有关，但没有在沙箱内直接捕获完整AccessStore失败调用，不外推用户此前隔离测试的每一次失败原因。没有为通过测试放宽安全写入或授权。

最终主程序及MCP/任务/状态/契约/wire目标构建成功，沙箱外同轮CTest **5/5，34.43s**（MCP29.60s、任务2.45s），没有运行无关全套。单代理只读复核未见新增高可信问题。后续仅补旧issued缺省与签名新token时间断言，两个测试目标重新构建；复验结果见本节续记。

日志：`/tmp/novaterm-input-timing-{red,tests}.log`、`/tmp/novaterm-task-fixture-{test,strace}.log`、`/tmp/novaterm-input-token-final-{build,tests}.log`。原始工作区差异快照 `/tmp/novaterm-before-input-token-diagnosis.patch`。未提交/推送，未重启用户实例，未跑其他平台/消毒器。软件验证与上述真实工具调用分列，未把本轮单测写入B4工具台账或宣称B4通过。

补充兼容断言复验：`novaterm_mcp_observation_state_tests` 与 `novaterm_mcp_terminal_tasks_tests` **2/2，2.30s**，日志 `/tmp/novaterm-input-token-compat-{build,tests}.log`。此轮只改测试，不冒称完整5目标重新运行。最终 `git diff --check` 通过。

## 后续：用户允许本次会话授权后的复验

用户明确选择“允许本次会话授权并复验”。本轮没有新增产品代码、没有修改永久Codex配置、没有重启NovaTerm，仍是同一在线实例/ttyUSB0/epoch。上一轮新诊断代码未进入该进程，因此此处不是新诊断构建的实机验收。

首次以不发送设备字节的release触发授权，等待256.791s后仍过期，保留失败；重新观察并以原operationId续调成功。实际MCP调用仍在毫秒级，客户端前置等待从约1～32s不等；仅凭本轮结果不能保证会话授权选项已被所有客户端层永久/完整记住，也不能断言逐条弹窗全部消失。用户授权仅限本次会话，没有替用户开启永久允许。

重新核对RK3588、video80、3840×2160 NV12、29.97Hz驱动时序、timeout可用且无其他采集进程后，只启动一次1800帧采集：operationId `56ef8d44-63de-4d5e-9327-f46c4a113003`，taskId `4a7bfdb7-a188-4b14-a3e6-6ed0edc47f6b`。远端仍45s超时/3s宽限、服务端55s期限。每10s查询原taskId，running→unknown/deadline、terminationConfirmed=false；屏幕出现 `NT_SESSION_CAPTURE_END_1104 rc=0` 和提示符。不将59.97fps进度文本升级为原生4K60独立画面证明。

- 采集后重新read_context并立即发起无害输入：成功submitted，Codex调用总耗时6.523s、获准调用6.791ms，观察到 `NT_SESSION_POST_CAPTURE_OK_1104`。
- 为避免租约到期改变控制代际混入TTL对照，先明确release（不写设备），再read_context取得新令牌；程序等待40000ms，墙钟记录40002ms后提交。成功queued→submitted，Codex调用总耗时162ms、获准调用6.108ms；观察到 `NT_SESSION_DELAY40_OK_1104`。
- 完全相同operationId/令牌/动作精确重试成功返回原submitted事实，调用总耗时32.515s；重试后及延后独立观察，完整匹配输出标记仍仅一次。没有换ID重复输入。
- 最后release成功，独立读取确认control=released、shell提示符在屏幕末尾。未触碰新出现的ttyUSB1。

上述是**本轮新的40秒实机成功记录**，不替代前轮额外305s客户端等待导致失败或更早用户中断的结果。脱敏逐调用时间追加在 `Input_Token_Timing_2026-11-04.json` 的 `session_authorization_retest`，没有覆盖历史记录。这证明现有实例在审批等待落入有效期时可以完成目标流程；若再次出现超过120s的客户端等待，过期拒绝仍按设计生效。本轮只更新文档/证据，未重新构建或跑软件测试；git diff --check通过，未提交/推送。
