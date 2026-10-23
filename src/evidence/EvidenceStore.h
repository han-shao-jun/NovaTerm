#pragma once
#include "EvidenceSourceReader.h"
#include <QObject>
#include <map>
#include <memory>
#include <chrono>
namespace NovaTerm::Evidence {
// Reference IDs convey no authority. Every create/resolve rechecks all source ranges.
class EvidenceStore final : public QObject {
public:
    explicit EvidenceStore(QObject* parent=nullptr):QObject(parent){}
    bool registerSource(SourceKind,std::shared_ptr<EvidenceSourceReader>);
    void create(Source,Context,int projectionVersion,std::function<void(Result)>);
    void resolve(const QString& id,Context,std::function<void(Result)>);
    void sweep();
private:
    struct Entry {Reference ref;Context owner;std::chrono::steady_clock::time_point expires;};
    std::map<SourceKind,std::shared_ptr<EvidenceSourceReader>> _sources;
    std::map<QString,Entry> _references;
    size_t _pending{0};
    void access(Source,Context,std::optional<Entry>,int,std::function<void(Result)>);
};
}
