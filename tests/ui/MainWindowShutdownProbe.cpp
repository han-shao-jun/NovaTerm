/** @file MainWindowShutdownProbe.cpp @brief 主窗口析构期间 SSH 面板标题回调回归。 */
#include "ui/app/MainWindow.h"
#include "ui/widgets/SftpPanel.h"
#include "ui/widgets/SystemMonitorPanel.h"
#include "ui/widgets/SshPanelStack.h"
#include "service/ConfigManager.h"
#include "ElaApplication.h"
#include <QApplication>
#include <QDockWidget>
#include <QTemporaryDir>
#include <cstdio>
#include <memory>

template<class Panel> int checkShutdown(MainWindow& window, QApplication& app)
{
    auto* placeholder = window.findChild<Panel*>();
    auto* owner = static_cast<QMainWindow&>(window).centralWidget();
    if (!placeholder || !owner) return 2;
    auto* stack = static_cast<SshPanelStack<Panel>*>(placeholder->parentWidget());
    auto* transport = new SshTransport(SshConfig{}, owner);
    stack->setSessionContext(QStringLiteral("shutdown-fixture"), transport);
    auto* dock = qobject_cast<QDockWidget*>(stack->parentWidget());
    if (!dock || !dock->windowTitle().contains(QStringLiteral("shutdown-fixture"))) return 3;
    // 正常关闭单个会话时仍须更新标题、移除面板。
    delete transport;
    if (stack->count() != 1 || dock->windowTitle().contains(QStringLiteral("shutdown-fixture"))) return 4;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    transport = new SshTransport(SshConfig{}, owner);
    stack->setSessionContext(QStringLiteral("shutdown-fixture"), transport);
    QObject::connect(transport, &QObject::destroyed, &app, [] {
        std::puts("transport destroyed during window teardown");
        std::fflush(stdout);
    });
    return 0;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    eApp->init();
    QTemporaryDir configuration;
    if (!configuration.isValid()) return 1;
    ConfigManager::instance().load(configuration.filePath("config.json"));
    auto window = std::make_unique<MainWindow>();
    const int setup = app.arguments().contains(QStringLiteral("--monitor"))
        ? checkShutdown<SystemMonitorPanel>(*window, app)
        : checkShutdown<SftpPanel>(*window, app);
    if (setup) return setup;
    std::puts("destroying real MainWindow with retained SSH panel");
    std::fflush(stdout);
    window.reset();
    std::puts("shutdown completed");
    return 0;
}
