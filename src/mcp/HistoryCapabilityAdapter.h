#pragma once
#include "history/HistorySharingPort.h"
#include "history/HistorySearchService.h"
#include "capabilities/CapabilityDispatcher.h"
#include "capabilities/McpCapabilityAdapter.h"
#include "mcp/contracts/HistoryToolRequest.h"
#include <QPointer>
namespace NovaTerm::Recording {class HistoryService;}
namespace NovaTerm::Mcp {
class AccessStore;
// Integration owner: adapts trusted MCP identity and P2 I/O. Domain services own
// authorization/cursors/search/evidence; this object owns only in-flight deliveries.
class HistoryCapabilityAdapter final : public QObject,public Recording::HistorySharingPort {
public:
    HistoryCapabilityAdapter(AccessStore&,Recording::HistoryService*,Capabilities::CapabilityDispatcher&,QString stateFile,QObject* parent=nullptr);
    ~HistoryCapabilityAdapter() override;
    void call(QString peer,QString id,HistoryToolRequest,const Capabilities::McpCapabilityAdapter&,QString client,std::function<void(QJsonObject)>);
    void cancel(const QString& peer,const QString& id={});
    void stop();
    std::optional<Capabilities::AuthorizationSnapshot> authorization(const QString& client,const Capabilities::HistoryCaptureBinding&) const;
    std::vector<Recording::SharingClient> clients() const override;
    std::optional<Recording::ResourceGrant> grant(const QString&,Recording::HistoryCaptureId) const override;
    bool share(Recording::ResourceGrant) override;
    bool revoke(const QString&,Recording::HistoryCaptureId) override;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
void registerHistoryCapabilities(Capabilities::CapabilityRegistry&);
}
