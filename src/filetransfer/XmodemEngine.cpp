/** @file XmodemEngine.cpp @brief XMODEM 握手、确认、重传及精确长度处理。 */
#include "XmodemEngine.h"
#include <algorithm>
#include <utility>
namespace NovaTerm::FileTransfer {
bool XmodemEngine::onStart(TimePoint now)
{
    _codec.reset(); _lastFrame.clear(); _phase = Phase::Handshake;
    _offset = 0; _pendingCount = 0; _block = 1; _canSeen = false; _haveBlock = false;
    _useCrc = _request.config.xmodemMode != XmodemMode::Checksum;
    _blockSize = _request.config.xmodemMode == XmodemMode::OneK ? 1024 : 128;
    if (_request.direction == Direction::Send) {
        if (_request.files.size() != 1) { fail(Error::InvalidRequest); return false; }
        _progress.lengthKnown = true;
        setDeadline(now, _request.config.handshakeRetryMs);
    } else {
        if (_request.receiveName.empty()) { fail(Error::InvalidRequest); return false; }
        _progress.lengthKnown = _request.expectedSize.has_value();
        TransferAction action; action.kind = ActionKind::OfferFile;
        action.file = {_request.receiveName, _request.expectedSize};
        requestAction(std::move(action), now);
    }
    return true;
}
void XmodemEngine::sendControl(std::uint8_t byte)
{
    clearDeadline(); emitByte(byte);
}
void XmodemEngine::sendNext(TimePoint now)
{
    const auto size = *_request.files.front().size;
    if (_offset == size) {
        _phase = Phase::Eot; _progress.state = State::Finishing;
        _lastFrame = {4}; clearDeadline(); emitBytes(_lastFrame); return;
    }
    _pendingCount = static_cast<std::size_t>(std::min<std::uint64_t>(_blockSize, size - _offset));
    TransferAction action; action.kind = ActionKind::ReadAt;
    action.file = _request.files.front(); action.offset = _offset; action.count = _pendingCount;
    clearDeadline(); requestAction(std::move(action), now);
}
void XmodemEngine::resend(TimePoint now)
{
    if (!retry()) return;
    clearDeadline(); emitBytes(_lastFrame);
    if (pendingOutput().size == 0) setDeadline(now, _request.config.responseTimeoutMs);
}
void XmodemEngine::receivePacket(TimePoint now)
{
    const auto& packet = _codec.packet();
    if (_haveBlock && packet.number == static_cast<std::uint8_t>(_block - 1U)) {
        sendControl(6); return;
    }
    if (packet.number != _block) {
        if (retry()) sendControl(0x15);
        return;
    }
    if (_request.expectedSize && _offset >= *_request.expectedSize) {
        fail(Error::SizeMismatch); return;
    }
    auto count = packet.bytes.size();
    if (_request.expectedSize)
        count = static_cast<std::size_t>(std::min<std::uint64_t>(count, *_request.expectedSize - _offset));
    if (count > MaxFileSize - _offset) { fail(Error::ResourceLimit); return; }
    _pendingCount = count;
    TransferAction action; action.kind = ActionKind::WriteAt;
    action.file = {_request.receiveName, _request.expectedSize};
    action.offset = _offset; action.bytes.assign(packet.bytes.begin(), packet.bytes.begin() + static_cast<std::ptrdiff_t>(count));
    _phase = Phase::Data; _progress.state = State::Transferring;
    clearDeadline(); requestAction(std::move(action), now);
}
void XmodemEngine::finishReceive(TimePoint now)
{
    if (_request.expectedSize && _offset != *_request.expectedSize) {
        fail(Error::SizeMismatch); return;
    }
    _progress.state = State::Finishing;
    TransferAction action; action.kind = ActionKind::FinishFile;
    action.file = {_request.receiveName, _request.expectedSize}; action.offset = _offset;
    clearDeadline(); requestAction(std::move(action), now);
}
void XmodemEngine::onByte(std::uint8_t byte, TimePoint now)
{
    if (_request.direction == Direction::Receive
        && (_codec.receiving() || byte == 1 || byte == 2)) {
        _canSeen = false;
        const auto result = _codec.push(byte, _useCrc);
        if (result == XyPacketCodec::Result::Valid && _phase != Phase::Closing) receivePacket(now);
        else if (result == XyPacketCodec::Result::Invalid && _phase != Phase::Closing) {
            if (retry()) sendControl(0x15);
        }
        else if (result == XyPacketCodec::Result::Partial
                 && _phase != Phase::Closing && !_deadline)
            setDeadline(now, _request.config.responseTimeoutMs);
        return;
    }
    if (byte == 0x18) {
        if (_canSeen) cancel(now); else _canSeen = true;
        return;
    }
    _canSeen = false;
    if (byte == 'G') { fail(Error::UnsupportedVariant); return; }
    if (_request.direction == Direction::Receive) {
        if (byte == 4) {
            if (_phase == Phase::Closing) emitByte(6);
            else finishReceive(now);
        }
        return;
    }
    // 已排队但尚未交给通道的包不能被旧反馈确认；部分写同样不能确认。
    if (pendingOutput().size != 0) return;
    if (_phase == Phase::Handshake && (byte == 'C' || byte == 0x15)) {
        _useCrc = byte == 'C';
        if (!_useCrc) _blockSize = 128;
        _phase = Phase::Data; _progress.state = State::Transferring;
        madeProgress(); sendNext(now);
    } else if ((_phase == Phase::Data || _phase == Phase::Eot) && byte == 0x15) {
        resend(now);
    } else if (_phase == Phase::Data && byte == 6) {
        _offset += _pendingCount; _progress.transferredBytes += _pendingCount;
        _block = static_cast<std::uint8_t>(_block + 1U); madeProgress(); sendNext(now);
    } else if (_phase == Phase::Eot && byte == 6) {
        TransferAction action; action.kind = ActionKind::FinishFile;
        action.file = _request.files.front(); action.offset = _offset;
        clearDeadline(); requestAction(std::move(action), now);
    }
}
void XmodemEngine::onOperation(const TransferAction& action, OperationResult result,
                               TimePoint now)
{
    if (action.kind == ActionKind::OfferFile) {
        if (!result.accepted) { cancel(now); return; }
        sendControl(_useCrc ? 'C' : 0x15);
    } else if (action.kind == ActionKind::ReadAt) {
        if (result.bytes.size() != _pendingCount) { fail(Error::SizeMismatch); return; }
        result.bytes.resize(_blockSize, 0x1a);
        _lastFrame = XyPacketCodec::encode(_block, result.bytes, _useCrc);
        clearDeadline(); emitBytes(_lastFrame);
    } else if (action.kind == ActionKind::WriteAt) {
        _offset += _pendingCount; _progress.transferredBytes += _pendingCount;
        _block = static_cast<std::uint8_t>(_block + 1U); _haveBlock = true;
        madeProgress(); sendControl(6);
    } else if (action.kind == ActionKind::FinishFile) {
        _progress.completedFiles = 1;
        if (_request.direction == Direction::Send) {
            clearDeadline(); _progress.state = State::Completed;
        } else {
            _phase = Phase::Closing; _progress.state = State::Closing;
            sendControl(6);
        }
    }
}
void XmodemEngine::onOutputDrained(TimePoint now)
{
    if (_phase == Phase::Closing) {
        if (!_deadline) setDeadline(now, _request.config.closingTimeoutMs);
    } else {
        setDeadline(now, _phase == Phase::Handshake
            ? _request.config.handshakeRetryMs : _request.config.responseTimeoutMs);
    }
}
void XmodemEngine::onTimeout(TimePoint now)
{
    if (_phase == Phase::Closing) { _progress.state = State::Completed; return; }
    _codec.reset();
    if (_request.direction == Direction::Send) {
        if (_phase == Phase::Handshake) {
            if (retry()) setDeadline(now, _request.config.handshakeRetryMs);
        } else resend(now);
    } else if (retry()) {
        sendControl(_phase == Phase::Handshake ? (_useCrc ? 'C' : 0x15) : 0x15);
    }
}
} // namespace NovaTerm::FileTransfer
