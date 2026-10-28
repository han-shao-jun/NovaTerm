#include "FileTransfers.h"
#include "contracts/Budget.h"
#include <QScopedValueRollback>
#include <QUuid>

namespace NovaTerm::Mcp {
namespace {
bool sameUpload(const StartFileUploadRequest& a, const StartFileUploadRequest& b)
{
    return a.reference.session == b.reference.session && a.reference.epoch == b.reference.epoch
        && a.resourceId == b.resourceId && a.resourceRevision == b.resourceRevision
        && a.relativePath == b.relativePath && a.scopeId == b.scopeId
        && a.scopeRevision == b.scopeRevision && a.remoteName == b.remoteName && a.operationId == b.operationId
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
    qint64 startedAt{0}, finishedAt{0};
    bool active{true}, published{false};
};
FileTransfers::FileTransfers(AccessStore& access, SessionDirectory& sessions, FileResourceAdapter& resources,
    QString instance, QObject* parent, Start start)
    : QObject(parent), _access(access), _sessions(sessions), _resources(resources),
      _instance(std::move(instance)), _start(std::move(start))
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
Result<FileTransfers::Authority> FileTransfers::authorize(const QString& client, quint64 identity,
    const StartFileUploadRequest& request, SftpFileTransferDirection direction)
{
    const bool download = direction == SftpFileTransferDirection::Download;
    const auto listing = _resources.list(client, identity, ListFileResourcesRequest{request.reference});
    if (const auto* error = std::get_if<Error>(&listing)) return *error;
    const auto& directory = std::get<FileResourceDirectory>(listing);
    std::optional<FileRoot> local;
    std::optional<RemoteFileRoot> remote;
    for (const auto& candidate : directory.local)
        if (candidate.id == request.resourceId && candidate.revision == request.resourceRevision
            && (download ? candidate.access.write : candidate.access.read)) local = candidate;
    for (const auto& candidate : directory.remote)
        if (candidate.root.id == request.scopeId && candidate.root.revision == request.scopeRevision
            && (download ? candidate.root.access.read : candidate.root.access.write)) remote = candidate;
    if (!local || !remote) return Error{ErrorCode::PermissionDenied};
    const auto entry = _sessions.find(request.reference.session.str());
    if (!entry || !entry->session || entry->epoch != request.reference.epoch.str()) return Error{ErrorCode::PermissionDenied};
    const SessionBinding binding{SessionId{entry->id}, AttachmentId{entry->attachmentId}, Epoch{entry->epoch},
        Generation{entry->generation}, entry->targetFingerprint};
    if (!FileResources::sameBinding(binding, remote->binding)) return Error{ErrorCode::PermissionDenied};
    // find() may synchronously invalidate grants. Re-read the pure directory
    // after that callback boundary before returning any authority to a worker.
    bool localCurrent = false, remoteCurrent = false;
    for (const auto& candidate : _resources.resources().local(client, identity))
        if (candidate.id == local->id && candidate.revision == local->revision
            && (download ? candidate.access.write : candidate.access.read)) localCurrent = true;
    for (const auto& candidate : _resources.resources().remote(client, identity, binding))
        if (candidate.id == remote->root.id && candidate.revision == remote->root.revision
            && (download ? candidate.access.read : candidate.access.write)) remoteCurrent = true;
    if (!localCurrent || !remoteCurrent) return Error{ErrorCode::PermissionDenied};
    return Authority{*local, *remote, entry->session};
}
bool FileTransfers::valid(const std::shared_ptr<Record>& record)
{
    const auto authority = authorize(record->client, record->identity, record->request, record->upload.direction);
    const auto* current = std::get_if<Authority>(&authority);
    return current && current->session == record->session
        && FileResources::sameBinding(current->remote.binding, record->binding)
        && current->local.path == record->upload.localRoot && current->remote.root.path == record->upload.remoteRoot;
}
QJsonObject FileTransfers::project(const Record& record) const
{
    return {{"instanceId", _instance}, {"transferId", record.id}, {"sessionId", record.request.reference.session.str()},
        {"epoch", record.request.reference.epoch.str()}, {"operationId", record.request.operationId}, {"state", record.state},
        {"transferredBytes", double(record.transferred)}, {"totalBytes", record.total ? QJsonValue(double(*record.total)) : QJsonValue(QJsonValue::Null)},
        {"published", record.published}, {"publication", record.publication}, {"temporary", record.temporary},
        {"errorCode", record.error.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(record.error)}, {"contentVerified", false}};
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
        fields = StartFileUploadRequest{download->reference, download->resourceId, download->resourceRevision,
            download->relativePath, download->scopeId, download->scopeRevision, download->remoteName, download->operationId, download->overwrite};
        direction = SftpFileTransferDirection::Download;
    }
    if (fields) {
        const auto* upload = &*fields;
        int count = 0, active = 0;
        const auto records = _records.values();
        for (const auto& record : records) {
            if (record->active) ++active;
            if (record->client != client || record->identity != identity) continue;
            ++count;
            if (record->request.operationId == upload->operationId) {
                if (record->upload.direction != direction || !sameUpload(record->request, *upload))
                    return Error{ErrorCode::TargetOperationConflict};
                return project(*record);
            }
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
        record->request = *upload; record->binding = authority.remote.binding; record->session = authority.session;
        record->startedAt = _clock.elapsed();
        record->upload = {++_nextRequest, authority.local.path, upload->relativePath,
            authority.remote.root.path, upload->remoteName, std::make_shared<SftpFileUploadControl>(), direction, upload->overwrite};
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
                if (!allowed) cancel(record);
            },
            [this, guard, record](const SftpFileUploadResult& result) {
                if (!guard || !record->active || result.requestId != record->upload.requestId) return;
                record->active = false; record->finishedAt = _clock.elapsed();
                record->transferred = qMax(record->transferred, result.transferredBytes);
                if (result.totalBytes) record->total = result.totalBytes;
                record->published = result.published;
                record->publication = result.publication; record->temporary = result.temporary;
                record->error = result.errorCode;
                record->state = result.published ? "completed" : result.publication == "unknown" ? "unknown"
                    : record->upload.control->state.load() == SftpFileUploadControl::State::Cancelled ? "cancelled" : "failed";
            }};
        // Recheck current file authority immediately before the SFTP queue handoff.
        if (!valid(record) || !record->session || !_start || !_start(*record->session, record->upload, std::move(delivery))) {
            if (record->active) {
                record->active = false; record->finishedAt = _clock.elapsed();
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
void FileTransfers::cancel(const std::shared_ptr<Record>& record)
{
    if (!record->active) return;
    record->upload.control->cancel();
    record->state = "cancel_requested";
}
void FileTransfers::check()
{
    if (_checking) return;
    const QScopedValueRollback<bool> checking(_checking, true);
    const auto now = _clock.elapsed();
    const auto records = _records.values();
    for (const auto& record : records) {
        if (record->active) {
            if (now - record->startedAt >= Budget::FileUploadTimeout.count() || !valid(record)) cancel(record);
        } else if (now - record->finishedAt >= Budget::FileTransferRetention.count()) _records.remove(record->id);
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
