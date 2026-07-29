#include "TerminalColorScheme.h"

TerminalColorScheme TerminalColorScheme::windowsTerminalCampbell()
{
    TerminalColorScheme s;
    s.name        = QStringLiteral("Windows Terminal Campbell");
    s.foreground  = QColor(204, 204, 204); // #CCCCCC
    s.background  = QColor( 12,  12,  12); // #0C0C0C
    s.cursorColor = QColor(255, 255, 255);
    s.selectionColor = QColor(255, 255, 255, 64);

    // Windows Terminal's built-in Campbell ANSI palette.
    s.palette[0]  = QColor( 12,  12,  12); // #0C0C0C
    s.palette[1]  = QColor(197,  15,  31); // #C50F1F
    s.palette[2]  = QColor( 19, 161,  14); // #13A10E
    s.palette[3]  = QColor(193, 156,   0); // #C19C00
    s.palette[4]  = QColor(  0,  55, 218); // #0037DA
    s.palette[5]  = QColor(136,  23, 152); // #881798
    s.palette[6]  = QColor( 58, 150, 221); // #3A96DD
    s.palette[7]  = QColor(204, 204, 204); // #CCCCCC
    s.palette[8]  = QColor(118, 118, 118); // #767676
    s.palette[9]  = QColor(231,  72,  86); // #E74856
    s.palette[10] = QColor( 22, 198,  12); // #16C60C
    s.palette[11] = QColor(249, 241, 165); // #F9F1A5
    s.palette[12] = QColor( 59, 120, 255); // #3B78FF
    s.palette[13] = QColor(180,   0, 158); // #B4009E
    s.palette[14] = QColor( 97, 214, 214); // #61D6D6
    s.palette[15] = QColor(242, 242, 242); // #F2F2F2

    return s;
}

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
