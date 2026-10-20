#pragma once
#include "CapabilityTypes.h"
namespace NovaTerm::Capabilities {
// Immutable after composition; no handlers, schemas, selected target or task state.
class CapabilityRegistry
{
public:
    static CapabilityRegistry terminalCapabilities();
    bool add(Definition definition);
    std::optional<Definition> find(const CapabilityId&) const;
    std::optional<Definition> findByMcpTool(const QString&) const;
    const std::vector<Definition>& definitions() const noexcept { return _definitions; }
    static constexpr unsigned CatalogVersion = 1;
private:
    std::vector<Definition> _definitions;
};
}
