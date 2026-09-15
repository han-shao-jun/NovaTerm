/**
 * @file   TerminalColorScheme.h
 * @brief  终端配色方案。
 *
 * 定义前景/背景、ANSI 16 色调色板、光标与选区颜色。独立于
 * UI 主题与配置格式。服务层 TerminalSchemeStore 负责命名预置、JSON
 * 解析和配置迁移；渲染器仅消费本结构的值。
 */
#pragma once
#include <QColor>
#include <QString>

// 终端配色方案。不可变值类型，供渲染层查表得到 Cell 实际显示色。
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

    /**
     * @brief 深色主题（对应原 DarkPastels.colorscheme）。
     */
    static TerminalColorScheme darkPastels();

    /**
     * @brief Windows Terminal 的默认 Campbell 配色。
     * @return 包含完整 ANSI 16 色调色板的配色方案对象。
     */
    static TerminalColorScheme windowsTerminalCampbell();

    /**
     * @brief 浅色主题（对应原 BlackOnWhite.colorscheme）。
     */
    static TerminalColorScheme blackOnWhite();

    // 根据明暗选择默认方案
    static TerminalColorScheme defaultDark()  { return windowsTerminalCampbell(); }
    static TerminalColorScheme defaultLight() { return blackOnWhite(); }
};
