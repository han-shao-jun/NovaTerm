#include "SessionManager.h"

#include "TerminalSession.h"

SessionManager::SessionManager(QObject* parent)
    : QObject(parent)
{
}

SessionManager::~SessionManager()
{
    closeAll(CloseMode::Abort);
}

SessionId SessionManager::add(std::unique_ptr<TerminalSession> session, bool start)
{
    if (!session)
        return {};
    const SessionId id = session->id();
    if (id.isNull() || _sessions.contains(id))
        return {};

    TerminalSession* raw = session.release();
    raw->setParent(this);
    _sessions.insert(id, raw);
    connect(raw, &TerminalSession::stateChanged, this,
            [this, id, raw](SessionState state) {
        if (state != SessionState::Closed || _sessions.value(id) != raw)
            return;
        _sessions.remove(id);
        raw->deleteLater();
        emit sessionRemoved(id);
    });
    connect(raw, &QObject::destroyed, this, [this, id, raw] {
        if (_sessions.value(id) == raw) {
            _sessions.remove(id);
            emit sessionRemoved(id);
        }
    });
    emit sessionAdded(id);
    if (start && !raw->start())
        raw->close(CloseMode::Abort);
    return id;
}

TerminalSession* SessionManager::find(const SessionId& id) const
{
    return _sessions.value(id, nullptr);
}

QList<SessionId> SessionManager::sessionIds() const
{
    return _sessions.keys();
}

bool SessionManager::close(const SessionId& id, CloseMode mode)
{
    TerminalSession* session = find(id);
    if (!session)
        return false;
    session->close(mode);
    return true;
}

void SessionManager::closeAll(CloseMode mode)
{
    const auto sessions = _sessions.values();
    for (TerminalSession* session : sessions) {
        if (session)
            session->close(mode);
    }
}
