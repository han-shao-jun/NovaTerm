# MCP 接入与旧命令通路清理（2026-10-29）

## 已确认范围

用户批准删除旧命令通路及对应接入UI；保留新终端交互、文件传输、保存目标、
历史录制和客户端身份隔离，客户端增删/令牌轮换进入次级窗口。
基于0.2.100实施；用户随后要求创建提交并推送。CPack主题先以0.2.101提交，MCP清理主题版本递增至0.2.102。以下分轮成绩保留原始实施版本，不改写为新版本重测。
本记录保留实施范围、结果和验证证据；过程设计与计划已清理，可从 Git 历史查阅。

## 结果

- 退役 `novaterm_list_commands`、`novaterm_execute_command`、`novaterm_run_command`、
  `novaterm_run_script`。目录和能力注册、Schema/解码及运行期接线一起移除；
  两代HTTP直接调用返回 `-32602 / Unknown tool.`，不转发为新输入。
- 当前完整工具目录30→26，终端DTO联合9→5；保留工具的字段/授权保持原有语义，
  list_sessions不再公布旧能力和human_confirmation。
- 删除旧命令执行器、Session适配、Typed命令端口、诊断目录、持久目标保护、
  签名确认Registry/编解码、短时去重和风险分类器，删除16个独占生产文件及
  旧执行器专属测试文件；其他旧专属测试随功能退役，现行契约按名称/新位置核对。
  历史fixture文件不改写，仅对已退役工具及能力枚举差异作显式处理。
- 接入会话表11→8列，移除旧诊断/脚本/交互授权、旧权限显示开关、执行记录与
  结果、确认远端停止/解除暂停区域；新增“管理客户端…”次级窗口，支持无客户端
  首次创建、添加、删除、轮换令牌及重复打开复用。窗口父子所有权和模态约束保留。
- 简化提示文字，清理25条已无该对话框消费者的中英文消息，补齐新文案；
  更新当前使用说明、契约、架构索引与路线图，不把历史Windows成绩改写为本轮成绩。

共用Session的Profile、执行/传输状态和独立本机诊断helper仍有Session层消费者，
未为删除MCP旧接口而重写它们。内部旧授权选择字段保留快照兼容，不再产生可发现/可调用
的旧工具，也不升级为完整输入。令牌轮换沿用原语义：旧令牌失效，身份代际变化，
文件/保存目标绑定回收；原会话权限选择不被扩张或挪给另一客户端。
既有历史录制、导入文件和用户数据未删除，旧目标保护文件不作破坏性清理。

## 验证证据

| 批次 | 准确结果 |
| --- | --- |
| 退役目录/解码回归RED | 2pass/4fail；四个旧工具仍被识别 |
| 对应GREEN | 6/6，旧工具不注册、不接受校验/解码 |
| 临时实际窗口回归RED→GREEN | 原11列且无管理入口→8列且存在管理入口；隔离配置复用实际主窗口对象 |
| 首轮完整契约迁移 | 串口工具测试还用旧索引选择启动参数，导致bad_variant_access；修正夹具索引后3个旧固定位置断言失败，再按现行位置修正；不删除保留工具对象比较 |
| 首轮UI回归 | 12pass/1fail，夹具未计入“仅本次运行”标签后缀；按独立客户端ID验证，保留创建/移除和独立授权检查 |
| 首轮MCP相关CTest（不含完整业务/契约） | 17/23，旧数量/确认用例待迁移；串口文件目标因Linux安全文件后端缺失失败并超时，原断言保留 |
| 完整业务首轮 | 222pass/3fail/3skip；失败是新夹具未先共享读取、提权提示仍取旧列、共用Session诊断测试残留旧UI预期；修正夹具/列及退役预期，未改变产品授权语义 |
| 最终相关CTest | 24/24，37.34秒；明确排除 `novaterm_mcp_serial_file_transfers_tests` 这一已确认Linux后端缺口目标，不宣称全套全绿 |
| 上述批次内完整MCP业务 | 225pass/0fail/3skip，37.304秒；3skip均为Windows属主验证/迁移，Linux不适用 |
| 两代互操作 | 官方MCP SDK2025 + raw HTTP2026，26项输入/输出Schema、等待/任务/重试/文件/连接契约及四旧工具拒绝均通过 |
| 实际截图复核 | 次级窗口首次截图发现默认字号过大裁切说明，改为12px；最终主界面1050×680与管理窗口520×250复核，临时画面在 `/tmp/novaterm-mcp-{cleanup,client-manager}.png` |
| 字号修正后 | Release all成功；客户端管理/首次创建与令牌轮换/提权条件/两代旧工具拒绝专项10/10（0.748秒）；未把先前24项目标成绩写成字号修正后全量重测 |
| 最终打包 | 默认CPack DEB+ZIP成功；分别解压后SQLite-only插件、同目录helper执行、随包SQLite建表/读写与隔离XDG的X11公共启动器短暂启动通过 |

最终仅字号有可视化修正，协议与业务源码沿用24目标和互操作批次；没有把多个轮次拼成一次全套。
未运行无关Core/ConPTY全套、其他平台、真实设备或跨发行版安装验收。

## 行为复核与本机证据

按qt-cpp-review流程运行确定性扫描，并在本会话按模型/生命周期/线程/API/错误处理/
性能六领域复核。环境无独立子代理，不能称为独立多代理复核。扫描输出主要为既有
optional/风格与测试宏提示，未做机械格式化；未确认新的高可信行为问题。
客户端管理复用原SettingsController；错误仍通过原保存失败路径反馈，回调连接以次级
窗口为context，排队刷新不在itemChanged栈中删除旧行，关闭父窗口回收次级窗口。

原始证据均为本机临时文件、不纳入仓库：

- `/tmp/novaterm-cleanup-retired-{red,green}.txt`
- `/tmp/novaterm-cleanup-contracts{1,2}.txt`、`/tmp/novaterm-cleanup-ui-tests{1,2}.txt`
- `/tmp/novaterm-cleanup-ctest{1,2}.log`、`/tmp/novaterm-cleanup-ctest-final.log`
- `/tmp/novaterm-cleanup-interop-final.log`、`/tmp/novaterm-cleanup-final-build.log`
- `/tmp/novaterm-cleanup-final-ui-tests.txt`、`/tmp/novaterm-cleanup-ui-preview-final.log`
- `/tmp/novaterm-cleanup-cpack-final.log`、`/tmp/novaterm-cleanup-package-verification-final.log`
- `/tmp/novaterm-cleanup-lint.log`

互操作依赖仅安装于 `build/mcp-test-deps`，应用不增加Python依赖。
安装器提示系统已有yt-dlp/websockets依赖冲突，但目标目录安装成功、互操作通过；
未为此修改系统Python包。首次旧索引触发的测试core已转存 `/tmp/novaterm-cleanup-contracts-test.core`。

## 0.2.102提交前独立验证

用户明确要求创建提交并推送。按主题先创建CPack提交c91a56c（0.2.101），MCP清理独立递增至0.2.102。最终源码Release all构建成功；`ctest --test-dir build/Release --output-on-failure -L 'mcp|recording' -E '^novaterm_mcp_serial_file_transfers_tests$' -j 3` 为28/28（37.66秒），包含完整MCP业务225pass/0fail/3Windows-only skip（37.626秒）。串口文件发送目标仍按已证实的Linux安全文件后端缺口排除，原失败保留，不称全套通过。

官方SDK2025/rawHTTP2026验证26工具和四旧工具拒绝通过。默认CPack生成`novaterm-0.2.102-linux-x64.deb/.zip`；两包独立解压后的SQLite-only驱动、同目录helper执行、SQLite内存库读写以及隔离XDG的X11启动通过。此批与0.2.100实施阶段分开。原始日志为`/tmp/novaterm-02102-{all-build,commit-tests,interop,cpack,package-verification}.log`，临时包探针`/tmp/novaterm-02102-package-verify.py`；所有产物、依赖及本机日志均不纳入Git。
