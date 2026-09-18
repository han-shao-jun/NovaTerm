/** @file SessionDirectory.cpp
 *  @brief 会话目录的弱引用、连接 epoch 与私有目标指纹。
 */
#include "SessionDirectory.h"
#include "transport/SshTransport.h"
#include <QCryptographicHash>
#include <QThread>

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString targetIdentity(ITransport* transport)
{
    auto* ssh = qobject_cast<SshTransport*>(transport);
    if (!ssh || !ssh->isConnected())
        return {};
    const auto fingerprint = ssh->serverHostKeyFingerprint();
    if (fingerprint.isEmpty())
        return {};
    const auto& config = ssh->sessionConfig();
    if (config.username.contains(QChar::Null)) return {};
    // 只使用已认证主机身份、端口和用户名；绝不散列或读取密码/私钥口令。
    const QByteArray identity = fingerprint.toUtf8() + '\0'
        + QByteArray::number(config.port) + '\0' + config.username.trimmed().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(identity,
        QCryptographicHash::Sha256).toHex());
}
}

SessionDirectory::SessionDirectory(QObject* parent) : QObject(parent) {}

void SessionDirectory::add(TerminalSession* session)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!session || _entries.contains(session))
        return;
    Entry entry;
    entry.session = session;
    entry.id = session->id().toString(QUuid::WithoutBraces);
    entry.epoch = uuid();
    entry.attachmentId = uuid();
    entry.title = session->runtimeConfig().title.left(1024);
    _entries.insert(session, entry);
    _byId.insert(entry.id, session);
    connect(session, &TerminalSession::stateChanged, this, [this, session] { refresh(session); });
    connect(session, &TerminalSession::connected, this, [this, session] { refresh(session); });
    connect(session, &TerminalSession::disconnected, this, [this, session] { refresh(session); });
    connect(session, &TerminalSession::titleChanged, this, [this, session](const QString& title) {
        auto it = _entries.find(session);
        if (it != _entries.end() && it->title != title) {
            it->title = title.left(1024);
            ++_revision;
            emit changed();
        }
    });
    connect(session, &QObject::destroyed, this, [this, session] { remove(session); });
    refresh(session);
    ++_revision;
    emit changed();
}

void SessionDirectory::remove(TerminalSession* session)
{
    Q_ASSERT(QThread::currentThread() == thread());
    auto it = _entries.find(session);
    if (it == _entries.end())
        return;
    const QString id = it->id;
    _byId.remove(id);
    _entries.erase(it);
    ++_revision;
    emit invalidated(id);
    emit changed();
}

void SessionDirectory::refresh(TerminalSession* session)
{
    auto it = _entries.find(session);
    if (it == _entries.end() || !it->session)
        return;
    const QString id = session->id().toString(QUuid::WithoutBraces);
    const auto generation = session->statistics().generation;
    const auto kind = session->runtimeConfig().transportKind;
    const auto state = session->state();
    const bool identityChanged = it->id != id || it->generation != generation
        || it->transport != session->transport();
    const auto target = targetIdentity(session->transport());
    const bool targetChanged = state == SessionState::Running && target != it->targetFingerprint;
    if (!identityChanged && !targetChanged && it->state == state && it->kind == kind)
        return;
    const QString oldId = it->id;
    if (identityChanged || targetChanged) {
        if (it->id != id || it->transport != session->transport() || targetChanged)
            it->attachmentId = uuid();
        it->epoch = uuid();
        it->id = id;
        if (oldId != id) { _byId.remove(oldId); _byId.insert(id, session); }
        it->generation = generation;
        it->transport = session->transport();
    }
    if (state == SessionState::Running)
        it->targetFingerprint = target;
    it->state = state;
    it->kind = kind;
    ++_revision;
    if (identityChanged || targetChanged || state == SessionState::Closing
        || state == SessionState::Closed)
        emit invalidated(oldId);
    emit changed();
}

QList<SessionDirectory::Entry> SessionDirectory::entries()
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto sessions = _entries.keys();
    for (auto* session : sessions)
        refresh(session);
    auto result = _entries.values();
    std::sort(result.begin(), result.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
    return result;
}

std::optional<SessionDirectory::Entry> SessionDirectory::find(const QString& id)
{
    Q_ASSERT(QThread::currentThread() == thread());
    auto* session = _byId.value(id, nullptr);
    if (!session) return std::nullopt;
    refresh(session);
    const auto entry = _entries.constFind(session);
    if (entry != _entries.cend() && entry->id == id && entry->session) return *entry;
    return std::nullopt;
}

QList<SessionDirectory::Entry> SessionDirectory::pageAfter(const QString& after, int limit,
    const std::function<bool(const Entry&)>& allowed)
{
    Q_ASSERT(QThread::currentThread() == thread());
    QList<Entry> result;
    for (auto it = _byId.upperBound(after); it != _byId.end() && result.size() < limit;) {
        const auto id = it.key();
        ++it;
        const auto entry = find(id);
        if (entry && allowed(*entry)) result.append(*entry);
    }
    return result;
}

QString SessionDirectory::stateName(SessionState state)
{
    switch (state) {
    case SessionState::Created: return QStringLiteral("created");
    case SessionState::Connecting: return QStringLiteral("connecting");
    case SessionState::Running: return QStringLiteral("running");
    case SessionState::Reconnecting: return QStringLiteral("reconnecting");
    case SessionState::Failed: return QStringLiteral("failed");
    case SessionState::Closing: return QStringLiteral("closing");
    case SessionState::Closed: return QStringLiteral("closed");
    }
    return QStringLiteral("closed");
}

QString SessionDirectory::transportName(TransportKind kind)
{
    switch (kind) {
    case TransportKind::LocalShell: return QStringLiteral("local_shell");
    case TransportKind::Ssh: return QStringLiteral("ssh");
    case TransportKind::Serial: return QStringLiteral("serial");
    case TransportKind::Telnet: return QStringLiteral("telnet");
    case TransportKind::Custom: return QStringLiteral("custom");
    }
    return QStringLiteral("custom");
}
