# NovaTerm 开发指南

> 基于 ChatGPT 对话中生成的架构设计文档，整合三张架构图的核心内容。

**参考项目：** WindTerm / WezTerm / Windows Terminal / Konsole / PuTTY / Contour / libvterm

---

## 目录

1. [整体架构](#1-整体架构)
2. [分层设计](#2-分层设计)
3. [数据流与线程模型](#3-数据流与线程模型)
4. [性能优化](#4-性能优化)
5. [技术栈](#5-技术栈)
6. [项目结构](#6-项目结构)
7. [开发路线图](#7-开发路线图)
8. [设计原则](#8-设计原则)
9. [性能目标](#9-性能目标)
10. [未来扩展方向](#10-未来扩展方向)

---

## 1. 整体架构

NovaTerm 采用分层架构设计，从上到下分为五层：

```
┌─────────────────────────────────────────────────────────────┐
│                      UI Layer (Qt 6.x)                       │
├─────────────────────────────────────────────────────────────┤
│                     Terminal Core Layer                      │
├─────────────────────────────────────────────────────────────┤
│                     Renderer Layer (GPU)                     │
├─────────────────────────────────────────────────────────────┤
│                     Transport Layer                          │
├─────────────────────────────────────────────────────────────┤
│                    System / OS Layer                         │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 分层设计

### 2.1 UI Layer（用户界面层）

基于 **Qt 6.x Widgets + QRhi** 构建，主要模块包括：

| 模块 | 说明 |
|------|------|
| **MainWindow** | 主窗口框架，统筹所有 UI 组件 |
| **Menu / Toolbar** | 菜单栏与工具栏 |
| **Session Tabs** | 多标签页管理，支持拖拽、分屏 |
| **StatusBar** | 状态栏，显示连接信息、光标位置等 |
| **Command Palette** | 命令面板（类似 VS Code），快速执行操作 |
| **Dock Widgets** | 可停靠工具面板 |
| **Theme System** | 主题系统，支持自定义配色 |
| **扩展系统** | Lua / JavaScript 脚本支持 |

### 2.2 Terminal Core Layer（终端核心层）

跨平台核心，不依赖 Qt GUI，可独立测试：

| 模块 | 说明 |
|------|------|
| **Input Engine** | 键盘/鼠标输入处理，Escape 序列生成 |
| **VT Parser** | ANSI/VT100/CSI/OSC/DCS/ESC 状态机解析器 |
| **Command Processor** | 命令队列处理，执行终端操作命令 |
| **Screen Buffer** | 屏幕缓冲区，支持滚动回看（Scrollback） |
| **Selection & Search** | 文本选择与搜索功能 |
| **Core Utils** | 编码转换（UTF-8/UTF-16）、日志、内存池、定时器等 |

**推荐解析方案（按优先级）：**

| 优先级 | 方案 | 说明 |
|--------|------|------|
| 1 | **libvterm** | 成熟稳定，广泛验证 |
| 2 | **Contour::vtparser + vtbackend** | 现代 C++20 实现 |
| 3 | **Konsole 解析器** | 源码参考 |
| 4 | **qtermwidget** | 可用但耦合重，不推荐直接使用 |

**libvterm 关键回调：**

- `damage(rect)` → 脏区域更新
- `moverect(src, dst)` → 滚动操作
- `settermprop(prop)` → 终端属性变化
- `bell()` → 铃声/视觉提示
- `resize()` → 终端尺寸变化
- `sb_pushline()` → 行推入回滚缓冲区
- `sb_popline()` → 行从回滚缓冲区恢复

### 2.3 Renderer Layer（GPU 渲染层）

基于 GPU 的高性能文本渲染管线：

```
Render Command Generator → Glyph System → GPU Renderer → Render Pipeline → Swapchain / Surface
```

| 模块 | 说明 |
|------|------|
| **Render Command Generator** | 将 Screen Buffer 脏区域转换为渲染命令 |
| **Glyph System** | 字形系统：Font Manager (FreeType)、Glyph Cache、Glyph Atlas |
| **GPU Renderer** | OpenGL / Vulkan / QRhi 后端 |
| **Render Pipeline** | 渲染管线：批处理 (Batching)、实例化 (Instancing)、图集合并 |
| **Swapchain / Surface** | 交换链与表面管理，支持 VSync |

**Glyph 缓存系统：**
- LRU 缓存策略，避免重复 Rasterize
- Instanced Draw：一次 Draw Call 渲染多个字形
- Glyph Atlas 纹理图集管理
- Fallback Font 回退字体链

### 2.4 Transport Layer（传输层）

抽象传输接口，统一不同协议的数据收发：

| 模块 | 说明 |
|------|------|
| **SerialTransport** | 串口连接（RS-232/485） |
| **SSHTransport** | SSH 远程连接（libssh2） |
| **TelnetTransport** | Telnet 协议 |
| **LocalShellTransport** | 本地 PTY/ConPTY |
| **CustomTransport** | 自定义传输扩展 |

### 2.5 System / OS Layer（系统层）

| 类别 | 说明 |
|------|------|
| **PTY** | WinAPI / POSIX PTY / ConPTY |
| **网络** | Socket API |
| **文件** | File API |
| **串口** | Serial Port |
| **剪切板** | Clipboard |
| **GPU** | OpenGL / Vulkan / QRhi |
| **字体** | FreeType |

---

## 3. 数据流与线程模型

### 3.1 核心数据流

```
用户输入 / 远程数据
        │
        ▼
    Transport ──→ libvterm (Parser + Backend)
                        │
                        ▼
              Screen Buffer (Dirty Rect)
                        │
                        ▼
               Render Command
                        │
                        ▼
                  GPU Renderer
                        │
                        ▼
                   显示屏
```

### 3.2 七线程流水线（性能优化版）

```
线程1: I/O 线程        ─→  异步批量读写
线程2: RingBuffer      ─→  无锁 SPSC 环形缓冲
线程3: 解析线程         ─→  libvterm / Contour Parser
线程4: 更新命令队列     ─→  无锁 MPSC 队列
线程5: Screen Buffer   ─→  Chunk + Ring + COW
线程6: 渲染命令生成     ─→  批量生成 + DirtyRect 合并
线程7: 渲染线程         ─→  GPU Instancing + Batching
```

**线程间通信：** 各线程通过无锁队列通信，避免锁竞争。

---

## 4. 性能优化

### 4.1 关键瓶颈分析与解决方案

| 瓶颈点 | 问题描述 | 解决方案 |
|--------|----------|----------|
| **I/O 瓶颈** | 小块读取导致系统调用频繁 | 异步 I/O + 批量读取（8KB~64KB Buffer） |
| **解析瓶颈** | 大量 CSI/OSC 转义序列解析开销 | 批量解析 + 分支预测 + SIMD 优化 |
| **更新队列瓶颈** | 多线程锁竞争 | 无锁队列 (SPSC/MPSC) + 批量提交 |
| **Screen Buffer 瓶颈** | 字符串频繁存储/复制 | Chunk 分块 + Copy-on-Write + 共享内存 |
| **渲染命令生成瓶颈** | 单字符逐条生成开销大 | DirtyRect 合并 + 批量生成 |
| **GPU 瓶颈** | DrawCall 过多、状态切换频繁 | Instancing + Batching + 纹理图集 |

### 4.2 Nova Screen Buffer 设计

- **固定大小 Chunk**：每 Chunk 4096 行
- **双向链表**：Chunk 间链表连接，支持快速导航
- **Ring 缓冲区**：环形管理光标位置
- **稀疏分配**：只分配可见区域附近内存
- **Copy-on-Write**：减少复制开销
- **Dirty Rect 追踪**：增量更新策略

### 4.3 内存管理策略

| 策略 | 说明 |
|------|------|
| **对象池 (Object Pool)** | 预分配常用对象，避免频繁 new/malloc |
| **Arena 分配器** | 大块内存一次分配，小对象从 Arena 切分 |
| **Chunk / Command 分配** | 针对特定数据结构的专用分配器 |
| **COW (Copy-on-Write)** | 共享数据直到修改时才复制 |
| **预分配** | 减少运行时分配开销 |

### 4.4 GPU 渲染优化

- **Instanced Draw**：一次 Draw Call 渲染多个字形实例
- **Batching**：合批渲染，减少状态切换
- **Texture Atlas**：字形纹理图集，减少纹理绑定切换
- **Dirty Rect 合并**：多个脏区域合并为一次更新
- **Shader 管理**：Fragment Shader 优化

### 4.5 其他潜在瓶颈

| 瓶颈 | 解决方案 |
|------|----------|
| **Glyph Rasterize** | 缓存 + 后台预渲染 |
| **字体回退** | 回退字体链预加载 |
| **VSync 同步** | 自适应 VSync / 三重缓冲 |

### 4.6 监控指标

实时监控以下指标：
- FPS（帧率）
- 输入/输出吞吐量
- 内存占用
- 各线程 CPU 占用
- 渲染延迟

---

## 5. 技术栈

| 类别 | 技术选型 | 说明 |
|------|----------|------|
| **语言** | C++20 | 现代 C++ |
| **UI 框架** | Qt 6.x (Widgets + QRhi) | 跨平台 GUI |
| **GPU 渲染** | OpenGL 4.5 / Vulkan / QRhi | 主力 OpenGL，可选 Vulkan |
| **终端解析** | libvterm / Contour | 成熟稳定的 VT 解析库 |
| **字体渲染** | FreeType 2 | 字形光栅化 |
| **SSH** | libssh2 | SSH 协议支持 |
| **JSON** | nlohmann::json | 配置序列化 |
| **日志** | spdlog | 高性能日志库 |
| **格式化** | fmt | 现代字符串格式化 |
| **压缩** | xxHash | 高速哈希 |
| **加密** | OpenSSL | 加密通信 |
| **ABI** | abseil-cpp | Google 基础库 |
| **构建** | CMake | 跨平台构建系统 |

---

## 6. 项目结构

```
NovaTerm/
├── core/           # 终端核心（libvterm 集成、Screen Buffer、日志）
│   ├── terminal.cpp
│   ├── screen.cpp
│   ├── parser.cpp
│   └── ...
├── renderer/       # 渲染层（OpenGL / Vulkan / QRhi）
│   ├── renderer.cpp
│   ├── glyph_cache.cpp
│   ├── shaders/
│   └── ...
├── transport/      # 传输层（Serial / SSH / Telnet / PTY）
│   ├── transport.cpp
│   ├── ssh_transport.cpp
│   ├── serial_transport.cpp
│   └── ...
├── ui/             # UI 层（Qt Widgets）
│   ├── main_window.cpp
│   ├── terminal_widget.cpp
│   ├── session_tabs.cpp
│   └── ...
├── plugins/        # 插件系统
│   └── ...
├── tests/          # 单元测试 / 集成测试
│   └── ...
└── thirdparty/     # 第三方库
    ├── libvterm/
    ├── freetype/
    └── ...
```

---

## 7. 开发路线图

### 第一阶段：最小可用版本
- 基础终端窗口 + OpenGL Widget 渲染
- Serial 传输 + libvterm 集成
- 基本 ANSI/VT100 支持

### 第二阶段：核心功能
- Glyph Atlas 字形图集系统
- 多标签页 / 会话管理
- 日志系统 / 配置持久化（JSON）
- 查找与搜索功能

### 第三阶段：高级功能
- SSH 远程连接（libssh2）
- 分屏 / 分标签
- 主题系统
- 自定义协议扩展
- Telnet 支持

### 第四阶段：性能优化
- Vulkan / QRhi 渲染后端
- GPU Instancing + Batching
- 百万行 Scrollback 支持
- 性能剖析与调优
- 跨平台适配（Windows / Linux / macOS）

### 第五阶段：扩展生态
- Lua / JavaScript 脚本支持
- 远程文件管理 / SFTP 浏览器
- 会话云同步
- 插件市场

---

## 8. 设计原则

1. **分层隔离**：UI / Core / Renderer / Transport 严格分离
2. **事件驱动**：所有 I/O 基于事件与回调机制
3. **高性能**：内存池 + GPU 实例化绘制
4. **可扩展**：插件系统 + 抽象 Transport 接口
5. **可测试**：Core 层不依赖 Qt GUI，可独立单元测试
6. **接口隔离**：Transport / Renderer 均使用抽象接口
7. **单一职责**：每个模块只关注自己的功能
8. **数据流向清晰**：Transport → Core → Renderer

---

## 9. 性能目标

| 指标 | 目标值 |
|------|--------|
| **持续吞吐量** | > 20 MB/s（极限场景） |
| **输入延迟** | < 10ms（端到端） |
| **帧率** | 稳定 60 FPS（支持高刷新率 120Hz / 144Hz / 240Hz） |
| **Scrollback** | 默认 100 万行，可配置 |
| **内存占用** | < 512MB（100 万行 Scrollback） |
| **CPU 占用** | 低 CPU 占用（增量渲染 + 批量绘制） |
| **分辨率** | 支持 4K / 8K |

---

## 10. 未来扩展方向

- **多后端渲染**：Vulkan / Metal / Direct3D 多后端支持
- **分屏/广播输入**：多终端分屏 + 同步输入广播
- **脚本扩展**：Lua / JavaScript / WASM 插件系统
- **远程管理**：远程文件管理 / SFTP 浏览器
- **会话同步**：跨设备云同步会话配置
- **AI 辅助**：终端 AI 命令建议与补全

---

## 图例

| 符号 | 含义 |
|------|------|
| 实线 → | 数据流 |
| 虚线 ⇢ | 控制流（事件/命令） |
| 不同颜色线程 | 无锁通信 |

---

*文档生成时间：2026-07-09 | 来源：ChatGPT 对话架构设计图片 OCR 提取*
