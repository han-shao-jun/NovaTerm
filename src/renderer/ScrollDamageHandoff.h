#pragma once

#include <QtGlobal>

#include <limits>
#include <utility>

namespace NovaTerm {

// Two-stage hand-off for terminal scroll publications. screenScrolled arrives
// before the scheduler publishes the matching damage frame, so rows remain
// queued until that content frame is handed to the renderer. A render callback
// consumes only pending rows and cannot erase scrolls queued for a later frame.
class ScrollDamageHandoff final
{
public:
    void queue(int rows) noexcept
    {
        if (rows <= 0)
            return;
        _queuedRows = saturatedAdd(_queuedRows, rows);
    }

    void publish() noexcept
    {
        _pendingRows = saturatedAdd(
            _pendingRows, std::exchange(_queuedRows, 0));
    }

    [[nodiscard]] int takePending() noexcept
    {
        return std::exchange(_pendingRows, 0);
    }

    [[nodiscard]] int queuedRows() const noexcept { return _queuedRows; }
    [[nodiscard]] int pendingRows() const noexcept { return _pendingRows; }

private:
    static int saturatedAdd(int current, int added) noexcept
    {
        Q_ASSERT(current >= 0);
        Q_ASSERT(added >= 0);
        return added > std::numeric_limits<int>::max() - current
            ? std::numeric_limits<int>::max() : current + added;
    }

    int _queuedRows{0};
    int _pendingRows{0};
};

} // namespace NovaTerm
