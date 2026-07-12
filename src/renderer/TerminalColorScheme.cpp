#include "TerminalColorScheme.h"

TerminalColorScheme TerminalColorScheme::darkPastels()
{
    TerminalColorScheme s;
    s.name        = QStringLiteral("Dark Pastels");
    s.foreground  = QColor(220, 220, 204);
    s.background  = QColor( 44,  44,  44);
    s.cursorColor = QColor(220, 220, 204);
    s.selectionColor = QColor(84, 107, 138);

    // 标准色
    s.palette[0]  = QColor( 63,  63,  63);   // Black
    s.palette[1]  = QColor(112,  80,  80);   // Red
    s.palette[2]  = QColor( 96, 180, 138);   // Green
    s.palette[3]  = QColor(223, 175, 143);   // Yellow
    s.palette[4]  = QColor(154, 184, 215);   // Blue
    s.palette[5]  = QColor(220, 140, 195);   // Magenta
    s.palette[6]  = QColor(140, 208, 211);   // Cyan
    s.palette[7]  = QColor(220, 220, 204);   // White

    // 亮色
    s.palette[8]  = QColor(112, 144, 128);   // Bright Black
    s.palette[9]  = QColor(220, 163, 163);   // Bright Red
    s.palette[10] = QColor(114, 213, 163);   // Bright Green
    s.palette[11] = QColor(240, 223, 175);   // Bright Yellow
    s.palette[12] = QColor(148, 191, 243);   // Bright Blue
    s.palette[13] = QColor(236, 147, 211);   // Bright Magenta
    s.palette[14] = QColor(147, 224, 227);   // Bright Cyan
    s.palette[15] = QColor(255, 255, 255);   // Bright White

    return s;
}

TerminalColorScheme TerminalColorScheme::blackOnWhite()
{
    TerminalColorScheme s;
    s.name        = QStringLiteral("Black on White");
    s.foreground  = QColor(  0,   0,   0);
    s.background  = QColor(255, 255, 255);
    s.cursorColor = QColor(  0,   0,   0);
    s.selectionColor = QColor(84, 107, 138);

    // 标准色
    s.palette[0]  = QColor(  0,   0,   0);   // Black
    s.palette[1]  = QColor(178,  24,  24);   // Red
    s.palette[2]  = QColor( 24, 178,  24);   // Green
    s.palette[3]  = QColor(178, 104,  24);   // Yellow
    s.palette[4]  = QColor( 24,  24, 178);   // Blue
    s.palette[5]  = QColor(178,  24, 178);   // Magenta
    s.palette[6]  = QColor( 24, 178, 178);   // Cyan
    s.palette[7]  = QColor(178, 178, 178);   // White

    // 亮色
    s.palette[8]  = QColor(104, 104, 104);   // Bright Black
    s.palette[9]  = QColor(255,  84,  84);   // Bright Red
    s.palette[10] = QColor( 84, 255,  84);   // Bright Green
    s.palette[11] = QColor(255, 255,  84);   // Bright Yellow
    s.palette[12] = QColor( 84,  84, 255);   // Bright Blue
    s.palette[13] = QColor(255,  84, 255);   // Bright Magenta
    s.palette[14] = QColor( 84, 255, 255);   // Bright Cyan
    s.palette[15] = QColor(255, 255, 255);   // Bright White

    return s;
}
