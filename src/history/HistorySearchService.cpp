#include "HistorySearchService.h"
#include "HistoryProjectionDecoder.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QMetaObject>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace NovaTerm::Recording {
namespace {
QString opaqueHandle() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
void appendRange(std::vector<HistoryRawRange>& ranges, const HistoryRawRange& next) {
    if (!ranges.empty() && ranges.back().recordId == next.recordId
        && ranges.back().byteOffset + ranges.back().byteCount == next.byteOffset) ranges.back().byteCount += next.byteCount;
    else ranges.push_back(next);
}
size_t unitBytes(const std::deque<HistoryProjectedUnit>& units) {
    size_t result = units.size() * sizeof(HistoryProjectedUnit);
    for (const auto& unit : units) result += unit.ranges.capacity() * sizeof(HistoryRawRange);
    return result;
}
}
struct HistorySearchService::Impl {
    struct State {
        ResourceContext context;
        ResourceGrant grant;
        HistorySearchMode mode{HistorySearchMode::Text};
        QByteArray queryBytes;
        std::u32string query;
        std::vector<size_t> prefix;
        quint64 highWater{0}, recordId{1};
        qsizetype offset{0};
        Integrity integrity{Integrity::UnknownTail};
        HistoryProjectionDecoder decoder;
        std::deque<HistoryProjectedUnit> ready, matched;
        std::optional<HistorySearchHit> pendingHit;
        bool pendingReset{false}, eofFlushed{false}, busy{false}, terminal{false};
        qint64 created{0}, touched{0};
        QString id, nextCursor, cachedCursor;
        HistorySearchPage cached;
        std::atomic<bool> cancelled{false}, expired{false};
    };
    struct Job {
        std::shared_ptr<State> state;
        HistorySearchRequest request;
        SearchCallback callback;
        HistorySearchPage page;
        QElapsedTimer timer;
        qsizetype scanned{0}, encoded{1024};
        bool stop{false}, terminal{false};
    };
    HistorySearchService* owner;
    HistoryReadPort port;
    HistoryAuthorizationStore& authorization;
    HistorySearchLimits limits;
    QThreadPool workers;
    QTimer sweep;
    QElapsedTimer clock;
    std::map<QString, std::shared_ptr<State>> states;
    std::set<QString> activePrincipals;
    int active{0};
    Impl(HistorySearchService* o, HistoryReadPort p, HistoryAuthorizationStore& a, HistorySearchLimits l)
        : owner(o), port(std::move(p)), authorization(a), limits(l) {
        limits.idleMilliseconds = std::clamp(limits.idleMilliseconds, 1, 60000);
        limits.lifetimeMilliseconds = std::clamp(limits.lifetimeMilliseconds, 1, 300000);
        limits.maxStates = std::clamp<size_t>(limits.maxStates, 1, 16);
        limits.maxStatesPerPrincipal = std::clamp<size_t>(limits.maxStatesPerPrincipal, 1, 2);
        workers.setMaxThreadCount(2); clock.start(); sweep.setInterval(1000);
        QObject::connect(&sweep, &QTimer::timeout, owner, [this] { expire(); }); sweep.start();
    }
    bool stale(const State& s) const {
        return clock.elapsed() - s.created >= limits.lifetimeMilliseconds
            || clock.elapsed() - s.touched >= limits.idleMilliseconds;
    }
    void expire() {
        for (auto i = states.begin(); i != states.end();) {
            if (stale(*i->second)) {
                i->second->expired = true; i->second->cancelled = true; i = states.erase(i);
            } else ++i;
        }
    }
    QString validate(const State& s) const {
        const auto resource = port.describe ? port.describe(s.grant.captureId) : std::nullopt;
        if (!resource) return "EVIDENCE_EXPIRED";
        const auto grant = authorization.authorize(s.context, s.grant.captureId, resource->generation);
        if (!grant || grant->accessVersion != s.grant.accessVersion
            || resource->generation != s.grant.resourceGeneration) return "PERMISSION_DENIED";
        if (resource->capture.highWaterRecordId < s.highWater) return "HISTORY_RANGE_MISSING";
        return {};
    }
    static bool cancelled(const Job& j) {
        return j.state->cancelled.load() || (j.request.cancelled && j.request.cancelled->load());
    }
    static bool budget(const Job& j) {
        return j.scanned >= j.request.scanBytes || j.timer.elapsed() >= j.request.scanMilliseconds;
    }
    static void projectionLimit(Job& j) {
        j.page.reason = "PROJECTION_LIMIT"; j.page.complete = false; j.stop = j.terminal = true;
        j.page.projectionGaps.push_back({j.state->recordId, j.state->offset, 0});
    }
    static bool appendHit(Job& j, HistorySearchHit hit) {
        size_t totalRanges = hit.ranges.size();
        for (const auto& existing : j.page.hits) totalRanges += existing.ranges.size();
        if (hit.ranges.size() > 128) { j.page.reason = "MATCH_RANGE_LIMIT"; j.stop = j.terminal = true; return false; }
        const auto cost = qsizetype(128 + 96 * hit.ranges.size());
        if (cost + 1024 > j.request.maxResponseBytes) {
            j.page.reason = "MATCH_RANGE_LIMIT"; j.stop = j.terminal = true; return false;
        }
        if (totalRanges > 128 || j.encoded + cost > j.request.maxResponseBytes || int(j.page.hits.size()) >= j.request.maxMatches) {
            j.state->pendingHit = std::move(hit); j.stop = true; return false;
        }
        j.encoded += cost; j.page.hits.push_back(std::move(hit));
        if (int(j.page.hits.size()) >= j.request.maxMatches) j.stop = true;
        return true;
    }
    static void matchUnit(Job& j, HistoryProjectedUnit unit) {
        auto& s = *j.state;
        auto matched = s.matched.size();
        while (matched && unit.value != s.query[matched]) {
            matched = s.prefix[matched - 1];
            while (s.matched.size() > matched) s.matched.pop_front();
        }
        if (unit.value != s.query[matched]) return;
        s.matched.push_back(std::move(unit));
        if (s.matched.size() == s.query.size()) {
            HistorySearchHit hit;
            for (const auto& item : s.matched) for (const auto& range : item.ranges) appendRange(hit.ranges, range);
            const auto keep = s.prefix.back();
            while (s.matched.size() > keep) s.matched.pop_front();
            appendHit(j, std::move(hit));
        }
    }
    static void drain(Job& j) {
        auto& s = *j.state;
        while (!s.ready.empty() && !j.stop) {
            if (cancelled(j)) { j.page.error = "CANCELLED"; j.stop = j.terminal = true; return; }
            if (j.timer.elapsed() >= j.request.scanMilliseconds) { j.stop = true; return; }
            auto unit = std::move(s.ready.front()); s.ready.pop_front(); matchUnit(j, std::move(unit));
        }
        if (s.ready.empty() && s.pendingReset) {
            s.decoder.reset(); s.matched.clear(); s.pendingReset = false;
        }
    }
    static void process(const std::shared_ptr<Job>& job, ReadResult result, bool bufferedOnly) {
        auto& j = *job; auto& s = *j.state;
        if (cancelled(j)) { j.page.error = "CANCELLED"; j.stop = j.terminal = true; return; }
        if (s.pendingHit) {
            auto hit = std::move(*s.pendingHit); s.pendingHit.reset(); appendHit(j, std::move(hit));
            if (j.stop) return;
        }
        drain(j); if (j.stop) return;
        if (s.recordId > s.highWater) {
            if (!s.eofFlushed) {
                s.decoder.finishLine(); s.ready = s.decoder.takeReady(); s.eofFlushed = true;
                if (s.decoder.limited()) { projectionLimit(j); return; }
            }
            drain(j);
            if (!j.stop && s.ready.empty()) { j.page.complete = true; j.stop = j.terminal = true; }
            return;
        }
        if (budget(j)) { j.stop = true; return; }
        if (bufferedOnly) return;
        if (result.missing) { j.page.error = "HISTORY_RANGE_MISSING"; j.stop = j.terminal = true; return; }
        if (!result.error.isEmpty()) { j.page.error = "SOURCE_UNAVAILABLE"; j.stop = j.terminal = true; return; }
        if (result.records.empty()) { j.page.error = "HISTORY_RANGE_MISSING"; j.stop = j.terminal = true; return; }
        size_t inputBytes = 0;
        for (const auto& record : result.records) inputBytes += size_t(record.payload.size());
        if (result.records.size() > 64 || inputBytes > 256 * 1024) {
            j.page.error = "SOURCE_LIMIT"; j.stop = j.terminal = true; return;
        }
        s.integrity = result.integrity;
        for (const auto& record : result.records) {
            const auto last = record.lastRecordId ? record.lastRecordId : record.recordId;
            if (record.recordId > s.recordId || last < s.recordId || last > s.highWater || s.offset > record.payload.size()) {
                j.page.error = "HISTORY_RANGE_MISSING"; j.stop = j.terminal = true; return;
            }
            if (record.kind != RecordKind::Payload || !HistoryAuthorizationStore::allowsRecord(s.grant, record)) {
                s.decoder.finishLine(); s.ready = s.decoder.takeReady(); s.pendingReset = true;
                if (s.decoder.limited()) { projectionLimit(j); return; }
                s.recordId = last + 1; s.offset = 0; ++j.scanned; drain(j);
                if (j.stop) return;
            } else {
                while (s.offset < record.payload.size()) {
                    if (cancelled(j)) { j.page.error = "CANCELLED"; j.stop = j.terminal = true; return; }
                    if (budget(j)) { j.stop = true; return; }
                    const auto offset = s.offset++;
                    const auto byte = static_cast<unsigned char>(record.payload[offset]); ++j.scanned;
                    if (s.mode == HistorySearchMode::RawBytes) matchUnit(j, {byte, {{record.recordId, offset, 1}}});
                    else {
                        s.decoder.feed(byte, record.recordId, offset);
                        if (s.decoder.limited()) { projectionLimit(j); return; }
                        s.ready = s.decoder.takeReady(); drain(j);
                    }
                    if (j.stop) return;
                }
                s.recordId = last + 1; s.offset = 0;
            }
            // Projection, matcher tail, one fetched block and cached page together remain bounded.
            if (s.decoder.memoryBytes() + unitBytes(s.ready) + unitBytes(s.matched)
                + inputBytes + size_t(j.request.maxResponseBytes) * 2 > 1024 * 1024) { projectionLimit(j); return; }
        }
        if (s.recordId > s.highWater) {
            s.decoder.finishLine(); s.ready = s.decoder.takeReady(); s.eofFlushed = true; drain(j);
            if (!j.stop && s.ready.empty()) { j.page.complete = true; j.stop = j.terminal = true; }
        } else if (budget(j)) j.stop = true;
    }
    void finish(const std::shared_ptr<Job>& j) {
        auto& s = *j->state;
        --active; activePrincipals.erase(s.context.principalId); s.busy = false;
        auto page = std::move(j->page); page.highWaterRecordId = s.highWater; page.sourceCompleteness = s.integrity;
        const auto error = validate(s);
        if (!error.isEmpty()) page.error = error;
        else if (s.expired || stale(s)) page.error = "SEARCH_CURSOR_EXPIRED";
        else if (cancelled(*j)) page.error = "CANCELLED";
        if (!page.error.isEmpty()) { page.hits.clear(); page.nextCursor.clear(); page.complete = false; j->terminal = true; }
        if (j->terminal) {
            s.decoder.reset(); s.ready.clear(); s.matched.clear(); s.pendingHit.reset(); s.terminal = true;
        } else {
            s.touched = clock.elapsed(); page.nextCursor = opaqueHandle(); s.nextCursor = page.nextCursor;
        }
        s.cachedCursor = j->request.cursor; s.cached = page;
        // Failed and cancelled states cannot be used as a capability or retained indefinitely.
        if (!page.error.isEmpty()) states.erase(s.id);
        j->callback(std::move(page));
    }
    void dispatch(const std::shared_ptr<Job>& job, ReadResult result, bool bufferedOnly = false) {
        const QPointer<HistorySearchService> self(owner);
        workers.start([self, job, result = std::move(result), bufferedOnly]() mutable {
            process(job, std::move(result), bufferedOnly);
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, job] {
                if (!self) return;
                if (job->stop) self->_impl->finish(job); else self->_impl->next(job);
            }, Qt::QueuedConnection);
        });
    }
    void next(const std::shared_ptr<Job>& job) {
        const auto error = validate(*job->state);
        if (!error.isEmpty()) { job->page.error = error; job->terminal = true; finish(job); return; }
        if (cancelled(*job) || stale(*job->state)) {
            job->page.error = stale(*job->state) ? "SEARCH_CURSOR_EXPIRED" : "CANCELLED";
            job->terminal = true; finish(job); return;
        }
        if (job->state->recordId > job->state->highWater || !job->state->ready.empty() || job->state->pendingHit) {
            dispatch(job, {}, true); return;
        }
        if (!port.readCommitted) { job->page.error = "SOURCE_UNAVAILABLE"; job->terminal = true; finish(job); return; }
        HistoryRange range; range.firstRecordId = job->state->recordId; range.lastRecordId = job->state->highWater;
        range.maxBytes = 256 * 1024; range.maxRecords = 64;
        const QPointer<HistorySearchService> self(owner);
        const bool accepted = port.readCommitted(job->state->grant.captureId, range, [self, job](ReadResult result) mutable {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, job, result = std::move(result)]() mutable {
                if (self) self->_impl->dispatch(job, std::move(result));
            }, Qt::QueuedConnection);
        });
        if (!accepted) { job->page.error = "BUSY"; job->terminal = true; finish(job); }
    }
};
HistorySearchService::HistorySearchService(HistoryReadPort port, HistoryAuthorizationStore& a, QObject* p, HistorySearchLimits limits)
    : QObject(p), _impl(std::make_unique<Impl>(this, std::move(port), a, limits)) {}
HistorySearchService::~HistorySearchService() {
    _impl->sweep.stop();
    for (const auto& entry : _impl->states) entry.second->cancelled = true;
    _impl->workers.waitForDone();
}
void HistorySearchService::searchAsync(HistorySearchRequest request, ResourceContext context, SearchCallback callback) {
    auto& p = *_impl;
    const auto fail = [&](const QString& error) { HistorySearchPage page; page.error = error; callback(std::move(page)); };
    if (!request.captureId.valid() || request.query.isEmpty() || request.query.size() > 4096
        || request.maxMatches < 1 || request.maxMatches > 128 || request.maxResponseBytes < 2048 || request.maxResponseBytes > 65536
        || request.scanBytes < 1 || request.scanBytes > 16 * 1024 * 1024
        || request.scanMilliseconds < 1 || request.scanMilliseconds > 2000) { fail("INVALID_ARGUMENT"); return; }
    p.expire();
    const auto resource = p.port.describe ? p.port.describe(request.captureId) : std::nullopt;
    if (!resource) { fail("EVIDENCE_EXPIRED"); return; }
    const auto grant = p.authorization.authorize(context, request.captureId, resource->generation);
    if (!grant) { fail("PERMISSION_DENIED"); return; }
    std::shared_ptr<Impl::State> state;
    bool retry = false;
    if (!request.cursor.isEmpty()) {
        for (const auto& entry : p.states) {
            if (entry.second->nextCursor == request.cursor || entry.second->cachedCursor == request.cursor) {
                state = entry.second; retry = state->cachedCursor == request.cursor; break;
            }
        }
        if (!state) { fail("SEARCH_CURSOR_EXPIRED"); return; }
        if (state->context.principalId != context.principalId || state->context.tokenEpoch != context.tokenEpoch
            || state->grant.accessVersion != grant->accessVersion) { fail("PERMISSION_DENIED"); return; }
        if (state->grant.captureId != request.captureId || state->queryBytes != request.query || state->mode != request.mode) {
            fail("CURSOR_MISMATCH"); return;
        }
        const auto error = p.validate(*state); if (!error.isEmpty()) { fail(error); return; }
        if (state->busy) { fail("BUSY"); return; }
        if (retry) { callback(state->cached); return; }
        if (state->terminal) { fail("SEARCH_CURSOR_EXPIRED"); return; }
    } else {
        size_t principalStates = 0;
        for (const auto& entry : p.states) if (entry.second->context.principalId == context.principalId) ++principalStates;
        if (p.states.size() >= p.limits.maxStates || principalStates >= p.limits.maxStatesPerPrincipal) { fail("BUSY"); return; }
        state = std::make_shared<Impl::State>(); state->context = context; state->grant = *grant;
        state->id = opaqueHandle(); state->created = state->touched = p.clock.elapsed();
        state->mode = request.mode; state->queryBytes = request.query; state->recordId = grant->firstRecordId;
        state->highWater = resource->capture.highWaterRecordId; state->integrity = resource->capture.integrity;
        if (grant->lastRecordId) state->highWater = (std::min)(state->highWater, grant->lastRecordId);
        if (request.mode == HistorySearchMode::RawBytes) {
            for (const auto c : request.query) state->query.push_back(static_cast<unsigned char>(c));
        } else {
            // A streaming decoder keeps an unfinished suffix without reporting an error.
            // Requests are complete strings: reject replacement or an unfinished code point.
            const QString text = QString::fromUtf8(request.query);
            if (text.isEmpty() || text.toUtf8() != request.query) { fail("INVALID_ARGUMENT"); return; }
            const auto unicode = text.toUcs4(); state->query.assign(unicode.begin(), unicode.end());
        }
        state->prefix.resize(state->query.size());
        for (size_t i = 1, count = 0; i < state->query.size(); ++i) {
            while (count && state->query[i] != state->query[count]) count = state->prefix[count - 1];
            if (state->query[i] == state->query[count]) ++count;
            state->prefix[i] = count;
        }
    }
    if (p.active >= 2 || p.activePrincipals.count(context.principalId)) { fail("BUSY"); return; }
    p.states[state->id] = state; state->busy = true; ++p.active; p.activePrincipals.insert(context.principalId);
    auto job = std::make_shared<Impl::Job>(); job->state = state; job->request = std::move(request);
    job->callback = std::move(callback); job->timer.start(); p.next(job);
}
bool HistorySearchService::cancel(const QString& cursor, const ResourceContext& context) {
    for (auto i = _impl->states.begin(); i != _impl->states.end(); ++i) {
        auto& state = *i->second;
        if ((state.nextCursor == cursor || state.cachedCursor == cursor) && !cursor.isEmpty()
            && state.context.principalId == context.principalId && state.context.tokenEpoch == context.tokenEpoch) {
            state.cancelled = true; _impl->states.erase(i); return true;
        }
    }
    return false;
}
}
