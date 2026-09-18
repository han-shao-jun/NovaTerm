/**
 * @file SessionDirectory.h
 * @brief 非 owning 的运行期会话目录；只允许在 Session/GUI 线程访问。
 */
#pragma once
#include "TerminalSession.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QMap>
#include <functional>

class SessionDirectory final : public QObject
{
    Q_OBJECT
public:
    struct Entry {
        QPointer<TerminalSession> session;
        QPointer<ITransport> transport;
        QString id;
        QString epoch;
        QString attachmentId; ///< Transport 换绑/目标变更时改变，普通同目标重连可保持
        QString title;
        QString targetFingerprint;
        quint64 generation{0};
        SessionState state{SessionState::Created};
        TransportKind kind{TransportKind::LocalShell};
    };
    explicit SessionDirectory(QObject* parent = nullptr);
    void add(TerminalSession* session);
    void remove(TerminalSession* session);
    [[nodiscard]] QList<Entry> entries();
    [[nodiscard]] std::optional<Entry> find(const QString& id);
    /** @brief 按 ID 有界列举，只复制本页通过授权过滤的条目。 */
    [[nodiscard]] QList<Entry> pageAfter(const QString& after, int limit,
        const std::function<bool(const Entry&)>& allowed);
    [[nodiscard]] quint64 revision() const noexcept { return _revision; }
    [[nodiscard]] static QString stateName(SessionState state);
    [[nodiscard]] static QString transportName(TransportKind kind);
signals:
    /** @brief 身份、状态或标题变化；调用方须重新验证授权和排队请求。 */
    void changed();
    void invalidated(const QString& sessionId);
private:
    void refresh(TerminalSession* session);
    QHash<TerminalSession*, Entry> _entries;
    QMap<QString, TerminalSession*> _byId;
    quint64 _revision{1};
};
