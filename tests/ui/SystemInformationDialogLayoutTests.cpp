/**
 * @file SystemInformationDialogLayoutTests.cpp
 * @brief UI 对话框的 Ela 控件与滚动范围回归检查。
 *
 * 背景：卡片里的文字标签开了 word-wrap，`QLabel` 对换行文本的
 * `minimumSizeHint()` 按极窄宽度估算，会把内容布局的最小高度抬到远超实际内容；
 * 滚动区用该值决定滚动内容高度，就会出现"能一直往下滚、滚出大片空白"的
 * 现象（2026-09-12 实机报告）。对话框用 `updateContentHeight()` 按视口宽度取
 * `heightForWidth()` 修正，本检查守住这条不变量：
 *
 *   内容高度 == max(视口高度, heightForWidth(视口宽度))
 *   滚动上限 == max(0, 内容高度 - 视口高度)
 *
 * 断言只依赖上述关系、不依赖绝对像素，因此与字体度量、平台无关。
 * 同时检查系统信息窗口使用 ElaScrollArea 且保留可见滚动条，以及 SSH 主机密钥
 * 对话框使用 Ela 控件并正确展开变更主机的端点标题。
 * 另外回归"点系统信息窗口关闭按钮崩溃"：该对话框带 `WA_DeleteOnClose`，
 * ElaAppBar 的关闭按钮处理会 `close()` 后 `processEvents()` 再碰
 * `windowHandle()`，对象此时已被析构（use-after-free）。
 * 需要 offscreen 平台（对话框是 Qt Widgets，检查不涉及 GPU/D3D11）。
 */
#include "ui/widgets/SystemInformationDialog.h"
#include "ui/widgets/SshHostKeyDialog.h"

#include "ElaDialog.h"
#include "ElaIconButton.h"
#include "ElaPushButton.h"
#include "ElaScrollArea.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"

#include <QApplication>
#include <QPointer>
#include <QScrollBar>
#include <cstdio>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <cstdlib>

namespace {

int verifyHostKeyDialogUsesElaWidgets()
{
    const SshHostKeyInfo info{
        QStringLiteral("example.test"), 2222,
        QStringLiteral("ssh-ed25519"),
        QStringLiteral("SHA256:test-fingerprint"),
        SshHostKeyStatus::New};
    SshHostKeyDialog dialog(info);

    int failures = 0;
    if (!qobject_cast<ElaDialog*>(&dialog)) {
        std::fprintf(stderr, "FAIL: host key dialog is not an ElaDialog\n");
        ++failures;
    }
    if (dialog.findChildren<ElaPushButton*>().size() != 2) {
        std::fprintf(stderr, "FAIL: host key actions are not Ela buttons\n");
        ++failures;
    }
    if (!dialog.findChild<ElaScrollPageArea*>()) {
        std::fprintf(stderr, "FAIL: host key details are not in an Ela area\n");
        ++failures;
    }
    if (dialog.findChildren<ElaText*>().size() < 8) {
        std::fprintf(stderr, "FAIL: host key labels are not Ela text controls\n");
        ++failures;
    }
    return failures;
}

int verifyChangedHostTitleContainsEndpoint()
{
    const SshHostKeyInfo info{
        QStringLiteral("changed.example.test"), 2200,
        QStringLiteral("ssh-ed25519"),
        QStringLiteral("SHA256:changed-fingerprint"),
        SshHostKeyStatus::Changed};
    const SshHostKeyDialog dialog(info);

    const QString expected = QStringLiteral(
        "Warning: the host key for changed.example.test:2200 has changed!");
    for (const auto* text : dialog.findChildren<ElaText*>()) {
        if (text->text() == expected)
            return 0;
    }
    std::fprintf(stderr,
                 "FAIL: changed-host warning does not contain the endpoint\n");
    return 1;
}

/**
 * @brief 点系统信息窗口右上角的关闭按钮：进程必须活下来，窗口必须被回收。
 *
 * `ElaAppBarPrivate::onCloseButtonClicked()`（`ElaAppBarPrivate.cpp:51-61`）在默认
 * 关闭路径上先 `window->close()`，再 `QApplication::processEvents()`，最后又用同一个
 * 裸指针取 `window->windowHandle()`。系统信息对话框带 `Qt::WA_DeleteOnClose`
 * （`SystemInformationDialog.cpp:228`），`close()` 排入的 deleteLater 正好被那次
 * `processEvents()` 处理掉 —— 窗口连同它自己的 app bar 和这个按钮一起析构，而
 * 处理器还在栈上，下一行就是 use-after-free。
 *
 * 2026-09-14 的 core dump 正是这里：崩溃帧
 * `QWidgetPrivate::windowHandle()` ← `ElaAppBarPrivate::onCloseButtonClicked`
 * （`ElaAppBarPrivate.cpp:57`）。core 里 `window` 指向的 0x80 字节块
 * （`sizeof(SystemInformationDialog) == 120`）内容已被 `QMetaCallEvent` 的 vtable
 * 覆盖，`d_ptr` 槽位是 `0x10001002b`，于是 `mov 0x78(%rdi)` 在 `0x1000100a3`
 * 上取地址失败。
 */
int verifyAppBarCloseButtonClosesDialogSafely()
{
#if defined(__GLIBC__)
    // 让 free() 用 0xAA 覆盖已释放内存：否则被释放的窗口块内容仍是原值，
    // 那段悬垂访问（读 d_ptr）可能"碰巧"不崩，回归就漏掉了。真实崩溃里这块内存
    // 恰好被 QMetaCallEvent 复用，d_ptr 变成 0x10001002b 才立刻炸。
    mallopt(M_PERTURB, 0xAA);
#endif
    // 与生产路径一致：对话框有父窗口（MainWindow），自身仍是顶层窗口。
    QWidget host;
    host.resize(400, 300);
    host.show();

    auto* dialog =
        new SystemInformationDialog(QStringLiteral("close-regression"), &host);
    dialog->show();
    QApplication::processEvents();
    QPointer<SystemInformationDialog> guard(dialog);

    // 只声明了 CloseButtonHint，因此可见的 ElaIconButton 只有关闭按钮一个。
    ElaIconButton* closeButton = nullptr;
    int visibleButtons = 0;
    for (ElaIconButton* button : dialog->findChildren<ElaIconButton*>()) {
        if (!button->isVisibleTo(dialog))
            continue;
        ++visibleButtons;
        closeButton = button;
    }
    if (visibleButtons != 1 || !closeButton) {
        std::fprintf(stderr,
                     "FAIL: expected exactly one visible app bar button, "
                     "found %d\n",
                     visibleButtons);
        delete dialog;
        return 1;
    }

    closeButton->click();  // 修复前：这里 SIGSEGV
    QApplication::processEvents();

    if (guard) {
        std::fprintf(stderr,
                     "FAIL: close button left the WA_DeleteOnClose dialog "
                     "alive\n");
        delete guard.data();
        return 1;
    }
    std::printf("app bar close button -> dialog deleted, no crash\n");
    return 0;
}

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
    int failures = verifyAppBarCloseButtonClosesDialogSafely();
    failures += verifyHostKeyDialogUsesElaWidgets();
    failures += verifyChangedHostTitleContainsEndpoint();

    SystemInformationDialog dialog(QStringLiteral("root@192.168.10.100"),
                                  nullptr);
    dialog.resize(1100, 760);
    dialog.populate(samplePayload(), /*pending=*/false);
    dialog.show();
    QApplication::processEvents();

    const auto informationCards =
        dialog.findChildren<ElaScrollPageArea*>();
    if (informationCards.size() != 8) {
        std::fprintf(stderr,
                     "FAIL: expected 8 Ela information cards, found %lld\n",
                     static_cast<long long>(informationCards.size()));
        ++failures;
    }

    auto* scroll = dialog.findChild<ElaScrollArea*>();
    if (!scroll || !scroll->widget()) {
        std::fprintf(stderr, "FAIL: dialog has no Ela scroll area\n");
        return 1;
    }
    if (scroll->verticalScrollBarPolicy() != Qt::ScrollBarAsNeeded) {
        std::fprintf(stderr,
                     "FAIL: Ela scroll area hides its vertical scroll bar\n");
        return 1;
    }
    QWidget* const content = scroll->widget();
    auto* const bar = scroll->verticalScrollBar();

    // 宽、窄、高三种视口：窄窗口换行加剧（内容更高），高窗口应无多余行程。
    const QList<QSize> windowSizes{{1100, 760}, {700, 420}, {1100, 900}};
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
