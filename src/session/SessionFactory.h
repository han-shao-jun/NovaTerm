#pragma once

#include "LocalShellProfile.h"
#include "SessionTypes.h"

#include <memory>
#include <optional>

class TerminalSession;
class CredentialStore;
struct ConnectionProfile;

class SessionFactory
{
public:
    [[nodiscard]] static std::optional<RuntimeConfig>
    resolve(const ConnectionProfile& profile, const QVariantMap& overrides,
            QString* error = nullptr);
    [[nodiscard]] static std::unique_ptr<TerminalSession>
    create(const RuntimeConfig& runtime, const CredentialStore* credentials = nullptr,
           QString* error = nullptr);
    [[nodiscard]] static std::unique_ptr<TerminalSession>
    createLocal(const LocalShellConfig& config, RuntimeConfig runtime = {});
    [[nodiscard]] static std::unique_ptr<TerminalSession>
    createSerial(const SerialConfig& config, RuntimeConfig runtime = {});
    [[nodiscard]] static std::unique_ptr<TerminalSession>
    createSsh(const SshConfig& config, RuntimeConfig runtime = {});
};
