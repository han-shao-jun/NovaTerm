# 切换主题色崩溃 — 滚动条悬垂指针修复

## 问题

在以下操作序列后，程序 **100% 崩溃**：

1. 点击「会话」菜单，打开会话对话框
2. 在「本地终端」页点击「确定」（新建一个本地终端，对话框关闭）
3. 点击标题栏的主题切换按钮（太阳/月亮图标）切换主题色

崩溃是确定性的（必现），仅在**会话对话框关闭之后**再切主题时触发。单纯切主题、或不经对话框直接新建终端，都不会崩。

---

## 定位手段

通过 `/var/coredump/` 下的 core dump + gdb 还原主线程调用栈，得到精确的崩溃点（关键帧）：

```
Program terminated with signal SIGBUS, Bus error.

#0  QWidgetPrivate::update<QRect>()                         ← 崩溃点
#1  QWidget::update()
#2  ElaScrollBarStyle.cpp:21  lambda(themeMode)             ← _pScrollBar->update()
#3  ...
#7  ElaTheme::themeModeChanged (emit)
#8  ElaTheme::setThemeMode                ElaTheme.cpp:24
#9  ElaWindowPrivate::onThemeReadyChange  ElaWindowPrivate.cpp:123
#14 ElaAppBar::themeChangeButtonClicked                     ← 点击标题栏主题按钮
#19 QAbstractButton::clicked
#42 main → QApplication::exec()
```

两个关键信号：

- **信号是 `SIGBUS`（总线错误），不是 `SIGSEGV`** —— 配合 core 里 `Can't open file /memfd:xorg (deleted)` 的提示，说明访问的是一块已被释放/解除映射的对象内存（典型的"对象已死、指针还在"）。
- 崩溃发生在 `QWidget::update()` 里，调用方是 **`ElaScrollBarStyle.cpp:21`** 那个响应 `themeModeChanged` 的 lambda，它对 `_pScrollBar` 调了 `update()`。

---

## 根因分析

问题出在 vendored 库 **ElaWidgetTools** 的 `ElaScrollBarStyle`，**不是应用层代码**。

`third_party/ElaWidgetTools/ElaWidgetTools/DeveloperComponents/ElaScrollBarStyle.cpp:18-23`：

```cpp
connect(eTheme, &ElaTheme::themeModeChanged, this, [=](ElaThemeType::ThemeMode themeMode) {
    _themeMode = themeMode;
    if (_pScrollBar) {          // _pScrollBar 是裸指针
        _pScrollBar->update();  // 滚动条已销毁时 → 对悬垂指针调用 → SIGBUS
    }
});
```

悬垂指针的形成链条：

1. **`ElaScrollBarStyle` 用裸指针 `_pScrollBar` 持有它所服务的滚动条**（`ElaScrollBar` 构造时通过 `setScrollBar(this)` 回填，见 `ElaScrollBar.cpp:28`）。

2. **该 `themeModeChanged` 连接的 receiver 是 `style` 自己**，而不是滚动条。也就是说，只要 style 还活着，这个 lambda 就会被调用。

3. **滚动条销毁时，`_pScrollBar` 不会被置空。** 当会话对话框（`SessionPage` 继承自 `ElaScrollPage`，内部满是 `ElaScrollBar`）随 `dialog->deleteLater()` 销毁后，对应的滚动条已被释放，但某个仍存活的 `ElaScrollBarStyle` 里的 `_pScrollBar` 变成了**指向已释放内存的悬垂指针**。

4. **下次切主题** → `eTheme` 发出 `themeModeChanged` → lambda 执行 → `if (_pScrollBar)` 判断为真（悬垂指针非空，置空逻辑根本不存在）→ `_pScrollBar->update()` 访问已释放的 `QWidget` 内存 → **SIGBUS 崩溃**。

这解释了为什么必须"经过会话对话框"才会崩：对话框的开闭制造了"滚动条已死、style 仍订阅主题信号"的悬垂状态；而标题栏主题按钮（`ElaWindowPrivate::onThemeReadyChange`）正是触发 `themeModeChanged` 的入口。

> 补充：同一文件中另外两处（`ElaScrollBarStyle.cpp:160`、`:182`）也存在相同的裸指针 `_pScrollBar->update()` 解引用，同样依赖该指针有效。下面的修复一并覆盖了这三处。

---

## 修复方法

在**滚动条销毁时，把 style 里的裸指针置空**，从根上断开悬垂引用。利用 Qt 的 `QObject::destroyed` 信号，在滚动条析构那一刻同步清空 `_pScrollBar`。

`third_party/ElaWidgetTools/ElaWidgetTools/ElaScrollBar.cpp`（构造函数内）：

```cpp
ElaScrollBarStyle* scrollBarStyle = new ElaScrollBarStyle(style());
scrollBarStyle->setScrollBar(this);
// 修复主题切换崩溃：ElaScrollBarStyle 以裸指针 _pScrollBar 持有本滚动条，
// 且其 themeModeChanged 连接以 style 为 receiver（style 可能比滚动条活得久）。
// 当滚动条被销毁（如关闭会话对话框）后再切主题，lambda 会对已释放的
// 滚动条调用 update() → SIGBUS。这里在滚动条析构时把裸指针清空，断开悬垂。
connect(this, &QObject::destroyed, scrollBarStyle, [scrollBarStyle]() {
    scrollBarStyle->setScrollBar(nullptr);
});
setStyle(scrollBarStyle);
```

修复要点：

- `this`（滚动条）析构 → 触发 `destroyed` → 回调把 style 的 `_pScrollBar` 置为 `nullptr`。
- 之后再切主题，lambda 里的 `if (_pScrollBar)` 就能正确拦住（指针已是 `nullptr`），不再解引用悬垂对象。
- receiver 选 `scrollBarStyle`：保证 style 一旦先行销毁，这个 `destroyed` 连接也会被 Qt 自动断开，不会反向悬垂。
- 该修复同时保护了 `ElaScrollBarStyle.cpp` 中全部三处 `_pScrollBar->update()` 解引用。

### 为什么选在 `ElaScrollBar` 构造函数里修

`_pScrollBar` 和它的 setter 由 `Q_PRIVATE_CREATE(ElaScrollBar*, ScrollBar)` 宏生成，且 `ElaScrollBarStyle` 构造时尚不知道自己将服务哪个滚动条。唯一同时能拿到"滚动条实例"和"对应 style 实例"的地方，就是 `ElaScrollBar` 构造函数中 `setScrollBar(this)` 之后——因此把生命周期绑定（`destroyed` → 置空）放在这里最干净、最不侵入宏与库结构。

---

## 验证

- 用 `build/` 配置重新构建后，按"会话 → 本地终端 → 确定 → 反复切主题"的必崩序列执行，**不再崩溃，不再产生新的 core dump**。
- core dump 的崩溃帧（`ElaScrollBarStyle.cpp:21` 对 `_pScrollBar` 的解引用）正是本修复所针对的代码路径。

---

## 影响范围与备注

- 修复位于 **vendored 第三方库 ElaWidgetTools** 内（`ElaScrollBar.cpp`）。若后续升级/重新拉取该库，需要重新应用本补丁，或向上游反馈。
- 本修复只增加一条生命周期绑定连接，不改变滚动条/样式的任何既有行为，零功能副作用。
- 该 bug 本质是"长生命周期对象用裸指针持有短生命周期 QObject 且未跟踪其销毁"的经典悬垂指针问题；同类隐患可统一用 `QPointer` 或 `destroyed` 信号绑定来规避。
