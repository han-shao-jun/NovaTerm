#include "HistoryService.h"
#include "HistoryRecorder.h"
#include "HistoryChunkStore.h"
#include "HistoryStorageIo.h"
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QPointer>
#include <condition_variable>
#include <deque>
#include <thread>
#include <algorithm>
#include <atomic>

namespace NovaTerm::Recording {
namespace {
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
qint64 leaseTime() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
quint64 directoryBytes(const QString& path, const IoProbe& probe = {}) {
    if (probe && !probe(IoPoint::DirectoryScan)) return 0;
    if (!StorageIo::safePath(path)) return 0;
    quint64 result = 0;
    for (const auto& f : QDir(path).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden))
        result += f.isDir() ? directoryBytes(f.absoluteFilePath(), probe) : quint64(f.size());
    return result;
}
}
struct HistoryService::Worker {
    enum class Kind { Start, Remove, Retain, Configure, Read };
    struct Job { Kind kind; quint64 request{0}; RecordingIntent intent; HistoryCaptureId id; bool keep{false}; RetentionSettings settings; HistoryRange range; };
    struct Completion { quint64 request; Job job; RecordingResult result; ReadResult read; std::shared_ptr<HistoryRecorder> recorder; };
    struct Active { std::unique_ptr<HistoryChunkStore> store; std::shared_ptr<HistoryRecorder> recorder; };
    const QString root;
    const QUuid boot{QUuid::createUuid()};
    StorageIo::DirectoryGuard rootGuard;
    std::unique_ptr<QLockFile> ownerLock;
    std::mutex mutex;
    std::condition_variable wake, exited;
    std::deque<Job> jobs;
    std::deque<Completion> completions;
    std::vector<CaptureDescriptor> snapshot;
    std::map<QString, CaptureDescriptor> catalog;
    std::map<QString, Active> active;
    std::map<QString, std::unique_ptr<HistoryChunkStore>> readable;
    std::shared_ptr<QueueBudget> budget{std::make_shared<QueueBudget>()};
    HistoryRetentionPolicy retention;
    IoProbe probe;
    bool stopping{false}, finished{false}, dirty{false};
    std::atomic<bool> cancelled{false};
    bool loadComplete{false}, publishedReady{false}, settingsValid{true};
    bool available{false}, catalogOverflow{false};
    QString error;
    RetentionSettings publishedSettings;
    explicit Worker(QString path, IoProbe ioProbe) : root(std::move(path)), probe([this, external = std::move(ioProbe)](IoPoint point) {
        return !cancelled.load() && (!external || external(point)) && !cancelled.load();
    }) {}
    bool enqueue(Job job) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || jobs.size() + completions.size() >= 64) return false;
        jobs.push_back(std::move(job)); wake.notify_one(); return true;
    }
    void publish(std::optional<Completion> completion = std::nullopt) {
        std::vector<CaptureDescriptor> values;
        for (const auto& entry : catalog) values.push_back(entry.second);
        std::lock_guard<std::mutex> lock(mutex);
        snapshot = std::move(values); publishedSettings = retention.settings(); publishedReady = loadComplete; dirty = true;
        if (completion) completions.push_back(std::move(*completion));
    }
    void reportError(const QString& text) { std::lock_guard<std::mutex> lock(mutex); error = text; dirty = true; }
    void load() {
        if (!StorageIo::privateDirectory(root) || !StorageIo::safeTree(root) || !rootGuard.lock(root)) { reportError("Unsafe history storage directory"); return; }
        ownerLock = std::make_unique<QLockFile>(root + "/history.lock"); ownerLock->setStaleLockTime(0);
        if (!ownerLock->tryLock(0)) { reportError("History storage is already owned by another process"); return; }
        available = true;
        QFile settings(root + "/retention.json");
        if (settings.exists()) {
            QJsonParseError parseError; QJsonDocument document;
            if (settings.open(QIODevice::ReadOnly) && settings.size() <= 16384) document = QJsonDocument::fromJson(settings.readAll(), &parseError);
            const auto o = document.object();
            RetentionSettings s; s.globalBytes = o["global"].toString().toULongLong(); s.captureBytes = o["capture"].toString().toULongLong();
            s.retentionMs = o["ttl"].toString().toLongLong();
            settingsValid = !document.isNull() && document.isObject() && parseError.error == QJsonParseError::NoError && retention.configure(s);
            if (!settingsValid) reportError("Invalid history retention settings; apply valid settings before recording");
        }
        for (const auto& directory : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (cancelled.load()) break;
            if (catalog.size() >= 256) { catalogOverflow = true; reportError("History capture catalog limit reached (256)"); break; }
            CaptureDescriptor d; d.id = {QUuid(directory)}; if (!d.id.valid() || d.id.text() != directory) continue;
            const bool kept = QFileInfo::exists(root + "/" + directory + "/retained");
            auto store = std::make_unique<HistoryChunkStore>(root, d, StorageOptions{}, probe);
            if (store->recover()) {
                d = store->descriptor(); d.bytes = directoryBytes(store->directory(), probe);
                readable.emplace(d.id.text(), std::move(store));
            } else { d.stopReason = store->error(); d.bytes = directoryBytes(root + "/" + directory, probe); }
            d.retained = kept; catalog[d.id.text()] = d;
        }
        if (settingsValid && !cancelled.load()) cleanup(0);
    }
    bool cleanup(quint64 reserve) {
        quint64 total = 0; for (const auto& entry : catalog) total += entry.second.bytes;
        std::vector<CaptureDescriptor> oldest; for (const auto& entry : catalog) if (!entry.second.active && !entry.second.retained) oldest.push_back(entry.second);
        std::sort(oldest.begin(), oldest.end(), [](const auto& a, const auto& b) { return a.createdMs < b.createdMs; });
        const auto settings = retention.settings();
        for (const auto& d : oldest) {
            const bool expired = d.createdMs > 0 && nowMs() - d.createdMs >= settings.retentionMs;
            if (!expired && total + reserve <= settings.globalBytes) continue;
            if (retention.leased(d.id, leaseTime())) continue;
            readable.erase(d.id.text()); // Close every handle before cleanup, never let a cache pin storage.
            if (retention.removeCapture(root, d.id, leaseTime(), false)) { total -= d.bytes; catalog.erase(d.id.text()); }
        }
        return total + reserve <= settings.globalBytes;
    }
    void runJob(const Job& job) {
        Completion done{job.request, job, {false, job.id, {}}, {}, {}};
        if (!available) {
            done.result.error = "history storage unavailable"; done.read.missing = true; done.read.error = done.result.error;
            std::lock_guard<std::mutex> lock(mutex); completions.push_back(std::move(done)); return;
        }
        if (job.kind == Kind::Start) {
            bool duplicate = false;
            for (const auto& entry : active) if (entry.second.store->descriptor().sessionId == job.intent.sessionId) duplicate = true;
            if (!settingsValid) done.result.error = "Invalid history retention settings";
            else if (duplicate || active.size() >= 16 || catalogOverflow || catalog.size() >= 256 || !cleanup(1024 * 1024)) done.result.error = "recording active or storage/queue limit reached";
            else {
                CaptureDescriptor d; d.id = job.id; d.sessionId = job.intent.sessionId; d.connectionGeneration = job.intent.connectionGeneration;
                d.bootId = boot; d.createdMs = nowMs();
                auto store = std::make_unique<HistoryChunkStore>(root, d, StorageOptions{4 * 1024 * 1024, retention.settings().captureBytes}, probe);
                if (store->open()) {
                    done.recorder = std::make_shared<HistoryRecorder>(d.id, d.sessionId, d.connectionGeneration, budget);
                    auto snapshotValue = store->descriptor(); snapshotValue.bytes = directoryBytes(store->directory(), probe);
                    catalog[d.id.text()] = snapshotValue;
                    active.emplace(d.id.text(), Active{std::move(store), done.recorder}); done.result.success = true;
                } else done.result.error = store->error();
            }
        } else if (job.kind == Kind::Configure) {
            const QJsonObject object{{"global", QString::number(job.settings.globalBytes)}, {"capture", QString::number(job.settings.captureBytes)}, {"ttl", QString::number(job.settings.retentionMs)}};
            if (job.settings.valid() && StorageIo::replaceDurable(root + "/retention.json", QJsonDocument(object).toJson()) && retention.configure(job.settings)) {
                for (auto& entry : active) entry.second.store->setCaptureLimit(job.settings.captureBytes);
                settingsValid = true; reportError({});
                done.result.success = true; cleanup(0);
            } else done.result.error = "invalid retention settings or storage unavailable";
        } else {
            const auto it = catalog.find(job.id.text());
            if (it == catalog.end()) { done.result.error = "capture missing or expired"; done.read.missing = true; done.read.error = done.result.error; }
            else if (job.kind == Kind::Remove) {
                if (!it->second.active && !retention.leased(job.id, leaseTime())) readable.erase(job.id.text());
                done.result.success = retention.removeCapture(root, job.id, leaseTime(), it->second.active);
                if (done.result.success) catalog.erase(it); else done.result.error = "capture active, leased, or unsafe";
            } else if (job.kind == Kind::Retain) {
                const auto path = root + "/" + job.id.text() + "/retained";
                if (StorageIo::safeTree(root + "/" + job.id.text())) {
                    done.result.success = job.keep ? StorageIo::replaceDurable(path, "retained\n") : StorageIo::removePrivateFile(path);
                    if (done.result.success) it->second.retained = job.keep;
                }
            } else if (job.kind == Kind::Read) {
                const auto lease = retention.acquire(job.id, leaseTime());
                if (!lease) done.read.error = "read lease limit reached";
                else {
                    const auto live = active.find(job.id.text());
                    if (live != active.end()) done.read = live->second.store->readCommitted(job.range);
                    else {
                        const auto ready = readable.find(job.id.text());
                        if (ready != readable.end()) done.read = ready->second->readCommitted(job.range);
                        else { done.read.missing = true; done.read.error = "capture unavailable; restart recovery required"; }
                    }
                    if (leaseTime() >= lease->expiresMs) { done.read = {}; done.read.missing = true; done.read.error = "read lease expired"; }
                    retention.release(*lease);
                }
            }
        }
        publish(std::move(done));
    }
    void run() {
        load(); loadComplete = true; publish();
        qint64 lastCleanup = leaseTime();
        for (;;) {
            std::optional<Job> job; bool stop;
            { std::unique_lock<std::mutex> lock(mutex);
              bool hasData = false;
              for (const auto& entry : active) if (!entry.second.recorder->drained() || !entry.second.recorder->accepting()) hasData = true;
              if (jobs.empty() && !stopping && !hasData) wake.wait_for(lock, std::chrono::milliseconds(active.empty() ? 500 : 20));
              stop = stopping;
              if (!jobs.empty() && !stop) { job = std::move(jobs.front()); jobs.pop_front(); }
            }
            if (cancelled.load()) break;
            if (job) runJob(*job);
            if (settingsValid && leaseTime() - lastCleanup >= 60000) { cleanup(0); publish(); lastCleanup = leaseTime(); }
            for (auto it = active.begin(); it != active.end();) {
                auto& a = it->second;
                if (stop) a.recorder->stop("application_shutdown");
                auto batch = a.recorder->takeBatch(); bool ok = true;
                QString quotaFailure;
                if (!batch.empty()) {
                    quint64 reserve = 0; for (const auto& record : batch) reserve += quint64(record.payload.size()) + 4096;
                    if (catalog[it->first].bytes + reserve > retention.settings().captureBytes) quotaFailure = "capture_quota";
                    else if (!cleanup(reserve)) quotaFailure = "global_quota";
                    if (!quotaFailure.isEmpty()) { a.recorder->stop(quotaFailure); ok = false; }
                    else ok = a.store->append(batch) && a.store->commit();
                }
                bool done = !ok || (!a.recorder->accepting() && a.recorder->drained());
                auto d = a.store->descriptor(); d.retained = catalog[it->first].retained;
                if (done) {
                    if (ok) ok = a.store->seal(a.recorder->stopReason());
                    if (!ok) { a.recorder->stop("storage_failure");
                        d.stopReason = a.store->error().isEmpty() ? quotaFailure : a.store->error(); d.integrity = Integrity::UnknownTail;
                    } else { const bool keep = d.retained; d = a.store->descriptor(); d.retained = keep; }
                    d.active = false;
                }
                if (!batch.empty() || done) { d.bytes = directoryBytes(a.store->directory(), probe); catalog[it->first] = d; }
                if (done) { readable[it->first] = std::move(a.store); it = active.erase(it); } else ++it;
                if (!batch.empty() || done) publish();
            }
            if (stop && active.empty()) break;
        }
        // SQL connections must close on the thread that created them, even when
        // the service's shared mailbox is finally released on the GUI thread.
        readable.clear(); active.clear(); ownerLock.reset();
        std::lock_guard<std::mutex> lock(mutex); finished = true; exited.notify_all();
    }
};
HistoryService::HistoryService(QString root, QObject* parent, IoProbe probe) : QObject(parent), _worker(std::make_unique<Worker>(std::move(root), std::move(probe))) {
    _poll.setInterval(50); connect(&_poll, &QTimer::timeout, this, &HistoryService::poll); _poll.start();
    _thread = std::thread([worker = _worker.get()] { worker->run(); });
}
HistoryService::~HistoryService() {
    _poll.stop(); for (auto& entry : _producers) entry.second.recorder->stop("application_shutdown");
    std::unique_lock<std::mutex> lock(_worker->mutex); _worker->stopping = true; _worker->wake.notify_one();
    // Give accepted data a short drain window, then cancel at I/O boundaries.
    // Never let Qt SQL resources outlive QApplication. An in-flight OS call may
    // delay final process exit: detaching cannot safely provide a hard deadline.
    _worker->exited.wait_for(lock, std::chrono::milliseconds(500), [this] { return _worker->finished; });
    _worker->cancelled = true; _worker->wake.notify_one(); lock.unlock();
    if (_thread.joinable()) _thread.join();
}
bool HistoryService::start(RecordingIntent intent, Done callback) {
    if (intent.sessionId.isNull() || _callbacks.size() + _readCallbacks.size() >= 64) return false;
    Worker::Job job{Worker::Kind::Start}; job.request = ++_request; job.intent = intent; job.id = {QUuid::createUuid()};
    if (!_worker->enqueue(job)) return false;
    _callbacks.emplace(job.request, std::move(callback)); _starts.emplace(job.request, PendingStart{intent}); return true;
}
void HistoryService::stop(RecordingIntent intent, QString reason) {
    const auto it = _producers.find(intent.sessionId.toString());
    if (it != _producers.end() && it->second.generation == intent.connectionGeneration) {
        it->second.recorder->stop(reason); _producers.erase(it); _worker->wake.notify_one();
    }
    // Also cancel a start whose durable-open result has not reached the GUI yet.
    for (auto& entry : _starts) if (entry.second.intent.sessionId == intent.sessionId
        && entry.second.intent.connectionGeneration == intent.connectionGeneration) entry.second.cancelled = true;
}
SubmitResult HistoryService::submit(QUuid session, quint64 generation, const QByteArray& bytes) {
    const auto it = _producers.find(session.toString());
    if (it == _producers.end() || it->second.generation != generation) return SubmitResult::Disabled;
    const auto result = it->second.recorder->submit(bytes); _worker->wake.notify_one(); return result;
}
void HistoryService::omit(QUuid session, quint64 generation, quint64 bytes, const QString& reason) {
    const auto it = _producers.find(session.toString());
    if (it != _producers.end() && it->second.generation == generation) it->second.recorder->omit(bytes, reason);
}
void HistoryService::boundary(QUuid session, quint64 generation, const QString& reason) {
    const auto it = _producers.find(session.toString());
    if (it != _producers.end() && it->second.generation == generation) it->second.recorder->boundary(reason);
}
bool HistoryService::remove(HistoryCaptureId id, Done callback) {
    if (_callbacks.size() + _readCallbacks.size() >= 64) return false;
    Worker::Job job{Worker::Kind::Remove}; job.id = id; job.request = ++_request;
    if (!_worker->enqueue(job)) return false; _callbacks.emplace(job.request, std::move(callback)); return true;
}
bool HistoryService::retain(HistoryCaptureId id, bool keep, Done callback) {
    if (_callbacks.size() + _readCallbacks.size() >= 64) return false;
    Worker::Job job{Worker::Kind::Retain}; job.id = id; job.keep = keep; job.request = ++_request;
    if (!_worker->enqueue(job)) return false; _callbacks.emplace(job.request, std::move(callback)); return true;
}
bool HistoryService::configure(RetentionSettings settings, Done callback) {
    if (!settings.valid() || _callbacks.size() + _readCallbacks.size() >= 64) return false;
    Worker::Job job{Worker::Kind::Configure}; job.settings = settings; job.request = ++_request;
    if (!_worker->enqueue(job)) return false; _callbacks.emplace(job.request, std::move(callback)); return true;
}
bool HistoryService::readCommitted(HistoryCaptureId id, HistoryRange range, ReadDone callback) {
    if (!callback || _callbacks.size() + _readCallbacks.size() >= 64) return false;
    Worker::Job job{Worker::Kind::Read}; job.id = id; job.range = range; job.request = ++_request;
    if (!_worker->enqueue(job)) return false; _readCallbacks.emplace(job.request, std::move(callback)); return true;
}
void HistoryService::poll() {
    std::deque<Worker::Completion> completions; bool dirty;
    { std::lock_guard<std::mutex> lock(_worker->mutex);
      completions.swap(_worker->completions); dirty = _worker->dirty; _worker->dirty = false;
      if (dirty) { _captures = _worker->snapshot; _settings = _worker->publishedSettings; _error = _worker->error; _ready = _worker->publishedReady; }
    }
    for (auto& completion : completions) {
        QPointer<HistoryService> guard(this);
        const auto pending = _starts.find(completion.request);
        if (pending != _starts.end()) {
            if (pending->second.cancelled && completion.recorder) {
                completion.recorder->stop("connection_changed"); completion.result.success = false;
                completion.result.error = "recording start cancelled";
            }
            _starts.erase(pending);
        }
        if (completion.recorder && completion.recorder->accepting())
            _producers[completion.job.intent.sessionId.toString()] = {completion.job.intent.connectionGeneration, completion.recorder};
        const auto cb = _callbacks.find(completion.request);
        if (cb != _callbacks.end()) { auto done = std::move(cb->second); _callbacks.erase(cb); if (done) done(std::move(completion.result)); }
        if (!guard) return;
        const auto read = _readCallbacks.find(completion.request);
        if (read != _readCallbacks.end()) { auto done = std::move(read->second); _readCallbacks.erase(read); done(std::move(completion.read)); }
        if (!guard) return;
    }
    for (auto it = _producers.begin(); it != _producers.end();) {
        if (!it->second.recorder->accepting()) it = _producers.erase(it); else ++it;
    }
    if (dirty) emit changed();
}
}
