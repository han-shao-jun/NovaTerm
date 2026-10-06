/** @file EngineSupport.cpp @brief 有界输出、时钟、重试和操作标识校验。 */
#include "EngineSupport.h"
#include <algorithm>
#include <limits>
#include <utility>
namespace NovaTerm::FileTransfer {
namespace {
TimePoint deadlineAfter(TimePoint now, TimePoint interval)
{
    return interval > std::numeric_limits<TimePoint>::max() - now
        ? std::numeric_limits<TimePoint>::max() : now + interval;
}
}
bool validFileNameUtf8(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 255) return false;
    for (std::size_t i = 0; i < name.size();) {
        const auto first = static_cast<std::uint8_t>(name[i++]);
        if (!first) return false;
        if (first < 0x80) continue;
        unsigned count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count=1; codepoint=first & 0x1fU; minimum=0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count=2; codepoint=first & 0x0fU; minimum=0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count=3; codepoint=first & 7U; minimum=0x10000; }
        else return false;
        if (count > name.size() - i) return false;
        while (count--) {
            const auto next = static_cast<std::uint8_t>(name[i++]);
            if ((next & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU
            || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
    }
    return true;
}
bool EngineSupport::start(const TransferRequest& request, TimePoint now)
{
    if (_progress.state != State::Idle && !terminal(_progress.state)) return false;
    _operation.reset(); _actionTaken = false;
    clearOutput(); _deadline.reset(); _progress = {}; _retries = 0;
    std::size_t metadataBytes = 0;
    if (request.files.size() > MaxFiles) { fail(Error::InvalidRequest); return false; }
    bool valid = request.files.size() <= MaxFiles
        && (request.direction == Direction::Send || request.direction == Direction::Receive);
    for (const auto& file : request.files) {
        valid = valid && validFileNameUtf8(file.name)
            && file.size && *file.size <= MaxFileSize;
        metadataBytes += file.name.size();
    }
    valid = valid && metadataBytes <= 256 * 1024
        && (!request.expectedSize || *request.expectedSize <= MaxFileSize)
        && validFileNameUtf8(request.receiveName)
        && request.config.handshakeTimeoutMs && request.config.handshakeRetryMs
        && request.config.responseTimeoutMs && request.config.operationTimeoutMs
        && request.config.closingTimeoutMs && request.config.maxRetries;
    if (!valid) { fail(Error::InvalidRequest); return false; }
    _request = request;
    _output.reserve(MaxOutputBytes);
    _handshakeEnd = deadlineAfter(now, request.config.handshakeTimeoutMs);
    _progress.state = State::Negotiating;
    const auto started = onStart(now);
    if (!started && !terminal(_progress.state)) fail(Error::InvalidRequest);
    return started;
}
ConsumeResult EngineSupport::consume(ByteView input, TimePoint now)
{
    ConsumeResult result;
    if (!input.data || input.size <= 0) return result;
    while (result.consumed < static_cast<std::size_t>(input.size)
           && !_operation && _output.size() - _outputHead <= MaxOutputBytes - 16384
           && !terminal(_progress.state) && _progress.state != State::Idle) {
        const auto byte = static_cast<std::uint8_t>(input.data[result.consumed]);
        ++result.consumed;
        onByte(byte, now);
    }
    result.waiting = _operation.has_value()
        || _output.size() - _outputHead > MaxOutputBytes - 16384;
    return result;
}
void EngineSupport::advance(TimePoint now)
{
    if (_progress.state == State::Idle || terminal(_progress.state)) return;
    if (_progress.state == State::Negotiating && now >= _handshakeEnd) {
        fail(Error::Timeout); return;
    }
    if (_operation) {
        if (now >= _operationDeadline) fail(Error::FileIo);
        return;
    }
    if (_deadline && now >= *_deadline) { _deadline.reset(); onTimeout(now); }
}
std::optional<TimePoint> EngineSupport::nextDeadline() const
{
    if (_progress.state == State::Idle || terminal(_progress.state)) return {};
    auto deadline = _operation ? std::optional<TimePoint>{_operationDeadline} : _deadline;
    if (_progress.state == State::Negotiating)
        deadline = deadline ? std::min(*deadline, _handshakeEnd) : _handshakeEnd;
    return deadline;
}
ByteView EngineSupport::pendingOutput() const
{
    if (_outputHead == _output.size()) return {};
    return {reinterpret_cast<const char*>(_output.data() + _outputHead),
            static_cast<isize>(_output.size() - _outputHead)};
}
bool EngineSupport::acknowledgeOutput(std::size_t accepted, TimePoint now)
{
    if (accepted > _output.size() - _outputHead) return false;
    _outputHead += accepted;
    if (accepted && _outputHead == _output.size()) {
        clearOutput();
        if (!terminal(_progress.state)) onOutputDrained(now);
    }
    return true;
}
std::optional<TransferAction> EngineSupport::takeAction()
{
    if (!_operation || _actionTaken) return {};
    _actionTaken = true;
    return _operation;
}
bool EngineSupport::completeOperation(OperationId id, OperationResult result, TimePoint now)
{
    if (!_operation || !_actionTaken || _operation->id != id) return false;
    // 完成事件也检查期限，不能依赖宿主先投递定时器再投递文件结果。
    advance(now);
    if (!_operation) return false;
    auto action = std::move(*_operation); _operation.reset(); _actionTaken = false;
    if (!result.success || result.bytes.size() > MaxInputBytes) {
        fail(Error::FileIo); return true;
    }
    onOperation(action, std::move(result), now);
    return true;
}
void EngineSupport::cancel(TimePoint now)
{
    if (terminal(_progress.state) || _progress.state == State::Idle) return;
    _operation.reset(); _actionTaken = false; _deadline.reset(); clearOutput();
    onCancel(now);
    _progress.state = State::Cancelled;
}
bool EngineSupport::emitBytes(const Bytes& bytes)
{
    const auto pending = _output.size() - _outputHead;
    if (bytes.size() > MaxOutputBytes - pending) { fail(Error::ResourceLimit); return false; }
    if (_outputHead && _output.size() + bytes.size() > MaxOutputBytes) {
        _output.erase(_output.begin(), _output.begin() + static_cast<std::ptrdiff_t>(_outputHead));
        _outputHead = 0;
    }
    _output.insert(_output.end(), bytes.begin(), bytes.end());
    return true;
}
bool EngineSupport::emitByte(std::uint8_t byte) { return emitBytes(Bytes{byte}); }
bool EngineSupport::requestAction(TransferAction action, TimePoint now)
{
    if (_operation || action.bytes.size() > 256 * 1024 || _nextOperation == UINT64_MAX) {
        fail(Error::ResourceLimit); return false;
    }
    action.id = ++_nextOperation;
    _operation = std::move(action); _actionTaken = false;
    _operationDeadline = deadlineAfter(now, _request.config.operationTimeoutMs);
    return true;
}
void EngineSupport::fail(Error error)
{
    _progress.error = error; _progress.state = State::Failed;
    _deadline.reset(); _operation.reset(); _actionTaken = false;
    clearOutput();
}
void EngineSupport::setDeadline(TimePoint now, TimePoint interval)
{ _deadline = deadlineAfter(now, interval); }
bool EngineSupport::retry()
{
    if (_retries >= _request.config.maxRetries) { fail(Error::RetryLimit); return false; }
    ++_retries; ++_progress.retransmissions; return true;
}
void EngineSupport::onOutputDrained(TimePoint) {}
void EngineSupport::onCancel(TimePoint) { emitBytes(Bytes{0x18, 0x18, 0x18, 0x18, 0x18}); }
} // namespace NovaTerm::FileTransfer
