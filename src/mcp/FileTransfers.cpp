#include "FileTransfers.h"
#include "contracts/Budget.h"
#include "contracts/FileTransferPaths.h"
#include <QScopedValueRollback>
#include <QUuid>

namespace NovaTerm::Mcp {
namespace {
bool sameUpload(const StartFileUploadRequest& a, const StartFileUploadRequest& b)
{
    return a.reference.session == b.reference.session && a.reference.epoch == b.reference.epoch
        && a.localPath == b.localPath && a.remotePath == b.remotePath && a.operationId == b.operationId
        && a.overwrite == b.overwrite;
}
}
struct FileTransfers::Record {
    QString id, client, connection;
    quint64 identity{0};
    StartFileUploadRequest request;
    SessionBinding binding;
    SftpFileUploadRequest upload;
    QPointer<TerminalSession> session;
    quint64 transferred{0};
    std::optional<quint64> total;
    QString state{"accepted"}, publication{"not_started"}, temporary{"none"}, error;
    QString abortError;
    qint64 startedAt{0}, finishedAt{0};
    bool active{true}, published{false};
};
FileTransfers::FileTransfers(AccessStore& access, SessionDirectory& sessions, FileResourceAdapter& resources,
    QString instance, QObject* parent, Start start, Now now)
    : QObject(parent), _access(access), _sessions(sessions), _resources(resources),
      _instance(std::move(instance)), _start(std::move(start)), _now(std::move(now))
{
    _clock.start();
    _timer.setInterval(100);
    connect(&_timer, &QTimer::timeout, this, [this] { check(); });
    connect(&_resources, &FileResourceAdapter::changed, this, [this] { check(); });
    connect(&_access, &AccessStore::changed, this, [this] { check(); });
    connect(&_sessions, &SessionDirectory::changed, this, [this] { check(); });
    connect(&_sessions, &SessionDirectory::invalidated, this, [this](const QString&) { check(); });
}
FileTransfers::~FileTransfers() { stop(); }
qint64 FileTransfers::now() const { return _now ? _now() : _clock.elapsed(); }
Result<FileTransfers::Authority> FileTransfers::authorize(const QString& client, quint64 identity,
    const StartFileUploadRequest& request, SftpFileTransferDirection direction)
{
    Q_UNUSED(direction)
    if (!_access.enabled() || !identity || _access.identityVersion(client) != identity)
        return Error{ErrorCode::PermissionDenied};
    const auto local = splitTransferPath(request.localPath, false);
    const auto remote = splitTransferPath(request.remotePath, true);
    if (!local || !remote) return Error{ErrorCode::InvalidArgument};
    const auto binding = _resources.liveBinding(request.reference.session.str());
    if (!binding || binding->epoch != request.reference.epoch) return Error{ErrorCode::PermissionDenied};
    const auto entry = _sessions.find(request.reference.session.str());
    if (!entry || !entry->session || entry->epoch != request.reference.epoch.str()
        || !_access.enabled() || _access.identityVersion(client) != identity)
        return Error{ErrorCode::PermissionDenied};
    const SessionBinding current{SessionId{entry->id}, AttachmentId{entry->attachmentId}, Epoch{entry->epoch},
        Generation{entry->generation}, entry->targetFingerprint};
    if (!FileResources::sameBinding(current, *binding)) return Error{ErrorCode::PermissionDenied};
    return Authority{*local, *remote, *binding, entry->session};
}
bool FileTransfers::valid(const std::shared_ptr<Record>& record)
{
    const auto authority = authorize(record->client, record->identity, record->request, record->upload.direction);
    const auto* current = std::get_if<Authority>(&authority);
    return current && current->session == record->session
        && FileResources::sameBinding(current->binding, record->binding)
        && current->local.root == record->upload.localRoot && current->remote.root == record->upload.remoteRoot;
}
QJsonObject FileTransfers::project(const Record& record) const
{
    return {{"instanceId", _instance}, {"transferId", record.id}, {"sessionId", record.request.reference.session.str()},
        {"epoch", record.request.reference.epoch.str()}, {"operationId", record.request.operationId}, {"state", record.state},
        {"transferredBytes", double(record.transferred)}, {"totalBytes", record.total ? QJsonValue(double(*record.total)) : QJsonValue(QJsonValue::Null)},
        {"published", record.published}, {"publication", record.publication}, {"temporary", record.temporary},
        {"errorCode", record.error.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(record.error)}, {"contentVerified", false}};
}
std::optional<Result<QJsonObject>> FileTransfers::replay(const QString& client, quint64 identity,
    const FileTransferRequest& request) const
{
    std::optional<StartFileUploadRequest> fields;
    auto direction = SftpFileTransferDirection::Upload;
    if (const auto* upload = std::get_if<StartFileUploadRequest>(&request)) fields = *upload;
    else if (const auto* download = std::get_if<StartFileDownloadRequest>(&request)) {
        fields = StartFileUploadRequest{download->reference, download->localPath, download->remotePath,
            download->operationId, download->overwrite, {}, {}};
        direction = SftpFileTransferDirection::Download;
    }
    if (!fields) return std::nullopt;
    if (!_access.enabled() || !identity || _access.identityVersion(client) != identity)
        return Result<QJsonObject>{Error{ErrorCode::PermissionDenied}};
    const auto normalize = [](StartFileUploadRequest value) {
        if (const auto local = splitTransferPath(value.localPath, false)) value.localPath = local->absolute;
        if (const auto remote = splitTransferPath(value.remotePath, true)) value.remotePath = remote->absolute;
        return value;
    };
    const auto normalized = normalize(*fields);
    for (const auto& record : _records) {
        if (record->client != client || record->identity != identity || record->request.operationId != fields->operationId) continue;
        if (!record->active && now() - record->finishedAt >= Budget::FileTransferRetention.count()) continue;
        if (record->upload.direction != direction || !sameUpload(normalize(record->request), normalized))
            return Result<QJsonObject>{Error{ErrorCode::TargetOperationConflict}};
        return Result<QJsonObject>{project(*record)};
    }
    return std::nullopt;
}
Result<QJsonObject> FileTransfers::call(const QString& client, quint64 identity, const QString& connection,
    const FileTransferRequest& request)
{
    check();
    if (!_access.enabled() || !identity || _access.identityVersion(client) != identity) return Error{ErrorCode::PermissionDenied};
    std::optional<StartFileUploadRequest> fields;
    auto direction = SftpFileTransferDirection::Upload;
    if (const auto* upload = std::get_if<StartFileUploadRequest>(&request)) fields = *upload;
    else if (const auto* download = std::get_if<StartFileDownloadRequest>(&request)) {
        fields = StartFileUploadRequest{download->reference, download->localPath, download->remotePath, download->operationId, download->overwrite, {}, {}};
        direction = SftpFileTransferDirection::Download;
    }
    if (fields) {
        if (const auto previous = replay(client, identity, request)) return *previous;
        const auto* upload = &*fields;
        int count = 0, active = 0;
        const auto records = _records.values();
        for (const auto& record : records) {
            if (record->active) ++active;
            if (record->client != client || record->identity != identity) continue;
            ++count;

        }
        const auto resolved = authorize(client, identity, *upload, direction);
        if (const auto* error = std::get_if<Error>(&resolved)) return *error;
        const auto authority = std::get<Authority>(resolved);
        if (count >= Budget::MaxFileTransfersPerClient || _records.size() >= Budget::MaxFileTransfers)
            return Error{ErrorCode::Busy};
        if (active >= Budget::MaxActiveFileTransfers || busy(upload->reference.session.str()) || !_start)
            return Error{ErrorCode::FileTransferBusy};
        for (const auto& record : records)
            if (record->active && record->client == client) return Error{ErrorCode::FileTransferBusy};
        auto record = std::make_shared<Record>();
        record->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        record->client = client; record->identity = identity; record->connection = connection;
        record->request = *upload; record->binding = authority.binding; record->session = authority.session;
        record->startedAt = now();
        record->upload = {++_nextRequest, authority.local.root, authority.local.name,
            authority.remote.root, authority.remote.name, std::make_shared<SftpFileUploadControl>(), direction, upload->overwrite};
        _records.insert(record->id, record);
        const QPointer<FileTransfers> guard(this);
        Delivery delivery{
            [this, guard, record](quint64 bytes, quint64 total) {
                if (!guard || !record->active) return;
                if (bytes > total || total > Budget::MaxLocalTransferFileBytes || bytes < record->transferred) { cancel(record); return; }
                record->transferred = bytes; record->total = total;
                if (record->state != "cancel_requested") record->state = record->upload.direction == SftpFileTransferDirection::Download
                    ? "downloading" : "uploading";
            },
            [this, guard, record] {
                if (!guard || !record->active) { record->upload.control->approve(false); return; }
                const bool allowed = valid(record);
                record->upload.control->approve(allowed);
                if (!allowed) cancel(record, "PERMISSION_DENIED");
            },
            [this, guard, record](const SftpFileUploadResult& result) {
                if (!guard || !record->active || result.requestId != record->upload.requestId) return;
                record->active = false; record->finishedAt = now();
                record->transferred = qMax(record->transferred, result.transferredBytes);
                if (result.totalBytes) record->total = result.totalBytes;
                record->published = result.published;
                record->publication = result.publication; record->temporary = result.temporary;
                record->error = result.errorCode;
                // The worker also uses its cancellation flag to unwind failures.
                // Preserve the cause; that flag alone does not mean user cancellation.
                if (!record->abortError.isEmpty() && (record->error.isEmpty()
                    || record->error == "TRANSFER_CANCELLED" || record->error == "CANCELLED"))
                    record->error = record->abortError;
                const bool cancelled = record->error == "TRANSFER_CANCELLED" || record->error == "CANCELLED"
                    || (record->error.isEmpty() && record->upload.control->state.load() == SftpFileUploadControl::State::Cancelled);
                record->state = result.published ? "completed" : result.publication == "unknown" ? "unknown"
                    : cancelled ? "cancelled" : "failed";
            }};
        // Recheck current file authority immediately before the SFTP queue handoff.
        if (!valid(record) || !record->session || !_start || !_start(*record->session, record->upload, std::move(delivery))) {
            if (record->active) {
                record->active = false; record->finishedAt = now();
                record->state = "failed"; record->error = "FILE_TRANSFER_BUSY";
                record->upload.control->cancel();
            }
        }
        _timer.start();
        return project(*record);
    }
    const auto id = std::holds_alternative<QueryFileTransferRequest>(request)
        ? std::get<QueryFileTransferRequest>(request).transferId : std::get<CancelFileTransferRequest>(request).transferId;
    const auto record = _records.value(id);
    if (!record || record->client != client || record->identity != identity) return Error{ErrorCode::PermissionDenied};
    if (std::holds_alternative<CancelFileTransferRequest>(request)) cancel(record);
    return project(*record);
}
bool FileTransfers::busy(const QString& session) const
{
    for (const auto& record : _records) if (record->active && record->request.reference.session.str() == session) return true;
    return false;
}
void FileTransfers::cancel(const std::shared_ptr<Record>& record, const QString& failure)
{
    if (!record->active) return;
    if (record->state != "cancel_requested") record->abortError = failure;
    record->upload.control->cancel();
    record->state = "cancel_requested";
}
void FileTransfers::check()
{
    if (_checking) return;
    const QScopedValueRollback<bool> checking(_checking, true);
    const auto currentTime = now();
    const auto records = _records.values();
    for (const auto& record : records) {
        if (record->active) {
            if (currentTime - record->startedAt >= Budget::FileUploadTimeout.count()) cancel(record, "TRANSFER_TIMEOUT");
            else if (!valid(record)) cancel(record, "PERMISSION_DENIED");
        } else if (currentTime - record->finishedAt >= Budget::FileTransferRetention.count()) _records.remove(record->id);
    }
    if (_records.isEmpty()) _timer.stop();
}
void FileTransfers::disconnect(const QString& connection)
{
    for (const auto& record : _records) if (record->connection == connection) cancel(record);
}
void FileTransfers::stop()
{
    _timer.stop();
    for (const auto& record : _records) cancel(record);
}
}
