# NovaTerm 统一架构设计

NovaTerm 的统一架构文档集中在本文件；文档索引、配置与主题、渲染架构、路线图及阶段实施说明仍位于 [architecture/README.md](architecture/README.md)。

本文件合并自原 `docs/ARCHITECTURE.md`、`docs/architecture/NovaTerm_Architecture.md` 和 `docs/01_Architecture.md`，完整保留三者内容。

该文档集包含：

- 当前实现与目标架构；
- 分层、数据流、线程、所有权、生命周期和背压设计；
- Profile、Session、配置和主题系统；
- QRhi 增量渲染与 Glyph/GPU 管线；
- P0～P7 路线图及每个阶段的独立实施文档。

`docs/` 中原有的设计和 P0～P3 实施记录继续作为历史资料保留；发生冲突时，以本文件、`docs/architecture/` 配套文档和当前源码为准。

## 1. 愿景与范围

NovaTerm 是基于 Qt 6、libvterm 和 QRhi 的跨平台 GPU 终端。核心目标是：协议解析与 UI 解耦、不同 Transport 共享同一数据通路、持续高输出下 UI 可响应、增量 GPU 渲染、百万行可控 Scrollback，以及可扩展但不破坏核心所有权的插件体系。

本文同时描述三类内容：

- **当前实现**：P0～P5 已完成；P5 已完成 Linux Vulkan/OpenGL、Windows D3D11/D3D12 与 Windows 30 分钟长稳验收，macOS Metal 和真实高刷硬件仍待补充；
- **近期目标**：P6；
- **远期扩展**：P7。

## 2. 架构原则

1. Parser 单写，Renderer、Search 和插件只读。
2. libvterm 类型只能存在于 VTAdapter 实现边界。
3. Transport 只处理字节和连接状态，不理解终端 Cell。
4. Renderer 不理解 ANSI、SSH 或配置文件格式。
5. 所有跨线程队列必须有上限、统计和停止语义。
6. 通过不可变快照或版本化共享数据跨线程，不共享无保护可变对象。
7. 先建立正确性测试和基线，再优化。
8. UI Theme、Terminal Scheme 和 Font Config 分离。
9. Session 是运行期资源和生命周期边界，Profile 是创建模板。
10. 每个阶段保持可构建、可测试、可回退定位。

## 3. 系统分层

```mermaid
flowchart TB
    UI[UI Layer<br/>MainWindow / Tabs / Settings / TerminalView]
    APP[Application Services<br/>Config / Profile / Theme / SessionManager]
    SES[TerminalSession<br/>生命周期与流控]
    TR[ITransport<br/>PTY / SSH / Serial / Telnet]
    CORE[Terminal Core<br/>ByteQueue / Parser Worker / VTAdapter]
    MODEL[Terminal Model<br/>ScreenBuffer / Scrollback / Snapshot]
    RS[Render Support<br/>RenderScheduler / RenderCommandBuffer]
    GPU[QRhi Renderer<br/>Glyph / Buffer / Pipeline]
    OS[OS & Runtime<br/>PTY / ConPTY / Socket / Serial / GPU]

    UI --> APP
    APP --> SES
    SES --> TR
    SES --> CORE
    TR --> OS
    TR -->|bytes| CORE
    CORE --> MODEL
    MODEL -->|snapshot + dirty| RS
    RS --> GPU
    GPU --> OS
```

### 3.1 UI Layer

负责窗口、标签、设置、输入事件、选择和用户反馈。UI 不解析 ANSI，不持有 libvterm，不实现 Transport 缓冲策略。当前 `TerminalView` 仍组合 Core、Renderer 和 Transport，并暂存竞争窗口中的输入；P6 应将这些运行期职责下沉到 `TerminalSession`。

### 3.2 Application Services

负责配置加载、Profile 解析、主题解析、Session 创建与列表管理。服务层输出结构化对象，不要求 Renderer 自行读取 JSON。

### 3.3 TerminalSession

目标 Session 聚合：Transport、输入泵、ByteQueue、TerminalCore、Scrollback、状态和渲染调度关联。它负责 start、close、resize、reconnect、后台策略和错误传播，但不负责绘制细节。

### 3.4 Terminal Core

`TerminalCore` 是线程安全 Qt 门面；Worker 独占 `VTAdapter` 和 libvterm 可变状态。`VTAdapter` 把 libvterm callback 转换成 NovaTerm 的 Cell、DirtyRegion、Cursor 和属性变化。

### 3.5 Renderer

Renderer 读取稳定 Snapshot，把 DirtyRegion 转为 row-local、按 8-cell block 缓存的 Render Command，仅上传变化的 GPU Buffer 区间。P5 Renderer 使用完整 cluster GlyphKey、字体 fallback、灰度/彩色多页 Atlas、局部纹理上传、实例化 Quad、material batch 和 GPU 行槽位环；Glyph Atlas 和 GPU 资源只属于 Renderer。live-bottom 不构建全历史 reflow，用户进入回看时才惰性生成历史布局。

### 3.6 Transport

所有连接实现 `ITransport`：连接、断开、写入、resize、暂停读取、错误和字节到达。SSH、Serial、Telnet 必须使用与 Local PTY 相同的数据入口。

## 4. 端到端数据流

### 4.1 远端输出到屏幕

```mermaid
sequenceDiagram
    participant T as Transport
    participant S as Session/InputPump
    participant Q as BoundedByteQueue
    participant P as Parser Worker
    participant V as VTAdapter/libvterm
    participant M as ScreenBuffer
    participant R as RenderScheduler
    participant G as QRhi Renderer

    T->>S: readyRead(bytes)
    S->>Q: 非阻塞批量入队
    alt 达到高水位
        S->>T: setReadPaused(true)
    end
    P->>Q: 最多 64 KiB 批量读取
    P->>V: input(bytes)
    V->>M: 更新 Cell/Cursor/Properties
    V-->>P: 合并 DirtyRegion 与事件
    P-->>R: 发布 damage/状态
    R->>R: 合并并按刷新率节流
    R-->>G: frameRequested
    G->>M: 获取稳定 Snapshot
    G->>G: 重建脏行并局部上传
```

### 4.2 用户输入到远端

```mermaid
flowchart LR
    E[Qt 键盘/鼠标/粘贴事件] --> C[平台无关命令或 KeyMapper]
    C --> Q[Worker 命令队列]
    Q --> V[VTAdapter 编码]
    V --> O[outputData bytes]
    O --> S[TerminalSession]
    S --> T[ITransport.write]
```

命令必须按“此前已接收字节完成屏障”排序，避免 resize、输入和输出状态越序。

## 5. 线程模型

NovaTerm 不采用固定“七线程”设计。队列和 ScreenBuffer 是数据结构，不是线程。推荐的最小运行模型如下：

```mermaid
flowchart TB
    subgraph GUI[Qt GUI / Render Thread]
      U[UI Events]
      RR[QRhi Render]
    end
    subgraph IO[Transport I/O Context]
      T[PTY/Socket/Serial]
    end
    subgraph PW[Parser Worker]
      P[Command + Byte Batch]
      V[VTAdapter]
      W[Writable Model]
    end
    subgraph OPTIONAL[按需 Worker]
      SE[Search]
      GR[Glyph Rasterization]
    end

    T -->|bounded queue| P
    U -->|bounded command queue| P
    P --> V --> W
    W -->|immutable publication| RR
    W -->|chunk snapshot| SE
    RR -. cache miss .-> GR
```

Transport 可以使用 Qt 事件循环、专用读线程或 OS 异步 I/O；这属于实现策略。关键约束是不能因 Parser 积压阻塞 GUI，也不能静默丢失普通终端输出。

## 6. 数据模型与所有权

| 数据 | 唯一写入者 | 读取者 | 发布方式 |
| --- | --- | --- | --- |
| libvterm state | Parser Worker / VTAdapter | VTAdapter | 不跨边界 |
| ScreenBuffer | Parser Worker | Renderer、测试 | Snapshot |
| Scrollback active chunk | Parser Worker | 无直接共享 | seal 后发布 |
| Scrollback sealed chunks | 无写入者 | Renderer、Search | 引用计数只读快照 |
| Cursor/Properties | Parser Worker | UI、Renderer | 批次事件/Snapshot |
| RenderCommandBuffer | GUI/Render 侧 | QRhi 提交 | Renderer 内部 |
| Selection | View/Renderer | Renderer | Overlay |
| Profile | ProfileManager | Session factory、UI | 不可变解析结果 |

当前值语义 Snapshot 保证安全，但后续应使用共享不可变存储、分行版本或 COW 降低每帧复制成本。优化不得破坏稳定读取语义。

## 7. 背压和过载

```mermaid
stateDiagram-v2
    [*] --> Normal
    Normal --> Paused: backlog >= 6 MiB
    Paused --> Normal: backlog <= 4 MiB
    Paused --> Overload: Transport 无法暂停或暂存预算耗尽
    Overload --> Normal: 队列恢复且数据完整
    Normal --> Stopping: Session close
    Paused --> Stopping: Session close
    Stopping --> [*]: 队列唤醒并完成线程回收
```

当前 ByteQueue 容量为 8 MiB，高低水位为 6/4 MiB。目标架构中，未入队片段由 Session/InputPump 管理，不由 TerminalView 管理。任何丢弃策略必须显式、可统计，并区分普通输出、遥测或可重试数据。

## 8. 生命周期

Session 状态建议统一为 `Created → Connecting → Running → Paused/Reconnecting → Closing → Closed/Failed`。关闭顺序为：停止接收新任务、暂停 Transport、按策略排空已提交任务、停止并唤醒队列、等待 Worker、销毁 VTAdapter、关闭 Transport、释放视图和 GPU 资源。

禁止 QObject 跨线程无所有权迁移、Worker 持有已销毁 UI 指针，以及 Session 销毁后仍投递回调。

## 9. 当前目录映射

```text
src/
├── core/terminal/       # Queue、Core、VTAdapter、Screen/Scrollback、类型
├── renderer/            # Scheduler、CommandBuffer、QRhi Renderer、Scheme
├── transport/           # ITransport、LocalShellTransport
├── service/             # Config、Language；后续 Profile/Theme/Session 服务
└── ui/                  # Application、页面、TerminalView、Widgets
tests/
├── core/
└── renderer/
benchmarks/
```

目标演进时可新增 `src/session/`、`src/profile/`、`src/theme/` 和 `src/search/`；不为追求目录形式而提前搬迁代码。

## 10. 非功能目标

| 指标 | 目标 | 说明 |
| --- | ---: | --- |
| 持续 Parser 吞吐 | > 20 MiB/s | 分纯解析与完整发布链路 |
| 突发输入 | > 100 MiB/s | 必须注明持续时间与队列变化 |
| UI 线程 Parser 时间 | 0 ms | 入队操作本身另测 P95/P99 |
| 输入端到端延迟 | < 10 ms | 报告 P50/P95/P99 |
| 普通输出帧率 | 60 FPS | 支持 120/144 Hz 配置 |
| 默认 Scrollback | 100,000 行 | 可配置至 1,000,000 行 |
| 内存 | 有明确预算 | 同时按行数和 bytes 淘汰 |

所有性能结果必须注明 OS、CPU、GPU、Qt、编译器、Release 配置、数据集、时长和统计口径。

## 11. 架构决策检查表

- 新类型是否泄漏 libvterm、QRhi 或具体 Transport？
- 跨线程数据是否不可变或受明确同步保护？
- 队列是否有容量、背压、统计、停止和取消？
- Session 关闭、resize、重连时事件顺序是否确定？
- 单字符变化是否避免全屏扫描和全量上传？
- 配置热更新是否明确影响应用、Profile 或当前 Session？
- 新功能是否有正确性测试、压力测试和 Release 指标？

---

## 附录 A：NovaTerm Architecture Design（原 docs/01_Architecture.md）

> Version: 1.0
>
> Author: NovaTerm Project
>
> Last Update: 2026-07-12

---

### 1. Project Vision

NovaTerm 是一款现代 GPU 加速终端，目标定位类似：

- WindTerm
- Windows Terminal
- WezTerm
- Ghostty

设计目标：

- 高性能 GPU 渲染
- 百万行 Scrollback
- ANSI/VT100/VT220/xterm 完整兼容
- 多 Session（SSH、Serial、PTY、Telnet）
- 支持 SFTP、服务端资源监视等功能
- 后续支持 AI Assistant
---

### 2. Design Principles

整个项目遵循以下原则：

#### 高内聚

每个模块只负责一种职责。

例如：

- Parser 负责解析 ANSI
- Renderer 负责 GPU 绘制
- Session 负责网络通信
- UI 负责用户交互

彼此互不关心实现细节。

---

#### 低耦合

任何模块均可独立替换。

例如：

```
libvterm
        ↓
Contour Parser
```

Renderer 不需要修改。

例如：

```
OpenGL
        ↓
QRhi
        ↓
Vulkan
```

Terminal Core 不需要修改。

---

#### 数据驱动

所有数据最终汇聚到：

```
ScreenBuffer
```

Renderer 只读取数据。

Parser 只修改数据。

---

### 3. Overall Architecture

```
                    +--------------------------------------+
                    |          FluentUI (QML)              |
                    | MainWindow / Dock / Tabs / Settings  |
                    +------------------+-------------------+
                                       |
                              ITerminalView
                                       |
                +----------------------+----------------------+
                |                                             |
        QQuickItem (QML)                           QOpenGLWidget
                |                                             |
                +----------------------+----------------------+
                                       |
                               Terminal Renderer
                                       |
              +------------------------+------------------------+
              |                                                 |
       Render Scheduler                              Glyph Atlas
              |                                                 |
              +------------------------+------------------------+
                                       |
                                 ScreenBuffer
                                       |
       +-------------+-----------------+----------------+--------------+
       |             |                                  |              |
 DirtyRegion      Cursor                         Selection      Scrollback
                                       |
                                  VT Adapter
                                       |
                                   libvterm
                                       |
                                  Session Layer
                                       |
      +--------------+----------------+---------------+--------------+
      |              |                |               |
    Serial          SSH              PTY           Telnet
```

---

### 4. Layer Responsibilities

#### UI Layer

负责：

- 主窗口
- Dock
- Tab
- Theme
- Settings
- Plugin UI

不负责：

- ANSI
- VT100
- Parser
- GPU

---

#### Terminal View

TerminalView 只是 GPU Renderer 的容器。

可提供两种实现：

- QWidget(QOpenGLWidget)
- QQuickItem(QML)

两者共享同一个 Renderer。

---

#### Renderer

Renderer 负责：

- Draw Glyph
- Draw Background
- Draw Cursor
- Draw Selection
- Draw Underline
- Draw Hyperlink

Renderer 永远不知道：

- libvterm
- ANSI
- SSH

Renderer 只读取：

```
ScreenBuffer
```

---

#### Render Scheduler

新增独立模块。

负责：

- Dirty Merge
- Frame Scheduling
- FPS 控制
- Render Trigger

避免：

```
Parser

↓

立即 Render
```

正确流程：

```
Parser

↓

Dirty Queue

↓

Merge

↓

Render Once
```

---

#### Glyph Atlas

统一管理：

- FreeType
- Glyph Cache
- Emoji
- Font Fallback
- Texture Atlas

Renderer 永远只使用：

```
Glyph ID
```

而不是：

```
FreeType API
```

---

### 5. ScreenBuffer（核心）

ScreenBuffer 是整个 Terminal Core 的中心。

推荐：

```cpp
struct Cell
{
    uint32_t codepoint;

    uint32_t foreground;

    uint32_t background;

    uint16_t attributes;

    uint16_t fontIndex;
};
```

每一行：

```cpp
class Line
{
    std::vector<Cell> cells;
};
```

整个屏幕：

```cpp
class ScreenBuffer
{
    std::vector<Line> visibleLines;
};
```

原则：

Parser：

```
Write
```

Renderer：

```
Read
```

任何时候：

不要：

```
Renderer 修改 Cell
```

---

### 6. Scrollback

Scrollback 不放在 ScreenBuffer 内。

推荐：

Chunk 化。

例如：

```
Chunk0

4096 Lines
```

```
Chunk1

4096 Lines
```

```
Chunk2

4096 Lines
```

优点：

- 百万行
- 快速滚动
- Search 快
- 内存连续

---

### 7. Dirty Region

libvterm：

产生：

```
Damage Rect
```

不要：

立即 Render。

正确：

```
Damage Rect

↓

Dirty Queue

↓

Merge

↓

Renderer
```

连续多个 Rect：

```
Rect1

Rect2

Rect3
```

合并：

```
Merged Rect
```

减少 GPU Draw Call。

---

### 8. Selection Model

不要：

```
Cell.selected = true
```

推荐：

```
SelectionModel
```

Renderer：

Overlay。

优势：

- Copy
- Search
- Undo
- Hyperlink

全部互不影响。

---

### 9. Search Engine

Search 独立线程。

流程：

```
Scrollback

↓

Search Thread

↓

Match List

↓

Highlight
```

不会阻塞 UI。

---

### 10. VT Adapter

新增：

```
VTAdapter
```

作用：

负责：

```
libvterm callback

↓

ScreenBuffer
```

以后：

如果 Parser 更换：

```
libvterm

↓

Contour Parser

↓

自研 Parser
```

Renderer：

无需修改。

---

### 11. Session Layer

统一接口：

```cpp
class ISession
{
public:

    virtual Read();

    virtual Write();

    virtual Resize();

    virtual Close();
};
```

实现：

- SSH
- Serial
- PTY
- Telnet

统一 Transport。

---

### 12. Thread Model

推荐四线程：

```
Transport Thread

↓

Byte Queue

↓

Parser Thread

↓

ScreenBuffer

↓

Dirty Queue

↓

Render Thread

↓

GPU

↓

UI Thread
```

职责：

Transport：

负责 IO。

Parser：

负责 ANSI。

Render：

负责 GPU。

UI：

负责事件。

互不阻塞。

---

### 13. Data Flow

整个 Terminal Pipeline：

```
Transport

↓

Byte Queue

↓

libvterm

↓

VT Adapter

↓

ScreenBuffer

↓

DirtyRegion

↓

Render Scheduler

↓

Renderer

↓

GPU

↓

TerminalView
```

核心原则：

Parser：

永远不 Render。

Renderer：

永远不解析 ANSI。

UI：

永远不操作 libvterm。

---

### 14. Future Extensions

未来可直接扩展：

- AI Assistant
- Session Recording
- Replay
- Macro
- Lua Plugin
- Python Plugin
- SFTP
- File Browser
- Terminal Split
- Workspace
- Cloud Sync

Terminal Core 无需修改。

---

### 15. Final Architecture

最终形成：

```
Transport
      │
      ▼
Parser (libvterm)
      │
      ▼
VT Adapter
      │
      ▼
ScreenBuffer
      │
      ▼
RenderCommand（可选）
      │
      ▼
Renderer
(OpenGL / QRhi / Vulkan)
      │
      ▼
TerminalView
(QML / QWidget)
      │
      ▼
Modern UI
(FluentUI)
```

---

### 16. Core Design Philosophy

NovaTerm 的核心思想：

- Parser 与 Renderer 解耦
- Renderer 与 UI 解耦
- UI 与 Session 解耦
- ScreenBuffer 是唯一的数据中心
- GPU Renderer 只负责绘制
- Parser 只负责协议解析
- 所有模块均可独立替换

最终实现：

**高性能、低耦合、可维护、可扩展的现代 GPU Terminal 架构。**
