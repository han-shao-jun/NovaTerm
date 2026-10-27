#pragma once
#include "session/SessionTypes.h"

namespace NovaTerm {
inline bool sshConnectionMatches(const SshConfig& a, const SshConfig& b)
{
    return a.host == b.host && a.username == b.username && a.port == b.port
        && a.authMethod == b.authMethod && a.password == b.password
        && a.privateKeyPath == b.privateKeyPath && a.keyPassphrase == b.keyPassphrase
        && a.terminalType == b.terminalType && a.keepAliveSeconds == b.keepAliveSeconds
        && a.label == b.label && a.interactiveShellKind == b.interactiveShellKind
        && a.environment == b.environment;
}
inline bool serialConnectionReserved(SessionState state, bool connected, bool pendingDisconnect, bool automaticReconnectPending)
{
    if (state == SessionState::Closed) return false;
    if (state == SessionState::Failed && !connected && !pendingDisconnect && !automaticReconnectPending) return false;
    return true;
}
}
