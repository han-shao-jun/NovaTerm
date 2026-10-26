#pragma once
#include "mcp/contracts/ToolRequests.h"
#include "mcp/contracts/Errors.h"
#include "core/scrollback/TerminalLog.h"
#include "mcp/contracts/Budget.h"
#include <QJsonObject>
class SessionDirectory;
namespace NovaTerm::Mcp {
class AccessStore;
[[nodiscard]] Result<QJsonObject> readLogContext(AccessStore&, SessionDirectory&,
    const QString& instance, const QString& client, const QByteArray& key, const ReadContextRequest&);
[[nodiscard]] QJsonObject encodeLogPage(const LogPage&, const QString& instance, const QString& client,
    const SessionReference&, quint64 grant, const QByteArray& key, std::optional<quint64> nextGeneration = std::nullopt,
    std::optional<qint64> capturedAtMs = std::nullopt, std::optional<qint64> ticketIssued = std::nullopt,
    quint64 userGeneration = 0);
[[nodiscard]] qint64 clearTicketClock();
inline constexpr qint64 ClearTicketLifetimeMs = Budget::ClearRecordRetention.count();
}
