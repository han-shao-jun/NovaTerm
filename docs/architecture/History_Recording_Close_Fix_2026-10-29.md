# 历史录制设置窗口关闭修复（2026-10-29）

基于0.2.103实施；下列实施成绩保留原轮次。用户随后于2026-10-30要求创建提交并推送，提交收口见末节。

## 原因与修复

`HistoryRecordingPanel`设置了`setIsDefaultClosed(false)`，Ela标题栏和原生关闭
请求因此转成`closeButtonClicked`信号；窗口却没有连接关闭处理，点击关闭后仍可见。
既有`.close()`或`.hide()`验证没有覆盖真实标题栏按钮。

沿用`SerialFileTransferDialog`已有模式，将该信号连接到`QDialog::reject()`。
关闭只隐藏可复用的窗口，既不销毁其模型/未应用设置，也不停止或删除录制。
`TerminalPage::showHistoryRecording()`仍通过已有子对象重新显示同一窗口。
不修改第三方库、默认授权或录制生命周期。

## 验证

- 新增`HistoryRecordingUiTests::titleBarCloseHidesReusableWindowAndKeepsRecording`，
  实际鼠标点击唯一可见的标题栏图标按钮。修复前2pass/1fail，失败点是窗口没有隐藏；
  修复后专项3/3（0.325秒）。验证关闭/再次显示/再次关闭、保留窗口和界面状态，
  以及关闭后录制仍活动且可接受并提交新输出。
- Release `all`目标构建成功。
- 录制recorder/store/service/recording_ui相关CTest4/4（7.29秒）；
  其中完整录制UI文件15/15（3.319秒）。
- 临时隔离配置程序复用实际主窗口构建对象：菜单打开→真实标题栏按钮关闭→
  菜单再次打开同一面板通过，空连接警告0次；offscreen不支持raise的两条提示保留。
- `git diff --check`通过；未跑无关全套、其他平台或设备测试，不连接服务器。

本机证据：`/tmp/novaterm-history-close-{red-build,build}.log`、
`/tmp/novaterm-history-close-{red,green}.txt`、
`/tmp/novaterm-history-close-related-tests.log`、
`/tmp/novaterm-history-close-menu-check.log`；临时菜单验证程序及脚本为
`/tmp/novaterm-history-close-smoke.{cpp,py}`，均不纳入提交。

## 0.2.104提交收口（2026-10-30）

按用户要求单独提交历史录制关闭修复，版本0.2.103→0.2.104。版本递增后Release all成功，录制相关CTest4/4（5.11秒）；日志为`/tmp/novaterm-02104-build.log`与`/tmp/novaterm-02104-recording-tests.log`。构建发生于同时保留后续目录浏览/文件源修复的工作区，不声称该提交在独立检出中重建；关闭行为的原始RED/GREEN如上。不纳入构建产物/本机脚本，不扩展到真实设备或其他平台测试。
