#pragma once
#include <QString>
#include <QByteArray>
namespace NovaTerm::Mcp {
// Separate domain request: persistent capture identity is never a live Session binding.
struct HistoryToolRequest {
    enum class Action { List, Read, Search };
    Action action{Action::List};
    QString capture,cursor;
    int limit{32},maxBytes{16384},maxMatches{8};
    bool rawBytes{false};
    QByteArray query;
};
}
