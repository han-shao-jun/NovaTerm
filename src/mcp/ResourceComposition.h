#pragma once
#include "resources/ResourceSharingService.h"
namespace NovaTerm::Mcp {
class AccessStore;
// Concrete identity/private-file adapters. The application service has no dependency on this header.
std::shared_ptr<Resources::ResourceSharingService> makeResourceSharingService(AccessStore&, const QString& stateDirectory);
}
