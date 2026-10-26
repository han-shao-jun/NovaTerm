#pragma once
#include "mcp/contracts/Errors.h"
#include "mcp/contracts/SessionBinding.h"
#include "mcp/contracts/ClearRequest.h"
#include "mcp/contracts/Budget.h"
#include "core/scrollback/TerminalLog.h"
#include <QMap>
#include <atomic>
#include <functional>
#include <memory>

namespace NovaTerm::Mcp {
// Separate domain request: the frozen nine terminal application requests stay unchanged.
struct ClearCompletion {
    quint64 generation{0};
    std::optional<LogPage> baseline;
    qint64 capturedAtMs{0}, ticketIssued{0};
    quint64 userGeneration{0};
};
// Application owner of bounded idempotency records. It never executes terminal commands.
class ClearOperations {
public:
    using Completion = std::function<void(Result<ClearCompletion>)>;
    struct Waiter { QString request; Completion done; };
    struct Record {
        quint64 id{0}; QString client; ClearRequest request; quint64 grant{0}; qint64 until{0};
        quint64 source{0}, userGeneration{0};
        std::shared_ptr<std::atomic_bool> cancelled{std::make_shared<std::atomic_bool>(false)};
        std::optional<Result<ClearCompletion>> result;
        std::vector<Waiter> waiters;
    };
    struct Admission { std::shared_ptr<Record> record; bool fresh{false}; };
    Result<Admission> begin(const QString& client, const ClearRequest& request, quint64 grant,
        qint64 until, qint64 now) {
        sweep(now);
        const auto key = client + '/' + request.operation.str();
        const auto found = _records.value(key);
        if (found) {
            const auto& old = found->request;
            if (old.session != request.session || old.epoch != request.epoch || old.all != request.all
                || old.expectedGeneration != request.expectedGeneration || old.clearToken != request.clearToken
                || found->grant != grant)
                return Error{ErrorCode::InputSequenceConflict, "operationId already names a different clear operation."};
            return Admission{found, false};
        }
        if (now >= until) return Error{ErrorCode::CursorStale, "Clear ticket expired; obtain a new baseline."};
        int count = 0;
        for (const auto& record : _records) if (record->client == client) ++count;
        if (count >= Budget::MaxClearRecordsPerClient || _records.size() >= Budget::MaxClearRecords) return Error{ErrorCode::Busy};
        auto record = std::make_shared<Record>();
        record->id = ++_next; record->client = client; record->request = request;
        record->grant = grant; record->until = until;
        _records.insert(key, record);
        return Admission{std::move(record), true};
    }
    bool wait(const std::shared_ptr<Record>& record, QString request, Completion done) {
        if (record->result) { done(*record->result); return true; }
        if (record->waiters.size() >= size_t(Budget::MaxWireCalls)) { done(Error{ErrorCode::Busy}); return false; }
        record->waiters.push_back({std::move(request), std::move(done)}); return true;
    }
    void finish(quint64 id, Result<ClearCompletion> result) {
        for (const auto& record : _records) if (record->id == id && !record->result) {
            const auto keep = record;
            keep->result = std::move(result);
            auto waiters = std::move(keep->waiters);
            for (auto& waiter : waiters) waiter.done(*keep->result);
            return;
        }
    }
    void cancel(const QString& request) {
        for (const auto& record : _records) {
            auto& waiters = record->waiters;
            for (auto it = waiters.begin(); it != waiters.end();) {
                if (it->request == request) { record->cancelled->store(true); it = waiters.erase(it); }
                else ++it;
            }
        }
    }
    void cancelSession(const QString& client, const QString& session) {
        for (const auto& record : _records)
            if ((client.isEmpty() || record->client == client)
                && (session.isEmpty() || record->request.session.str() == session) && !record->result)
                record->cancelled->store(true);
    }
    void sweep(qint64 now) {
        for (auto it = _records.begin(); it != _records.end();)
            if (it.value()->result && now >= it.value()->until) it = _records.erase(it);
            else ++it;
    }
    auto records() const { return _records.values(); }
    std::size_t size() const { return std::size_t(_records.size()); }
private:
    QMap<QString, std::shared_ptr<Record>> _records;
    quint64 _next{0};
};
} // namespace NovaTerm::Mcp
