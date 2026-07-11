# Terminal Core Layer

跨平台终端核心，不依赖 Qt GUI，可独立测试。

计划模块：
- **VT Parser** — libvterm 集成，ANSI/VT100/CSI/OSC 状态机解析
- **Screen Buffer** — Chunk + Ring + COW，支持百万行 Scrollback
- **Input Engine** — 键盘/鼠标输入处理，Escape 序列生成
- **Selection & Search** — 文本选择与正则搜索
- **Core Utils** — 编码转换（UTF-8）、日志、内存池

当前状态：终端解析与渲染暂由 qtermwidget 第三方库提供。
