# NovaTerm QRhi Renderer Architecture

## 1. 目标

设计高性能 GPU 终端字符渲染架构，实现：

-   百万级字符输出
-   60FPS滚动
-   GPU字体缓存
-   ANSI颜色
-   TrueColor
-   高DPI支持

# 2. 总体架构

    Terminal Emulator Core

            |

         Screen Buffer

            |

         Render Model

            |

     QRhiTerminalRenderer

            |

           QRhi

            |

     GPU Pipeline

            |

     Display

# 3. 模块划分

    TerminalRenderer

     |
     +-- GlyphManager

     |
     +-- FontAtlas

     |
     +-- ColorTable

     |
     +-- CellBuffer

     |
     +-- QRhiPipeline

# 4. Cell模型

每个字符单元：

``` cpp
struct Cell
{

uint32_t unicode;

uint8_t fg;

uint8_t bg;

uint16_t attributes;

};
```

# 5. Glyph缓存

目标：

避免每个字符重新生成纹理。

流程：

    Unicode

     |

    FontEngine

     |

    Glyph Bitmap

     |

    FontAtlas Texture

     |

    GPU Sampling

# 6. ColorTable

禁止Renderer硬编码颜色。

``` cpp
class TerminalColorTable
{

QColor ansi[16];

QColor foreground;

QColor background;

QColor cursor;

};
```

# 7. QRhi Pipeline

    Vertex Buffer

          |

    Vertex Shader

          |

    Texture Atlas

          |

    Fragment Shader

          |

    ColorTable

          |

    Framebuffer

# 8. 滚动优化

不要移动所有字符。

采用：

    Virtual Scroll Offset

            +

    Dirty Region Update

            +

    GPU Instance Rendering

# 9. 输入输出流程

输入：

    Keyboard

     |

    Qt Event

     |

    TerminalSession

     |

    PTY

输出：

    PTY

     |

    libvterm

     |

    Screen Buffer

     |

    QRhiRenderer

     |

    GPU

# 10. 开发阶段

## Phase 1

基础字符绘制：

-   QRhi初始化
-   FontAtlas
-   单字符渲染

## Phase 2

终端能力：

-   ANSI颜色
-   光标
-   选择

## Phase 3

性能优化：

-   Instance Rendering
-   Dirty Region
-   百万字符测试

## Phase 4

高级能力：

-   Ligature
-   GPU动画
-   平滑滚动

# 11. Agent开发约束

1.  不允许QPainter绘制终端字符。

2.  Renderer不能依赖SSH实现。

3.  所有颜色来自ColorTable。

4.  字符必须走GPU Pipeline。

5.  Screen Buffer和Render Buffer分离。
