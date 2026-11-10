/** @file SessionDirectory.cpp
 *  @brief 会话目录的弱引用、连接 epoch 与私有目标指纹。
 */
#include "SessionDirectory.h"
#include "ShellIntegration.h"
#include "transport/SshTransport.h"
#include <QCryptographicHash>
#include <QSysInfo>
#include <QThread>

#include <optional>

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString targetFingerprint(
    TerminalSession* owner, ITransport* transport, TransportKind kind)
{
    const auto fingerprint = [](const QByteArray& identity) {
        return QString::fromLatin1(
            QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    };
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    if (kind == TransportKind::LocalShell) {
        const auto machineId = QSysInfo::machineUniqueId();
        if (machineId.isEmpty()) return {};
        // 保持已有本机凭据绑定的身份域，不再依赖诊断程序或执行器。
#ifdef Q_OS_WIN
        return fingerprint(machineId + '\0' + "windows-local-v1");
#else
        return fingerprint(machineId + '\0' + "linux-local-v1");
#endif
    }
#endif
    if (kind == TransportKind::Ssh) {
        const auto* ssh = qobject_cast<SshTransport*>(transport);
        if (!ssh || !ssh->isConnected()) return {};
        const auto hostKey = ssh->serverHostKeyFingerprint();
        const auto& config = ssh->sessionConfig();
        if (hostKey.isEmpty() || config.username.contains(QChar::Null)) return {};
        return fingerprint(hostKey.toUtf8() + '\0' + QByteArray::number(config.port)
            + '\0' + config.username.trimmed().toUtf8());
    }
    const auto interactiveProfile = owner
        ? ShellIntegration::profileFor(owner->runtimeConfig()) : std::nullopt;
    if (interactiveProfile && !interactiveProfile->allowUnverifiedPrompt
        && kind != TransportKind::LocalShell) {
        const auto& values = owner->runtimeConfig().transport;
        const QString endpoint = kind == TransportKind::Serial
            ? values.value(QStringLiteral("portName")).toString()
            : values.value(QStringLiteral("host")).toString()
                + QLatin1Char(':') + values.value(QStringLiteral("port")).toString();
        return fingerprint((endpoint.isEmpty()
            ? owner->id().toString(QUuid::WithoutBraces) : endpoint).toUtf8());
    }
    return {};
}
}

SessionDirectory::SessionDirectory(QObject* parent) : QObject(parent) {}

QString SessionDirectory::targetFingerprintFor(TerminalSession* session)
{
    return session ? targetFingerprint(session, session->transport(),
        session->runtimeConfig().transportKind) : QString{};
}

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
    connect(session, &QObject::destroyed, this, [this, session] { remove(session); });
    refresh(session);
    ++_revision;
    emit changed();
}

void SessionDirectory::setDisplayTitle(TerminalSession* session, const QString& title)
{
    Q_ASSERT(QThread::currentThread() == thread());
    auto it = _entries.find(session);
    const QString boundedTitle = title.left(1024);
    if (it == _entries.end() || it->title == boundedTitle)
        return;
    it->title = boundedTitle;
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
    const auto target = targetFingerprintFor(session);
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
