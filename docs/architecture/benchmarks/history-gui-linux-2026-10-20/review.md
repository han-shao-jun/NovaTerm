## Qt Code Review Report

**Scope**: 本轮 GUI 基准、tests/CMakeLists.txt 新目标及 TerminalRenderer 新增统计的 diff；不扩展到既有渲染代码重构。
**Files reviewed**: 4
**Issues found**: 2（0 个 lint 确认行为缺陷，2 个深审计量缺陷；均已修复）

### Lint findings

原始确定性输出见 [review-lint.txt](review-lint.txt)，同时包含 Renderer 既有未改行的提示。
新增基准的三条机械提示逐项核对如下，未以批量风格重写代替行为验证。

- L-001 / DEP-10：`nowNs()` 使用 `std::chrono::duration::count()`；这是时长取值 API，不能改成容器 size。
- L-002 / TMO-1：本地 `await` 接收毫秒整数，调用点与计时单位明确且有超时；保留现有 Qt 基准惯例，本轮无超时语义缺陷。
- L-003 / ERR-1：`QSaveFile::open()` 返回值已传给 `require`；失败会抛异常并非零退出。写入长度和 commit 同样受检。

新代码的 min/max 已按 Windows 宏安全形式书写。没有修改既有无关行。

### Deep analysis findings

#### D-001 增量帧可能提前认领新视口（已修复）

- **File**: `src/renderer/TerminalRenderer.cpp` 的完成统计更新处。
- **Category**: Performance & Quality / Model Contracts。
- **Confidence**: 88/100。
- **Finding**: 滚动先改变映射，完整重绘经 scheduler 后续交付；已经排队的 sparse 帧可能先执行，不能据此宣告整个新目标完成。
- **Trace**: 真实 GPU self-test 先交付 sparse frameRequested，再滚动，再同步触发绘制；修复前报 `sparse frame must not complete a new viewport mapping`。
- **Mitigation**: 新 `_contentViewportRevision` 只在当前映射的实际完整内容绘制后推进；所有 glyph/history 就绪后才发布完成标记。保留后续 glyph 修复帧正常完成的路径。

#### D-002 旧画面帧可掩盖正确内容停顿（已修复）

- **File**: `tests/benchmarks/HistoryGuiScrollBenchmark.cpp` 的间隔统计与场景 JSON。
- **Category**: Performance & Quality。
- **Confidence**: 90/100。
- **Finding**: 等待冷历史时仍可提交旧画面；只统计 GPU frame gaps 不能代表历史内容流畅度。
- **Trace**: 计量分别检查所有提交帧与匹配目标锚点/代次的帧，并核对请求合并路径。
- **Mitigation**: 输出 correct_content_frame_interval/fps 及两套 16.7/33.3 ms 超预算计数；为被合并请求记录最早请求批次等待，不把中间请求冒充完成。

### Investigation targets (human verification needed)

无剩余 60–79 分代码调查项；GUI 测量的边界限制保留在主报告：阶段后的 grabFramebuffer 会重新绘制，不能视为逐计时帧像素捕获；闭环帧间隔受输入节拍影响。

### Summary

| Category | Confirmed lint | Deep findings | Remaining confirmed |
| --- | ---: | ---: | ---: |
| Model Contracts | 0 | 与 D-001 交叉复核 | 0 |
| Ownership & Lifecycle | 0 | 0 | 0 |
| Thread Safety | 0 | 0 | 0 |
| API & C++ Correctness | 0 | 0 | 0 |
| Error Handling | 0 | 0 | 0 |
| Performance & Quality | 0 | 2（已修复并复审） | 0 |

六个领域各由独立只读分析覆盖，没有启动 GPU 或构建干扰采样。
实现修复在只读审查之外完成，遵循 effective-cpp 约束：值类型统计、显式线程 RAII 回收、无新生产 owner。
Debug/Release self-test、Renderer 专项及矩阵均通过；RED/GREEN 日志与验证范围见主报告。
