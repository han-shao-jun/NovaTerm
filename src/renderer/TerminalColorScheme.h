#pragma once
#include <QColor>
#include <QString>

// 终端配色方案，独立于 qtermwidget 的 .colorscheme 格式。
// 初始硬编码两套配色，后续可从 JSON 文件加载。
struct TerminalColorScheme {
    QString name;

    QColor foreground;
    QColor background;

    // ANSI 标准 16 色调色板: 0=Black, 1=Red, 2=Green, 3=Yellow,
    // 4=Blue, 5=Magenta, 6=Cyan, 7=White, 8-15=亮色变体
    QColor palette[16];

    QColor cursorColor;
    QColor selectionColor;

    // ── 预置配色方案 ──

    // 深色主题（对应原 DarkPastels.colorscheme）
    static TerminalColorScheme darkPastels();

    // Windows Terminal 的默认 Campbell 配色。
    static TerminalColorScheme windowsTerminalCampbell();

    // 浅色主题（对应原 BlackOnWhite.colorscheme）
    static TerminalColorScheme blackOnWhite();

    // 根据明暗选择默认方案
    static TerminalColorScheme defaultDark()  { return windowsTerminalCampbell(); }
    static TerminalColorScheme defaultLight() { return blackOnWhite(); }
};
