/** @file TerminalTextWait.cpp @brief 有界文本匹配、观察续扫和失效交付。 */
#include "TerminalTextWait.h"
#include "LogReadAdapter.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "ObservationService.h"
#include "tasks/TextWaitMatcher.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTimer>
#include <QDateTime>
#include <QTimeZone>
#include <algorithm>

namespace NovaTerm::Mcp {
namespace {
constexpr int RoundBytes = 16384;
constexpr int ContextBytes = 2048;
bool sameCondition(const WaitTextRequest& a, const WaitTextRequest& b) {
    return a.session == b.session && a.epoch == b.epoch && a.source == b.source
        && a.texts == b.texts && a.match == b.match && a.caseSensitive == b.caseSensitive
        && a.screenStart == b.screenStart;
}
QJsonObject reject(ErrorCode code, const QString& message = {}) {
    return failure(QString::fromLatin1(errorCodeName(code)), message);
}
QString contextTail(const QString& text) {
    auto bytes = text.toUtf8().right(ContextBytes);
    while (!bytes.isEmpty() && (static_cast<unsigned char>(bytes.front()) & 0xc0) == 0x80) bytes.remove(0, 1);
    return QString::fromUtf8(bytes);
}
}

class TerminalTextWait::Impl final {
public:
    struct State {
        State(QString principal, WaitTextRequest value)
            : client(std::move(principal)), request(std::move(value)),
              matcher(request.texts, request.match, request.caseSensitive) {}
        QString client;
        WaitTextRequest request;
        TextWaitMatcher matcher;
        quint64 grant{0}, generation{0}, attachmentGeneration{0};
        QString attachment, startToken, nextToken, revision, capturedAt, context, sourceStatus;
        QJsonObject nextPosition;
        QJsonObject gapDetails;
        QHash<int, QJsonObject> evidence;
        qint64 expires{0}, deadline{0};
        int scanned{0};
        bool initialized{false}, screenTruncated{false}, screenNeedsScan{false};
        Completion done;
    };
    TerminalTextWait* q;
    AccessStore& access;
    SessionDirectory& sessions;
    ObservationService& observation;
    QString instance;
    QByteArray logKey;
    QElapsedTimer clock;
    std::function<qint64()> monotonicClock;
    LogReader logReader;
    QTimer timer;
    QHash<QString, std::shared_ptr<State>> retained;
    QHash<QString, std::shared_ptr<State>> jobs;
    int offset{0};
    Impl(TerminalTextWait* owner, AccessStore& a, SessionDirectory& s, ObservationService& o,
        QString id, QByteArray key, std::function<qint64()> clockSource, LogReader reader)
        : q(owner), access(a), sessions(s), observation(o), instance(std::move(id)), logKey(std::move(key)),
          monotonicClock(std::move(clockSource)), logReader(std::move(reader)) {
        clock.start();
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, q, [this] { poll(); });
        QObject::connect(&access, &AccessStore::revoked, q,
            [this](const QString& client, const QString& session) { revoke(client, session); });
        QObject::connect(&sessions, &SessionDirectory::invalidated, q,
            [this](const QString& session) { revoke({}, session); });
    }
    qint64 now() const { return monotonicClock ? monotonicClock() : clock.elapsed(); }
    Result<QJsonObject> readPage(const QString& client, const ReadContextRequest& request) {
        return logReader ? logReader(client, request)
            : readLogContext(access, sessions, instance, client, logKey, request);
    }
    void sweep() {
        for (auto it = retained.begin(); it != retained.end();) {
            if (it.value()->expires <= now()) it = retained.erase(it);
            else ++it;
        }
    }
    bool valid(const State& state, SessionDirectory::Entry* found = nullptr) {
        const auto entry = sessions.find(state.request.session.str());
        if (!entry || !entry->session || !entry->session->core()
            || !access.canRead(state.client, *entry) || access.version(state.client) != state.grant)
            return false;
        if (found) *found = *entry;
        return true;
    }
    bool bindingValid(const State& state, const SessionDirectory::Entry& entry) const {
        return entry.epoch == state.request.epoch.str()
            && entry.attachmentId == state.attachment && entry.generation == state.attachmentGeneration
            && entry.session->core()->cacheGeneration() == state.generation;
    }
    void schedule(int delay = 0) {
        if (!jobs.isEmpty() && (!timer.isActive() || timer.remainingTime() > delay)) timer.start(delay);
    }
    void finish(const QString& requestKey, const std::shared_ptr<State>& state, const QString& status,
        std::optional<QJsonObject> error = {}) {
        // 先移除再调用外部回调；取消和撤权可能重入。
        if (jobs.value(requestKey) != state) return;
        jobs.remove(requestKey);
        auto done = std::move(state->done);
        SessionDirectory::Entry entry;
        if (!valid(*state, &entry)) { if (done) done(reject(ErrorCode::SessionNotAvailable)); return; }
        const auto effectiveStatus = bindingValid(*state, entry) ? status : QStringLiteral("invalidated");
        if (error && effectiveStatus != "invalidated") { if (done) done(*error); return; }
        QJsonArray matched;
        for (const auto index : state->matcher.matchedIndices()) {
            auto item = state->evidence.value(index);
            item.insert("index", index); item.insert("text", state->request.texts[index]);
            matched.append(item);
        }
        QJsonObject result{{"instanceId", instance}, {"sessionId", state->request.session.str()},
            {"epoch", state->request.epoch.str()}, {"source", state->request.source == WaitTextSource::Log ? "log" : "screen"},
            {"status", effectiveStatus}, {"matched", effectiveStatus == "matched"},
            {"matchedTexts", matched}, {"scanBytes", state->scanned},
            {"cacheGeneration", QString::number(state->generation)}, {"capturedAt", state->capturedAt},
            {"context", state->context}, {"evidenceKind", "literal_text"}, {"provesSuccess", false}};
        if (!state->gapDetails.isEmpty()) result.insert("gap", state->gapDetails);
        if (state->request.source == WaitTextSource::Log) {
            result.insert("startToken", state->startToken);
            result.insert("nextToken", state->nextToken);
            if (!state->nextPosition.isEmpty()) result.insert("nextPosition", state->nextPosition);
            if (!state->sourceStatus.isEmpty()) result.insert("sourceStatus", state->sourceStatus);
        } else {
            if (!state->revision.isEmpty()) result.insert("revision", state->revision);
            result.insert("snapshotTruncated", state->screenTruncated);
        }
        QString resume;
        if (effectiveStatus == "timeout" || effectiveStatus == "scan_budget_exhausted" || effectiveStatus == "cancelled") {
            // 活动与保留状态共用限额，不静默淘汰另一条续扫记录。
            resume = newId();
            retained.insert(resume, state);
            result.insert("resumeToken", resume);
            result.insert("resumeExpiresInMs", double(std::max(qint64{0}, state->expires - now())));
        }
        // Parser 清空可与 JSON 构造重叠；此处为 GUI 最终交付复核。
        if (!valid(*state, &entry)) {
            retained.remove(resume);
            if (done) done(reject(ErrorCode::SessionNotAvailable));
            return;
        }
        if (effectiveStatus == "invalidated" || !bindingValid(*state, entry)) {
            retained.remove(resume);
            result.remove("resumeToken"); result.remove("resumeExpiresInMs");
            result.insert("status", "invalidated"); result.insert("matched", false);
            // 清空后不能将旧代际命中继续作为实时投影交付。
            result.insert("context", QString{});
            result.insert("matchedTexts", QJsonArray{});
            if (state->request.source == WaitTextSource::Log) {
                result.insert("sourceStatus", "invalidated");
                ReadContextRequest hint;
                hint.session = state->request.session; hint.epoch = state->request.epoch;
                hint.source = ContextSource::Log; hint.maxBytes = 1; hint.maxLines = 1;
                if (!state->nextToken.isEmpty()) hint.sinceToken = state->nextToken;
                else hint.start = LogInitialPosition::Tail;
                const auto read = readPage(state->client, hint);
                if (const auto* page = std::get_if<QJsonObject>(&read)) result.insert("gap", QJsonObject{
                    {"earliestToken", page->value("earliestToken")},
                    {"earliestPosition", page->value("earliestPosition")},
                    {"cacheGeneration", page->value("cacheGeneration")}});
                // 失效提示只是元数据，不自动替换调用方的游标或基线。
            }
        }
        if (!valid(*state)) {
            retained.remove(resume);
            if (done) done(reject(ErrorCode::SessionNotAvailable));
            return;
        }
        if (done) done(success(result));
    }
    void call(const QString& client, const QString& requestKey, const WaitTextRequest& request, Completion done) {
        sweep();
        if (request.texts.isEmpty() || request.texts.size() > Budget::MaxTextWaitCandidates || request.wait.count() < 0
            || request.wait.count() > Budget::MaxTerminalWaitMs || request.scanBytes < 1 || request.scanBytes > Budget::MaxTextWaitScanBytes
            || (request.resumeToken && request.sinceToken)
            || (request.source == WaitTextSource::Screen && request.sinceToken)) {
            done(reject(ErrorCode::InvalidArgument)); return;
        }
        for (const auto& text : request.texts) if (text.isEmpty() || text.toUtf8().size() > Budget::MaxTextWaitCandidateBytes) {
            done(reject(ErrorCode::InvalidArgument)); return;
        }
        if (jobs.contains(requestKey)) { done(reject(ErrorCode::Busy)); return; }
        const auto entry = sessions.find(request.session.str());
        if (!entry || !entry->session || !entry->session->core() || !access.canRead(client, *entry)) {
            done(reject(ErrorCode::SessionNotAvailable)); return;
        }
        if (entry->epoch != request.epoch.str()) { done(reject(ErrorCode::StaleSessionEpoch)); return; }
        std::shared_ptr<State> state;
        if (request.resumeToken) {
            state = retained.value(*request.resumeToken);
            if (!state || state->client != client || !sameCondition(state->request, request)) {
                done(reject(ErrorCode::CursorStale)); return;
            }
            if (!valid(*state)) { done(reject(ErrorCode::SessionNotAvailable)); return; }
            retained.remove(*request.resumeToken);
        } else {
            int clientStates = 0;
            for (const auto& value : retained) if (value->client == client) ++clientStates;
            for (const auto& value : jobs) if (value->client == client) ++clientStates;
            if (clientStates >= Budget::MaxTextWaitStatesPerClient || retained.size() + jobs.size() >= Budget::MaxTextWaitStates) {
                done(reject(ErrorCode::Busy, "Text wait state limit reached; resume an existing wait or let it expire.")); return;
            }
            state = std::make_shared<State>(client, request);
            state->grant = access.version(client);
            state->generation = entry->session->core()->cacheGeneration();
            state->attachment = entry->attachmentId;
            state->attachmentGeneration = entry->generation;
            state->expires = now() + Budget::TextWaitStateLifetime.count();
            if (request.sinceToken) {
                state->startToken = *request.sinceToken;
                state->nextToken = *request.sinceToken;
            }
        }
        state->request.wait = request.wait;
        state->request.scanBytes = request.scanBytes;
        state->scanned = 0;
        state->deadline = now() + request.wait.count();
        state->done = std::move(done);
        jobs.insert(requestKey, state);
        const QPointer<TerminalTextWait> guard(q);
        step(requestKey, state);
        if (guard) schedule(50);
    }
    void step(const QString& requestKey, const std::shared_ptr<State>& state) {
        SessionDirectory::Entry entry;
        if (!valid(*state, &entry)) { finish(requestKey, state, {}, reject(ErrorCode::SessionNotAvailable)); return; }
        if (!bindingValid(*state, entry)) { finish(requestKey, state, "invalidated"); return; }
        if (state->expires <= now()) { finish(requestKey, state, {}, reject(ErrorCode::CursorStale)); return; }
        if (state->scanned >= state->request.scanBytes) { finish(requestKey, state, "scan_budget_exhausted"); return; }
        if (state->request.source == WaitTextSource::Log) readLog(requestKey, state);
        else readScreen(requestKey, state, entry);
    }
    void readLog(const QString& requestKey, const std::shared_ptr<State>& state) {
        ReadContextRequest request;
        request.session = state->request.session;
        request.epoch = state->request.epoch;
        request.source = ContextSource::Log;
        if (state->initialized) request.sinceToken = state->nextToken;
        else if (state->request.sinceToken) request.sinceToken = state->request.sinceToken;
        else request.start = LogInitialPosition::Tail;
        request.maxBytes = std::min(RoundBytes, state->request.scanBytes - state->scanned);
        request.maxLines = Budget::MaxFullLines;
        const auto read = readPage(state->client, request);
        if (const auto* error = errorIfAny(read)) {
            if (error->code == ErrorCode::TextBudgetTooSmall) finish(requestKey, state, "scan_budget_exhausted");
            else if (error->code == ErrorCode::Busy) {
                state->sourceStatus = "busy";
                if (now() >= state->deadline) {
                    if (!state->nextToken.isEmpty()) finish(requestKey, state, "timeout");
                    else finish(requestKey, state, {}, failure(QString::fromLatin1(errorCodeName(ErrorCode::Busy)),
                        error->message, true, 50));
                }
            } else
                finish(requestKey, state, {}, reject(error->code, error->message));
            return;
        }
        const auto page = std::get<QJsonObject>(read);
        state->capturedAt = page.value("capturedAt").toString();
        const auto status = page.value("status").toString();
        state->sourceStatus = status;
        if (state->nextToken.isEmpty() && status != "gap" && status != "invalidated") {
            state->nextToken = page.value("nextToken").toString();
            state->startToken = state->nextToken;
            state->nextPosition = page.value("nextPosition").toObject();
        }
        if (status == "gap" || status == "invalidated") {
            state->gapDetails = {{"earliestToken", page.value("earliestToken")},
                {"earliestPosition", page.value("earliestPosition")}};
            finish(requestKey, state, status); return;
        }
        if (!state->initialized) {
            state->startToken = state->request.sinceToken.value_or(page.value("nextToken").toString());
            state->initialized = true;
        }
        const auto fragments = page.value("fragments").toArray();
        if (fragments.isEmpty()) {
            // 阻塞来源可能返回空页；保留既有游标，
            // 仅有位置元数据不能证明文本已经扫描。
            if (state->nextPosition.isEmpty()) state->nextPosition = page.value("nextPosition").toObject();
            if (now() >= state->deadline) finish(requestKey, state, "timeout");
            return;
        }
        QString text;
        // B1 交付有效前缀后仍可能遇到未就绪或不可用的块。
        // 先消费该前缀，再处理来源的续读状态。
        for (const auto& value : fragments) {
            const auto part = value.toObject();
            const auto fragment = part.value("text").toString();
            const auto before = state->matcher.tail();
            state->matcher.feed(fragment);
            for (const auto& hit : state->matcher.hits()) {
                const auto endOffset = part.value("byteOffset").toString().toULongLong()
                    + quint64(fragment.left(hit.endOffset).toUtf8().size());
                state->evidence.insert(hit.index, {{"endPosition", QJsonObject{
                    {"lineId", part.value("lineId")}, {"byteOffset", QString::number(endOffset)}}},
                    {"context", contextTail(before + fragment.left(hit.endOffset))}});
            }
            text += fragment;
        }
        state->scanned += int(text.toUtf8().size());
        const bool matched = state->matcher.satisfied();
        state->context = contextTail(state->context + text);
        // 完整扫描本页全部分段后才推进 nextToken。
        state->nextToken = page.value("nextToken").toString();
        state->nextPosition = page.value("nextPosition").toObject();
        if (matched) finish(requestKey, state, "matched");
        else if (state->scanned >= state->request.scanBytes) finish(requestKey, state, "scan_budget_exhausted");
        else if (status != "ok") {
            if (now() >= state->deadline) finish(requestKey, state, "timeout");
        }
        else if (page.value("hasMore").toBool()) schedule();
        else if (now() >= state->deadline) finish(requestKey, state, "timeout");
    }
    void readScreen(const QString& requestKey, const std::shared_ptr<State>& state, const SessionDirectory::Entry& entry) {
        const auto read = observation.snapshot(entry.id, entry.epoch,
            [&entry] { return entry.session->tryTerminalContext(); }, nullptr, state->generation);
        if (!read) {
            if (now() >= state->deadline) {
                if (state->initialized) finish(requestKey, state, "timeout");
                else finish(requestKey, state, {}, reject(ErrorCode::ContextNotReady));
            }
            return;
        }
        if (read->state.cacheGeneration != state->generation) { finish(requestKey, state, "invalidated"); return; }
        const auto capturedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            read->capturedAt.time_since_epoch()).count();
        state->capturedAt = QDateTime::fromMSecsSinceEpoch(capturedMs, QTimeZone::UTC).toString(Qt::ISODateWithMs);
        const bool first = !state->initialized;
        const auto revision = QString::number(read->state.revision);
        state->screenTruncated = read->state.truncated;
        const bool changed = state->screenNeedsScan || state->revision != revision;
        state->initialized = true;
        state->revision = revision;
        if (changed && !(first && state->request.screenStart == ScreenWaitStart::Next)) {
            QString text;
            bool firstLine = true;
            for (const auto& line : read->state.viewport) {
                if (!firstLine) text += '\n';
                firstLine = false;
                text += QString::fromStdString(line.text);
            }
            const int bytes = int(text.toUtf8().size());
            if (bytes > state->request.scanBytes - state->scanned) {
                // 一份屏幕按完整版本扫描；续扫重新核对可用快照，
                // 不将旧版本的部分文本拼入新屏幕。
                state->screenNeedsScan = true;
                finish(requestKey, state, "scan_budget_exhausted"); return;
            }
            state->screenNeedsScan = false;
            state->scanned += bytes;
            TextWaitMatcher matcher(state->request.texts, state->request.match, state->request.caseSensitive);
            const auto matched = matcher.feed(text);
            state->matcher = std::move(matcher); // 不拼接不同屏幕版本。
            state->evidence.clear();
            for (const auto& hit : state->matcher.hits()) state->evidence.insert(hit.index,
                {{"revision", revision}, {"context", contextTail(text.left(hit.endOffset))}});
            state->context = contextTail(text);
            if (matched) { finish(requestKey, state, "matched"); return; }
        }
        if (now() >= state->deadline) finish(requestKey, state, "timeout");
    }
    void poll() {
        const QPointer<TerminalTextWait> guard(q);
        const auto keys = jobs.keys();
        QElapsedTimer work; work.start();
        const int count = std::min(4, int(keys.size()));
        int visited = 0;
        for (; visited < count && (visited == 0 || work.elapsed() < 8); ++visited) {
            const auto& key = keys[(offset + visited) % keys.size()];
            const auto state = jobs.value(key);
            if (state) step(key, state);
            if (!guard) return;
        }
        if (!keys.isEmpty()) offset = (offset + visited) % int(keys.size());
        schedule(50);
    }
    void cancel(const QString& requestKey) {
        const auto state = jobs.value(requestKey);
        if (state) finish(requestKey, state, "cancelled");
    }
    void discard(const QString& requestKey) {
        const auto state = jobs.take(requestKey);
        if (state) state->done = {};
        if (jobs.isEmpty()) timer.stop();
    }
    void revoke(const QString& client, const QString& session) {
        const QPointer<TerminalTextWait> guard(q);
        for (auto it = retained.begin(); it != retained.end();) {
            if ((client.isEmpty() || it.value()->client == client)
                && (session.isEmpty() || it.value()->request.session.str() == session)) it = retained.erase(it);
            else ++it;
        }
        const auto keys = jobs.keys();
        for (const auto& key : keys) {
            const auto state = jobs.value(key);
            if (state && (client.isEmpty() || state->client == client)
                && (session.isEmpty() || state->request.session.str() == session))
                finish(key, state, "invalidated");
            if (!guard) return;
        }
    }
};

TerminalTextWait::TerminalTextWait(AccessStore& access, SessionDirectory& sessions,
    ObservationService& observation, QString instance, QByteArray key, QObject* parent,
    std::function<qint64()> monotonicClock, LogReader logReader)
    : QObject(parent), _impl(std::make_unique<Impl>(this, access, sessions, observation,
          std::move(instance), std::move(key), std::move(monotonicClock), std::move(logReader))) {}
TerminalTextWait::~TerminalTextWait() = default;
void TerminalTextWait::call(const QString& client, const QString& request, const WaitTextRequest& value, Completion done) {
    _impl->call(client, request, value, std::move(done));
}
void TerminalTextWait::cancel(const QString& request) { _impl->cancel(request); }
void TerminalTextWait::discard(const QString& request) { _impl->discard(request); }
void TerminalTextWait::revoke(const QString& client, const QString& session) { _impl->revoke(client, session); }
}
