/** @file SessionDirectory.cpp
 *  @brief 会话目录的弱引用、连接 epoch 与私有目标指纹。
 */
#include "SessionDirectory.h"
#include "LocalSessionCommandExecutor.h"
#include "InteractiveSessionCommandExecutor.h"
#include "SessionCommandFacade.h"
#include "ShellIntegration.h"
#include "SshSessionCommandExecutor.h"
#include "transport/SshTransport.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QThread>

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
std::unique_ptr<ISessionCommandExecutor> commandExecutor(
    TerminalSession* owner, ITransport* transport, TransportKind kind)
{
    if (owner && ShellIntegration::profileFor(owner->runtimeConfig())) {
        QString identity;
        if (kind == TransportKind::Ssh) {
            auto* ssh = qobject_cast<SshTransport*>(transport);
            if (ssh) {
                SshSessionCommandExecutor probe(ssh);
                identity = probe.targetFingerprint();
            }
        } else if (kind == TransportKind::LocalShell) {
            const QString helper = QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("novaterm-local-diag.exe"));
            LocalSessionCommandExecutor probe(helper);
            identity = probe.targetFingerprint();
        } else {
            const auto& values = owner->runtimeConfig().transport;
            const QString endpoint = kind == TransportKind::Serial
                ? values.value(QStringLiteral("portName")).toString()
                : values.value(QStringLiteral("host")).toString()
                    + QLatin1Char(':')
                    + values.value(QStringLiteral("port")).toString();
            const QByteArray source = (endpoint.isEmpty()
                ? owner->id().toString(QUuid::WithoutBraces)
                : endpoint).toUtf8();
            identity = QString::fromLatin1(QCryptographicHash::hash(
                source, QCryptographicHash::Sha256).toHex());
        }
        if (!identity.isEmpty()) {
            return std::make_unique<InteractiveSessionCommandExecutor>(
                owner->commandCoordinator(),
                CommandPlatformProfile::interactiveFor(kind), identity);
        }
    }
    if (kind == TransportKind::Ssh) {
        auto* ssh = qobject_cast<SshTransport*>(transport);
        return ssh ? std::make_unique<SshSessionCommandExecutor>(ssh) : nullptr;
    }
#ifdef Q_OS_WIN
    if (kind == TransportKind::LocalShell) {
        const QString helper = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("novaterm-local-diag.exe"));
        return std::make_unique<LocalSessionCommandExecutor>(helper);
    }
#endif
    return {};
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
    if (identityChanged) {
        session->commandFacade()->installExecutor(
            commandExecutor(session, session->transport(), kind), generation);
    }
    const auto target = session->commandFacade()->targetFingerprint();
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
