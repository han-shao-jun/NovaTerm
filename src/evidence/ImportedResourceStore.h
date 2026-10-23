#pragma once
#include "EvidenceSourceReader.h"
#include <map>
#include <chrono>
namespace NovaTerm::Evidence {
// Local-only import/diagnostic publication API. No model-facing path resolver.
// Bytes are immutable, memory-only, owner-bound and expire after five minutes.
class ImportedResourceStore final : public EvidenceSourceReader {
public:
    std::optional<Source> publish(QByteArray,Context,QJsonObject provenance={},SourceKind=SourceKind::ImportedFile);
    void remove(const QString& resource);
    bool authorized(const Source&,const Context&) const override;
    bool available(const Source&) const override;
    void read(const Source&,const Context&,Done) override;
private:
    struct Entry {QByteArray bytes;Context owner;QJsonObject provenance;SourceKind kind;std::chrono::steady_clock::time_point expires;};
    std::map<QString,Entry> _resources;
    void sweep();
};
}
