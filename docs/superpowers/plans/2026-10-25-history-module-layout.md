# 历史模块按职责拆分

用户已要求实施，并明确 MCP 可以引用独立模块。保持本轮之前的 B0/B1 改动及所有发布接口/持久格式；不提交、不推送、不连接设备。

## 布局与边界

- `src/history/`：纯 C++ 内存 Scrollback 后端、LZ4 codec/后台服务/冷读；保留 `novaterm_history` 和独立无 Qt 构建。
- `src/recording/`：接收流持久录制、块文件、SQLite 索引、配额及纯值记录类型；保留 `novaterm_history_recording`。
- `src/historysearch/`：基于窄 HistoryReadPort 的离线授权/分页读取/搜索/投影；保留 `novaterm_history_search`，仅使用 recording 的值类型，不依赖具体录制后端。
- `src/resources/RecordingHistoryReadAdapter.h`：仅由组合根消费的具体录制源 → 读取端口适配。MCP 可以引用这些独立模块，不把模块实现搬入 MCP。
- 测试同样拆为 `tests/history`（纯后端）、`tests/recording`、`tests/historysearch`；既有目标名、标签、断言保持。录制/search CMake 各自位于对应目录，不保留旧阶段命名的 include 注册或兼容转发头。

## 执行与验证

1. 保存当前工作区快照，按生产依赖盘点文件；定向移动，更新 include/CMake/根 glob 排除和真实运行脚本路径。
2. 对照迁移前后源码，确认仅路径/include/构建注册变化；不改数据类型命名、线程/执行行为或持久格式。
3. 同步模块 README、统一架构、项目导航与记录；历史原始证据不批量改写。
4. Release 完整构建、注册数与目标清单比较、独立无 Qt 构建及相关历史/MCP/录制/Session 回归。因模块跨界及 CMake 注册变化，按 AGENTS 运行全套并单列已知 ConPTY 失败。

本任务是目录和构建边界重构，不新增功能或镜像实现测试；复用现有行为回归与源内容迁移校验。


进度：30 文件移动/include 迁移完成，删除两个旧阶段 CMake include；完整 Release 构建通过，47 个 CTest 名称保持，独立 no-Qt 1/1 通过。三个模块 README 与统一架构/导航已同步；只读独立审查未发现问题。全套当前运行，最终结果写入 docs/architecture/History_Module_Layout_2026-10-25.md。

完成：Release 构建通过；本轮固定全套 46/47（351.54 秒），仅原 ConPTY 目标失败；MCP 16/16、录制4/4、独立 no-Qt 1/1。模块/测试/注册/文档已拆开，未提交、推送或连接设备。
