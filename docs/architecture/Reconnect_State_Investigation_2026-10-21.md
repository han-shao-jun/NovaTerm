# 同一标签重连与串口换绑状态排查（2026-10-21）

## SSH 断线后重连乱码

`TerminalSession::beginReconnect()` 原先只更新连接世代、命令运行期和输入泵，
并未重置共享 `TerminalCore` 内的 VT 解析器。上一连接若留下 DEC 线框字符集、
未完成 OSC/CSI 或半截 UTF-8，新连接的普通文本仍会被旧解析状态解释。
这解释了关闭标签新建会话正常、保留标签重连异常的区别；不说明最初断线的原因。

修复前，Windows Release 的 `reconnectResetsTerminalParser` 五个数据行中：
线框字符集、半截 OSC、半截 CSI、半截 UTF-8 四行失败，分别出现字母变线框、
吞掉输出、丢首字符与替换字符。备用屏行另补模式状态断言。

修复在 Parser 的有界命令队列加入连接重置命令，沿用字节屏障：已接受的旧字节
先完成解析，随后重建 libvterm 实例，新连接字节在重置后解析。保留滚动历史、
尺寸、默认颜色、LF 兼容配置和宿主回调；清空当前屏幕、解析缓存和终端模式。
同步更新备用屏、鼠标及光标状态，不向远端发送重置指令。命令拒绝时不启动新连接。
覆盖手动/自动重连、Failed 后 start 及关闭后复用标签的入口。
串口自动重试在重置后重新显示完整等待提示；连续两次重试的回归用例修复前
第二次只显示 `.`，修复后通过。

## 串口标签编辑后传输窗口失效

Session 换绑会销毁旧 `SerialFileTransferController`，旧传输窗口保留但不再绑定
有效控制器。右键菜单构建时 `fileTransferActive()` 可先缓存新控制器；
`showFileTransfer()` 原先比较这份缓存，错误地认为旧窗口也已绑定新控制器。
窗口的开始按钮因旧控制器析构而禁用，无法恢复。

修复直接比较传输窗口实际观察的 `controller()`，发现不同就销毁旧窗口并为
新控制器创建窗口。窗口销毁与控制器观察继续使用 QPointer/Qt 生命周期管理。

## 验证边界

回归涉及 `novaterm_session_tests`、`novaterm_core_tests` 与
`novaterm_serial_file_transfer_ui_tests`，跨三处模块另执行本机全套回归。
最终运行结果以本次测试日志为准，不从历史通过记录推断。
自动化复现使用内存 Transport，不连接用户 SSH 服务器，也不自动占用串口。
真实 SSH 意外断线及修改 COM 号后的文件收发仍需实机验证。

Windows Release 全量重建成功；本机 CTest 注册 39 项，本轮全套 38/39，
耗时 177.13 秒。唯一失败目标为 `novaterm_conpty_tests`。单独取文件日志复核
`injectedStartupStagesRollBack` 与 `repeatedLifecycleReturnsResourcesToBaseline`，
失败均为项目已记录的句柄计数断言：分别 229→266、288→485；后者线程
7→6、子进程 0→0 检查通过。没有修改 ConPTY 生产代码或放宽断言。
全套之后完善等待提示并重新构建应用、核心/MCP/终端联通目标。最终相关四项
`novaterm_core_tests`、`novaterm_session_tests`、
`novaterm_terminal_session_tests`、`novaterm_serial_file_transfer_ui_tests`
复验 4/4 通过，耗时 45.13 秒。SSH 五行专项 5/5 通过，等待提示专项通过。
本轮日志在本地 `build/reconnect-{red,green}.txt`、
`build/reconnect-prompt-{red,green}.txt`、`build/reconnect-conpty.txt` 及
`build/reconnect-final-ctest.log`；build 产物不纳入版本控制。

无设备的串口 UI 用例仅覆盖窗口观察的控制器销毁、QPointer 归零及新控制器身份
判定，不宣称端口换绑全链路已验收。实机步骤应包含：先打开旧传输窗口，编辑
标签切换串口并连接，再先展开右键菜单（触发活动状态查询），最后打开传输窗口
验证可启动并完成收发。SSH 应在保留同一标签时触发断线后重连，检查中文、
普通字母及全屏程序退出后的终端显示。
