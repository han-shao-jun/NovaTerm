#pragma once
#include <QString>
#include <chrono>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace NovaTerm::Capabilities {
template<class Tag> class Id
{
public:
    Id() = default;
    explicit Id(QString value) : _value(std::move(value)) {}
    const QString& value() const noexcept { return _value; }
    bool empty() const noexcept { return _value.isEmpty(); }
    friend bool operator==(const Id& a, const Id& b) { return a._value == b._value; }
    friend bool operator!=(const Id& a, const Id& b) { return !(a == b); }
    friend bool operator<(const Id& a, const Id& b) { return a._value < b._value; }
private:
    QString _value;
};
using CapabilityId = Id<struct CapabilityTag>;
using PrincipalId = Id<struct PrincipalTag>;
using OperationId = Id<struct OperationTag>;
using SessionId = Id<struct SessionTag>;
using AttachmentId = Id<struct AttachmentTag>;
using Epoch = Id<struct EpochTag>;
using ResourceId = Id<struct ResourceTag>;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
enum class EntryKind { Mcp };
enum class ResourceKind { Directory, Session, HistoryCapture, DiagnosticResult, ImportedFile };
enum class ImplementationStatus { Implemented, Planned };
enum class Policy { DirectoryRead, ReadOutput, Diagnostic, RiskCommand, Script, FullInput, Elevation, HistoryRead, ClearTerminal };
struct DirectoryBinding {};
struct SessionBinding {
    SessionId session;
    AttachmentId attachment;
    Epoch epoch;
    quint64 connectionGeneration{0};
    std::optional<QString> targetFingerprint;
};
struct HistoryCaptureBinding { ResourceId id; quint64 generation{0}; };
struct DiagnosticResultBinding { ResourceId id; quint64 generation{0}; };
struct ImportedFileBinding { ResourceId id; QString contentHash; quint64 size{0}; };
using ResourceBinding = std::variant<DirectoryBinding, SessionBinding, HistoryCaptureBinding,
    DiagnosticResultBinding, ImportedFileBinding>;
ResourceKind resourceKind(const ResourceBinding& binding);
bool sameResource(const ResourceBinding& a, const ResourceBinding& b);
struct Definition {
    CapabilityId id;
    unsigned version{1};
    ImplementationStatus status{ImplementationStatus::Implemented};
    ResourceKind resource{ResourceKind::Session};
    Policy policy{Policy::ReadOutput};
    QString mcpTool;
};
} // namespace NovaTerm::Capabilities
