#!/usr/bin/env bash

# NovaTerm P3 manual visual regression fixture.
# Run this inside NovaTerm. Each page is intentionally paused so that the
# operator can inspect it and capture a screenshot before moving on.

set -u

ESC=$'\033'
page_number=0
auto_delay=""

usage() {
    printf 'Usage: %s [--auto SECONDS]\n' "${0##*/}"
    printf '  default          press Enter to advance each visual test page\n'
    printf '  --auto SECONDS   advance automatically after SECONDS per page\n'
}

if [[ ${1-} == "--help" || ${1-} == "-h" ]]; then
    usage
    exit 0
fi

if [[ ${1-} == "--auto" ]]; then
    if [[ -z ${2-} || ! ${2-} =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        printf 'error: --auto requires a non-negative delay in seconds\n' >&2
        exit 2
    fi
    auto_delay=$2
    shift 2
fi

if (( $# != 0 )); then
    usage >&2
    exit 2
fi

cleanup() {
    # Reset attributes, restore a visible steady block cursor and leave the
    # operator on a fresh line even if the script was interrupted.
    printf '%s[0m%s[?25h%s[2 q\n' "$ESC" "$ESC" "$ESC"
}
trap cleanup EXIT INT TERM

clear_page() {
    printf '%s[2J%s[H' "$ESC" "$ESC"
}

page_header() {
    page_number=$((page_number + 1))
    clear_page
    printf '%s[1;36mNovaTerm P3 Visual Regression — %02d — %s%s[0m\n' \
        "$ESC" "$page_number" "$1" "$ESC"
    printf '截图建议文件名：p3-%02d-%s.png\n' "$page_number" "$2"
    printf '%s\n\n' '────────────────────────────────────────────────────────────'
}

pause_page() {
    printf '\n%s\n' '────────────────────────────────────────────────────────────'
    if [[ -n $auto_delay ]]; then
        printf '请检查并截图；%s 秒后自动继续。' "$auto_delay"
        sleep "$auto_delay"
        printf '\n'
    else
        printf '请检查并截图，然后按 Enter 继续（Ctrl+C 可安全退出）：'
        IFS= read -r _
    fi
}

print_ruler() {
    local cols=${COLUMNS:-80}
    local i
    if command -v tput >/dev/null 2>&1; then
        cols=$(tput cols 2>/dev/null || printf '%s' "$cols")
    fi
    (( cols > 100 )) && cols=100
    printf '列个位数：'
    for ((i = 1; i <= cols - 10; ++i)); do
        printf '%d' "$((i % 10))"
    done
    printf '\n'
}

page_header 'CJK、全角字符与网格对齐' 'cjk-grid'
print_ruler
printf 'ASCII : ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\n'
printf '中文  : 你好世界，终端渲染测试。中文字符应占两个 Cell。\n'
printf '日文  : 日本語の表示テストです。カタカナ。\n'
printf '韩文  : 한글 터미널 렌더링 테스트입니다.\n'
printf '混合  : A中B文C日D本E한F글G — 后继字母不得覆盖宽字符。\n'
printf '全角  : ＡＢＣＤＥＦ　１２３４５６\n'
printf '\n检查：基线、双宽占位、后继字符列位置、无重复/裁切/方框残影。\n'
pause_page

page_header 'CJK 行末换行边界' 'cjk-wrap'
cols=${COLUMNS:-80}
if command -v tput >/dev/null 2>&1; then
    cols=$(tput cols 2>/dev/null || printf '%s' "$cols")
fi
(( cols < 20 )) && cols=20
printf '当前检测列数：%d\n\n' "$cols"
printf '双宽字符恰好占满行尾（下一行应从 X 开始）：\n'
printf '%*s界X\n' "$((cols - 2))" ''
printf '\n只剩一个 Cell 时放入双宽字符（界不得显示半个或被 X 覆盖）：\n'
printf '%*s界X\n' "$((cols - 1))" ''
printf '\n检查完成后可先拖动窗口宽度，再观察是否有旧 glyph 残留。\n'
pause_page

page_header '组合字符与 Cell advance' 'combining'
printf '预组合  : café | Ångström | naïve\n'
printf $'分解组合: cafe\u0301 | A\u030a | n\u0303\n'
printf $'多重组合: a\u0301\u0323 | e\u0308\u0301 | o\u0302\u0301\n'
printf $'混合网格: A|e\u0301|B|中\u0301|C|x\u0323|D\n'
printf $'连续重复: e\u0301 e\u0301 e\u0301 e\u0301 e\u0301\n'
printf '\n检查：附加符号跟随基础字符，不额外占格，不漂移、裁切或残留。\n'
printf '说明：超过 6 codepoint 的极端 cluster 属于已知模型限制，留待 P5。\n'
pause_page

page_header 'Underline、Double Underline 与 Strike' 'decorations'
printf '%s[0m普通文本：ASCII 中文界面%s[0m\n' "$ESC" "$ESC"
printf '%s[4m单下划线：ASCII 中文界面%s[0m\n' "$ESC" "$ESC"
printf '%s[4:2m双下划线：ASCII 中文界面%s[0m\n' "$ESC" "$ESC"
printf '%s[4:3m曲线下划线：ASCII 中文界面%s[0m（P3 允许安全回退）\n' "$ESC" "$ESC"
printf '%s[9m删除线：ASCII 中文界面%s[0m\n' "$ESC" "$ESC"
printf '%s[1;3;4;9m粗体 + 斜体 + 下划线 + 删除线%s[0m\n' "$ESC" "$ESC"
printf '%s[31;4m红色下划线%s[0m  %s[42;9m背景色删除线%s[0m\n' \
    "$ESC" "$ESC" "$ESC" "$ESC"
printf '\n检查：线条位置、双线间距、CJK 两格宽度、reset 后属性清除。\n'
pause_page

cursor_page() {
    local title=$1
    local code=$2
    local slug=$3
    page_header "Cursor：$title" "cursor-$slug"
    printf '%s[%s q' "$ESC" "$code"
    printf '把光标停在本行末尾 → ASCII 中文 é '
    printf '\n\n请用左右方向键让光标经过 ASCII、CJK 和组合字符。\n'
    printf '检查：形状与 Cell 对齐；移走后底层字形完整；闪烁不影响正文。\n'
    pause_page
}

cursor_page '稳定块状' 2 'block'
cursor_page '稳定下划线' 4 'underline'
cursor_page '稳定左竖线' 6 'bar'
cursor_page '闪烁块状（观察至少两个周期）' 1 'blink'

page_header 'Cursor 显示、隐藏与恢复' 'cursor-visibility'
printf '光标现在隐藏 2 秒；正文不应变化，隐藏位置不得留下色块。\n'
printf '%s[?25l' "$ESC"
sleep 2
printf '%s[?25h%s[2 q' "$ESC" "$ESC"
printf '光标已经恢复为稳定块状。\n'
printf '\n检查：隐藏无残影；恢复位置正确；向上滚动时 Cursor 应隐藏。\n'
pause_page

page_header 'Selection：方向、CJK、组合字符和多行' 'selection'
printf '请用鼠标执行并分别观察：\n'
printf '  1. 左→右与右→左选择同一段；\n'
printf '  2. 跨行选择；选择中文字符和 é；\n'
printf '  3. 截图时保留 Selection；复制后粘贴到文本编辑器核对。\n\n'
printf '001 | ASCII alpha beta | 中文界面 | café | END\n'
printf '002 | ASCII gamma delta| 日本語   | Ångström | END\n'
printf '003 | mixed A中B文C    | 한글     | ë́ | END\n'
printf '004 | line boundary selection target..................END\n'
printf '005 | %s[4munderline 中文%s[0m | %s[9mstrike 中文%s[0m | END\n' \
    "$ESC" "$ESC" "$ESC" "$ESC"
printf '\n检查：背景按 Cell 对齐、CJK 不半选、首尾无多格、正文仍可辨认。\n'
pause_page

page_header '增量覆盖：同一行高频更新' 'incremental-row'
printf '请观察动态过程：旧数字、CJK、组合符和装饰不得残留。\n\n'
for ((i = 1; i <= 150; ++i)); do
    printf '\r%s[2K动态更新 %04d | 中文界面 | café | %s[4munderline%s[0m | %s[9mstrike%s[0m' \
        "$ESC" "$i" "$ESC" "$ESC" "$ESC" "$ESC"
    sleep 0.02
done
printf '\n\n最终值必须为 0150，停止后画面必须稳定且完整。\n'
pause_page

page_header '持续滚屏与最终画面收敛' 'scroll-final'
printf '即将输出 120 行；请观察滚动过程是否错行、闪烁或残留。\n'
sleep 1
for ((i = 1; i <= 120; ++i)); do
    printf '%03d | ASCII | 中文界面 | café | %s[4mU%s[0m | %s[9mS%s[0m\n' \
        "$i" "$ESC" "$ESC" "$ESC" "$ESC"
    sleep 0.01
done
printf '\nFINAL-REVISION-MARKER-120\n'
printf '检查：最终标记存在，最后几行连续，停止后无补帧造成的内容跳变。\n'
printf '再向上滚动检查历史内容；此时 Cursor 应隐藏，回到底部后恢复。\n'
pause_page

page_header '验收结束' 'complete'
printf '已完成 %d 个页面。请确认 Vulkan 与 OpenGL 各执行一次。\n' "$page_number"
printf '将截图按页面编号命名并发送给检查人员。\n'
printf '\n建议同时记录：后端、主题、字体/字号、DPR、窗口尺寸和发现的问题。\n'

