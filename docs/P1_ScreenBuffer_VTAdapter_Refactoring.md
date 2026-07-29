# NovaTerm P1 ScreenBuffer 与 VTAdapter 重构

> 完成日期：2026-07-29  
> 状态：已完成  
> 范围：数据模型和 Parser/Renderer 边界；不包含多线程 Parser。

## 1. P1 目标

P1 的目标是消除 Renderer、Scrollback 和 TerminalCore 公共接口对
libvterm 数据类型的直接依赖，为 P2 Parser Worker 建立清晰的数据所有权。

本阶段不改变以下行为：

- Transport 仍将数据交给 `TerminalCore`；
- Parser 仍运行在调用 `TerminalCore::writeInput()` 的线程；
- Renderer 仍按当前 QRhi 全屏命令生成方式工作；
- Scrollback 仍使用现有行级环形存储，Chunk 化属于 P4。

## 2. 详细迁移步骤

### 2.1 自有数据模型

新增 `TerminalTypes.h`，定义：

- `Cell`
- `CellAttributes`
- `TerminalColor`
- `ColorType`
- `UnderlineStyle`
- `Position`
- `DirtyRegion`
- `CursorShape`
- `CursorState`

这些类型不包含 `vterm.h`，是 Parser、ScreenBuffer 和 Renderer 之间的稳定契约。

### 2.2 可见区 ScreenBuffer

新增 `ScreenBuffer`：

- 按 `row * columns + column` 连续存储可见 Cell；
- Parser/Adapter 拥有写权限；
- Renderer 通过 `TerminalCore::getCell()` 或只读引用读取；
- resize 时保留重叠区域；
- 越界访问返回空指针或 `false`。

### 2.3 值语义快照

新增 `TerminalSnapshot`：

- 保存 columns、rows、visible cells 和 cursor；
- 快照创建后不受后续 Parser 写入影响；
- 当前 P1 用于验证稳定读取语义；
- P2 将在此基础上选择双缓冲、COW 或发布式快照。

### 2.4 VTAdapter

新增 `VTAdapter` 并使用 Pimpl 隔离 libvterm：

```text
Transport bytes
    → TerminalCore
    → VTAdapter
    → libvterm
    → libvterm callbacks
    → NovaTerm Cell / ScreenBuffer / Scrollback
    → TerminalCore signals
    → Renderer
```

`VTAdapter` 负责：

- `VTerm/VTermScreen/VTermState` 生命周期；
- ANSI/VT 数据输入；
- 键盘、鼠标、粘贴和焦点编码；
- `VTermScreenCell` 到 NovaTerm `Cell` 的转换；
- libvterm Damage Rect 到 `DirtyRegion` 的转换；
- Cursor、title、bell 和 resize 回调；
- Scrollback push/pop 的双向转换；
- 默认前景色和背景色设置。

### 2.5 TerminalCore 门面

`TerminalCore` 当前承担：

- Qt Event 到 VTAdapter 输入调用的桥接；
- Qt signals；
- ScreenBuffer、Scrollback 和 Cursor 的只读查询；
- `TerminalSnapshot` 创建；
- VTAdapter 生命周期管理。

其公共头文件不再提供：

- `VTerm*`
- `VTermScreen*`
- `VTermState*`
- `VTermScreenCell`
- `VTermColor`
- `VTermRect`
- `VTermPos`

### 2.6 Renderer 迁移

Renderer 已迁移为只消费：

- `NovaTerm::Cell`
- `NovaTerm::TerminalColor`
- `NovaTerm::CellAttributes`
- `NovaTerm::Position`
- `NovaTerm::DirtyRegion`
- `NovaTerm::CursorShape`

颜色、下划线、Cursor、Selection、文本提取和 Glyph 生成均通过 NovaTerm
类型完成。Renderer 不再调用 libvterm screen/state API。

### 2.7 构建边界

`novaterm_core` 的公开 include 路径只暴露 `src`：

- libvterm include 路径为 `PRIVATE`；
- libvterm 链接依赖为 `PRIVATE`；
- UI 和 Renderer 不需要 libvterm 头文件；
- libvterm 的具体依赖集中在 `VTAdapter.cpp` 和当前输入 KeyMapper 实现。

## 3. 数据所有权

P1 的所有权规则：

| 数据 | 写入者 | 读取者 |
| --- | --- | --- |
| libvterm state | VTAdapter | VTAdapter |
| Visible ScreenBuffer | VTAdapter | TerminalCore、Renderer |
| ScrollbackBuffer | VTAdapter/TerminalCore 管理命令 | Renderer |
| CursorState | VTAdapter 回调 | TerminalCore、Renderer |
| TerminalSnapshot | 创建后不可变 | 测试、未来 Renderer Worker |
| Selection | Renderer | Renderer |

P1 仍为单线程，因此 ScreenBuffer 当前没有锁。P2 不允许直接把这一可变
ScreenBuffer 跨线程共享给 Renderer，必须先引入发布式快照或双缓冲。

## 4. 自动化测试

当前 Core 测试覆盖：

1. ANSI SGR 和 UTF-8/CJK Cell 转换；
2. UTF-8 跨输入包解析；
3. Damage 信号；
4. Screen resize；
5. TerminalSnapshot 值稳定性；
6. OSC title 回调；
7. Scrollback 环形缓冲保留最新行。

Debug 和 Release 测试均通过。

## 5. 性能结果

Release、MSVC 19.44、Qt 6.8.3：

| 指标 | P0 | P1 |
| --- | ---: | ---: |
| Parser/Core throughput | 11.52 MiB/s | 4.22 MiB/s |
| 10 MiB elapsed | 868.07 ms | 2371.15 ms |
| 100,000 Scrollback lines | 337.53 ms | 1140.33 ms |

P1 与 P0 的差异来自 P1 为保证 ScreenBuffer 对 Renderer 立即可见，在每次
`writeInput()` 后 flush damage，并将脏区从 libvterm 转换到自有 Cell。
当前 benchmark 的输入块较小，因此放大了 flush 和模型转换成本。

这不是在 P1 中绕过的成本。P2 应通过以下方式解决：

- 8–64 KiB Parser 批处理；
- 有界 ByteQueue；
- 每批只 flush 一次；
- 合并 DirtyRegion；
- 发布稳定快照，而不是每个 Transport 信号触发完整同步；
- 分离纯 Parser 吞吐和 ScreenBuffer 发布吞吐指标。

## 6. P1 验收结果

- [x] Renderer 头文件不包含 `vterm.h`
- [x] Renderer 活动实现不使用 `VTerm*` 类型
- [x] TerminalCore 公共头文件不暴露 libvterm
- [x] Scrollback 不保存 `VTermScreenCell`
- [x] VTAdapter 独占 libvterm 生命周期和 callback
- [x] Parser 是 ScreenBuffer 的唯一数据写入来源
- [x] TerminalSnapshot 创建后保持稳定
- [x] Debug 应用构建成功
- [x] Debug/Release Core 测试通过

## 7. P2 前置约束

P2 实施时必须遵守：

1. 不允许 Renderer 跨线程读取正在被 Parser 修改的 ScreenBuffer；
2. ByteQueue 必须有容量上限和背压策略；
3. Parser Worker 独占 VTAdapter；
4. 每批输入只发布一次 DirtyRegion/快照；
5. close、resize 和 reconnect 必须进入明确的命令队列；
6. P2 优化后必须同时比较纯 Parser 和完整发布路径性能。
