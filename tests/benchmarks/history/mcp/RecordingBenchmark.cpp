// Isolated synthetic receive source. Never starts NovaTerm, a transport, or reconnects devices.
#include "recording/HistoryChunkStore.h"
#include "recording/HistoryService.h"
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <ctime>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

using namespace NovaTerm::Recording;
static double cpuSeconds() {
#ifdef Q_OS_WIN
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    ULARGE_INTEGER k{}, u{}; k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return double(k.QuadPart + u.QuadPart) / 1e7;
#else
    rusage usage{}; if (getrusage(RUSAGE_SELF, &usage)) return -1;
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec)/1e6;
#endif
}
static quint64 peakBytes() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS memory{}; GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
    return quint64(memory.PeakWorkingSetSize);
#else
    rusage usage{}; getrusage(RUSAGE_SELF, &usage);
#ifdef Q_OS_MACOS
    return quint64(usage.ru_maxrss);
#else
    return quint64(usage.ru_maxrss) * 1024;
#endif
#endif
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); if (app.arguments().size() != 3 || (app.arguments()[1] != "on" && app.arguments()[1] != "off")) return 2;
    const bool enabled = app.arguments()[1] == "on";
    QTemporaryDir dir; QWidget window; window.resize(640, 300); window.show();
    HistoryService service(dir.path()); const QUuid session = QUuid::createUuid();
    if (enabled) {
        QEventLoop wait; bool ok = false; service.start({session, 1}, [&](auto r) { ok = r.success; wait.quit(); });
        QTimer::singleShot(10000, &wait, &QEventLoop::quit); wait.exec(); if (!ok) return 3;
    }
    const QByteArray bytes = QByteArray::fromHex("1b5b33316d5f5245434f5244494e470d0a00ff").repeated(4096).left(64 * 1024);
    QEventLoop loop; QTimer source, gui; source.setTimerType(Qt::PreciseTimer); source.setInterval(16);
    gui.setTimerType(Qt::PreciseTimer); gui.setInterval(5);
    QElapsedTimer time; time.start(); qint64 previous = 0; std::vector<qint64> delays;
    quint64 accepted = 0, dropped = 0; int count = 0; const auto cpuStart = cpuSeconds();
    QObject::connect(&gui, &QTimer::timeout, &gui, [&] { const auto t = time.nsecsElapsed(); if (previous) delays.push_back(t-previous); previous = t; window.update(); });
    QObject::connect(&source, &QTimer::timeout, &source, [&] {
        const auto result = service.submit(session, 1, bytes);
        if (!enabled || result == SubmitResult::Accepted) accepted += quint64(bytes.size()); else dropped += quint64(bytes.size());
        if (++count == 1024) { source.stop(); gui.stop(); loop.quit(); }
    });
    source.start(); gui.start(); loop.exec(); const double elapsed = double(time.nsecsElapsed()) / 1e9;
    const double cpu = cpuSeconds() - cpuStart;
    if (enabled) {
        service.stop({session, 1}); QEventLoop drain; QTimer poll; poll.setInterval(20);
        QObject::connect(&poll, &QTimer::timeout, &poll, [&] { if (!service.captures().empty() && !service.captures().front().active) drain.quit(); });
        QTimer::singleShot(30000, &drain, &QEventLoop::quit); poll.start(); drain.exec();
    }
    std::sort(delays.begin(), delays.end()); quint64 stored = 0;
    for (const auto& f : QDir(dir.path()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto sum = [&](const QString& path, const auto& self) -> quint64 {
            quint64 total = 0; for (const auto& entry : QDir(path).entryInfoList(QDir::AllEntries|QDir::NoDotAndDotDot)) total += entry.isDir() ? self(entry.absoluteFilePath(), self) : quint64(entry.size()); return total;
        }; stored += sum(f.absoluteFilePath(), sum);
    }
    QJsonObject result{{"mode", enabled ? "on" : "off"}, {"source_bytes", double(1024ull * quint64(bytes.size()))},
        {"accepted_bytes", double(accepted)}, {"dropped_bytes", double(dropped)}, {"stored_bytes", double(stored)},
        {"elapsed_s", elapsed}, {"source_mib_s", (1024.0 * double(bytes.size()) / (1024*1024)) / elapsed},
        {"event_loop_interval_p95_ms", delays.empty() ? 0 : double(delays[size_t(double(delays.size()-1)*0.95)]) / 1e6},
        {"cpu_s", cpu}, {"peak_process_bytes", double(peakBytes())}, {"note", "synthetic QWidget repaint / event-loop interval; not real terminal GUI P95"}};
    QFile out(app.arguments()[2]); if (!out.open(QIODevice::WriteOnly)) return 4; out.write(QJsonDocument(result).toJson());
    if (enabled && (dropped || service.captures().empty() || service.captures().front().integrity != Integrity::Lossless)) return 5;
    return 0;
}
