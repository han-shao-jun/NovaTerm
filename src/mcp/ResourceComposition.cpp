#include "ResourceComposition.h"
#include "McpAccess.h"
#include <QPointer>
namespace NovaTerm::Mcp {
std::shared_ptr<Resources::ResourceSharingService> makeResourceSharingService(AccessStore& access, const QString&) {
    const QPointer<AccessStore> authority(&access);
    auto identity = [authority](const QString& principal) -> quint64 {
        if (authority && authority->enabled() && authority->accessId() == principal)
            return authority->identityVersion(principal);
        return 0;
    };
    return std::make_shared<Resources::ResourceSharingService>(std::move(identity));
}
}
