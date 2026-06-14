// wcwidth / wcswidth implementation for MSVC
// Based on Markus Kuhn's public domain implementation, adapted for Windows.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <wchar.h>

// Simple wcwidth: returns 1 for most characters, 2 for CJK wide chars
// Full implementation would need Unicode tables, but this covers common cases.
static __inline int compat_wcwidth(wchar_t ucs)
{
    // C0 control characters
    if (ucs == 0)
        return 0;
    if (ucs < 32 || (ucs >= 0x7F && ucs < 0xA0))
        return -1;

    // Combining characters (simplified)
    if (ucs >= 0x0300 && ucs <= 0x036F)
        return 0;
    if (ucs >= 0x0483 && ucs <= 0x0489)
        return 0;
    if (ucs >= 0x0591 && ucs <= 0x05BD)
        return 0;
    if (ucs >= 0x0610 && ucs <= 0x061A)
        return 0;
    if (ucs >= 0x064B && ucs <= 0x065F)
        return 0;
    if (ucs >= 0x06D6 && ucs <= 0x06DC)
        return 0;
    if (ucs >= 0x0730 && ucs <= 0x074A)
        return 0;
    if (ucs >= 0x0900 && ucs <= 0x0902)
        return 0;

    // CJK characters (wide)
    if (ucs >= 0x1100 && ucs <= 0x115F)
        return 2;
    if (ucs >= 0x2329 && ucs <= 0x232A)
        return 2;
    if (ucs >= 0x2E80 && ucs <= 0xA4CF)
        return 2;
    if (ucs >= 0xA960 && ucs <= 0xA97C)
        return 2;
    if (ucs >= 0xAC00 && ucs <= 0xD7A3)
        return 2;
    if (ucs >= 0xF900 && ucs <= 0xFAFF)
        return 2;
    if (ucs >= 0xFE10 && ucs <= 0xFE19)
        return 2;
    if (ucs >= 0xFE30 && ucs <= 0xFE6F)
        return 2;
    if (ucs >= 0xFF01 && ucs <= 0xFF60)
        return 2;
    if (ucs >= 0xFFE0 && ucs <= 0xFFE6)
        return 2;
    if (ucs >= 0x1F300 && ucs <= 0x1F64F)
        return 2;
    if (ucs >= 0x1F680 && ucs <= 0x1F6FF)
        return 2;
    if (ucs >= 0x20000 && ucs <= 0x2FFFD)
        return 2;
    if (ucs >= 0x30000 && ucs <= 0x3FFFD)
        return 2;

    // Zero-width characters
    if (ucs == 0x200B || ucs == 0x200C || ucs == 0x200D || ucs == 0xFEFF)
        return 0;
    if (ucs >= 0x200E && ucs <= 0x200F)
        return 0;
    if (ucs >= 0x202A && ucs <= 0x202E)
        return 0;
    if (ucs >= 0x2060 && ucs <= 0x2064)
        return 0;

    return 1;
}

#ifdef __cplusplus
}
#endif
