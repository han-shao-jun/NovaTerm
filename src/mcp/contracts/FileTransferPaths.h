#pragma once
#include "Budget.h"
#include <QDir>
#include <optional>

namespace NovaTerm::Mcp {
struct TransferPath { QString root, name, absolute; };
// Pure syntax only. Fixed-handle workers perform filesystem validation.
inline std::optional<TransferPath> splitTransferPath(QString path, bool remote)
{
    if (!remote) path = QDir::fromNativeSeparators(path);
    if (path.isEmpty() || path.toUtf8().size() > Budget::MaxFileRootPathBytes
        || QString::fromUtf8(path.toUtf8()) != path || path.startsWith("//")
        || path.contains('\\') || path.endsWith('/')) return std::nullopt;
    for (const auto c : path) if (c.category() == QChar::Other_Control) return std::nullopt;
    if (remote) {
        if (!path.startsWith('/') || path.contains(':')) return std::nullopt;
    } else {
#ifdef Q_OS_WIN
        if (path.size() < 4 || !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
            || path[1] != ':' || path[2] != '/' || path.mid(2).contains(':')) return std::nullopt;
        path[0] = path[0].toUpper();
#else
        if (!path.startsWith('/') || path.contains(':')) return std::nullopt;
#endif
    }
    if (QDir::cleanPath(path) != path) return std::nullopt;
    const auto slash = path.lastIndexOf('/');
    auto root = path.left(slash);
    if (root.isEmpty()) root = "/";
    else if (!remote && root.size() == 2 && root[1] == ':') root += '/';
    const auto name = path.mid(slash + 1);
    if (name.isEmpty() || name == "." || name == ".." || name.toUtf8().size() > 255) return std::nullopt;
    return TransferPath{root, name, path};
}
}
