#pragma once
#include "historysearch/HistorySearchTypes.h"
#include "evidence/EvidenceTypes.h"
#include <QObject>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace NovaTerm::Resources {
// Actual boundaries: authenticated principal facts and private grant persistence.
// Implementations are injected by the composition root; no protocol or Session objects.
struct GrantPersistence {
    std::function<std::vector<Recording::ResourceGrant>()> load;
    std::function<bool(const std::vector<Recording::ResourceGrant>&)> save;
};
struct CaptureListQuery { int limit{50}; QString after, digest; };
struct SharedCapture {
    Recording::HistoryCaptureId id;
    qint64 createdMs{0};
    quint64 highWater{0}, first{1}, last{0};
    Recording::Integrity integrity{Recording::Integrity::UnknownTail};
    bool active{false}, persistent{false};
};
struct CaptureListPage { std::vector<SharedCapture> captures; QString nextAfter, digest, error; };
struct EvidenceSearchPage { Recording::HistorySearchPage page; Evidence::EvidenceRef evidence; };
struct ImportResult { QString error, resourceId{}, contentHash{}; quint64 byteCount{0}; };

// Application-thread owner of explicit sharing transactions and query/evidence publication.
// Recording source lifetime is independent of imported resources and their evidence.
class ResourceSharingService final : public QObject {
public:
    using IdentityLookup = std::function<quint64(const QString&)>;
    ResourceSharingService(IdentityLookup, GrantPersistence = {}, QObject* parent = nullptr);
    ~ResourceSharingService() override;
    ResourceSharingService(const ResourceSharingService&) = delete;
    ResourceSharingService& operator=(const ResourceSharingService&) = delete;
    // Replace the recording adapter only; imported resources and evidence stay owned here.
    void setHistorySource(Recording::HistoryReadPort);
    Recording::ResourceContext context(const QString& principal) const;
    std::optional<Recording::ResourceGrant> authorize(const Recording::ResourceContext&, Recording::HistoryCaptureId, quint64 generation = 1) const;
    std::optional<Recording::HistoryResource> describe(Recording::HistoryCaptureId) const;
    QString share(const QString& principal, Recording::HistoryCaptureId, quint64 first, quint64 last, bool persistent);
    QString revoke(const QString& principal, Recording::HistoryCaptureId);
    QString sharingStatus(const QString& principal, Recording::HistoryCaptureId) const;
    CaptureListPage list(const Recording::ResourceContext&, const CaptureListQuery&) const;
    void readAsync(Recording::HistoryReadRequest, Recording::ResourceContext, std::function<void(Recording::HistoryReadPage)>);
    void searchAsync(Recording::HistorySearchRequest, Recording::ResourceContext, std::function<void(EvidenceSearchPage)>);
    void importFile(QString principal, QString selectedPath, std::function<void(ImportResult)>);
    void clearImportedFiles(const QString& principal);
    void createEvidence(Evidence::EvidenceRequest, Recording::ResourceContext, Evidence::EvidenceCallback);
    void resolveEvidence(QString id, Recording::ResourceContext, qsizetype maxBytes, Evidence::EvidenceCallback);
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
