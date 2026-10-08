/**
 * @file SystemInformationDialogLayoutTests.cpp
 * @brief 终端标签及 UI 对话框的 Ela 控件与布局回归检查。
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
 * 终端标签检查右侧合并连接动作、动态紧凑宽度、完整 Tooltip 与过渡态隐藏。
 * 另外回归"点系统信息窗口关闭按钮崩溃"：该对话框带 `WA_DeleteOnClose`，
 * ElaAppBar 的关闭按钮处理会 `close()` 后 `processEvents()` 再碰
 * `windowHandle()`，对象此时已被析构（use-after-free）。
 * 需要 offscreen 平台（对话框是 Qt Widgets，检查不涉及 GPU/D3D11）。
 */
#include "ui/widgets/SystemInformationDialog.h"
#include "ui/widgets/SshHostKeyDialog.h"
#include "ui/widgets/TerminalTabWidget.h"
#include "ui/widgets/TerminalSchemeSettings.h"
#include "service/ConfigManager.h"
#include "service/TerminalSchemeStore.h"

#include "ElaComboBox.h"
#include "ElaApplication.h"
#include "ElaLineEdit.h"
#include "ElaTheme.h"
#include "ElaDialog.h"
#include "ElaIconButton.h"
#include "ElaPushButton.h"
#include "ElaScrollArea.h"
#include "ElaScrollBar.h"
#include "ElaScrollPageArea.h"
#include "ElaTabBar.h"
#include "ElaText.h"

#include <QHBoxLayout>
#include <QTabWidget>
#include <QStyle>

#include <QApplication>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QTemporaryDir>
#include <QTranslator>
#include <QPointer>
#include <QScrollBar>
#include <cstdio>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <cstdlib>

namespace {

int schemePersistenceStep(const QString& path, int phase)
{
    if (phase == 5) {
        QFile legacy(path);
        if (!legacy.open(QIODevice::WriteOnly))
            return 13;
        const QByteArray data = QByteArrayLiteral("{\"terminal\":{\"scrollbackLines\":10000,\"futureHistoryField\":\"preserve\"}}");
        if (legacy.write(data) != data.size())
            return 14;
    }
    auto& manager = ConfigManager::instance();
    // 资源路径用于模拟保存失败，不改权限，也不碰真实用户配置。
    manager.load(phase == 4 ? QStringLiteral(":/novaterm/terminal-color-schemes.json") : path);
    auto config = manager.root();
    const auto count = config.value(QStringLiteral("schemes")).toArray().size();
    const auto presetCount = TerminalSchemeStore::builtins().size();
    auto persist = [&config] {
        return ConfigManager::setValues({
            {QStringLiteral("schemes"), config.value(QStringLiteral("schemes")).toVariant()},
            {QStringLiteral("terminal"), config.value(QStringLiteral("terminal")).toVariant()}});
    };
    if (phase == 0) {
        if (count != presetCount)
            return 1;
        auto custom = *TerminalSchemeStore::find(config, QStringLiteral("Dracula"));
        custom.name = QStringLiteral("Persisted dark");
        custom.palette[1] = QColor(QStringLiteral("#123abc"));
        if (!TerminalSchemeStore::save(config, custom))
            return 2;
        auto catalog = config.value(QStringLiteral("schemes")).toArray();
        auto entry = catalog.last().toObject();
        entry[QStringLiteral("futureField")] = QStringLiteral("preserve me");
        catalog[catalog.size() - 1] = entry;
        config[QStringLiteral("schemes")] = catalog;
        config[QStringLiteral("terminal")] = QJsonObject{
            {QStringLiteral("appearance"), QStringLiteral("dark")},
            {QStringLiteral("colorScheme"), QJsonObject{
                {QStringLiteral("dark"), custom.name},
                {QStringLiteral("light"), QStringLiteral("One Half Light")}}}};
        return persist() ? 0 : 3;
    }
    if (phase == 1) {
        auto custom = TerminalSchemeStore::find(config, QStringLiteral("Persisted dark"));
        if (count != presetCount + 1 || !custom || custom->palette[1] != QColor("#123abc")
            || TerminalSchemeStore::resolve(config, true).name != custom->name)
            return 4;
        custom->name = QStringLiteral("Renamed dark");
        auto builtin = *TerminalSchemeStore::find(config, QStringLiteral("Campbell"));
        builtin.background = QColor(QStringLiteral("#151819"));
        if (!TerminalSchemeStore::save(config, *custom, QStringLiteral("Persisted dark"))
            || !TerminalSchemeStore::save(config, builtin, builtin.name))
            return 5;
        return persist() ? 0 : 6;
    }
    if (phase == 2) {
        if (TerminalSchemeStore::find(config, QStringLiteral("Persisted dark"))
            || TerminalSchemeStore::resolve(config, true).name != QStringLiteral("Renamed dark")
            || TerminalSchemeStore::find(config, QStringLiteral("Campbell"))->background != QColor("#151819"))
            return 7;
        bool extensionPreserved = false;
        for (const auto& value : config.value(QStringLiteral("schemes")).toArray()) {
            const auto entry = value.toObject();
            if (entry.value(QStringLiteral("name")) == QStringLiteral("Renamed dark"))
                extensionPreserved = entry.value(QStringLiteral("futureField")) == QStringLiteral("preserve me");
        }
        if (!extensionPreserved || !TerminalSchemeStore::remove(config, QStringLiteral("Renamed dark"))
            || !TerminalSchemeStore::remove(config, QStringLiteral("Campbell")))
            return 8;
        return persist() ? 0 : 9;
    }
    if (phase == 3) {
        return count == presetCount
            && !TerminalSchemeStore::find(config, QStringLiteral("Renamed dark"))
            && TerminalSchemeStore::resolve(config, true).name == QStringLiteral("Campbell")
            && TerminalSchemeStore::find(config, QStringLiteral("Campbell"))->background == QColor("#0c0c0c") ? 0 : 10;
    }
    if (phase == 4) {
        int notifications = 0;
        QObject::connect(&manager, &ConfigManager::configChanged, &manager,
            [&notifications] { ++notifications; });
        const bool saved = ConfigManager::setValues({
            {QStringLiteral("terminal.appearance"), QStringLiteral("light")},
            {QStringLiteral("schemes"), QVariantList{}}});
        return !saved && manager.root() == config && notifications == 0 ? 0 : 11;
    }
    if (phase == 5) {
        const auto terminal = config.value(QStringLiteral("terminal")).toObject();
        QFile saved(path);
        if (!saved.open(QIODevice::ReadOnly))
            return 15;
        const auto stored = QJsonDocument::fromJson(saved.readAll()).object()
            .value(QStringLiteral("terminal")).toObject();
        return !terminal.contains(QStringLiteral("scrollbackLines"))
            && !stored.contains(QStringLiteral("scrollbackLines"))
            && terminal.value(QStringLiteral("futureHistoryField")) == QStringLiteral("preserve") ? 0 : 16;
    }
    return 12;
}

int verifySchemePersistenceAcrossProcesses()
{
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;
    const QString path = directory.filePath(QStringLiteral("novaterm.json"));
    for (int phase = 0; phase < 6; ++phase) {
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {
            QStringLiteral("--scheme-persistence-step"), path, QString::number(phase)});
        if (!child.waitForFinished(5000) || child.exitStatus() != QProcess::NormalExit
            || child.exitCode() != 0) {
            child.kill();
            child.waitForFinished(1000);
            std::fprintf(stderr, "FAIL: scheme persistence phase %d, code %d: %s\n",
                         phase, child.exitCode(), child.readAll().constData());
            return 1;
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 1;
    const auto saved = QJsonDocument::fromJson(file.readAll()).object();
    return saved.value(QStringLiteral("schemes")).toArray().size()
        == TerminalSchemeStore::builtins().size() ? 0 : 1;
}

int verifyTerminalSchemeSettings()
{
    // 不 load()，防止单测写入可执行文件旁的真实配置。
    ConfigManager::set(QStringLiteral("terminal"), QVariantMap{
        {QStringLiteral("colorScheme"), QStringLiteral("Campbell")},
        {QStringLiteral("appearance"), QStringLiteral("dark")}});
    TerminalSchemeSettings settings;
    settings.resize(740, settings.sizeHint().height());
    settings.show();
    QApplication::processEvents();
    auto* mode = settings.findChild<ElaComboBox*>(QStringLiteral("terminalSchemeMode"));
    auto* dark = settings.findChild<ElaComboBox*>(QStringLiteral("terminalDarkScheme"));
    auto* light = settings.findChild<ElaComboBox*>(QStringLiteral("terminalLightScheme"));
    auto* apply = settings.findChild<ElaPushButton*>(QStringLiteral("applyTerminalSchemes"));
    auto* duplicate = settings.findChild<ElaPushButton*>(QStringLiteral("duplicateTerminalScheme"));
    auto* discard = settings.findChild<ElaPushButton*>(QStringLiteral("discardTerminalSchemeChanges"));
    auto* name = settings.findChild<ElaLineEdit*>(QStringLiteral("terminalSchemeName"));
    if (!mode || !dark || !light || !apply || !duplicate || !discard || !name)
        return 1;
    int failures = 0;
    auto check = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    check(mode->count() == 2 && mode->currentIndex() == 0, "terminal categories default to dark");
    check(dark->count() > light->count(), "dark schemes are the primary collection");
    for (int i = 0; i < dark->count(); ++i)
        check(TerminalSchemeStore::isDark(*TerminalSchemeStore::find({}, dark->itemText(i))), "dark category contains light scheme");
    for (int i = 0; i < light->count(); ++i)
        check(!TerminalSchemeStore::isDark(*TerminalSchemeStore::find({}, light->itemText(i))), "light category contains dark scheme");
    dark->setCurrentText(QStringLiteral("Dracula"));
    check(ConfigManager::get<QString>(QStringLiteral("terminal.colorScheme")) == QStringLiteral("Campbell"), "preview modified live settings");
    duplicate->click();
    check(!name->isReadOnly(), "duplicated scheme name must be editable");
    name->setText(QStringLiteral("Test custom dark"));
    QMetaObject::invokeMethod(name, "editingFinished", Qt::DirectConnection);
    apply->click();
    const auto saved = ConfigManager::instance().root();
    check(TerminalSchemeStore::resolve(saved, true).name == QStringLiteral("Test custom dark"), "save did not select duplicated scheme");
    check(saved.value(QStringLiteral("schemes")).toArray().size()
        == TerminalSchemeStore::builtins().size() + 1, "save lost complete scheme library");
    mode->setCurrentIndex(1);
    check(light->isVisible() && !dark->isVisible(), "category visibility does not follow terminal selection");
    check(ConfigManager::get<QString>(QStringLiteral("terminal.appearance")) == QStringLiteral("dark"), "category preview changed live terminal");
    discard->click();
    check(mode->currentIndex() == 0, "discard did not restore dark category");
    check(ConfigManager::instance().root() == saved, "discard modified stored configuration");

    // 可选输出真实控件截图，供人工核对深浅 UI 下终端预览的独立性。
    const QString shots = qEnvironmentVariable("NOVATERM_SCHEME_SCREENSHOTS");
    if (!shots.isEmpty()) {
        QDir().mkpath(shots);
        auto setWindowPalette = [&settings](bool darkMode) {
            auto palette = settings.palette();
            palette.setColor(QPalette::Window, darkMode ? QColor(30, 30, 30) : QColor(248, 248, 248));
            settings.setAutoFillBackground(true);
            settings.setPalette(palette);
        };
        eTheme->setThemeMode(ElaThemeType::Dark);
        setWindowPalette(true);
        dark->setCurrentText(QStringLiteral("Dracula"));
        QApplication::processEvents();
        settings.grab().save(QDir(shots).filePath(QStringLiteral("terminal-dark.png")));
        eTheme->setThemeMode(ElaThemeType::Light);
        setWindowPalette(false);
        QApplication::processEvents();
        settings.grab().save(QDir(shots).filePath(QStringLiteral("app-light-terminal-dark.png")));
        mode->setCurrentIndex(1);
        light->setCurrentText(QStringLiteral("One Half Light"));
        QApplication::processEvents();
        settings.grab().save(QDir(shots).filePath(QStringLiteral("terminal-light.png")));
        settings.resize(540, settings.sizeHint().height());
        QApplication::processEvents();
        settings.grab().save(QDir(shots).filePath(QStringLiteral("terminal-narrow.png")));
    }
    return failures;
}

int verifyTerminalTabConnectionAction()
{
    TerminalTabWidget tabs;
    tabs.setTabsClosable(true);
    auto* page = new QWidget;
    const QString fullTitle = QStringLiteral(
        "192.168.10.100 — production-terminal-with-a-long-name");
    const int index = tabs.addTab(page, fullTitle);
    tabs.setTabConnectionAction(
        page, TerminalTabWidget::ConnectionAction::Disconnect);

    auto* const bar = tabs.findChild<ElaTabBar*>();
    QWidget* const rightSide = bar
        ? bar->tabButton(index, QTabBar::RightSide) : nullptr;
    const auto buttons = rightSide
        ? rightSide->findChildren<ElaIconButton*>()
        : QList<ElaIconButton*>{};
    QList<ElaIconButton*> actionButtons;
    for (auto* button : buttons) {
        if (button->property("novatermConnectionActionButton").toBool())
            actionButtons.append(button);
    }
    int failures = 0;
    if (!bar || !rightSide || actionButtons.size() != 1) {
        std::fprintf(stderr,
                     "FAIL: terminal tab has no combined connection action\n");
        return 1;
    }

    auto* const actionButton = actionButtons.constFirst();
    if (actionButton->size() != QSize(22, 22)
        || actionButton->getAwesome() != ElaIconType::PowerOff) {
        std::fprintf(stderr,
                     "FAIL: connected terminal action is not 22px disconnect\n");
        ++failures;
    }
    const int expectedWidth = QFontMetrics(bar->font()).horizontalAdvance(
        QStringLiteral("192.168.10.100")) + 70;
    if (tabs.tabToolTip(index) != fullTitle
        || tabs.getTabSize().width() != expectedWidth) {
        std::fprintf(stderr,
                     "FAIL: compact terminal tab tooltip=%d width=%d expected=%d\n",
                     tabs.tabToolTip(index) == fullTitle,
                     tabs.getTabSize().width(), expectedWidth);
        ++failures;
    }

    bool disconnectRequested = false;
    QObject::connect(&tabs, &TerminalTabWidget::disconnectRequested,
                     &tabs, [&disconnectRequested, page](QWidget* target) {
        disconnectRequested = target == page;
    });
    actionButton->click();
    if (!disconnectRequested) {
        std::fprintf(stderr, "FAIL: disconnect action targets wrong tab\n");
        ++failures;
    }

    tabs.setTabConnectionAction(
        page, TerminalTabWidget::ConnectionAction::Hidden);
    if (!actionButton->isHidden()) {
        std::fprintf(stderr, "FAIL: transitional terminal action is visible\n");
        ++failures;
    }
    tabs.setTabConnectionAction(
        page, TerminalTabWidget::ConnectionAction::Reconnect);
    if (actionButton->isHidden()
        || actionButton->getAwesome() != ElaIconType::ArrowRotateRight) {
        std::fprintf(stderr,
                     "FAIL: reconnect visible=%d hidden=%d icon=%d\n",
                     actionButton->isVisible(), actionButton->isHidden(),
                     static_cast<int>(actionButton->getAwesome()));
        ++failures;
    }
    return failures;
}

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

// 与主窗口一致：终端先继承样式表，再由 addTab 改挂到堆叠布局。
// 宽度设置不得把 QStyleSheetStyle 包装对象当成 Ela 样式写入。
static int verifyTerminalScrollBarWithParentStyleSheet()
{
    for (int iteration = 0; iteration < 3; ++iteration) {
        QWidget window;
        window.setStyleSheet(QStringLiteral("QWidget { background-color: #202020; }"));
        QTabWidget tabs(&window);
        auto* view = new QWidget(&tabs);
        auto* layout = new QHBoxLayout(view);
        auto* bar = new ElaScrollBar(Qt::Vertical, view);
        bar->setScrollBarExtent(16);
        bar->setRange(0, 100);
        bar->setPageStep(10);
        layout->addWidget(bar);
        tabs.addTab(view, QStringLiteral("Terminal"));
        bar->setScrollBarExtent(18);
        window.setStyleSheet(QStringLiteral("QWidget { background-color: #f0f0f0; }"));
        if (bar->sizeHint().width() != 18) {
            std::fprintf(stderr, "FAIL: styled terminal scrollbar width is not 18\n");
            return 1;
        }
        bar->setValue(100);
        if (bar->value() != 100)
            return 1;
    }
    std::printf("styled terminal scrollbar reparent/width/theme/lifecycle -> ok\n");
    return 0;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--terminal-scrollbar-only")))
        return verifyTerminalScrollBarWithParentStyleSheet();
    if (app.arguments().size() == 4 && app.arguments().at(1) == QStringLiteral("--scheme-persistence-step"))
        return schemePersistenceStep(app.arguments().at(2), app.arguments().at(3).toInt());
    if (app.arguments().contains(QStringLiteral("--terminal-scheme-only"))) {
        // 人工视觉检查使用与主程序相同的 Ela 字体与图标初始化。
        eApp->init();
        QTranslator translator;
        const auto qm = qEnvironmentVariable("NOVATERM_SCHEME_TRANSLATION");
        if (!qm.isEmpty() && translator.load(qm))
            app.installTranslator(&translator);
        return verifyTerminalSchemeSettings();
    }
    int failures = verifyTerminalTabConnectionAction();
    failures += verifyTerminalScrollBarWithParentStyleSheet();
    failures += verifySchemePersistenceAcrossProcesses();
    failures += verifyTerminalSchemeSettings();
    failures += verifyAppBarCloseButtonClosesDialogSafely();
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
