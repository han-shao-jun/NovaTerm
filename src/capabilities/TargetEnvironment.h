#pragma once
#include "CapabilityTypes.h"
namespace NovaTerm::Capabilities {
enum class EnvironmentKind { Unknown, Posix, Linux, PowerShell, Cmd, Bootloader, DeviceCli };
enum class EnvironmentSource { None, UserDeclaration, PairedAdapter, LocalConfirmation };
struct EnvironmentSnapshot {
    SessionBinding binding;
    EnvironmentKind kind{EnvironmentKind::Unknown};
    EnvironmentSource source{EnvironmentSource::None};
    quint64 revision{0};
    TimePoint observedAt{}, validUntil{};
    QString readinessEvidenceId;
    bool ready{false}, oneSubmission{false};
    PrincipalId consentPrincipal;
    CapabilityId consentRecipe;
    QString planDigest;
};
struct AdapterEnvironmentReport {
    SessionBinding binding;
    QString adapter;
    quint64 sequence{0};
    EnvironmentKind kind{EnvironmentKind::Unknown};
    QString readinessEvidenceId;
    bool ready{false};
};
}
