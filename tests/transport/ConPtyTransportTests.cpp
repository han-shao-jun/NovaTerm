#include "platform/windows/conpty/ConPtySession.h"
#include "session/LocalShellProfile.h"
#include "transport/ITransport.h"
#include "transport/LocalShellTransport.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <limits>

namespace {

QString childExecutable()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("novaterm_conpty_test_child.exe"));
}

LocalShellConfig childConfig(QStringList arguments)
{
    LocalShellConfig config;
    config.profile.name = QStringLiteral("ConPTY test child");
    config.profile.executable = childExecutable();
    config.profile.arguments = std::move(arguments);
    return config;
}

int currentThreadCount()
{
    NovaTerm::Windows::WinHandle snapshot(
        CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
    if (!snapshot)
        return -1;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    SetLastError(ERROR_SUCCESS);
    if (Thread32First(snapshot.get(), &entry)) {
        do {
            if (entry.th32OwnerProcessID == GetCurrentProcessId())
                ++count;
        } while (Thread32Next(snapshot.get(), &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            return -1;
    } else {
        return -1;
    }
    return count;
}

DWORD currentHandleCount()
{
    DWORD count = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &count) ? count : 0;
}

int currentTestChildProcessCount()
{
    NovaTerm::Windows::WinHandle snapshot(
        CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot)
        return -1;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    SetLastError(ERROR_SUCCESS);
    if (Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile,
                         L"novaterm_conpty_test_child.exe") == 0) {
                ++count;
            }
        } while (Process32NextW(snapshot.get(), &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            return -1;
    } else {
        return -1;
    }
    return count;
}

QByteArray collectUntilDisconnected(LocalShellTransport& transport,
                                    int timeoutMs = 10000)
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

class ConPtyTransportTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void profilesAndEnvironmentMerge();
    void windowsArgumentRoundTrip();
    void workingDirectoryAndEnvironmentReachChild();
    void exitReasonsAndIdempotentClose();
    void duplexLoadAndBackpressure();
    void inputDispatchOverloadIsBounded();
    void latestResizeWins();
    void startFailureRollsBackAndCanRestart();
    void injectedStartupStagesRollBack();
    void repeatedLifecycleReturnsResourcesToBaseline();
};

void ConPtyTransportTests::initTestCase()
{
    QVERIFY2(QFileInfo::exists(childExecutable()), qPrintable(childExecutable()));
    qRegisterMetaType<TransportExitReason>();
}

void ConPtyTransportTests::profilesAndEnvironmentMerge()
{
    const auto profiles = LocalShellProfiles::defaults(QStringLiteral("C:/missing"));
    QCOMPARE(profiles.size(), 4);
    QCOMPARE(profiles[0].executable, QStringLiteral("cmd.exe"));
    QCOMPARE(profiles[1].executable, QStringLiteral("powershell.exe"));
    QCOMPARE(profiles[2].executable, QStringLiteral("pwsh.exe"));
    QCOMPARE(profiles[3].executable, QStringLiteral("wsl.exe"));
    QVERIFY(profiles[0].environment.value(QStringLiteral("TERM")).isEmpty());
    QCOMPARE(profiles[3].environment.value(QStringLiteral("TERM")),
             QStringLiteral("xterm-256color"));

    const auto distro = LocalShellProfiles::wslDistribution(QStringLiteral("Ubuntu Test"));
    QCOMPARE(distro.arguments,
             QStringList({QStringLiteral("--distribution"), QStringLiteral("Ubuntu Test")}));

    LocalShellConfig config;
    config.profile = profiles[0];
    config.profile.environment.insert(QStringLiteral("NOVATERM_MERGE"),
                                      QStringLiteral("profile"));
    config.environment.insert(QStringLiteral("NOVATERM_MERGE"),
                              QStringLiteral("session"));
    QCOMPARE(config.mergedEnvironment().value(QStringLiteral("NOVATERM_MERGE")),
             QStringLiteral("session"));
    const QByteArray block = NovaTerm::Windows::environmentBlockForTest(
        config.mergedEnvironment());
    QVERIFY(block.size() >= 2 * qsizetype(sizeof(wchar_t)));
    const auto* end = reinterpret_cast<const wchar_t*>(block.constData()
                                                       + block.size());
    QCOMPARE(end[-1], wchar_t(0));
    QCOMPARE(end[-2], wchar_t(0));
}

void ConPtyTransportTests::windowsArgumentRoundTrip()
{
    const QString executable = childExecutable();
    const QStringList arguments = {
        QStringLiteral("argv"), QStringLiteral("with space"), QString(),
        QStringLiteral("embedded\"quote"), QStringLiteral("trailing\\"),
        QStringLiteral("two\\\\\"forms")};
    const QString commandLine = NovaTerm::Windows::buildWindowsCommandLine(
        executable, arguments);
    int argc = 0;
    const std::wstring wide = commandLine.toStdWString();
    LPWSTR* parsed = CommandLineToArgvW(wide.c_str(), &argc);
    QVERIFY(parsed != nullptr);
    QCOMPARE(argc, arguments.size() + 1);
    QCOMPARE(QString::fromWCharArray(parsed[0]), executable);
    for (int index = 0; index < arguments.size(); ++index)
        QCOMPARE(QString::fromWCharArray(parsed[index + 1]), arguments[index]);
    LocalFree(parsed);

    LocalShellTransport transport;
    transport.setSessionConfig(childConfig(arguments));
    const QByteArray output = collectUntilDisconnected(transport);
    for (const QString& argument : arguments.mid(1)) {
        const QByteArray utf8 = argument.toUtf8();
        QVERIFY2(output.contains(QByteArray::number(utf8.size()) + ':' + utf8),
                 output.constData());
    }
}

void ConPtyTransportTests::workingDirectoryAndEnvironmentReachChild()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto config = childConfig({QStringLiteral("probe")});
    config.workingDirectory = directory.path();
    config.profile.environment.insert(QStringLiteral("NOVATERM_CONPTY_PROBE"),
                                      QStringLiteral("profile"));
    config.environment.insert(QStringLiteral("NOVATERM_CONPTY_PROBE"),
                              QStringLiteral("session value"));
    LocalShellTransport transport;
    transport.setSessionConfig(config);
    const QByteArray output = collectUntilDisconnected(transport);
    QVERIFY2(output.contains("ENV=session value"), output.constData());
    QVERIFY2(output.contains(QDir::toNativeSeparators(directory.path()).toUtf8()),
             output.constData());
}

void ConPtyTransportTests::exitReasonsAndIdempotentClose()
{
    {
        LocalShellTransport transport;
        transport.setSessionConfig(childConfig({QStringLiteral("exit"), QStringLiteral("0")}));
        QSignalSpy exited(&transport, &ITransport::exited);
        QSignalSpy disconnected(&transport, &ITransport::disconnected);
        QVERIFY(transport.connectToHost());
        QVERIFY(exited.wait(5000));
        QCOMPARE(exited.size(), 1);
        QCOMPARE(exited.first()[0].toUInt(), 0U);
        QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
                 TransportExitReason::NormalExit);
        if (disconnected.isEmpty())
            QVERIFY(disconnected.wait(1000));
        QCOMPARE(disconnected.size(), 1);
    }
    {
        LocalShellTransport transport;
        transport.setSessionConfig(childConfig({QStringLiteral("exit"), QStringLiteral("7")}));
        QSignalSpy exited(&transport, &ITransport::exited);
        QVERIFY(transport.connectToHost());
        QVERIFY(exited.wait(5000));
        QCOMPARE(exited.first()[0].toUInt(), 7U);
        QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
                 TransportExitReason::FailedExit);
    }
    {
        LocalShellTransport transport;
        transport.setSessionConfig(childConfig({QStringLiteral("crash")}));
        QSignalSpy exited(&transport, &ITransport::exited);
        QVERIFY(transport.connectToHost());
        QVERIFY(exited.wait(5000));
        QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
                 TransportExitReason::Crash);
    }
    {
        LocalShellTransport transport;
        transport.setSessionConfig(childConfig({QStringLiteral("hold")}));
        QSignalSpy connected(&transport, &ITransport::connected);
        QSignalSpy exited(&transport, &ITransport::exited);
        QSignalSpy disconnected(&transport, &ITransport::disconnected);
        bool wrongThread = false;
        connect(&transport, &ITransport::disconnected, this, [&wrongThread] {
            wrongThread = QThread::currentThread() != qApp->thread();
        });
        connect(&transport, &ITransport::exited, this,
                [&wrongThread](quint32, TransportExitReason) {
            wrongThread = wrongThread
                || QThread::currentThread() != qApp->thread();
        });
        connect(&transport, &ITransport::errorOccurred, this,
                [&wrongThread](const QString&) {
            wrongThread = wrongThread
                || QThread::currentThread() != qApp->thread();
        });
        QVERIFY(transport.connectToHost());
        QVERIFY(connected.wait(5000));
        int heartbeats = 0;
        QTimer heartbeat;
        heartbeat.setInterval(1);
        connect(&heartbeat, &QTimer::timeout, this, [&heartbeats] { ++heartbeats; });
        heartbeat.start();
        QElapsedTimer callTime;
        callTime.start();
        transport.disconnect();
        transport.disconnect();
        transport.write(QByteArrayLiteral("after close"));
        transport.resizeTerminal(100, 40);
        QVERIFY2(callTime.elapsed() < 50, "disconnect blocked the GUI thread");
        QVERIFY(disconnected.wait(10000));
        QTRY_VERIFY_WITH_TIMEOUT(heartbeats > 0, 100);
        QCOMPARE(disconnected.size(), 1);
        QCOMPARE(exited.size(), 1);
        QVERIFY(exited.first()[0].toUInt()
                != (std::numeric_limits<quint32>::max)());
        QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
                 TransportExitReason::UserClosed);
        QVERIFY(!wrongThread);
    }
}

void ConPtyTransportTests::duplexLoadAndBackpressure()
{
    constexpr qsizetype InputBytes = 2 * 1024 * 1024;
    constexpr qsizetype OutputBytes = 12 * 1024 * 1024;
    QByteArray input(InputBytes, 'I');
    std::uint64_t expectedHash = 1469598103934665603ULL;
    for (const char value : input) {
        expectedHash ^= static_cast<unsigned char>(value);
        expectedHash *= 1099511628211ULL;
    }

    LocalShellTransport transport;
    transport.setSessionConfig(childConfig({QStringLiteral("duplex"),
        QString::number(InputBytes), QString::number(OutputBytes)}));
    QSignalSpy connected(&transport, &ITransport::connected);
    QSignalSpy disconnected(&transport, &ITransport::disconnected);
    QByteArray output;
    bool paused = false;
    bool readPauseActive = false;
    bool deliveredWhilePaused = false;
    connect(&transport, &ITransport::readyRead, this,
            [&](const QByteArray& data) {
        output.append(data);
        if (readPauseActive)
            deliveredWhilePaused = true;
        if (!paused && output.size() >= 512 * 1024) {
            paused = true;
            readPauseActive = true;
            transport.setReadPaused(true);
            QTimer::singleShot(150, &transport, [&transport, &readPauseActive] {
                readPauseActive = false;
                transport.setReadPaused(false);
            });
        }
    });
    QVERIFY(transport.connectToHost());
    QVERIFY(connected.wait(5000));
    transport.write(input);
    QVERIFY(disconnected.wait(20000));
    QVERIFY(paused);
    QVERIFY(!deliveredWhilePaused);
    QVERIFY2(output.count('O') >= OutputBytes, QByteArray::number(output.size()).constData());
    const QByteArray marker = QByteArray("READ=") + QByteArray::number(InputBytes)
        + ";HASH=" + QByteArray::number(expectedHash);
    QVERIFY2(output.contains(marker), output.right(256).constData());
}

void ConPtyTransportTests::inputDispatchOverloadIsBounded()
{
    LocalShellTransport transport;
    transport.setSessionConfig(childConfig({QStringLiteral("hold")}));
    QSignalSpy connected(&transport, &ITransport::connected);
    QSignalSpy errors(&transport, &ITransport::errorOccurred);
    QSignalSpy disconnected(&transport, &ITransport::disconnected);
    QVERIFY(transport.connectToHost());
    QVERIFY(connected.wait(5000));
    transport.write(QByteArray(5 * 1024 * 1024, 'X'));
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.first().first().toString().contains(QStringLiteral("capacity")));
    transport.disconnect();
    QVERIFY(disconnected.wait(5000));
}

void ConPtyTransportTests::latestResizeWins()
{
    LocalShellTransport transport;
    transport.setSessionConfig(childConfig({QStringLiteral("size")}));
    QByteArray output;
    connect(&transport, &ITransport::readyRead, this,
            [&output](const QByteArray& data) { output.append(data); });
    QSignalSpy connected(&transport, &ITransport::connected);
    QSignalSpy disconnected(&transport, &ITransport::disconnected);
    QVERIFY(transport.connectToHost());
    QVERIFY(connected.wait(5000));
    for (int index = 0; index < 200; ++index)
        transport.resizeTerminal(80 + index % 30, 24 + index % 10);
    transport.resizeTerminal(123, 47);
    QVERIFY(disconnected.wait(5000));
    QVERIFY2(output.contains("SIZE=123x47"), output.constData());
}

void ConPtyTransportTests::startFailureRollsBackAndCanRestart()
{
    LocalShellTransport transport;
    auto invalid = childConfig({QStringLiteral("exit"), QStringLiteral("0")});
    invalid.profile.executable = QStringLiteral("Z:/missing/NovaTerm-no-such-program.exe");
    transport.setSessionConfig(invalid);
    QSignalSpy exited(&transport, &ITransport::exited);
    QSignalSpy disconnected(&transport, &ITransport::disconnected);
    QVERIFY(transport.connectToHost());
    QVERIFY(disconnected.wait(5000));
    QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
             TransportExitReason::StartFailed);

    transport.setSessionConfig(childConfig({QStringLiteral("exit"), QStringLiteral("0")}));
    exited.clear();
    disconnected.clear();
    QVERIFY(transport.connectToHost());
    QVERIFY(disconnected.wait(5000));
    QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
             TransportExitReason::NormalExit);
}

void ConPtyTransportTests::injectedStartupStagesRollBack()
{
    using NovaTerm::Windows::ConPtyFailureStage;
    const QList<ConPtyFailureStage> stages = {
        ConPtyFailureStage::InputPipe,
        ConPtyFailureStage::OutputPipe,
        ConPtyFailureStage::PseudoConsole,
        ConPtyFailureStage::AttributeProbe,
        ConPtyFailureStage::AttributeInitialize,
        ConPtyFailureStage::AttributeUpdate,
        ConPtyFailureStage::ProcessCreate,
        ConPtyFailureStage::JobCreate,
        ConPtyFailureStage::JobConfigure,
        ConPtyFailureStage::JobAssign,
        ConPtyFailureStage::ResumeThread,
        ConPtyFailureStage::WorkerThreads,
        ConPtyFailureStage::ReaderThread,
        ConPtyFailureStage::WriterThread,
        ConPtyFailureStage::ProcessWaitThread,
    };
    const DWORD baselineHandles = currentHandleCount();
    const int baselineProcesses = currentTestChildProcessCount();
    QVERIFY(baselineProcesses >= 0);
    for (const ConPtyFailureStage stage : stages) {
        LocalShellTransport transport;
        transport.setSessionConfig(childConfig({QStringLiteral("exit"), QStringLiteral("0")}));
        QSignalSpy exited(&transport, &ITransport::exited);
        QSignalSpy disconnected(&transport, &ITransport::disconnected);
        NovaTerm::Windows::setConPtyFailureStageForTest(stage);
        QVERIFY(transport.connectToHost());
        QVERIFY(disconnected.wait(5000));
        QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
                 TransportExitReason::StartFailed);
        NovaTerm::Windows::setConPtyFailureStageForTest(ConPtyFailureStage::None);

        exited.clear();
        disconnected.clear();
        QVERIFY(transport.connectToHost());
        QVERIFY(disconnected.wait(5000));
        QCOMPARE(qvariant_cast<TransportExitReason>(exited.first()[1]),
                 TransportExitReason::NormalExit);
        QTRY_COMPARE_WITH_TIMEOUT(currentTestChildProcessCount(),
                                  baselineProcesses, 5000);
    }
    NovaTerm::Windows::setConPtyFailureStageForTest(ConPtyFailureStage::None);
    QTRY_VERIFY_WITH_TIMEOUT(currentHandleCount() <= baselineHandles, 5000);
}

void ConPtyTransportTests::repeatedLifecycleReturnsResourcesToBaseline()
{
    {
        LocalShellTransport warmup;
        warmup.setSessionConfig(childConfig({QStringLiteral("exit"), QStringLiteral("0")}));
        QSignalSpy disconnected(&warmup, &ITransport::disconnected);
        QVERIFY(warmup.connectToHost());
        QVERIFY(disconnected.wait(5000));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    const DWORD baselineHandles = currentHandleCount();
    const int baselineThreads = currentThreadCount();
    const int baselineProcesses = currentTestChildProcessCount();
    QVERIFY(baselineHandles > 0);
    QVERIFY(baselineThreads > 0);
    QVERIFY(baselineProcesses >= 0);

    for (int iteration = 0; iteration < 200; ++iteration) {
        LocalShellTransport transport;
        transport.setSessionConfig(childConfig({QStringLiteral("exit"), QStringLiteral("0")}));
        QSignalSpy disconnected(&transport, &ITransport::disconnected);
        QVERIFY2(transport.connectToHost(), qPrintable(transport.errorString()));
        QVERIFY2(disconnected.wait(5000), qPrintable(QString::number(iteration)));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    QTRY_COMPARE_WITH_TIMEOUT(currentHandleCount(), baselineHandles, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(currentThreadCount(), baselineThreads, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(currentTestChildProcessCount(), baselineProcesses, 5000);
}

QTEST_GUILESS_MAIN(ConPtyTransportTests)
#include "ConPtyTransportTests.moc"
