/** @file TargetReservation.cpp @brief 持久目标标记 owner 实现。 */
#include "TargetReservation.h"
#include "mcp/CommandPolicy.h"

#include <utility>

namespace NovaTerm::Mcp {

TargetReservation::TargetReservation(TargetGuard& guard, QString fingerprint,
    QString executionId)
    : _guard(&guard), _fingerprint(std::move(fingerprint)),
      _executionId(std::move(executionId))
{
}

TargetReservation::~TargetReservation()
{
    // 显式 release 失败时保留 owner；析构再试一次，仍失败则保留隔离标记。
    if (_guard && _releasePending)
        _guard->release(_fingerprint, _executionId);
}

TargetReservation::TargetReservation(TargetReservation&& other) noexcept
    : _guard(other._guard), _fingerprint(std::move(other._fingerprint)),
      _executionId(std::move(other._executionId)),
      _releasePending(other._releasePending)
{
    other._guard = nullptr;
    other._releasePending = false;
}

TargetReservation& TargetReservation::operator=(TargetReservation&& other) noexcept
{
    if (this == &other) return *this;
    if (_guard && _releasePending)
        _guard->release(_fingerprint, _executionId);
    _guard = other._guard;
    _fingerprint = std::move(other._fingerprint);
    _executionId = std::move(other._executionId);
    _releasePending = other._releasePending;
    other._guard = nullptr;
    other._releasePending = false;
    return *this;
}

std::unique_ptr<TargetReservation> TargetReservation::acquire(TargetGuard& guard,
    const QString& fingerprint, const QString& executionId, const QString& instance,
    QString& error)
{
    if (!guard.reserve(fingerprint, executionId, instance, error))
        return nullptr;
    return std::unique_ptr<TargetReservation>(
        new TargetReservation(guard, fingerprint, executionId));
}

bool TargetReservation::held() const
{
    return _guard && !_executionId.isEmpty();
}

bool TargetReservation::release()
{
    if (!held())
        return false;
    _releasePending = true;
    if (!_guard->release(_fingerprint, _executionId))
        return false;
    relinquish();
    return true;
}

void TargetReservation::relinquish()
{
    _guard = nullptr;
    _fingerprint.clear();
    _executionId.clear();
    _releasePending = false;
}

} // namespace NovaTerm::Mcp
