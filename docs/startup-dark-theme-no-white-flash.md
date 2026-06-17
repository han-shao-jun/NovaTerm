# 深色主题启动无白屏闪烁 — 技术方案

## 问题

NovaTerm 支持"跟随系统"主题。当操作系统处于深色模式、配置为 `auto` 时，程序启动会出现约 100-200ms 的白色背景闪烁，然后才变为深色界面。本问题在 Windows / Linux / macOS 上均可能出现，根因相同但解决方案因平台而异。

---

## 根因分析

白色闪烁由 **三层叠加** 导致，每一层都需要独立处理。以下以 Windows DWM 为例剖析，但核心逻辑跨平台适用：

```
层 1: ElaWidgetTools 初始化时序
  eApp->init() 先于配置读取 → ElaApplication 构造时 eTheme 仍是默认 Light
  → ElaWindow 内部的 _themeMode 字段为 Light
  → paintEvent 用浅色填充（虽然后面立即改了，但已经晚了一步）

层 2: Qt 样式表透明度
  ElaWindow 构造函数中执行:
    setStyleSheet("#ElaWindow{background-color:transparent;}")
  → Qt 在处理背景擦除时不填充颜色
  → 原生窗口系统的默认背景色（白色）透出

层 3: 合成器时序 (最核心)
  窗口创建/显示 → 合成器为窗口分配 Surface（默认白色）
                → 窗口在屏幕上可见
  背景擦除 → 被拦截填充深色（但窗口已显示了白色首帧）
  首次绘制 → paintEvent 绘制深色背景
  合成器合成下一帧 → 用户看到深色

  白帧出现在 Surface 分配与首次渲染之间的帧间隔。
  无论应用层多快，只要「先显示再渲染」，就必然有至少一帧是白色的。
```

**诊断验证方法**：将背景擦除的填充色从 `#202020` 临时改为 `#FF0000`（红色）。若闪烁变为红色 → 证明背景擦除生效但晚于合成器一帧；若仍为白色 → 证明背景擦除未被调用。

---

## 解决方案：分层防御

### 第一层：提前确定主题（Application.cpp）★ 跨平台

将配置加载和主题设置移到 `eApp->init()` **之前**：

```
修改前: eApp->init() → loadConfig() → setTheme() → createWindow
修改后: loadConfig() → setTheme() → eApp->init() → createWindow
```

确保 `ElaApplication` 构造函数调用 `eTheme->getThemeMode()` 时已返回正确的 Dark。

**Linux 适配**：无需修改，直接可用。`ConfigManager::load()` 和 `eTheme->setThemeMode()` 均为跨平台 API。

---

### 第二层：设置 QPalette（Application.cpp）★ 跨平台

在深色主题下，将 `QApplication` 的调色板强制设为深色：

```cpp
darkPal.setColor(QPalette::Window, QColor(0x20, 0x20, 0x20));  // WindowBase
darkPal.setColor(QPalette::Base,   QColor(0x34, 0x34, 0x34));  // BasicBase
```

即使 ElaWindow 的样式表设了 `transparent`，Qt 内部处理背景擦除时也会使用正确的颜色。

**Linux 适配**：无需修改，`QPalette` 是跨平台的。

---

### 第三层：拦截原生背景擦除（SettingsPage.cpp）◆ 平台相关

在原生事件过滤器（`QAbstractNativeEventFilter`）中拦截平台的"擦除背景"事件，直接用深色画刷填充窗口客户区，兜底样式表的 `transparent` 背景。

#### Windows 实现（已实现）

```cpp
// 拦截 WM_ERASEBKGND
if (msg->message == WM_ERASEBKGND) {
    if (eTheme->getThemeMode() == ElaThemeType::Dark) {
        static HBRUSH darkBrush = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
        FillRect((HDC)msg->wParam, &clientRect, darkBrush);
        *result = 1;   // 告诉 Windows 背景已擦除
        return true;   // 阻止 Qt 的默认处理
    }
}
```

#### Linux 适配指南

Linux 上没有统一的"背景擦除"消息。背景由 Qt 在绘制流程中处理，不同平台插件的路径不同：

**X11（xcb 插件）**：
- Qt 使用 `QBackingStore` 管理窗口后备缓冲
- 窗口背景由 `QWidgetPrivate::paintBackground()` 在 `paintEvent` 之前调用
- 如果在 `paintBackground` 阶段样式表返回 `transparent`，则依赖 X11 窗口的默认背景（通常白色）
- **适配路径**：在 Linux 分支使用 X11 API 设置窗口背景像素图（`XSetWindowBackgroundPixmap` 设为 `None`，配合 `WA_OpaquePaintEvent` 属性让 Qt 跳过背景擦除）。或者在 ElaWindow 的子类中重写 `paintEvent` 确保在绘制开始前先用深色填充整个 rect。

**Wayland（wayland 插件）**：
- 没有传统的"背景擦除"概念
- 窗口内容是双缓冲的：每个帧作为 `wl_buffer` 提交给合成器
- 合成器在收到首个 buffer 之前可能显示占位背景（通常白色）
- **适配路径**：Wayland 的解决方案见第四层

**KWin 合成器（KDE 默认）**：
- `KWin` 同时支持 X11 和 Wayland，其行为在上述两种路径下分别适用
- KWin 特有的 `_KDE_NET_WM_COLOR_SCHEME` 属性可传递配色方案，但属于可选优化

---

### 第四层：延迟可见性策略（main.cpp）★ 核心 ◆ 平台相关

前三层解决的是「渲染什么颜色」，但无法解决「窗口已经可见了才渲染」的时序问题。
**第四层解决的是「何时让窗口变得可见」。**

核心思想：**先不让合成器显示窗口，等深色内容渲染完毕后再让窗口出现在屏幕上。**

#### Windows 实现（已实现）：DWMWA_CLOAK

```
winId()           → 创建原生 HWND（仍隐藏）
                     ↓
DWMWA_CLOAK=true  → 告诉 DWM: "不要合成此窗口"
                    窗口在 DWM 层面不可见，用户看不到任何内容
                     ↓
show()            → Qt 层面显示窗口
                    DWM 因 Cloak 状态不绘制，用户仍看不到
                     ↓
RedrawWindow()    → 同步完成 WM_ERASEBKGND + WM_PAINT
 +processEvents()   深色内容写入 DWM Surface
                     ↓
DWMWA_CLOAK=false → DWM 恢复合成 → 窗口首次出现在屏幕上
                    首帧即深色，无闪烁
```

Windows 8+ 均支持 `DWMWA_CLOAK`（属性 13）。通过 `GetProcAddress` 动态加载 `DwmSetWindowAttribute`，不硬链接 `dwmapi.lib`。

#### Linux 适配指南

在 Linux 上没有 `DWMWA_CLOAK` 的直接等价物，窗口可见性控制依赖于具体的显示协议：

**X11 方案**：使用窗口映射控制

```
思路：创建窗口但不映射（map），渲染后映射。

Qt 的 show() 在 X11 上会依次调用:
  1. XCreateWindow     — 创建窗口（不可见，未映射）
  2. XMapWindow        — 映射窗口（变得可见）

如果可以在 XCreateWindow 之后、XMapWindow 之前完成渲染，则首帧即深色。

可行路径:
  - 不使用 Qt 的 show()，改用手动 X11 调用:
    1. winId() 强制创建窗口（XCreateWindow）
    2. 强制绘制到窗口后备缓冲
    3. XMapWindow(display, winId) 使窗口可见
  - 或使用 Qt::WA_DontShowOnScreen 延迟映射，手动调用 XMapWindow

注意事项:
  - 需要链接 X11 库（libX11）或在运行时动态加载
  - 多显示器 / HiDPI 场景需要处理 _NET_WM_STRUT 等属性
```

**Wayland 方案**：延迟首帧 buffer 提交

```
思路：创建 xdg_surface 但不立即提交 buffer，深色内容准备好后再提交。

Wayland 协议中:
  1. 客户端创建 wl_surface + xdg_surface
  2. 合成器收到 xdg_surface 角色后等待首个 buffer
  3. 首个 wl_surface.commit() + buffer → 合成器首次显示

Qt 的 show() 会立即创建 surface 并提交首帧 buffer。
要让首帧即深色，需要确保在 show() 之前渲染已完成。

可行路径:
  - Qt 6.7+ 的 Wayland 插件支持 xdg_surface 协议，但没有暴露 buffer 提交时机 API
  - 最务实的方案: 在 show() 后立即调用 processEvents() + 强制 repaint()
    Wayland 合成器在收到第二个 buffer commit 后会原子更新
  - 配合 QPalette 确保首帧 buffer 已经是深色

KWin 特定:
  - KWin 支持 server-side decorations (SSD)，窗口装饰由合成器渲染
  - 窗口装饰（标题栏）的颜色可以通过 xdg_toplevel 配置或遵循系统主题
  - 如果使用 ElaAppBar 自定义标题栏，则不受影响
```

**KDE 通用替代**：设置透明度

```
如果上述方案都不可行，KDE/KWin 上还有一个折中:
  - 窗口创建时设为完全透明（opacity = 0）
  - 渲染完成后过渡到不透明（opacity = 1）
  - KWin 支持 _NET_WM_WINDOW_OPACITY 属性
  - QWidget::setWindowOpacity(0.0) → render → setWindowOpacity(1.0)
  - 缺点: 有一个淡入效果而非瞬间出现（用户可感知时间略长）
```

---

### 第五层：运行时跟随（SettingsPage.cpp）★ 跨平台框架 ◆ 平台检测待补充

系统主题变化时，同步更新 `eTheme->setThemeMode()` 和 `QApplication::setPalette()`，确保运行时切换系统明暗模式也不会出现闪烁。

**Windows**（已实现）：
- 通过 `WM_SETTINGCHANGE` / `ImmersiveColorSet` 监听

**macOS**（框架已有）：
- 监听 `AppleInterfaceStyle` 变化（CFNotification）

**Linux 适配指南**：

当前 `isSystemDarkTheme()` 的 Linux 分支仅调用了 GNOME 的 `gsettings`：
```cpp
proc.start("gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"});
```

这在 KDE 上无效。KDE 需要读取 `kdeglobals`：

```cpp
// KDE 系统配色检测
QSettings kdeGlobals(
    QDir::homePath() + "/.config/kdeglobals",
    QSettings::IniFormat);
QString colorScheme = kdeGlobals.value("General/ColorScheme").toString();
// colorScheme 形如 "BreezeDark" 或 "BreezeLight"
// 深色方案通常以 "Dark" 结尾，或直接检查 ColorScheme 关键词
return colorScheme.contains("Dark", Qt::CaseInsensitive);
```

运行时监听 KDE 主题变化：
- 监听 `~/.config/kdeglobals` 文件的 inotify 变更（`QFileSystemWatcher`）
- 或监听 D-Bus 信号 `org.kde.KGlobalSettings` / `notifyChange`

其他 DE 的检测方法汇总：

| 桌面环境 | 读取方式 | 运行时监听 |
|----------|----------|------------|
| GNOME | `gsettings get org.gnome.desktop.interface color-scheme` | D-Bus `org.gnome.SettingsDaemon.Color` |
| KDE Plasma | `~/.config/kdeglobals` `[General] ColorScheme` | inotify + `QFileSystemWatcher` |
| XFCE | `xfconf-query -c xsettings -p /Net/ThemeName` | `xfconf-query --monitor` |
| Cinnamon | `gsettings get org.cinnamon.desktop.interface gtk-theme` | D-Bus (同 GNOME) |
| Deepin | `gsettings get com.deepin.dde.appearance gtk-theme` | D-Bus |
| Sway/Hyprland | 环境变量 `GTK_THEME` 或检查 `~/.config/gtk-3.0/settings.ini` | 无标准方法 |

---

## 完整执行流程

```
main()
  │
  ├─ QApplication 创建
  │
  ├─ Application::init()
  │   ├─ ConfigManager::load()               // [层1] 读取配置
  │   ├─ eTheme->setThemeMode(Dark)          // [层1] 确定主题
  │   ├─ qApp->setPalette(darkPalette)       // [层2] 设置调色板
  │   ├─ eApp->init()                        // Ela 初始化
  │   ├─ LanguageManager::install()
  │   ├─ create MainWindow()
  │   │   └─ ElaWindow 构造函数:
  │   │       eTheme->getThemeMode() → Dark  // [层1] 读到正确主题
  │   │       setStyleSheet(transparent)
  │   ├─ set WA_NoSystemBackground
  │   └─ installNativeEventFilter            // [层3] 安装 WM_ERASEBKGND 拦截器
  │
  ├─ winId() → 创建 HWND (隐藏)              // [层4]
  ├─ DWMWA_CLOAK = true                      // [层4] 阻止 DWM 合成
  ├─ show() → 窗口"显示"(用户看不到)          // [层4]
  ├─ RedrawWindow(RDW_UPDATENOW)             // [层4] 同步渲染
  │   ├─ WM_ERASEBKGND → 拦截,填充深色        // [层3]
  │   └─ WM_PAINT → paintEvent(WindowBase)   // 绘制深色背景
  ├─ processEvents()
  ├─ DWMWA_CLOAK = false                     // [层4] 恢复合成→首帧深色
  │
  └─ a.exec() → 主事件循环
       │
       └─ [运行时] WM_SETTINGCHANGE           // [层5]
           ├─ isSystemDarkTheme()
           ├─ eTheme->setThemeMode()
           └─ qApp->setPalette()
```

---

## 涉及文件与平台覆盖

| 文件 | 改动 | Windows | Linux |
|------|------|---------|-------|
| `src/main.cpp` | ★ Cloak 策略 | `DWMWA_CLOAK` + `RedrawWindow` | 待实现 (XMapWindow / Wayland buffer defer) |
| `src/app/Application.cpp` | init顺序 + QPalette + WA_NoSystemBackground | ✅ 完成 | ✅ 直接可用 |
| `src/pages/SettingsPage.cpp` | WM_ERASEBKGND 拦截 + 运行时跟随 | ✅ 完成 | 待实现 + KDE 检测待补充 |

---

## Linux/KDE 适配清单

当做 Linux 适配时，按以下 checklist 逐项处理：

- [ ] **系统主题检测** — `isSystemDarkTheme()` 增加 KDE 路径（读 `kdeglobals`），GNOME/其他 DE 补充 gsettings 回退
- [ ] **运行时主题监听** — X11: `QFileSystemWatcher` 监控 `kdeglobals`；Wayland: 监听 `org.freedesktop.portal.Settings` D-Bus 信号
- [ ] **第三层背景擦除** — X11: `XSetWindowBackgroundPixmap(None)` + `WA_OpaquePaintEvent`；Wayland: Qt paintEvent 首行先 fillRect 墨色
- [ ] **第四层延迟可见** — X11: 手动 `XMapWindow` 替换 Qt `show()`；Wayland: 探究 `QWaylandWindow` 的 buffer 提交时机或降级使用 opacity 过渡
- [ ] **测试环境** — 在 KDE Plasma (X11)、KDE Plasma (Wayland)、GNOME (Wayland) 三种环境各验证一次

---

## 核心设计原则

> 传统思路是「尽快渲染，抢在用户看到之前完成」。
> 正确的思路是「先不让用户看，渲染好了再放行」。
>
> 前者是在和时间赛跑（永远可能输掉一帧），后者是设置路障（100% 杜绝）。
>
> 这个原则跨平台适用——Windows 上叫做 DWMWA_CLOAK，X11 上叫做延迟 Map，
> Wayland 上叫做延迟首帧 buffer commit。机制不同但思想一致。

## 兼容性

- `DWMWA_CLOAK`（属性 13）自 Windows 8 起可用
- 所有 Windows 专属代码由 `#ifdef Q_OS_WIN` 保护，不影响 Linux/macOS 编译
- Linux 平台需单独实现等价机制（见上文"Linux 适配指南"各节）
