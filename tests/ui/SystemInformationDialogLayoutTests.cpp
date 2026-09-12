/**
 * @file SystemInformationDialogLayoutTests.cpp
 * @brief 系统信息对话框的滚动范围回归检查。
 *
 * 背景：卡片里的文字标签开了 word-wrap，`QLabel` 对换行文本的
 * `minimumSizeHint()` 按极窄宽度估算，会把内容布局的最小高度抬到远超实际内容；
 * `QScrollArea` 用该值决定滚动内容高度，就会出现"能一直往下滚、滚出大片空白"的
 * 现象（2026-09-12 实机报告）。对话框用 `updateContentHeight()` 按视口宽度取
 * `heightForWidth()` 修正，本检查守住这条不变量：
 *
 *   内容高度 == max(视口高度, heightForWidth(视口宽度))
 *   滚动上限 == max(0, 内容高度 - 视口高度)
 *
 * 断言只依赖上述关系、不依赖绝对像素，因此与字体度量、平台无关。
 * 需要 offscreen 平台（对话框是 Qt Widgets，检查不涉及 GPU/D3D11）。
 */
#include "ui/widgets/SystemInformationDialog.h"

#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <cstdio>
#include <cstdlib>

namespace {

QByteArray samplePayload()
{
    QByteArray out;
    const auto row = [&out](const QByteArray& type,
                            const QList<QByteArray>& values) {
        out += type;
        for (const auto& value : values)
            out += '\t' + value;
        out += '\n';
    };
    // 与实机 root@192.168.10.100 抓到的字段规模相当：概览 + CPU + GPU +
    // 占用分解 + 内存/交换 + 双网卡 + 5 个文件系统。
    row("OV", {"Ubuntu 22.04 LTS", "5.15.0", "zynq", "192.168.10.100",
               "0.10 0.08 0.05", "armv7l", "86400",
               "192.168.10.5 51234 192.168.10.100 22"});
    row("CPU", {"ARMv7 Processor rev 0 (v7l)", "2", "666.7", "512 KB",
                "ARM / 0.00"});
    row("GPU", {"Zynq-7000 Display Controller", "-", "-", "-"});
    row("CPUUSE", {"2.0", "1.0", "0.0", "95.0", "1.0", "1.0"});
    row("MEM", {"509000", "27000", "481000", "5.4", "35600"});
    row("SWAP", {"0", "0", "0", "0.0"});
    row("NET", {"eth0", "6700000", "1150000", "-", "-"});
    row("NET", {"sit0", "0", "0", "-", "-"});
    row("FS", {"/dev/root", "30000000", "0%", "28400000", "/"});
    row("FS", {"devtmpfs", "245000", "0%", "245000", "/dev"});
    row("FS", {"tmpfs", "253000", "0%", "253000", "/dev/shm"});
    row("FS", {"tmpfs", "253000", "0%", "253000", "/tmp"});
    row("FS", {"tmpfs", "253000", "0%", "253000", "/run"});
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    SystemInformationDialog dialog(QStringLiteral("root@192.168.10.100"),
                                  nullptr);
    dialog.resize(1100, 760);
    dialog.populate(samplePayload(), /*pending=*/false);
    dialog.show();
    QApplication::processEvents();

    auto* scroll = dialog.findChild<QScrollArea*>();
    if (!scroll || !scroll->widget()) {
        std::fprintf(stderr, "FAIL: dialog has no scroll area\n");
        return 1;
    }
    QWidget* const content = scroll->widget();
    auto* const bar = scroll->verticalScrollBar();

    // 宽、窄、高三种视口：窄窗口换行加剧（内容更高），高窗口应无多余行程。
    const QList<QSize> windowSizes{{1100, 760}, {700, 420}, {1100, 900}};
    int failures = 0;
    for (const QSize& size : windowSizes) {
        dialog.resize(size);
        QApplication::processEvents();
        const int viewportHeight = scroll->viewport()->height();
        const int viewportWidth = std::max(1, scroll->viewport()->width());
        int expected = content->heightForWidth(viewportWidth);
        if (expected <= 0)
            expected = content->sizeHint().height();
        expected = std::max(expected, viewportHeight);
        const int expectedMax = std::max(0, expected - viewportHeight);

        const bool heightOk = std::abs(content->height() - expected) <= 8;
        const bool rangeOk = std::abs(bar->maximum() - expectedMax) <= 8;
        std::printf(
            "size=%dx%d viewport=%dx%d content=%d hfw=%d expected=%d "
            "scrollMax=%d expectedMax=%d -> %s\n",
            size.width(), size.height(), viewportWidth, viewportHeight,
            content->height(), content->heightForWidth(viewportWidth), expected,
            bar->maximum(), expectedMax,
            (heightOk && rangeOk) ? "ok" : "FAIL");
        if (!heightOk || !rangeOk)
            ++failures;

        // 滚到底时内容底边必须正好贴住视口底边：若 content bottom 落在视口之内，
        // 说明滚动上限大于真实溢出，用户可以继续往下滚进纯空白区。
        bar->setValue(bar->maximum());
        QApplication::processEvents();
        const QPoint offset = content->mapTo(scroll->viewport(), QPoint(0, 0));
        const int bottom = offset.y() + content->height();
        const bool bottomOk = std::abs(bottom - viewportHeight) <= 8;
        std::printf("  contentBottom=%d viewportBottom=%d -> %s\n", bottom,
                    viewportHeight, bottomOk ? "ok" : "FAIL");
        if (!bottomOk)
            ++failures;
    }

    std::printf("RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
