/** @file TerminalTextWait.h @brief 基于现有日志与屏幕的只读有界等待。 */
#pragma once
#include "mcp/contracts/WaitTextRequest.h"
#include "mcp/contracts/ToolRequests.h"
#include "mcp/contracts/Errors.h"
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>
class SessionDirectory;
namespace NovaTerm::Mcp {
class AccessStore;
class ObservationService;
/** @brief GUI 线程只读协调器，不拥有终端输入或历史缓存。 */
class TerminalTextWait final : public QObject {
public:
    using Completion = std::function<void(QJsonObject)>;
    /** @brief 窄数据源端口；缺省复用既有 B1 日志适配器。 */
    using LogReader = std::function<Result<QJsonObject>(const QString& client, const ReadContextRequest&)>;
    TerminalTextWait(AccessStore&, SessionDirectory&, ObservationService&,
        QString instance, QByteArray logKey, QObject* parent = nullptr,
        std::function<qint64()> monotonicClock = {}, LogReader logReader = {});
    ~TerminalTextWait() override;
    void call(const QString& client, const QString& requestKey, const WaitTextRequest&, Completion);
    void cancel(const QString& requestKey);
    /** @brief 回收 RPC 等待，不产生无法交付的续扫票据。 */
    void discard(const QString& requestKey);
    void revoke(const QString& client, const QString& session);
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
}
