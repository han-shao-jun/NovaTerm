#pragma once
#include "mcp/contracts/ToolRequests.h"
#include "mcp/contracts/Errors.h"
#include "capabilities/CapabilityDispatcher.h"
#include <QByteArray>
#include <QObject>
#include <QJsonObject>
#include <functional>
#include <memory>

class SessionDirectory;
namespace NovaTerm::Mcp {
class AccessStore;
class ObservationService;
/** @brief 内部调用方可要求新的输入受理，不改变公开工具的精确重试。 */
enum class InputReplayPolicy { Allow, Reject };
class TerminalInteraction final : public QObject {
public:
    using Clock = std::function<qint64()>;
    using Completion = std::function<void(QJsonObject)>;
    using Submitted = std::function<void()>;
    TerminalInteraction(AccessStore*, SessionDirectory*, ObservationService*, Capabilities::CapabilityDispatcher&, QObject* parent, Clock clock = {});
    ~TerminalInteraction() override;
    // Keep caller-owned request/guard/context alive across synchronous control callbacks.
    void call(QString client, QString request,
              NovaTerm::Mcp::ToolRequest toolRequest, Completion completion,
              QByteArray legacyCursorKey, Capabilities::InvocationContext,
              InputReplayPolicy = InputReplayPolicy::Allow, Submitted = {}, QString receiptInputToken = {});
    void cancel(const QString& request);
    void revoke(const QString& client, const QString& session);
    /** @brief Internal receipt guard for tokenless calls; never exposed as a wire parameter. */
    [[nodiscard]] Result<QString> captureInputGuard(const QString& client, const SessionReference&) const;
    /** @brief 新任务受理不能借用既有操作的精确重试分支。 */
    [[nodiscard]] std::optional<Error> validateNewInput(const QString& client,
        const SessionReference&, const QString& inputToken) const;
    QJsonObject capabilities(const QString& client, const QString& session);
    bool ownsCapture(const QString& client, const QString& capture) const;
    bool ownsCursor(const QString& client, const QString& cursor) const;
    std::function<void()> onChanged;
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
}
