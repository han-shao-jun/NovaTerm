/** @file ITransferEngine.h @brief 不依赖通道、线程或文件系统的协议事件接口。 */
#pragma once
#include "TransferTypes.h"
namespace NovaTerm::FileTransfer {
class ITransferEngine {
public:
    virtual ~ITransferEngine() = default;
    /** @brief 启动新任务；活动任务期间拒绝启动，旧操作标识永不复用。 */
    virtual bool start(const TransferRequest& request, TimePoint now) = 0;
    /** @brief 消费输入前缀；调用方必须保留未消费后缀。 */
    virtual ConsumeResult consume(ByteView input, TimePoint now) = 0;
    virtual void advance(TimePoint now) = 0;
    [[nodiscard]] virtual std::optional<TimePoint> nextDeadline() const = 0;
    /** @brief 输出视图在下一次非 const 调用前有效。 */
    [[nodiscard]] virtual ByteView pendingOutput() const = 0;
    /** @brief 仅确认通道已接受的前缀；超出当前输出长度返回 false。 */
    virtual bool acknowledgeOutput(std::size_t accepted, TimePoint now) = 0;
    [[nodiscard]] virtual std::optional<TransferAction> takeAction() = 0;
    /** @brief 返回 false 表示过期、未取出或不匹配的操作结果。 */
    virtual bool completeOperation(OperationId id, OperationResult result,
                                   TimePoint now) = 0;
    virtual void cancel(TimePoint now) = 0;
    [[nodiscard]] virtual Progress progress() const = 0;
};
} // namespace NovaTerm::FileTransfer
