# 文件授权目录浏览与Linux串口发送修复

日期：2026-10-30。实施版本0.2.103；用户追加提交与推送要求后按主题收口，见末节。保留同工作区的[历史录制窗口关闭修复](History_Recording_Close_Fix_2026-10-29.md)。用户明确要求修复审查发现的全部问题并包含目录浏览器，随后补充串口ZMODEM发送问题；未收到更具体的设备症状，本轮修复独立复现的软件缺口。

## 行为与实现

- 文件授权说明和状态统一12px，状态按纯文本显示。选择本地目录后以QPointer检查窗口存活，并复核接入身份和选择代际；切换类型后再切回也不接受旧结果。
- SSH模式提供真正的远端目录浏览器：主目录、上级、绝对路径转到、双击/回车进入、明确选择。只列目录，显示服务器规范路径；选择不自动共享，仍须明确提交读写授权。切换连接、撤权、重置身份或关闭时取消旧选择。
- 复用原SshPanelStack/SftpPanel/SftpSession所有者和worker，后台获取不会切换可见面板。请求使用独立单调ID和结果信号，不消费原directoryListed，不改变面板目录或错误状态。原Transport连接代际/指纹和统一身份在准入与结果交付前复核。
- 适配器最多8个待完成请求；SFTP最多16个目录请求且沿用总队列上限。单目录最多1024个目录项、8192个扫描项，截断明确提示。worker在同步库调用之间检查10秒预算，UI在15秒超时后取消交付；不承诺同步libssh调用内部硬退出。队列取消、自然退出、连接销毁、适配器销毁和迟到结果均有结算保护。
- 浏览路径最多4096个UTF-8字节，共享根保持原Budget的1024字节上限。较长目录可以继续浏览但禁止选为共享根；本地选择器返回超长路径明确拒绝，防止QLineEdit静默截断后共享错误目录。根目录不能整体共享。
- 串口模式隐藏无关路径与读取控件，明确先共享本地读取目录，再选择并授权串口发送。接收端准备和MCP输入lease仍须由调用方处理，不自动rz、不抢占输入、不把ZSKIP等同送达。

Linux的MCP安全文件Reader此前恒返回UnsupportedPlatform，UART发送无法打开源文件。现通过逐级O_PATH/O_NOFOLLOW固定目录和叶子描述符，先拒绝链接、多硬链接、FIFO/设备等非普通文件，再从本进程仍持有的/proc/self/fd描述符打开读取；校验设备/inode/大小/属性快照，采用有界pread并在每次读取前后复核。沿用64MiB文件、64KiB块预算；随机读取不改变顺序游标，只在原传输worker打开一次。

flock是协作锁，不能保证Windows的强制禁止写入/重命名；目录其他写入者须可信，不承诺任意并发恶意写入下的原子内容快照。procfs不可用时拒绝打开。Linux Writer及macOS Reader/Writer仍未实现；本轮没有扩大到跨平台下载后端。

面板回归另复现SystemMonitorPanel销毁时，系统信息子窗口的destroyed回调访问已析构QPointer成员，造成Qt共享对象警告及堆损坏。析构时先撤事件过滤器、断开子窗口回调并清空成员，再关闭子窗口，避免QWidget基类删除子窗口时回调已析构成员。

## 真实失败与回归

各轮成绩保留原样，不拼为一次全套：

| 场景 | 失败证据与修复后结果 |
| --- | --- |
| 本地选择重入/父窗口销毁 | RED 6pass/4fail；父窗口销毁子进程CrashExit，gdb定位currentIdentity；原GREEN 10/10 |
| Linux安全读取源 | RED 2pass/9fail；GREEN 11/11，包含软链/祖先软链/硬链/FIFO/目录/超限/修改/固定描述符 |
| Linux MCP UART发送 | 修复后34/34（3.271秒），保留授权、lease、旧代际、取消及完成事实断言 |
| 原ZMODEM协议文件源集成 | 初次启用Linux有3项Windows强制禁止rename断言失败；该平台专属断言保留于Windows，Linux固定描述符语义另测；GREEN 7/7（2.427秒） |
| 适配器销毁 | RED 2pass/1fail，存活浏览器没有收到终态；修复为安全的排队错误交付，加入迟到结果/队列回收回归 |
| 长路径回填 | RED 2pass/1fail，1205字符结果被输入框截成1024；修复保持原共享预算并禁止选取，未扩大Budget |
| 系统信息子窗口析构 | RED 2pass/2fail，以failOnWarning捕获Qt共享对象警告；GREEN专项4/4，完整SFTP面板23/23（14.738秒） |

远端请求API最初缺失时有编译RED，不把它记作已执行失败。独立浏览器早期有一项夹具在加载中点击禁用按钮，改为先交付响应再导航；SFTP面板新增夹具曾遗漏connected后的directoryListed，补足既有协议；产品未为这两项夹具修改而放宽行为。

最后路径边界新增后，本地/远端选择专项16/16（1.125秒）、浏览器独立9/9（0.028秒），包含ASCII和中文UTF-8超长路径拒绝与恢复。

## 最终验证

- `cmake --build build/Release --target all -j 6` 成功，保留-Werror；日志 `/tmp/novaterm-resource-browser-bounds-final-build.log`。
- `QT_QPA_PLATFORM=offscreen ctest --test-dir build/Release --output-on-failure -L 'mcp|recording' -j 3`：29/29（43.90秒），本轮不再排除Linux串口文件源目标。日志 `/tmp/novaterm-resource-browser-final-tests.log`。
- 相关Session、SessionPanel、SftpPanel及串口传输UI另轮4/4（18.19秒）；日志 `/tmp/novaterm-resource-browser-panels-acceptance.log`。原串口Controller完整目标在前一联合批次通过（96.16秒）；同批SFTP面板失败/超时未隐去，见 `/tmp/novaterm-resource-browser-session-serial-final.log`，后续修复面板再复验。
- `python3 tests/mcp/ssh_loopback_check.py build/Release/bin/novaterm_mcp_tests --directories`：真实受控SFTP worker目录规范化、UTF-8/空格、符号链接、1024条预算、错误和旧信号隔离通过；只连接本机夹具，未连接用户SSH设备。日志 `/tmp/novaterm-directory-loopback-acceptance.log`。
- 官方SDK2025/raw HTTP2026二十二工具、8项退役工具拒绝及原工作流通过；日志 `/tmp/novaterm-resource-browser-interop-acceptance.log`。最后路径UI边界改动未改工具Schema，未重复该轮协议验收。
- 默认CPack DEB+ZIP通过；两包分别解压后helper执行、仅SQLite驱动、SQLite读写及隔离配置X11短暂启动通过。日志 `/tmp/novaterm-resource-browser-final-cpack.log`、`/tmp/novaterm-resource-browser-final-package-verify.log`。未系统安装，构建产物与临时脚本/日志不纳入源码。

按Qt/C++所有权、模型、线程、API、错误和性能六领域人工行为复核，并运行确定性检查；当前环境无独立子代理，不声称独立多代理审查。隔离offscreen截图核对远端浏览、串口授权及字号；未改第三方样式库。未做真实UART/设备验收、其他平台/发行版、无关全套或完整消毒器验证。实施阶段所有中间失败日志保持于本机/tmp，不将绿色结果改写为设备已经送达。

## 0.2.105提交收口

按用户要求分主题创建提交并推送Gitee master。历史关闭先以93e54d8（0.2.104）提交；本项纳入目录浏览、授权窗口、Linux读取源、系统信息窗口析构及对应测试/翻译/文档，版本递增至0.2.105。没有修改二十二工具Schema或扩大共享预算。

版本更新后Release all成功；MCP/录制相关CTest29/29（42.97秒），会话/面板另轮4/4（18.23秒）。两代二十二工具互操作、受控SFTP真实worker目录浏览、默认DEB/ZIP及两包分别解压helper/SQLite/隔离配置X11启动均通过。原0.2.103实施结果保留原轮次，不冒充0.2.105的全部单项重测；原串口Controller96.16秒属于上文较早批次，未重复。相关日志为`/tmp/novaterm-02105-{build,related-tests,panels-tests,interop,directory-loopback,cpack,package-verify}.log`。构建产物与本机依赖/临时文件不纳入，未连接真实设备或验证其他平台。
