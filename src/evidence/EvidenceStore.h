#pragma once
#include "EvidenceAuthorization.h"
#include "EvidenceSourceReader.h"
#include <memory>

namespace NovaTerm::Evidence {
// One thread owns this service and its callbacks. No process-global state, disk, or QObject dependency.
class EvidenceStore final {
public:
    using Clock = std::function<qint64()>; // Monotonic milliseconds.
    explicit EvidenceStore(EvidenceAuthorization, Clock = {});
    ~EvidenceStore();
    EvidenceStore(const EvidenceStore&) = delete;
    EvidenceStore& operator=(const EvidenceStore&) = delete;
    EvidenceStore(EvidenceStore&&) = delete;
    EvidenceStore& operator=(EvidenceStore&&) = delete;
    void registerSource(SourceKind, std::shared_ptr<const EvidenceSourceReader>);
    void createAsync(EvidenceRequest, ResourceContext, EvidenceCallback);
    void resolveAsync(QString evidenceId, ResourceContext, qsizetype maxBytes, EvidenceCallback);
    void remove(const QString& evidenceId);
    void clear();
    qsizetype size() const;
    static constexpr qsizetype MaxBytes = 64 * 1024;
    static constexpr qsizetype MaxReferences = 256;
    static constexpr qsizetype MaxFragments = 128;
private:
    struct State;
    std::shared_ptr<State> _state;
};
}
