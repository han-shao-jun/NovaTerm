# NovaTerm Architecture Design

> Version: 1.0
>
> Author: NovaTerm Project
>
> Last Update: 2026-07-12

---

# 1. Project Vision

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
- 插件化架构
- 支持 Qt Widgets 和 QML 两套 UI
- 后续支持 AI Assistant、SFTP、文件管理等功能

---

# 2. Design Principles

整个项目遵循以下原则：

## 高内聚

每个模块只负责一种职责。

例如：

- Parser 负责解析 ANSI
- Renderer 负责 GPU 绘制
- Session 负责网络通信
- UI 负责用户交互

彼此互不关心实现细节。

---

## 低耦合

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

## 数据驱动

所有数据最终汇聚到：

```
ScreenBuffer
```

Renderer 只读取数据。

Parser 只修改数据。

---

# 3. Overall Architecture

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

# 4. Layer Responsibilities

## UI Layer

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

## Terminal View

TerminalView 只是 GPU Renderer 的容器。

可提供两种实现：

- QWidget(QOpenGLWidget)
- QQuickItem(QML)

两者共享同一个 Renderer。

---

## Renderer

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

## Render Scheduler

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

## Glyph Atlas

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

# 5. ScreenBuffer（核心）

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

# 6. Scrollback

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

# 7. Dirty Region

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

# 8. Selection Model

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

# 9. Search Engine

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

# 10. VT Adapter

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

# 11. Session Layer

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

# 12. Thread Model

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

# 13. Data Flow

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

# 14. Future Extensions

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

# 15. Final Architecture

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

# 16. Core Design Philosophy

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