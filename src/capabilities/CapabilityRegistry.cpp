#include "CapabilityRegistry.h"
#include <type_traits>
namespace NovaTerm::Capabilities {
ResourceKind resourceKind(const ResourceBinding& binding) { return static_cast<ResourceKind>(binding.index()); }
bool sameResource(const ResourceBinding& a, const ResourceBinding& b)
{
    if (a.index() != b.index()) return false;
    return std::visit([&b](const auto& left) {
        using T = std::decay_t<decltype(left)>;
        const auto& right = std::get<T>(b);
        if constexpr (std::is_same_v<T, DirectoryBinding>) return true;
        else if constexpr (std::is_same_v<T, SessionBinding>)
            return left.session == right.session && left.attachment == right.attachment
                && left.epoch == right.epoch && left.connectionGeneration == right.connectionGeneration
                && left.targetFingerprint == right.targetFingerprint;
        else if constexpr (std::is_same_v<T, ImportedFileBinding>)
            return left.id == right.id && left.contentHash == right.contentHash && left.size == right.size;
        else return left.id == right.id && left.generation == right.generation;
    }, a);
}
bool CapabilityRegistry::add(Definition definition)
{
    if (definition.id.empty() || definition.version == 0
        || definition.status != ImplementationStatus::Implemented || find(definition.id)
        || (!definition.mcpTool.isEmpty() && findByMcpTool(definition.mcpTool))) return false;
    _definitions.push_back(std::move(definition));
    return true;
}
std::optional<Definition> CapabilityRegistry::find(const CapabilityId& id) const
{
    for (const auto& definition : _definitions) if (definition.id == id) return definition;
    return std::nullopt;
}
std::optional<Definition> CapabilityRegistry::findByMcpTool(const QString& tool) const
{
    if (tool.isEmpty()) return std::nullopt;
    for (const auto& definition : _definitions) if (definition.mcpTool == tool) return definition;
    return std::nullopt;
}
CapabilityRegistry CapabilityRegistry::terminalCapabilities()
{
    CapabilityRegistry registry;
    const auto add = [&registry](const char* id, const char* tool, Policy policy,
                                ResourceKind kind = ResourceKind::Session) {
        registry.add({CapabilityId{QString::fromLatin1(id)}, 1, ImplementationStatus::Implemented,
            kind, policy, QString::fromLatin1(tool)});
    };
    add("session.list", "novaterm_list_sessions", Policy::DirectoryRead, ResourceKind::Directory);
    add("terminal.context.read", "novaterm_read_context", Policy::ReadOutput);
    add("terminal.context.search", "novaterm_search_context", Policy::ReadOutput);
    add("terminal.diagnostic.list", "novaterm_list_commands", Policy::ReadOutput);
    add("terminal.diagnostic.execute", "novaterm_execute_command", Policy::Diagnostic);
    add("terminal.command.run", "novaterm_run_command", Policy::RiskCommand);
    add("terminal.script.run", "novaterm_run_script", Policy::Script);
    add("terminal.interact", "novaterm_interact_terminal", Policy::FullInput);
    add("terminal.elevation.request", "novaterm_request_elevation", Policy::Elevation);
    return registry;
}
CapabilityRegistry CapabilityRegistry::implementedCapabilities()
{
    auto registry = terminalCapabilities();
    registry.add({CapabilityId{"history.capture.list"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Directory, Policy::HistoryRead, "novaterm_list_captures"});
    registry.add({CapabilityId{"history.raw.read"}, 1, ImplementationStatus::Implemented,
        ResourceKind::HistoryCapture, Policy::HistoryRead, "novaterm_read_history"});
    registry.add({CapabilityId{"history.literal.search"}, 1, ImplementationStatus::Implemented,
        ResourceKind::HistoryCapture, Policy::HistoryRead, "novaterm_search_history"});
    registry.add({CapabilityId{"terminal.clear"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Session, Policy::ClearTerminal, "novaterm_clear_terminal"});
    registry.add({CapabilityId{"terminal.text.wait"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Session, Policy::ReadOutput, "novaterm_wait_text"});
    registry.add({CapabilityId{"terminal.task.start"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Session, Policy::FullInput, "novaterm_start_task"});
    registry.add({CapabilityId{"terminal.task.query"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Session, Policy::ReadOutput, "novaterm_query_task"});
    registry.add({CapabilityId{"terminal.task.stop"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Session, Policy::FullInput, "novaterm_stop_task"});
    registry.add({CapabilityId{"file.resources.list"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Directory, Policy::DirectoryRead, "novaterm_list_file_resources"});
    registry.add({CapabilityId{"target.list"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Directory, Policy::DirectoryRead, "novaterm_list_targets"});
    registry.add({CapabilityId{"target.open"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Directory, Policy::DirectoryRead, "novaterm_open_target"});
    registry.add({CapabilityId{"target.query"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Directory, Policy::DirectoryRead, "novaterm_query_target"});
    registry.add({CapabilityId{"target.close"}, 1, ImplementationStatus::Implemented,
        ResourceKind::Directory, Policy::DirectoryRead, "novaterm_close_target"});
    return registry;
}
}
