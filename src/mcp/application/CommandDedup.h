/**
 * @file CommandDedup.h
 * @brief 同一目标指纹的短时重复命令门控；过期由显式 sweep 负责。
 */
#pragma once

#include <QHash>
#include <QString>

#include <chrono>

namespace NovaTerm::Mcp {

class CommandDedup
{
public:
    explicit CommandDedup(std::chrono::milliseconds window = std::chrono::seconds(5));

    /** @brief 该目标是否已有未过期的登记；是否过期由 sweep 决定。 */
    [[nodiscard]] bool blocked(const QString& fingerprint) const;
    void remember(const QString& fingerprint, qint64 now);
    void sweep(qint64 now);
    [[nodiscard]] bool empty() const;
    void clear();

private:
    QHash<QString, qint64> _seen;
    std::chrono::milliseconds _window;
};

} // namespace NovaTerm::Mcp
