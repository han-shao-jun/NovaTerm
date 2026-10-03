/** @file EngineSupport.h @brief 单线程引擎的有界输出与异步文件操作公共支持。 */
#pragma once
#include "ITransferEngine.h"
namespace NovaTerm::FileTransfer {
/** @brief 统一生命周期契约；各协议只实现 on* 状态机回调。 */
class EngineSupport : public ITransferEngine {
public:
    bool start(const TransferRequest& request, TimePoint now) final;
    ConsumeResult consume(ByteView input, TimePoint now) final;
    void advance(TimePoint now) final;
    [[nodiscard]] std::optional<TimePoint> nextDeadline() const final;
    [[nodiscard]] ByteView pendingOutput() const final;
    bool acknowledgeOutput(std::size_t accepted, TimePoint now) final;
    [[nodiscard]] std::optional<TransferAction> takeAction() final;
    bool completeOperation(OperationId id, OperationResult result, TimePoint now) final;
    void cancel(TimePoint now) final;
    [[nodiscard]] Progress progress() const final { return _progress; }
protected:
    virtual bool onStart(TimePoint now) = 0;
    virtual void onByte(std::uint8_t byte, TimePoint now) = 0;
    virtual void onTimeout(TimePoint now) = 0;
    virtual void onOperation(const TransferAction& action, OperationResult result,
                              TimePoint now) = 0;
    virtual void onOutputDrained(TimePoint now);
    virtual void onCancel(TimePoint now);
    /** @brief 追加输出；超限使任务失败，输出不静默丢弃。 */
    bool emitBytes(const Bytes& bytes);
    bool emitByte(std::uint8_t byte);
    /** @brief 创建唯一在途操作；宿主必须先 takeAction 再完成。 */
    bool requestAction(TransferAction action, TimePoint now);
    void fail(Error error);
    void setDeadline(TimePoint now, TimePoint interval);
    void clearDeadline() { _deadline.reset(); }
    /** @brief 重试预算；返回 false 时已进入失败状态。 */
    bool retry();
    void madeProgress() { _retries = 0; }
    void clearOutput() { _output.clear(); _outputHead = 0; }
    TransferRequest _request;
    Progress _progress;
    std::optional<TimePoint> _deadline;
private:
    Bytes _output;
    std::size_t _outputHead{0};
    std::optional<TransferAction> _operation;
    bool _actionTaken{false};
    OperationId _nextOperation{0};
    TimePoint _operationDeadline{0};
    TimePoint _handshakeEnd{0};
    unsigned _retries{0};
};
} // namespace NovaTerm::FileTransfer
