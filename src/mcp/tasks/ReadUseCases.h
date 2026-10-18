/** @file ReadUseCases.h @brief 只读应用用例；Session 与令牌编码由集成层适配。 */
#pragma once

#include "mcp/application/CapabilityResolver.h"
#include "mcp/contracts/Budget.h"
#include "mcp/contracts/Errors.h"
#include "mcp/contracts/SessionBinding.h"

#include <QList>
#include <QSet>
#include <QStringList>

namespace NovaTerm::Mcp {

struct DirectoryCursor
{
    SessionId after;
    quint64 directoryRevision{0};
    ClientAccessVersion accessVersion;
};

struct ContextCursor
{
    SessionReference session;
    quint64 revision{0};
    ClientAccessVersion accessVersion;
    bool includeViewport{true};
};

/** @brief 一页所需的不可变会话事实；不暴露 Session 对象。 */
struct ReadSession
{
    SessionReference reference;
    QString state;
    QString transport;
    QString displayName;
    bool displayNameTruncated{false};
    bool running{false};
    bool visible{true};
    CapabilitySnapshot capabilities;
};

struct SessionPage
{
    QList<ReadSession> sessions;
    std::optional<DirectoryCursor> next;
};

struct ContextQuery
{
    ClientId client;
    SessionReference session;
    QString owner;
    CaptureId capture;
    bool includeViewport{true};
    std::optional<quint64> sinceRevision;
    qsizetype maxBytes{Budget::ObservationDefaultPayloadBytes};
    int maxLines{Budget::ObservationDefaultLines};
};

/** @brief 有界读取结果；不携带协议对象或已编码令牌。 */
struct ContextSummary
{
    CaptureId capture;
    quint64 revision{0};
    QString capturedAt;
    QString title;
    bool sourceTruncated{false};
    bool outputTruncated{false};
    bool resetRequired{false};
    bool alternateScreen{false};
    bool historyMayPrecedeEpoch{true};
    int cursorRow{0};
    int cursorColumn{0};
    quint64 suppressedDuplicates{0};
    QStringList viewport;
    QStringList recent;
};

struct ContextPage
{
    ContextSummary summary;
    std::optional<ContextCursor> next;
};

struct ReadDiagnostic
{
    DiagnosticId id;
    QString title;
    QString preview;
};

struct DiagnosticCatalog
{
    QString policyVersion;
    QString profileVersion;
    bool profileAvailable{false};
    bool executorAvailable{false};
    bool targetVerified{false};
    QSet<DiagnosticId> allowed;
    QList<ReadDiagnostic> commands;
};

struct CommandList
{
    SessionReference session;
    QString policyVersion;
    QString profileVersion;
    ClientAccessVersion accessVersion;
    qint64 expires{0};
    std::optional<ErrorCode> disabledReason;
    QList<ReadDiagnostic> commands;
};

/**
 * @brief 读取所需的真实 Session/授权边界，所有调用均在所属线程同步完成。
 *
 * pageAfter 仅返回当前客户端可读的条目，按 SessionId 排序且不超过 limit。
 * readContext 必须重新核对 session/epoch，再读取并保存 capture；端口可返回
 * Busy、SessionNotAvailable 等类型化错误，不要求应用层了解采样实现。
 */
class ReadSessionPort
{
public:
    virtual ~ReadSessionPort() = default;
    [[nodiscard]] virtual quint64 directoryRevision() const = 0;
    [[nodiscard]] virtual ClientAccessVersion accessVersion(const ClientId& client) const = 0;
    [[nodiscard]] virtual QList<ReadSession> pageAfter(const ClientId& client,
        const SessionId& after, int limit, bool humanConfirmation) const = 0;
    [[nodiscard]] virtual Result<ContextSummary> readContext(const ContextQuery& query) const = 0;
    [[nodiscard]] virtual DiagnosticCatalog commandCatalog(const ClientId& client,
        const SessionReference& session) const = 0;
};

class ReadUseCases
{
public:
    explicit ReadUseCases(const ReadSessionPort& sessions);

    [[nodiscard]] Result<SessionPage> listSessions(const ClientId& client, int limit,
        const std::optional<DirectoryCursor>& cursor, bool humanConfirmation) const;
    [[nodiscard]] Result<ContextPage> readContext(const ClientId& client,
        ContextQuery query, const std::optional<ContextCursor>& cursor) const;
    [[nodiscard]] Result<CommandList> listCommands(const ClientId& client,
        const ReadSession& session, qint64 now) const;

private:
    const ReadSessionPort& _sessions;
};

} // namespace NovaTerm::Mcp
