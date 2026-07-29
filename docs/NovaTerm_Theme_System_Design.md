# NovaTerm Theme System 架构设计方案

## 1. 目标

为 NovaTerm 增加类似 Windows Terminal / VS Code 的主题系统，实现：

-   UI 外观主题管理
-   Terminal ANSI 配色方案管理
-   字体配置管理
-   Profile 独立配置
-   JSON 配置文件
-   运行时动态切换主题
-   支持用户自定义主题
-   支持主题导入/导出

设计原则：

UI Theme、Terminal Scheme、Font Config 三者完全解耦。

------------------------------------------------------------------------

# 2. 总体架构

    NovaTerm Application
            |
            |
     ThemeManager
            |
     +------+------+----------------+
     |             |                |
     v             v                v

    UIThemeManager  TerminalSchemeManager  FontManager

     |             |                |
     v             v                v

    ui_theme.json  terminal_scheme.json  font.json

                    |
                    v

              ProfileManager

                    |
                    v

              TerminalSession

                    |
                    v

            QRhiTerminalRenderer

                    |
         +----------+----------+
         |          |          |
         v          v          v

     GlyphCache ColorTable FontAtlas

------------------------------------------------------------------------

# 3. 配置目录设计

    NovaTerm
    |
    ├── config
    |
    ├── settings.json
    |
    ├── profiles.json
    |
    ├── themes
    │   ├── dark.json
    │   ├── light.json
    │   └── fluent_dark.json
    |
    ├── schemes
    │   ├── campbell.json
    │   ├── dracula.json
    │   ├── nord.json
    │   └── solarized.json
    |
    └── fonts
        ├── default.json
        └── coding.json

------------------------------------------------------------------------

# 4. UI Theme设计

负责 NovaTerm 界面颜色。

示例：

``` json
{
    "name":"Dark",

    "window":{
        "background":"#202020",
        "border":"#404040"
    },

    "toolbar":{
        "background":"#252526",
        "foreground":"#FFFFFF"
    },

    "tab":{
        "active":"#1E1E1E",
        "inactive":"#2D2D30"
    }
}
```

------------------------------------------------------------------------

# 5. Terminal Scheme设计

负责终端字符颜色。

示例：

``` json
{
"name":"Dracula",

"foreground":"#F8F8F2",
"background":"#282A36",

"cursor":"#F8F8F0",
"selection":"#44475A",

"ansi":[
"#000000",
"#FF5555",
"#50FA7B",
"#F1FA8C",
"#BD93F9",
"#FF79C6",
"#8BE9FD",
"#BFBFBF"
]
}
```

------------------------------------------------------------------------

# 6. Profile设计

Profile绑定：

-   Shell
-   Font
-   Theme
-   Scheme

``` json
{
"profiles":[

{
"name":"PowerShell",
"shell":"powershell.exe",
"theme":"dark",
"scheme":"campbell",
"font":"Cascadia Mono",
"fontSize":12
}

]
}
```

------------------------------------------------------------------------

# 7. 核心类设计

## ThemeManager

``` cpp
class ThemeManager
{
public:

bool loadTheme(QString name);

bool loadScheme(QString name);

void applyTheme();

signals:

void themeChanged();

};
```

------------------------------------------------------------------------

## TerminalColorTable

Renderer 使用：

``` cpp
class TerminalColorTable
{
public:

QColor foreground;
QColor background;

QColor cursor;
QColor selection;

QColor ansi[16];

};
```

------------------------------------------------------------------------

# 8. Renderer修改方案

禁止 Renderer 内部保存固定 QColor。

修改：

    TerminalRenderer

            |
            v

    TerminalColorTable

            |
            v

    ThemeManager

Renderer 只负责：

-   字符布局
-   Glyph绘制
-   GPU资源管理
-   Shader渲染

颜色由 ThemeManager 提供。

------------------------------------------------------------------------

# 9. 动态切换流程

用户选择：

    Settings
     |
    Theme
     |
    Dracula

流程：

    ThemeManager

        |
        v

    加载 JSON

        |
        v

    更新 TerminalColorTable

        |
        v

    QRhiRenderer::setColorTable()

        |
        v

    requestUpdate()

        |
        v

    GPU重新绘制

无需：

-   重启程序
-   重建Session
-   重建GPU资源

------------------------------------------------------------------------

# 10. QRhi Renderer适配

增加：

``` cpp
class TerminalRenderContext
{
public:

TerminalColorTable colors;

QRhiTexture *fontTexture;

QRhiBuffer *vertexBuffer;

};
```

渲染流程：

    Terminal Cell

          |
          v

    字符属性

          |
          v

    ANSI Color Index

          |
          v

    ColorTable

          |
          v

    GPU Shader

          |
          v

    显示

------------------------------------------------------------------------

# 11. ANSI颜色处理

libvterm输出：

    SGR 31

转换：

    ANSI RED

        |

    TerminalColorTable

        |

    QColor

        |

    QRhi Shader

------------------------------------------------------------------------

# 12. 开发阶段规划

## Phase 1 基础主题支持

实现：

-   ThemeManager
-   JSON加载
-   ColorTable
-   Renderer接入

目标：

黑白主题切换。

------------------------------------------------------------------------

## Phase 2 Terminal Scheme

增加：

-   ANSI16色
-   Cursor
-   Selection

支持：

-   Dracula
-   Nord
-   Solarized

------------------------------------------------------------------------

## Phase 3 Profile系统

增加：

-   Shell配置
-   Font配置
-   Scheme绑定

类似 Windows Terminal Profile。

------------------------------------------------------------------------

## Phase 4 UI Theme

增加：

-   Tab颜色
-   Toolbar
-   Sidebar
-   Settings界面

------------------------------------------------------------------------

## Phase 5 高级能力

支持：

-   主题导入
-   主题导出
-   VS Code主题转换
-   Windows Terminal主题转换

------------------------------------------------------------------------

# 13. Agent开发任务拆分

## Task 1

实现：

    ThemeManager

要求：

-   Qt6
-   C++17
-   QJsonDocument

------------------------------------------------------------------------

## Task 2

实现：

    TerminalColorTable

要求：

替换 Renderer 内硬编码颜色。

------------------------------------------------------------------------

## Task 3

修改：

    QRhiTerminalRenderer

要求：

支持运行时颜色刷新。

------------------------------------------------------------------------

## Task 4

增加：

    settings.json
    profiles.json

------------------------------------------------------------------------

## Task 5

增加设置界面：

    Settings

     └── Appearance

           ├── Theme

           ├── Color Scheme

           └── Font

------------------------------------------------------------------------

# 14. 设计约束

Agent必须遵守：

1.  Renderer禁止读取JSON。

2.  禁止代码中散落QColor。

3.  所有终端颜色必须来自：

```{=html}
<!-- -->
```
    TerminalColorTable

4.  UI Theme和Terminal Scheme必须分离。

5.  Theme切换不能影响TerminalSession。

6.  Renderer必须支持运行时刷新。

------------------------------------------------------------------------

# 15. 最终目标架构

                     NovaTerm

                         |
                  ThemeManager

                         |

          +--------------+--------------+

          |              |              |

     UI Theme    Terminal Scheme    Font


          |              |              |

     QWidget       QRhiRenderer     FontAtlas


                         |

                  GPU Accelerated


                         |

                 Terminal Display
