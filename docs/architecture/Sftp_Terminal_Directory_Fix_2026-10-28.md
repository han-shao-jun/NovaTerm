# SFTP切换到终端当前目录修复（2026-10-28，0.2.98）

用户点击“将当前SFTP切换到当前终端目录”后，探测命令已执行，但面板报告无法读取终端目录。

## 原因与修复

- Core原来只交付每个解析批次的最后一个标题。目录探针使用OSC 2返回路径，Bash随后通过OSC 0更新提示符标题，两者同批时探针响应被覆盖。现在按顺序交付各个完整标题，沿用原有批量GUI投递。
- libvterm会分片回调标题。原适配器把每片当作完整标题，可能提前提交截断路径。现在按initial/final累积UTF-8字节，仅在BEL/ST结束后发布；标题最多64 KiB，超限整条丢弃，不发布截断路径。
- 处理探针响应时，如果Core已有新标题，就不再回写探针前的旧标题。
- 不改变SFTP连接、授权、传输队列或MCP工具。

## 分轮验证

实现阶段版本仍为0.2.97，以下成绩不能当作0.2.98重新测试：

1. 新回归首次运行：2pass/2fail（含初始化/清理）；同批响应丢失、分片提前返回均有真实失败。
2. 修复后原两项回归：4/4；随后补齐无提示符标题更新时的原标题恢复分支，共四个业务场景。
3. Release主程序、Core测试及终端视图测试目标构建通过。
4. 相关CTest首次2/3（80.06秒）：Core通过（18.00秒），SFTP面板通过（15.88秒，QTest22/22），终端视图失败。
5. 终端视图offscreen完整QTest：21pass/2fail（46.362秒）。失败为`savedSchemeRepaintsExistingTerminalPixels`和`fontZoomRoundTripRestoresTerminalContent`，该后端不支持QRhi；四项目录回归均通过。未更改或放宽画面断言。
6. Windows原生Qt后端完整终端视图QTest：23/23（71.806秒）。独立只读行为复核未见高可信新问题。

本地原始日志（不纳入提交）：`build/sftp-cwd-red.txt`、`build/sftp-cwd-green.txt`、`build/sftp-cwd-green-build.txt`、`build/sftp-cwd-ctest.txt`、`build/sftp-cwd-terminal-offscreen.txt`、`build/sftp-cwd-terminal-windows.txt`。

复核命令：

```powershell
ctest --test-dir build/Release -C Release -R '^novaterm_(core|terminal_session|sftp_panel)_tests$' --output-on-failure
$env:QT_QPA_PLATFORM = 'windows'
& build/Release/bin/novaterm_terminal_session_tests.exe -o build/sftp-cwd-terminal-windows.txt,txt
```

上述命令还需按AGENTS.md设置Qt bin PATH与QT_PLUGIN_PATH；首次CTest继承了offscreen设置，不能把后续Windows原生复核拼成一次全绿CTest。

用户随后要求创建Git提交，按约定递增至0.2.98，仅重新构建Release主程序；日志为`build/sftp-cwd-commit-build.txt`。未连接用户服务器，未执行无关全套、其他平台或消毒器，不推送。
