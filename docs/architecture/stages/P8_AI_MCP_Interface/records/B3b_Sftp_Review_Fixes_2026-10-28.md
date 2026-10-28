# B3b SFTP 上传行为审查修复（2026-10-28）

基于632bec0/0.2.92修复qt-cpp-review确认的两项P2。只改行为，不查机械风格；不提交/推送、不改版本/工具Schema、不连接真实设备。

- [x] 用真实SftpSession准入和worker退出收尾复现：已受理而未出队的上传在独立SFTP自然断开后无终态。退出须与队列准入串行关闭，并为未执行请求交付一次未发布终态；显式stop交错不能丢失或重复交付，不改活动发布结果。
- [x] 用FileTransfers实际交付边界复现：backend销毁兜底的未知总量覆盖已知总量。区分未知与空文件0字节，保留已观察进度、发布未知和原操作事实，终态后迟到结果不能改写。
- [x] 正常Windows Release默认全部目标构建，相关CTest、完整MCP业务和两代二十五工具互操作分别记录；不跑无关全套/Core/ConPTY。
- [x] 同步现行契约/实机清单/记录入口及AGENTS，diff与链接复核。

既有非Windows本地I/O拒绝、SFTPv3同账号信任和同步调用非硬期限限制保持；这两项异常回收修复不能据此写成真机验收通过。

## 实现与真实RED/GREEN

- 退出专项RED **3pass/1fail，650ms**（`build/b3b-sftp-review-worker-red.txt`）：自然退出无authorizedFileFinished，显式stop仍会清队列的对照行通过。使用已有friend测试边界，调用真实准入和finishWorker，没有新增产品测试钩子或连接网络。
- finishWorker在queueMutex内关闭running/connected，并取出尚未执行的授权上传、取消其控制句柄；结果通过与活动上传共用的postFileUploadResult交付。终态不受连接generation过滤，QObject context仍保障析构后事件不执行。显式stop与自然退出只会有一方取走原队列项，活动上传仍由原worker结算。
- 总量专项RED **3pass/3fail，183ms**（`build/b3b-sftp-review-total-red.txt`）：无进度时错误显示0，已有进度和发布中两行总量均被归零；已知空文件对照通过。测试使用FileTransfers实际交付边界的销毁兜底形态，非MainWindow窗口销毁集成或实机测试。
- SftpFileUploadResult内部总量改为optional，只有worker实际打开本地文件后才有值；FileTransfers只用已观察的结果总量更新，兜底无值时保留旧事实。协议本来允许totalBytes为null，二十五工具Schema无需改变。
- 修复后完整SFTP安全目标 **26/26，16ms**（`build/b3b-sftp-review-worker-green.txt`），完整FileTransfers目标 **10/10，484ms**（`build/b3b-sftp-review-total-green.txt`），均退出0。包含自然退出后立即显式stop、一次交付/队列释放、未知/空文件/已知总量、发布未知、迟到结果不改写及原发布/取消断言。

修复后独立只读复核未见高可信Critical/Important新问题；核对了queueMutex准入/自然退出/显式stop交错、通知跨generation且受QObject生命周期保护、活动发布不参与队列清理，以及optional总量的旧消费方兼容。审查者未修改代码、构建、运行测试或连接设备，不将静态结论替代动态成绩。

## 固定源码完整验证

正常Windows MSVC BuildTools/Qt6.8.3 Release默认全部目标构建通过，退出0（`build/b3b-sftp-review-final-build.txt`）。本轮固定源码分别验证：

| 验证 | 实际结果 | 原始日志 |
| --- | --- | --- |
| 相关CTest（排除完整MCP业务） | **26/26，60.20秒** | `build/b3b-sftp-review-final-ctest.txt` |
| 完整MCP业务文件 | **250pass/0fail/0skip，132.165秒** | `build/b3b-sftp-review-final-mcp.txt` |
| 官方SDK2025/rawHTTP2026 | **二十五工具Schema及必要互操作通过** | `build/b3b-sftp-review-final-interop.txt` |

原外来属主ACL夹具与产品断言本轮通过，没有跳过或放宽，也不据此改写上传实现记录中旧轮Windows1307结果。构建/CTest/业务/互操作退出码均0（`build/b3b-sftp-review-final-results.json`）；四批独立记录，不拼成全项目全套。

```text
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build/Release -j 4'
ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|sftp_panel|command_safety|resource_sharing|session|session_panel|ui_dialog_layout)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure
build/Release/bin/novaterm_mcp_tests.exe -o build/b3b-sftp-review-final-mcp.txt,txt
python tests/mcp/interop_check.py
```

Qt bin/plugins使用6.8.3，QT_QPA_PLATFORM=offscreen、NO_PROXY=*，TEMP/TMP独立使用 `build/b3b-sftp-review-final-temp`，SDK依赖沿用build/mcp-test-deps。没有增加应用Python依赖或连接真实设备；互操作覆盖Schema和未授权上传拒绝，不代表真实文件交付。没有运行无关全套/Core/ConPTY。

同步现行契约、records入口与实机清单U05，实机项目仍未执行。diff空白检查及文档链接检查无新增缺失（`build/b3b-sftp-review-doc-check.json`）；AGENTS两条HEAD已有历史缺失链接单列。版本0.2.92及二十五工具Schema保持，未提交/推送、不改机械风格；日志/本机验证脚本留在忽略的build目录。
