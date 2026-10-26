/** @file TerminalTasks.h @brief GUI 线程中的有界长任务索引，不拥有执行器。 */
#pragma once
#include "contracts/TerminalTaskRequest.h"
#include "capabilities/CapabilityAuthorization.h"
#include <QObject>
#include <QJsonObject>
#include <functional>
#include <memory>

class SessionDirectory;
namespace NovaTerm::Mcp {
class AccessStore;
class TerminalInteraction;
/** @brief 仅关联 Session 拥有的执行事实；所有调用在 GUI 线程。 */
class TerminalTasks final : public QObject {
public:
    using Completion = std::function<void(QJsonObject)>;
    TerminalTasks(AccessStore&, SessionDirectory&, TerminalInteraction&,
        QString instance, QByteArray logKey, QObject* parent = nullptr);
    ~TerminalTasks() override;
    void call(const QString& client, const QString& requestKey,
        const TerminalTaskRequest&, Completion, const Capabilities::InvocationContext&);
    void cancel(const QString& requestKey);
    void revoke(const QString& client, const QString& session);
    void stop();
    QJsonObject capabilities(const QString& client, const QString& session);
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
}
