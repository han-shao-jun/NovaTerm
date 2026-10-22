#include "ImportedResourceStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QObject>
#include <QThreadPool>
#include <QUuid>
#include <map>
#include <mutex>
#include <optional>

namespace NovaTerm::Evidence {
namespace {
bool safeFile(const QString& path) {
    QFileInfo file(path);
    if (!file.isAbsolute() || !file.isFile() || file.isSymLink()) return false;
    auto parent = file.dir();
    for (;;) {
        const QFileInfo info(parent.absolutePath());
        if (info.isSymLink()) return false;
        if (parent.isRoot()) return true;
        if (!parent.cdUp()) return false;
    }
}
std::optional<QByteArray> readFile(const QString& path) {
    if (!safeFile(path)) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > ImportedResourceStore::MaxFileBytes) return {};
    auto bytes = file.read(ImportedResourceStore::MaxFileBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > ImportedResourceStore::MaxFileBytes || !safeFile(path)) return {};
    return bytes;
}
}
struct ImportedResourceStore::State {
    struct Entry { ResourceBinding binding; QString path; QByteArray hash; quint64 size{0}; };
    std::mutex mutex;
    std::map<QString, Entry> entries;
    QObject* delivery{nullptr}; // Protected by mutex. Owned by the local store on its owner thread.
    int active{0};
    size_t pendingImports{0};
    quint64 revision{0};
    bool alive{true};
    class Reader final : public EvidenceSourceReader {
    public:
        explicit Reader(std::weak_ptr<State> state) : _state(std::move(state)) {}
        void readAsync(const ResourceBinding& binding, const QVector<RawFragment>& fragments, qsizetype maximum,
                       std::function<void(SourceReadResult)> callback) const override {
            const auto state = _state.lock();
            if (!state) { callback({Error::EvidenceExpired}); return; }
            std::optional<Entry> entry;
            Error error = Error::None;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                const auto it = state->entries.find(binding.resourceId);
                if (!state->alive || it == state->entries.end() || binding.kind != SourceKind::ImportedFile
                    || binding.generation != it->second.binding.generation || binding.ownerPrincipalId != it->second.binding.ownerPrincipalId)
                    error = Error::EvidenceExpired;
                else if (state->active >= 2) error = Error::LimitExceeded;
                else { entry = it->second; ++state->active; }
            }
            if (error != Error::None) { callback({error}); return; }
            QThreadPool::globalInstance()->start([state, entry = *entry, fragments, maximum, callback = std::move(callback)]() mutable {
                SourceReadResult result;
                const auto bytes = readFile(entry.path);
                if (!bytes || quint64(bytes->size()) != entry.size
                    || QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256).toHex() != entry.hash)
                    result.error = Error::EvidenceExpired;
                else if (maximum < 0 || maximum > EvidenceStoreLimit || fragments.isEmpty() || fragments.size() > 128)
                    result.error = Error::LimitExceeded;
                else {
                    for (const auto& part : fragments) {
                        if (part.recordId != 0 || part.byteOffset > entry.size || part.byteCount > entry.size - part.byteOffset
                            || part.byteCount > quint64(maximum - result.bytes.size())) {
                            result.error = Error::LimitExceeded; result.bytes.clear(); break;
                        }
                        result.bytes += bytes->mid(qsizetype(part.byteOffset), qsizetype(part.byteCount));
                    }
                    result.generation = entry.binding.generation; result.sourceCompleteness = "complete";
                }
                // Posting and destruction of the delivery context share the same mutex.
                std::lock_guard<std::mutex> lock(state->mutex);
                --state->active;
                if (!state->alive || !state->delivery) return;
                QMetaObject::invokeMethod(state->delivery, [state, entry, result = std::move(result), callback = std::move(callback)]() mutable {
                    {
                        std::lock_guard<std::mutex> guard(state->mutex);
                        if (!state->alive || !state->entries.count(entry.binding.resourceId)) result = {Error::EvidenceExpired};
                    }
                    callback(std::move(result));
                }, Qt::QueuedConnection);
            });
        }
    private:
        static constexpr qsizetype EvidenceStoreLimit = 64 * 1024;
        std::weak_ptr<State> _state;
    };
};
ImportedResourceStore::ImportedResourceStore()
    : _state(std::make_shared<State>()), _delivery(std::make_unique<QObject>()), _reader(std::make_shared<State::Reader>(_state)) {
    _state->delivery = _delivery.get();
}
ImportedResourceStore::~ImportedResourceStore() {
    std::lock_guard<std::mutex> lock(_state->mutex);
    _state->alive = false; _state->delivery = nullptr; _state->entries.clear();
}
ImportedResource ImportedResourceStore::shareLocalFile(const TrustedLocalSelection& selection, const QString& owner) {
    if (owner.isEmpty()) return {Error::InvalidRequest};
    const QString path = QFileInfo(selection.localPath).absoluteFilePath();
    const auto bytes = readFile(path);
    if (!bytes) return {Error::InvalidRequest};
    State::Entry entry;
    entry.binding = {SourceKind::ImportedFile, QUuid::createUuid().toString(QUuid::WithoutBraces), 1, owner};
    entry.path = path; entry.hash = QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256).toHex(); entry.size = quint64(bytes->size());
    std::lock_guard<std::mutex> lock(_state->mutex);
    if (!_state->alive) return {Error::EvidenceExpired};
    if (_state->entries.size() + _state->pendingImports >= size_t(MaxResources)) return {Error::LimitExceeded};
    _state->entries.emplace(entry.binding.resourceId, entry);
    return {Error::None, entry.binding, entry.hash, entry.size};
}
void ImportedResourceStore::shareLocalFileAsync(TrustedLocalSelection selection, QString owner,
                                               std::function<void(ImportedResource)> callback) {
    if (!callback) return;
    const auto state = _state;
    quint64 revision = 0;
    Error admission = Error::None;
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        if (!state->alive) admission = Error::EvidenceExpired;
        else if (state->active >= 2 || state->entries.size() + state->pendingImports >= size_t(MaxResources)) admission = Error::LimitExceeded;
        else { ++state->active; ++state->pendingImports; revision = state->revision; }
    }
    if (admission != Error::None) { callback({admission}); return; }
    QThreadPool::globalInstance()->start([state, revision, selection = std::move(selection), owner = std::move(owner), callback = std::move(callback)]() mutable {
        ImportedResource result;
        State::Entry entry;
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            cancelled = !state->alive || revision != state->revision;
        }
        if (cancelled) result.error = Error::EvidenceExpired;
        else {
            const QString path = QFileInfo(selection.localPath).absoluteFilePath();
            const auto bytes = owner.isEmpty() ? std::optional<QByteArray>{} : readFile(path);
            if (!bytes) result.error = Error::InvalidRequest;
            else {
                entry.binding = {SourceKind::ImportedFile, QUuid::createUuid().toString(QUuid::WithoutBraces), 1, owner};
                entry.path = path; entry.hash = QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256).toHex(); entry.size = quint64(bytes->size());
                result = {Error::None, entry.binding, entry.hash, entry.size};
            }
        }
        // Workers retain plain state only. The owner destroys its QObject context
        // under this same mutex, so neither posting nor late destruction races it.
        std::lock_guard<std::mutex> guard(state->mutex);
        --state->active; --state->pendingImports;
        if (!state->alive || !state->delivery) return;
        if (revision != state->revision) result = {Error::EvidenceExpired};
        if (result.error == Error::None) {
            const auto resourceId = entry.binding.resourceId;
            state->entries.emplace(resourceId, std::move(entry));
        }
        QMetaObject::invokeMethod(state->delivery, [state, revision, result = std::move(result), callback = std::move(callback)]() mutable {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->alive) return;
                if (revision != state->revision || (result.error == Error::None && !state->entries.count(result.binding.resourceId))) result = {Error::EvidenceExpired};
            }
            callback(std::move(result));
        }, Qt::QueuedConnection);
    });
}
void ImportedResourceStore::remove(const QString& id) { std::lock_guard<std::mutex> lock(_state->mutex); _state->entries.erase(id); }
void ImportedResourceStore::clear() { std::lock_guard<std::mutex> lock(_state->mutex); _state->entries.clear(); ++_state->revision; }
std::shared_ptr<const EvidenceSourceReader> ImportedResourceStore::reader() const { return _reader; }
}
