#pragma once
#include "EvidenceSourceReader.h"
#include <memory>
class QObject;

namespace NovaTerm::Evidence {
// Only the trusted local UI may construct this after explicit file selection/sharing.
// Never expose this type or shareLocalFile to model/tool argument decoding.
struct TrustedLocalSelection { QString localPath; };
struct ImportedResource {
    Error error = Error::None;
    ResourceBinding binding{};
    QByteArray contentHash{};
    quint64 byteCount = 0;
};
class ImportedResourceStore final {
public:
    ImportedResourceStore();
    ~ImportedResourceStore();
    ImportedResourceStore(const ImportedResourceStore&) = delete;
    ImportedResourceStore& operator=(const ImportedResourceStore&) = delete;
    ImportedResourceStore(ImportedResourceStore&&) = delete;
    ImportedResourceStore& operator=(ImportedResourceStore&&) = delete;
    // Blocking trusted local import; run on a file worker, never in an MCP/GUI handler.
    ImportedResource shareLocalFile(const TrustedLocalSelection&, const QString& ownerPrincipalId);
    // Owner-thread entry for a local file selection. Disk work runs on a bounded worker;
    // destruction suppresses pending delivery. Model/tool entry points must not call this.
    void shareLocalFileAsync(TrustedLocalSelection, QString ownerPrincipalId,
                             std::function<void(ImportedResource)>);
    void remove(const QString& resourceId);
    void clear();
    std::shared_ptr<const EvidenceSourceReader> reader() const;
    static constexpr qsizetype MaxFileBytes = 16 * 1024 * 1024;
    static constexpr qsizetype MaxResources = 32;
private:
    struct State;
    std::shared_ptr<State> _state;
    std::unique_ptr<QObject> _delivery;
    std::shared_ptr<const EvidenceSourceReader> _reader;
};
}
