#pragma once

#include "SessionTypes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <memory>

class TerminalSession;

class SessionManager final : public QObject
{
    Q_OBJECT
public:
    explicit SessionManager(QObject* parent = nullptr);
    ~SessionManager() override;

    SessionId add(std::unique_ptr<TerminalSession> session, bool start = true);
    [[nodiscard]] TerminalSession* find(const SessionId& id) const;
    [[nodiscard]] QList<SessionId> sessionIds() const;
    [[nodiscard]] qsizetype size() const noexcept { return _sessions.size(); }
    bool close(const SessionId& id, CloseMode mode = CloseMode::Graceful);
    void closeAll(CloseMode mode = CloseMode::Graceful);

signals:
    void sessionAdded(const SessionId& id);
    void sessionRemoved(const SessionId& id);

private:
    QHash<SessionId, TerminalSession*> _sessions;
};
