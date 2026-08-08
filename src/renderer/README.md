# Renderer Layer

基于 GPU 的高性能文本渲染管线。

计划模块：
- **Render Command Generator** — Screen Buffer 脏区域 → 渲染命令
- **Glyph System** — Font Manager (FreeType)、Glyph Cache、Glyph Atlas
- **GPU Renderer** — OpenGL / Vulkan / QRhi 后端
- **Render Pipeline** — 批处理、实例化、图集合并
