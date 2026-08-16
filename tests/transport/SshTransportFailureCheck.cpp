// SshTransport 失败路径控制台回归检查。
//
// 不依赖真实 SSH 服务器，验证最容易出错的部分：
//   • 无效配置 → connectToHost() 同步失败并给出 errorString；
//   • 连接被拒（127.0.0.1:1）→ 异步 errorOccurred 且线程正常回收、不崩溃。
//
// 运行：build/bin/novaterm_ssh_transport_check.exe
#include "transport/SshTransport.h"

#include <QCoreApplication>
#include <QTimer>

#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;

    // ── 用例 1：无效配置 → 同步失败 ─────────────────────────
    {
        SshConfig cfg;   // host / username 均为空
        SshTransport transport(cfg);
        // 未连接时不得接受资源监控等辅助命令，避免请求滞留到下一代连接。
        if (transport.executeCommand(1, QByteArrayLiteral("true")))
            ++failures;
        const bool ok = transport.connectToHost();
        std::printf("[invalid-config] connectToHost=%d error='%s'\n",
                    static_cast<int>(ok),
                    transport.errorString().toUtf8().constData());
        if (ok)
            ++failures;
    }

    // ── 用例 2：连接被拒 → 异步 errorOccurred + 线程回收 ─────
    {
        SshConfig cfg;
        cfg.host = QStringLiteral("127.0.0.1");
        cfg.port = 1;          // 无服务监听 → 立即 connection refused
        cfg.username = QStringLiteral("test");
        cfg.password = QStringLiteral("test");

        SshTransport transport(cfg);
        QObject::connect(&transport, &SshTransport::errorOccurred,
                         [&](const QString& error) {
            std::printf("[refused] errorOccurred='%s'\n",
                        error.toUtf8().constData());
            if (error.isEmpty())
                ++failures;
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
        });
        QObject::connect(&transport, &SshTransport::connected, [&]() {
            std::printf("[refused] UNEXPECTED connected\n");
            ++failures;
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
        });
        QObject::connect(&transport, &SshTransport::disconnected, [&]() {
            std::printf("[refused] UNEXPECTED disconnected\n");
            ++failures;
        });

        const bool ok = transport.connectToHost();
        std::printf("[refused] connectToHost=%d\n", static_cast<int>(ok));
        if (!ok) {
            ++failures;
            return 1;
        }

        QTimer::singleShot(15000, &app, [&]() {
            std::printf("[refused] TIMEOUT: no error within 15s\n");
            ++failures;
            app.quit();
        });
        app.exec();

        // 错误应已触发；此时析构会回收工作线程（不崩溃即通过）。
        std::printf("[refused] teardown ok, errorString='%s'\n",
                    transport.errorString().toUtf8().constData());
    }

    std::printf(failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL (%d)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
