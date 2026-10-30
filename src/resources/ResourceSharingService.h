#pragma once
#include "historysearch/HistorySearchTypes.h"
#include "evidence/EvidenceTypes.h"
#include <QObject>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace NovaTerm::Resources {
// Actual boundary: authenticated principal facts.
// Implementations are injected by the composition root; no protocol or Session objects.
struct CaptureListQuery { int limit{50}; QString after, digest; };
struct SharedCapture {
    Recording::HistoryCaptureId id;
    qint64 createdMs{0};
    quint64 highWater{0};
    Recording::Integrity integrity{Recording::Integrity::UnknownTail};
    bool active{false};
};
struct CaptureListPage { std::vector<SharedCapture> captures; QString nextAfter, digest, error; };
struct EvidenceSearchPage { Recording::HistorySearchPage page; Evidence::EvidenceRef evidence; };
struct ImportResult { QString error, resourceId{}, contentHash{}; quint64 byteCount{0}; };

// Application-thread owner of history access and query/evidence publication.
// Recording source lifetime is independent of imported resources and their evidence.
class ResourceSharingService final : public QObject {
public:
    using IdentityLookup = std::function<quint64(const QString&)>;
    ResourceSharingService(IdentityLookup, QObject* parent = nullptr);
    ~ResourceSharingService() override;
    ResourceSharingService(const ResourceSharingService&) = delete;
    ResourceSharingService& operator=(const ResourceSharingService&) = delete;
    // Replace the recording adapter only; imported resources and evidence stay owned here.
    void setHistorySource(Recording::HistoryReadPort);
    Recording::ResourceContext context(const QString& principal) const;
    std::optional<Recording::ResourceGrant> authorize(const Recording::ResourceContext&, Recording::HistoryCaptureId, quint64 generation = 1) const;
    std::optional<Recording::HistoryResource> describe(Recording::HistoryCaptureId) const;
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
