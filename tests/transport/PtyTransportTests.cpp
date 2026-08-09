#include "session/LocalShellProfile.h"
#include "transport/ITransport.h"
#include "transport/LocalShellTransport.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

LocalShellConfig shellConfig(const QString& script)
{
    LocalShellConfig config;
    config.profile.name = QStringLiteral("Linux PTY test shell");
    config.profile.executable = QStringLiteral("/bin/sh");
    config.profile.arguments = {QStringLiteral("-c"), script};
    return config;
}

QByteArray collectUntilDisconnected(LocalShellTransport& transport,
                                    int timeoutMs = 5000)
{
    QByteArray output;
    QObject::connect(&transport, &ITransport::readyRead, &transport,
                     [&output](const QByteArray& data) { output.append(data); });
    QSignalSpy disconnected(&transport, &ITransport::disconnected);
    if (!transport.connectToHost())
        return {};
    if (disconnected.isEmpty())
        disconnected.wait(timeoutMs);
    return output;
}

} // namespace

class PtyTransportTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void workingDirectoryAndMergedEnvironmentReachChild();
    void exitReasonsAreReported();
    void closeIsAsynchronousAndIdempotent();
    void queuedInputAndResizeWork();
    void startFailureRollsBackAndCanRestart();
};

void PtyTransportTests::initTestCase()
{
    qRegisterMetaType<TransportExitReason>();
    QCOMPARE(LocalShellProfiles::platformDefault().environment.value(
                 QStringLiteral("TERM")),
             QStringLiteral("xterm-256color"));
}

void PtyTransportTests::workingDirectoryAndMergedEnvironmentReachChild()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto config = shellConfig(QStringLiteral("printf 'CWD=%s\\nENV=%s\\n' \"$PWD\" \"$NOVATERM_PTY_PROBE\""));
    config.workingDirectory = directory.path();
    config.profile.environment.insert(QStringLiteral("NOVATERM_PTY_PROBE"),
                                      QStringLiteral("profile"));
    config.environment.insert(QStringLiteral("NOVATERM_PTY_PROBE"),
                              QStringLiteral("session value"));
    LocalShellTransport transport;
    transport.setSessionConfig(config);
    const QByteArray output = collectUntilDisconnected(transport);
    QVERIFY2(output.contains("CWD=" + directory.path().toUtf8()), output.constData());
    QVERIFY2(output.contains("ENV=session value"), output.constData());
}

void PtyTransportTests::exitReasonsAreReported()
{
    {
        LocalShellTransport transport;
        transport.setSessionConfig(shellConfig(QStringLiteral("exit 7")));
        QSignalSpy exited(&transport, &ITransport::exited);
        QVERIFY(transport.connectToHost());
        QVERIFY(exited.wait(5000));
        QCOMPARE(exited.first()[0].toUInt(), 7U);
        QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
                 TransportExitReason::FailedExit);
    }
    {
        LocalShellTransport transport;
        transport.setSessionConfig(shellConfig(QStringLiteral("kill -SEGV $$")));
        QSignalSpy exited(&transport, &ITransport::exited);
        QVERIFY(transport.connectToHost());
        QVERIFY(exited.wait(5000));
        QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
                 TransportExitReason::Crash);
    }
}

void PtyTransportTests::closeIsAsynchronousAndIdempotent()
{
    LocalShellTransport transport;
    transport.setSessionConfig(shellConfig(
        QStringLiteral("trap '' HUP TERM; while :; do sleep 1; done")));
    QSignalSpy connected(&transport, &ITransport::connected);
    QSignalSpy exited(&transport, &ITransport::exited);
    QSignalSpy disconnected(&transport, &ITransport::disconnected);
    QVERIFY(transport.connectToHost());
    QVERIFY(connected.wait(5000));
    QElapsedTimer elapsed;
    elapsed.start();
    transport.disconnect();
    transport.disconnect();
    QVERIFY2(elapsed.elapsed() < 50, "disconnect blocked the event thread");
    QVERIFY(disconnected.wait(5000));
    QCOMPARE(disconnected.size(), 1);
    QCOMPARE(exited.size(), 1);
    QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
             TransportExitReason::UserClosed);
}

void PtyTransportTests::queuedInputAndResizeWork()
{
    LocalShellTransport transport;
    transport.setSessionConfig(shellConfig(QStringLiteral(
        "stty raw -echo; printf READY; dd bs=1024 count=256 iflag=fullblock 2>/dev/null | wc -c; stty size")));
    QByteArray output;
    bool inputSent = false;
    connect(&transport, &ITransport::readyRead, this,
            [&transport, &output, &inputSent](const QByteArray& data) {
        output.append(data);
        if (!inputSent && output.contains("READY")) {
            inputSent = true;
            transport.resizeTerminal(123, 47);
            transport.write(QByteArray(256 * 1024, 'I'));
        }
    });
    QSignalSpy connected(&transport, &ITransport::connected);
    QSignalSpy disconnected(&transport, &ITransport::disconnected);
    QVERIFY(transport.connectToHost());
    QVERIFY(connected.wait(5000));
    QVERIFY(disconnected.wait(10000));
    QVERIFY(inputSent);
    QVERIFY2(output.contains("262144"), output.constData());
    QVERIFY2(output.contains("47 123"), output.constData());
}

void PtyTransportTests::startFailureRollsBackAndCanRestart()
{
    LocalShellTransport transport;
    auto config = shellConfig(QStringLiteral("exit 0"));
    config.profile.executable = QStringLiteral("/definitely/missing/novaterm-shell");
    transport.setSessionConfig(config);
    QSignalSpy exited(&transport, &ITransport::exited);
    QSignalSpy disconnected(&transport, &ITransport::disconnected);
    QVERIFY(transport.connectToHost());
    QVERIFY(disconnected.wait(5000));
    QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
             TransportExitReason::StartFailed);

    exited.clear();
    disconnected.clear();
    transport.setSessionConfig(shellConfig(QStringLiteral("exit 0")));
    QVERIFY(transport.connectToHost());
    QVERIFY(disconnected.wait(5000));
    QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
             TransportExitReason::NormalExit);
}

QTEST_GUILESS_MAIN(PtyTransportTests)
#include "PtyTransportTests.moc"
