#pragma once
#include <QString>
namespace NovaTerm::Mcp {
struct ListSavedTargetsRequest { int limit{25}; QString cursor; };
}
