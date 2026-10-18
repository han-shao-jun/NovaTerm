#pragma once
#include "mcp/contracts/ToolRequests.h"
#include <QByteArray>
#include <QObject>
#include <QJsonObject>
#include <functional>
#include <memory>

class SessionDirectory;
namespace NovaTerm::Mcp {
class AccessStore;
class ObservationService;
class TerminalInteraction final : public QObject {
public:
    using Completion = std::function<void(QJsonObject)>;
    TerminalInteraction(AccessStore*, SessionDirectory*, ObservationService*, QObject* parent);
    ~TerminalInteraction() override;
    void call(const QString& client, const QString& request,
              const NovaTerm::Mcp::ToolRequest& toolRequest, Completion completion,
              const QByteArray& legacyCursorKey = {});
    void cancel(const QString& request);
    void revoke(const QString& client, const QString& session);
    QJsonObject capabilities(const QString& client, const QString& session,
                             bool humanConfirmation = false);
    bool ownsCapture(const QString& client, const QString& capture) const;
    bool ownsCursor(const QString& client, const QString& cursor) const;
    std::function<void()> onChanged;
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
}
