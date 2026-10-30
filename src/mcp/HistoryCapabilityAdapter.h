#pragma once
#include "capabilities/CapabilityAuthorization.h"
#include "recording/HistoryTypes.h"
#include "protocol/HistoryToolRequest.h"
#include "resources/ResourceSharingService.h"
#include <QJsonObject>
#include <QObject>
#include <atomic>
#include <memory>
namespace NovaTerm::Recording { class HistoryService; }
namespace NovaTerm::Capabilities { class CapabilityDispatcher; }
namespace NovaTerm::Mcp {
class AccessStore;
// Integration boundary only: authenticated identity and JSON.
// History owns read/search state; Evidence owns source-independent references.
class HistoryCapabilityAdapter final : public QObject {
public:
    using ImportResult = Resources::ImportResult;
    explicit HistoryCapabilityAdapter(AccessStore&, QString stateDirectory, QObject* parent = nullptr);
    HistoryCapabilityAdapter(AccessStore&, std::shared_ptr<Resources::ResourceSharingService>, QObject* parent = nullptr);
    ~HistoryCapabilityAdapter() override;
    void bindDispatcher(Capabilities::CapabilityDispatcher&);
    std::shared_ptr<Resources::ResourceSharingService> resources() const;
    HistoryCapabilityAdapter(const HistoryCapabilityAdapter&) = delete;
    HistoryCapabilityAdapter& operator=(const HistoryCapabilityAdapter&) = delete;
    HistoryCapabilityAdapter(HistoryCapabilityAdapter&&) = delete;
    HistoryCapabilityAdapter& operator=(HistoryCapabilityAdapter&&) = delete;
    void setSource(Recording::HistoryService*);
    void importFile(QString client, QString selectedPath, std::function<void(ImportResult)>);
    void clearImportedFiles(const QString& client);
    Capabilities::ResourceBinding binding(const HistoryToolRequest&) const;
    quint64 accessVersion(const QString& client, const Capabilities::ResourceBinding&) const;
    std::optional<Capabilities::AuthorizationSnapshot> snapshot(const QString& client,
        const Capabilities::ResourceBinding&) const;
    void call(const HistoryToolRequest&, const Capabilities::InvocationContext&, std::shared_ptr<std::atomic<bool>> cancelled,
              std::function<void(QJsonObject)>);
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
