/** @file CommandDedup.cpp @brief 短时重复命令门控实现。 */
#include "CommandDedup.h"

namespace NovaTerm::Mcp {

CommandDedup::CommandDedup(std::chrono::milliseconds window) : _window(window) {}

bool CommandDedup::blocked(const QString& fingerprint) const
{
    return _seen.contains(fingerprint);
}

void CommandDedup::remember(const QString& fingerprint, qint64 now)
{
    _seen.insert(fingerprint, now);
}

void CommandDedup::sweep(qint64 now)
{
    for (auto it = _seen.begin(); it != _seen.end();) {
        if (now - it.value() >= _window.count())
            it = _seen.erase(it);
        else
            ++it;
    }
}

bool CommandDedup::empty() const { return _seen.isEmpty(); }

void CommandDedup::clear() { _seen.clear(); }

} // namespace NovaTerm::Mcp
