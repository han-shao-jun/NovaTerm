#include "SerialFileTransfers.h"
#include "contracts/Budget.h"
#include "files/LocalFileAccess.h"
#include "transport/SerialTransport.h"
#include <QScopedValueRollback>
#include <QUuid>
#include <atomic>
#include <limits>

namespace NovaTerm::Mcp {
namespace {
constexpr quint64 UnknownSize = std::numeric_limits<quint64>::max();
struct SourceState {
    std::atomic_bool stopped{false};
    std::atomic<quint64> total{UnknownSize};
};
class ReaderSource final : public SerialTransferSource {
public:
    ReaderSource(std::unique_ptr<Files::LocalFileReader> reader, std::shared_ptr<SourceState> state)
        : _reader(std::move(reader)), _state(std::move(state)) {}
    quint64 size() const noexcept override { return _reader->size(); }
    bool readAt(quint64 offset, qsizetype count, QByteArray& bytes, QString& error) override
    {
        if (_state->stopped.load() || count <= 0 || count > Budget::LocalTransferChunkBytes) {
            error = "Source read refused"; return false;
        }
        auto result = _reader->readAt(offset, count);
        if (_state->stopped.load() || std::holds_alternative<Files::FileIoError>(result)) {
            error = "Source read refused"; return false;
        }
        bytes = std::move(std::get<QByteArray>(result));
        return true;
    }
private:
    std::unique_ptr<Files::LocalFileReader> _reader;
    std::shared_ptr<SourceState> _state;
};
bool sameUpload(const StartSerialFileUploadRequest& a, const StartSerialFileUploadRequest& b)
{
    return a.reference.session == b.reference.session && a.reference.epoch == b.reference.epoch
        && a.resourceId == b.resourceId && a.resourceRevision == b.resourceRevision
        && a.relativePath == b.relativePath && a.targetId == b.targetId
        && a.targetRevision == b.targetRevision && a.remoteName == b.remoteName && a.operationId == b.operationId;
}
QString errorCode(NovaTerm::FileTransfer::Error error)
{
    using E = NovaTerm::FileTransfer::Error;
    switch (error) {
    case E::None: return {};
    case E::InvalidRequest: return "INVALID_ARGUMENT";
    case E::Protocol: return "PROTOCOL_ERROR";
    case E::UnsupportedVariant: return "UNSUPPORTED_VARIANT";
    case E::RetryLimit: return "RETRY_LIMIT";
    case E::Timeout: return "TIMEOUT";
    case E::FileIo: return "FILE_IO_ERROR";
    case E::SizeMismatch: return "SIZE_MISMATCH";
    case E::ResourceLimit: return "RESOURCE_LIMIT";
    }
    return "FILE_TRANSFER_FAILED";
}
}
struct SerialFileTransfers::Record {
    QString id, client, connection, localRoot;
    quint64 identity{0}, ownerId{0};
    StartSerialFileUploadRequest request;
    SerialFileBinding binding;
    QPointer<TerminalSession> session;
    QPointer<SerialFileTransferController> controller;
    std::shared_ptr<SourceState> source{std::make_shared<SourceState>()};
    QList<QMetaObject::Connection> callbacks;
    quint64 confirmed{0}, completedFiles{0};
    std::optional<quint64> total;
    std::optional<qint64> timeout;
    QString state{"accepted"}, error;
    qint64 startedAt{0}, finishedAt{0};
    int baudRate{0};
    bool active{true}, protocolCompleted{false}, cancelling{false}, stopping{false};
};
SerialFileTransfers::SerialFileTransfers(AccessStore& access, SessionDirectory& sessions, FileResourceAdapter& resources,
    SerialFileTargetAdapter& targets, QString instance, QObject* parent, ControllerResolver resolver, Now clock)
    : QObject(parent), _access(access), _sessions(sessions), _resources(resources), _targets(targets),
      _instance(std::move(instance)), _resolve(std::move(resolver)), _now(std::move(clock))
{
    if (!_resolve) _resolve = [](TerminalSession& session) { return session.serialFileTransfer(); };
    _clock.start();
    _timer.setInterval(100);
    connect(&_timer, &QTimer::timeout, this, [this] { check(); });
    connect(&_access, &AccessStore::changed, this, [this] { check(); });
    connect(&_resources, &FileResourceAdapter::changed, this, [this] { check(); });
    connect(&_targets, &SerialFileTargetAdapter::changed, this, [this] { check(); });
    connect(&_sessions, &SessionDirectory::changed, this, [this] { check(); });
    connect(&_sessions, &SessionDirectory::invalidated, this, [this] { check(); });
}
SerialFileTransfers::~SerialFileTransfers()
{
    stop();
    for (const auto& record : _records)
        for (const auto& callback : record->callbacks) QObject::disconnect(callback);
}
qint64 SerialFileTransfers::now() const { return _now ? _now() : _clock.elapsed(); }
Result<SerialFileTransfers::Authority> SerialFileTransfers::authorize(const QString& client, quint64 identity,
    const StartSerialFileUploadRequest& request)
{
    if (!_access.enabled() || !identity || _access.identityVersion(client) != identity) return Error{ErrorCode::PermissionDenied};
    const auto listed = _targets.list(client, identity);
    if (const auto* error = std::get_if<Error>(&listed)) return *error;
    const auto entry = _sessions.find(request.reference.session.str());
    if (!entry || !entry->session || entry->epoch != request.reference.epoch.str()) return Error{ErrorCode::PermissionDenied};
    const auto binding = _targets.liveBinding(entry->id);
    if (!binding || binding->connection.epoch != request.reference.epoch
        || binding->connection.attachment.str() != entry->attachmentId
        || binding->connection.generation.value() != entry->generation) return Error{ErrorCode::PermissionDenied};
    const auto* serial = qobject_cast<SerialTransport*>(entry->transport.data());
    if (!serial || !entry->session || entry->session->transport() != serial || serial->config().baudRate <= 0)
        return Error{ErrorCode::PermissionDenied};
    // All directory callbacks have returned; only pure grants are read below.
    std::optional<FileRoot> local;
    for (const auto& candidate : _resources.resources().local(client, identity))
        if (candidate.id == request.resourceId && candidate.revision == request.resourceRevision && candidate.access.read) local = candidate;
    bool allowed = false;
    for (const auto& candidate : _targets.targets().list(client, identity))
        if (candidate.id == request.targetId && candidate.revision == request.targetRevision
            && SerialFileTargets::sameBinding(candidate.binding, *binding)) allowed = true;
    if (!local || !allowed || !_access.enabled() || _access.identityVersion(client) != identity)
        return Error{ErrorCode::PermissionDenied};
    return Authority{*local, *binding, entry->session, serial->config().baudRate};
}
bool SerialFileTransfers::valid(const std::shared_ptr<Record>& record)
{
    const auto resolved = authorize(record->client, record->identity, record->request);
    const auto* authority = std::get_if<Authority>(&resolved);
    return authority && authority->session == record->session && authority->local.path == record->localRoot
        && SerialFileTargets::sameBinding(authority->binding, record->binding);
}
bool SerialFileTransfers::currentBinding(const std::shared_ptr<Record>& record)
{
    const auto binding = _targets.liveBinding(record->request.reference.session.str());
    return record->session && binding && SerialFileTargets::sameBinding(*binding, record->binding);
}
bool SerialFileTransfers::admit(const std::shared_ptr<Record>& record)
{
    if (!record->active || record->source->stopped.load()) return false;
    const auto total = record->source->total.load();
    if (total != UnknownSize) {
        record->total = total;
        const auto baud = quint64(qMax(1, record->baudRate));
        const auto duration = Budget::SerialFileUploadBaseTimeout.count()
            + qint64((total * Budget::SerialFileUploadBitsPerByte * 1000 + baud - 1) / baud);
        const auto timeout = qMax<qint64>(Budget::SerialFileUploadMinTimeout.count(), duration);
        if (total > Budget::MaxLocalTransferFileBytes || timeout > Budget::SerialFileUploadMaxTimeout.count()) {
            record->timeout.reset();
            record->error = "RESOURCE_LIMIT"; record->source->stopped = true;
            if (record->controller && record->ownerId)
                static_cast<void>(record->controller->abort(record->ownerId, "Transfer budget exceeded"));
            if (record->active) settle(record, "failed", record->error);
            return false;
        }
        record->timeout = timeout;
    }
    if (now() - record->startedAt >= record->timeout.value_or(Budget::SerialFileUploadMinTimeout.count())) {
        record->error = "TIMEOUT"; cancel(record); return false;
    }
    if (!valid(record)) {
        if (record->error.isEmpty()) record->error = "PERMISSION_DENIED";
        cancel(record); return false;
    }
    return record->active && !record->source->stopped.load();
}
void SerialFileTransfers::observe(const std::shared_ptr<Record>& record, const SerialTransferProgress& progress)
{
    if (!record->active || !record->ownerId || record->ownerId != progress.transferId) return;
    if (progress.totalBytes) record->total = progress.totalBytes;
    record->confirmed = qMax(record->confirmed, progress.transferredBytes);
    record->completedFiles = qMax(record->completedFiles, progress.completedFiles);
    if (!record->cancelling && !progress.preparing && progress.active) record->state = "sending";
}
void SerialFileTransfers::settle(const std::shared_ptr<Record>& record, const QString& state, const QString& error)
{
    if (!record->active) return;
    const auto total = record->source->total.load();
    if (total != UnknownSize) record->total = total;
    record->active = false; record->finishedAt = now();
    record->state = state; record->error = error; record->source->stopped = true;
    for (const auto& callback : record->callbacks) QObject::disconnect(callback);
    record->callbacks.clear();
}
void SerialFileTransfers::finish(const std::shared_ptr<Record>& record, const SerialTransferResult& result)
{
    if (!record->active || !record->ownerId || result.transferId != record->ownerId) return;
    observe(record, result.progress);
    record->protocolCompleted = result.protocolCompleted;
    if (result.protocolCompleted) {
        settle(record, record->completedFiles == 1 ? "completed" : "skipped", {});
    } else {
        const auto error = record->error.isEmpty() ? errorCode(result.error) : record->error;
        const auto state = record->cancelling && error != "TIMEOUT" && error != "RESOURCE_LIMIT" ? "cancelled" : "failed";
        settle(record, state, error.isEmpty() ? QString("FILE_TRANSFER_FAILED") : error);
    }
}
QJsonObject SerialFileTransfers::project(const Record& record) const
{
    return {{"instanceId", _instance}, {"transferId", record.id}, {"sessionId", record.request.reference.session.str()},
        {"epoch", record.request.reference.epoch.str()}, {"operationId", record.request.operationId}, {"protocol", "zmodem"},
        {"state", record.state}, {"confirmedBytes", double(record.confirmed)},
        {"totalBytes", record.total ? QJsonValue(double(*record.total)) : QJsonValue(QJsonValue::Null)},
        {"completedFiles", double(record.completedFiles)}, {"protocolCompleted", record.protocolCompleted},
        {"fileConfirmed", record.completedFiles == 1}, {"remotePublication", "unknown"}, {"contentVerified", false},
        {"timeoutMs", record.timeout ? QJsonValue(double(*record.timeout)) : QJsonValue(QJsonValue::Null)},
        {"errorCode", record.error.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(record.error)}};
}
Result<QJsonObject> SerialFileTransfers::call(const QString& client, quint64 identity, const QString& connection,
    const SerialFileTransferRequest& request)
{
    check();
    if (!_access.enabled() || !identity || _access.identityVersion(client) != identity) return Error{ErrorCode::PermissionDenied};
    if (const auto* upload = std::get_if<StartSerialFileUploadRequest>(&request)) {
        for (const auto& record : _records)
            if (record->client == client && record->identity == identity && record->request.operationId == upload->operationId) {
                if (!sameUpload(record->request, *upload)) return Error{ErrorCode::TargetOperationConflict};
                return project(*record);
            }
        const auto resolved = authorize(client, identity, *upload);
        if (const auto* error = std::get_if<Error>(&resolved)) return *error;
        const auto authority = std::get<Authority>(resolved);
        const QPointer<SerialFileTransferController> controller = authority.session ? _resolve(*authority.session) : nullptr;
        int count = 0, active = 0;
        for (const auto& record : _records) {
            if (record->client == client) ++count;
            if (record->active) {
                ++active;
                if (record->client == client || record->request.reference.session == upload->reference.session)
                    return Error{ErrorCode::FileTransferBusy};
            }
        }
        if (count >= Budget::MaxFileTransfersPerClient || _records.size() >= Budget::MaxFileTransfers) return Error{ErrorCode::Busy};
        if (active >= Budget::MaxActiveFileTransfers || !controller) return Error{ErrorCode::FileTransferBusy};
        auto record = std::make_shared<Record>();
        record->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        record->client = client; record->identity = identity; record->connection = connection;
        record->request = *upload; record->binding = authority.binding; record->session = authority.session;
        record->localRoot = authority.local.path; record->baudRate = authority.baudRate;
        record->controller = controller; record->startedAt = now();
        _records.insert(record->id, record);
        record->callbacks.append(connect(controller, &SerialFileTransferController::progressChanged, this,
            [this, record](const SerialTransferProgress& progress) { observe(record, progress); }));
        record->callbacks.append(connect(controller, &SerialFileTransferController::resultReady, this,
            [this, record](const SerialTransferResult& result) { finish(record, result); }));
        record->callbacks.append(connect(controller, &QObject::destroyed, this, [this, record] {
            settle(record, "unknown", "SESSION_NOT_AVAILABLE");
        }));
        const QPointer<SerialFileTransfers> guard(this);
        ::SerialTransferRequest transfer;
        transfer.admission = [this, guard, record] { return guard && admit(record); };
        transfer.source = SerialTransferSourceSpec{upload->remoteName,
            [root = authority.local.path, relative = upload->relativePath, state = record->source]
            (QString& error) -> std::unique_ptr<SerialTransferSource> {
                if (state->stopped.load()) { error = "Source cancelled"; return {}; }
                auto opened = Files::LocalFileReader::open(root, relative);
                if (std::holds_alternative<Files::FileIoError>(opened)) { error = "Source open refused"; return {}; }
                auto reader = std::move(std::get<std::unique_ptr<Files::LocalFileReader>>(opened));
                state->total = reader->size();
                if (state->stopped.load() || reader->size() > Budget::MaxLocalTransferFileBytes) {
                    error = "Source unavailable"; return {};
                }
                return std::make_unique<ReaderSource>(std::move(reader), state);
            }};
        if (!admit(record) || !record->controller || !record->controller->start(transfer, &record->ownerId)) {
            if (record->active) settle(record, "failed", record->error.isEmpty() ? QString("FILE_TRANSFER_BUSY") : record->error);
        }
        _timer.start();
        return project(*record);
    }
    const auto id = std::holds_alternative<QuerySerialFileTransferRequest>(request)
        ? std::get<QuerySerialFileTransferRequest>(request).transferId : std::get<CancelSerialFileTransferRequest>(request).transferId;
    const auto record = _records.value(id);
    if (!record || record->client != client || record->identity != identity) return Error{ErrorCode::PermissionDenied};
    if (std::holds_alternative<CancelSerialFileTransferRequest>(request)) cancel(record);
    return project(*record);
}
bool SerialFileTransfers::busy(const QString& session) const
{
    for (const auto& record : _records) if (record->active && record->request.reference.session.str() == session) return true;
    return false;
}
void SerialFileTransfers::cancel(const std::shared_ptr<Record>& record)
{
    if (!record->active || record->stopping) return;
    const QScopedValueRollback<bool> stopping(record->stopping, true);
    record->source->stopped = true; record->cancelling = true; record->state = "cancel_requested";
    if (record->error.isEmpty()) record->error = "CANCELLED";
    const bool current = currentBinding(record);
    if (!record->active) return;
    if (!record->controller) { settle(record, "unknown", "SESSION_NOT_AVAILABLE"); return; }
    if (!record->ownerId) return;
    if (current) static_cast<void>(record->controller->cancel(record->ownerId));
    else static_cast<void>(record->controller->abort(record->ownerId, "Original UART binding changed"));
}
void SerialFileTransfers::check()
{
    if (_checking) return;
    const QScopedValueRollback<bool> checking(_checking, true);
    const auto records = _records.values();
    for (const auto& record : records) {
        if (record->active) {
            if (record->cancelling) cancel(record);
            else static_cast<void>(admit(record));
        } else if (now() - record->finishedAt >= Budget::FileTransferRetention.count()) _records.remove(record->id);
    }
    if (_records.isEmpty()) _timer.stop();
}
void SerialFileTransfers::disconnect(const QString& connection)
{
    const auto records = _records.values();
    for (const auto& record : records) if (record->connection == connection) cancel(record);
}
void SerialFileTransfers::stop()
{
    _timer.stop();
    const auto records = _records.values();
    for (const auto& record : records) cancel(record);
}
}
