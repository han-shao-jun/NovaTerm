/**
 * @file TargetReservation.h
 * @brief 目标持久标记的 move-only 所有者。
 *
 * 先写标记再提交有副作用操作；析构**不会**释放标记——未知结果必须保留
 * quarantine 保护。只有能证明未开始或已确定终止时才调用 `release()`；
 * 移交给执行记录时直接销毁所有者（执行终态负责解除）。
 */
#pragma once

#include "mcp/contracts/Errors.h"
#include <QString>

#include <memory>

namespace NovaTerm::Mcp {

class CommandTargetStore;

class TargetReservation
{
public:
    TargetReservation() = default;
    TargetReservation(CommandTargetStore& guard, QString fingerprint, QString executionId);
    ~TargetReservation();

    TargetReservation(TargetReservation&& other) noexcept;
    TargetReservation& operator=(TargetReservation&& other) noexcept;
    TargetReservation(const TargetReservation&) = delete;
    TargetReservation& operator=(const TargetReservation&) = delete;

    /**
     * @brief 持久标记写入成功后创建立即生效的所有者。
     * @return 失败时返回空（标记未写入）。
     */
    [[nodiscard]] static std::unique_ptr<TargetReservation> acquire(
        CommandTargetStore& guard, const QString& fingerprint, const QString& executionId,
        const QString& instance, Error& error);

    [[nodiscard]] bool held() const;
    [[nodiscard]] const QString& fingerprint() const { return _fingerprint; }
    [[nodiscard]] const QString& executionId() const { return _executionId; }

    /** @brief 仅在确定未产生副作用或已确认终止时解除；返回是否执行了删除。 */
    bool release();
    /** @brief 放弃所有权但不释放标记；用于移交给执行终态。 */
    void relinquish();

private:
    CommandTargetStore* _guard{nullptr};
    QString _fingerprint;
    QString _executionId;
    bool _releasePending{false};
};

} // namespace NovaTerm::Mcp
