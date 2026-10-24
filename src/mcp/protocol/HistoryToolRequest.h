#pragma once
#include "mcp/contracts/Identifiers.h"
#include <QByteArray>
#include <QString>
#include <variant>
namespace NovaTerm::Mcp {
// Wire DTOs only. Historical resources are not terminal automation requests.
struct ListCapturesRequest { int limit{50}; QString cursor; };
struct ReadHistoryRequest { HistoryResourceId capture; QString cursor; int maxBytes{65536}; };
struct SearchHistoryRequest {
    HistoryResourceId capture; QByteArray query; bool rawBytes{false}; QString cursor; int maxMatches{128};
};
using HistoryToolRequest = std::variant<ListCapturesRequest, ReadHistoryRequest, SearchHistoryRequest>;
}
