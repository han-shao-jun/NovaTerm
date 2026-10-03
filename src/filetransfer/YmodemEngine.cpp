/** @file YmodemEngine.cpp @brief 文件头校验、确认后落盘与完整批次握手。 */
#include "YmodemEngine.h"
#include <algorithm>
#include <utility>
namespace NovaTerm::FileTransfer {
bool YmodemEngine::onStart(TimePoint now)
{
    _codec.reset(); _lastFrame.clear(); _lastHeader.clear(); _file = {};
    _phase = Phase::Header; _fileIndex = 0; _metadataBytes = 0;
    _offset = 0; _pendingCount = 0; _block = 1;
    _canSeen = false; _haveBlock = false; _finishedFile = false;
    if (_request.direction == Direction::Receive) sendControl({'C'});
    else setDeadline(now, _request.config.handshakeRetryMs);
    return true;
}
void YmodemEngine::sendControl(const Bytes& bytes)
{
    clearDeadline(); emitBytes(bytes);
}
void YmodemEngine::sendHeader()
{
    Bytes payload(128, 0);
    if (_fileIndex < _request.files.size()) {
        _file = _request.files[_fileIndex];
        const auto size = std::to_string(*_file.size);
        const auto required = _file.name.size() + 1 + size.size() + 1;
        if (required > 128) payload.resize(1024, 0);
        std::copy(_file.name.begin(), _file.name.end(), payload.begin());
        std::copy(size.begin(), size.end(),
                  payload.begin() + static_cast<std::ptrdiff_t>(_file.name.size() + 1));
        _phase = Phase::HeaderAck;
        _offset = 0; _block = 1; _progress.fileIndex = _fileIndex;
        _progress.lengthKnown = true;
    } else {
        _phase = Phase::EndAck; _progress.state = State::Finishing;
    }
    _lastFrame = XyPacketCodec::encode(0, payload, true);
    clearDeadline(); emitBytes(_lastFrame);
}
void YmodemEngine::sendNext(TimePoint now)
{
    if (_offset == *_file.size) {
        _phase = Phase::Eot; _progress.state = State::Finishing;
        _lastFrame = {4}; clearDeadline(); emitBytes(_lastFrame); return;
    }
    _phase = Phase::Data; _progress.state = State::Transferring;
    // 小尾包使用 SOH，其余使用 STX，避免为少量尾字节传输 1 KiB。
    _blockSize = *_file.size - _offset <= 128 ? 128 : 1024;
    _pendingCount = static_cast<std::size_t>(std::min<std::uint64_t>(_blockSize, *_file.size - _offset));
    TransferAction action; action.kind = ActionKind::ReadAt; action.file = _file;
    action.fileIndex = _fileIndex; action.offset = _offset; action.count = _pendingCount;
    clearDeadline(); requestAction(std::move(action), now);
}
void YmodemEngine::resend()
{
    if (retry()) { clearDeadline(); emitBytes(_lastFrame); }
}
bool YmodemEngine::parseHeader(const Bytes& bytes, FileInfo& file) const
{
    const auto nameEnd = std::find(bytes.begin(), bytes.end(), 0);
    if (nameEnd == bytes.end()) return false;
    const auto nameSize = static_cast<std::size_t>(nameEnd - bytes.begin());
    if (nameSize > 255) return false;
    file.name.assign(bytes.begin(), nameEnd);
    if (file.name.empty()) return true;
    if (!validFileNameUtf8(file.name)) return false;
    const auto metadataStart = nameEnd + 1;
    const auto metadataEnd = std::find(metadataStart, bytes.end(), 0);
    if (metadataEnd == bytes.end()) return false;
    if (metadataStart == metadataEnd) { file.size.reset(); return true; }
    // 元信息字段为 ASCII 数字，以空格分隔；首字段是十进制真实长度。
    std::uint64_t size = 0;
    auto cursor = metadataStart;
    if (*cursor < '0' || *cursor > '9') return false;
    while (cursor != metadataEnd && *cursor >= '0' && *cursor <= '9') {
        const auto digit = static_cast<unsigned>(*cursor - '0');
        if (size > (MaxFileSize - digit) / 10) return false;
        size = size * 10 + digit; ++cursor;
    }
    for (; cursor != metadataEnd; ++cursor)
        if (*cursor != ' ' && (*cursor < '0' || *cursor > '9')) return false;
    file.size = size;
    return true;
}
void YmodemEngine::receiveHeader(const XyPacketCodec::Packet& packet, TimePoint now)
{
    if (packet.number != 0) { if (retry()) sendControl({0x15}); return; }
    FileInfo file;
    if (!parseHeader(packet.bytes, file)) { fail(Error::Protocol); return; }
    if (file.name.empty()) {
        _lastHeader = packet.bytes;
        _phase = Phase::Closing; _progress.state = State::Closing;
        sendControl({6}); return;
    }
    if (_fileIndex >= MaxFiles || packet.bytes.size() > 256 * 1024 - _metadataBytes) {
        fail(Error::ResourceLimit); return;
    }
    _metadataBytes += packet.bytes.size();
    _file = std::move(file); _lastHeader = packet.bytes;
    _offset = 0; _pendingCount = 0; _block = 1; _haveBlock = false; _finishedFile = false;
    _progress.fileIndex = _fileIndex; _progress.lengthKnown = _file.size.has_value();
    TransferAction action; action.kind = ActionKind::OfferFile;
    action.fileIndex = _fileIndex; action.file = _file;
    clearDeadline(); requestAction(std::move(action), now);
}
void YmodemEngine::receivePacket(TimePoint now)
{
    const auto& packet = _codec.packet();
    if (_phase == Phase::Closing) {
        if (packet.number == 0 && packet.bytes == _lastHeader) emitByte(6);
        return;
    }
    if (_phase == Phase::Header) { receiveHeader(packet, now); return; }
    if (_phase != Phase::Data && _phase != Phase::Eot) return;
    if (!_haveBlock && packet.number == 0 && packet.bytes == _lastHeader) {
        sendControl({6, 'C'}); return;
    }
    if (_haveBlock && packet.number == static_cast<std::uint8_t>(_block - 1U)) {
        sendControl({6}); return;
    }
    if (packet.number != _block || _phase == Phase::Eot) {
        if (retry()) sendControl({0x15});
        return;
    }
    if (_file.size && _offset >= *_file.size) { fail(Error::SizeMismatch); return; }
    auto count = packet.bytes.size();
    if (_file.size)
        count = static_cast<std::size_t>(std::min<std::uint64_t>(count, *_file.size - _offset));
    if (count > MaxFileSize - _offset) { fail(Error::ResourceLimit); return; }
    _pendingCount = count;
    TransferAction action; action.kind = ActionKind::WriteAt; action.file = _file;
    action.fileIndex = _fileIndex; action.offset = _offset;
    action.bytes.assign(packet.bytes.begin(), packet.bytes.begin() + static_cast<std::ptrdiff_t>(count));
    _progress.state = State::Transferring;
    clearDeadline(); requestAction(std::move(action), now);
}
void YmodemEngine::receiveEot(TimePoint now)
{
    if (_phase == Phase::Header && _finishedFile) { sendControl({6, 'C'}); return; }
    if (_phase == Phase::Data) {
        if (_file.size && _offset != *_file.size) { fail(Error::SizeMismatch); return; }
        _phase = Phase::Eot; _progress.state = State::Finishing;
        sendControl({0x15});
    } else if (_phase == Phase::Eot) {
        TransferAction action; action.kind = ActionKind::FinishFile;
        action.file = _file; action.fileIndex = _fileIndex; action.offset = _offset;
        clearDeadline(); requestAction(std::move(action), now);
    }
}
void YmodemEngine::onByte(std::uint8_t byte, TimePoint now)
{
    if (_request.direction == Direction::Receive
        && (_codec.receiving() || byte == 1 || byte == 2)) {
        _canSeen = false;
        const auto result = _codec.push(byte, true);
        if (result == XyPacketCodec::Result::Valid) receivePacket(now);
        else if (result == XyPacketCodec::Result::Invalid && _phase != Phase::Closing) {
            if (retry()) sendControl({0x15});
        } else if (result == XyPacketCodec::Result::Partial
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
        if (byte == 4) receiveEot(now);
        return;
    }
    // 已排队但尚未交给通道的包不能被旧反馈确认；部分写同样不能确认。
    if (pendingOutput().size != 0) return;
    if ((_phase == Phase::Header || _phase == Phase::NextHeader) && byte == 'C') {
        _progress.state = State::Transferring; madeProgress(); sendHeader();
    } else if (_phase == Phase::HeaderAck && byte == 6) {
        _phase = Phase::WaitData; madeProgress();
        setDeadline(now, _request.config.responseTimeoutMs);
    } else if (_phase == Phase::WaitData && byte == 'C') {
        madeProgress(); sendNext(now);
    } else if (_phase == Phase::Data && byte == 6) {
        _offset += _pendingCount; _progress.transferredBytes += _pendingCount;
        _block = static_cast<std::uint8_t>(_block + 1U); madeProgress(); sendNext(now);
    } else if (_phase == Phase::Eot && byte == 6) {
        TransferAction action; action.kind = ActionKind::FinishFile;
        action.file = _file; action.fileIndex = _fileIndex; action.offset = _offset;
        clearDeadline(); requestAction(std::move(action), now);
    } else if (_phase == Phase::EndAck && byte == 6) {
        clearDeadline(); _progress.state = State::Completed;
    } else if (byte == 0x15 && (_phase == Phase::HeaderAck || _phase == Phase::Data
                               || _phase == Phase::Eot || _phase == Phase::EndAck)) {
        resend();
    } else if (_phase == Phase::HeaderAck && byte == 'C') {
        resend();
    }
}
void YmodemEngine::onOperation(const TransferAction& action, OperationResult result,
                               TimePoint now)
{
    if (action.kind == ActionKind::OfferFile) {
        if (!result.accepted) { cancel(now); return; }
        _phase = Phase::Data; _progress.state = State::Transferring;
        madeProgress(); sendControl({6, 'C'});
    } else if (action.kind == ActionKind::ReadAt) {
        if (result.bytes.size() != _pendingCount) { fail(Error::SizeMismatch); return; }
        result.bytes.resize(_blockSize, 0x1a);
        _lastFrame = XyPacketCodec::encode(_block, result.bytes, true);
        clearDeadline(); emitBytes(_lastFrame);
    } else if (action.kind == ActionKind::WriteAt) {
        _offset += _pendingCount; _progress.transferredBytes += _pendingCount;
        _block = static_cast<std::uint8_t>(_block + 1U); _haveBlock = true;
        madeProgress(); sendControl({6});
    } else if (action.kind == ActionKind::FinishFile) {
        ++_fileIndex; ++_progress.completedFiles;
        _phase = _request.direction == Direction::Receive ? Phase::Header : Phase::NextHeader;
        _finishedFile = true; _progress.state = State::Transferring; madeProgress();
        if (_request.direction == Direction::Receive) sendControl({6, 'C'});
        else setDeadline(now, _request.config.responseTimeoutMs);
    }
}
void YmodemEngine::onOutputDrained(TimePoint now)
{
    if (_phase == Phase::Closing) {
        if (!_deadline) setDeadline(now, _request.config.closingTimeoutMs);
    } else {
        setDeadline(now, _phase == Phase::Header
            ? _request.config.handshakeRetryMs : _request.config.responseTimeoutMs);
    }
}
void YmodemEngine::onTimeout(TimePoint now)
{
    if (_phase == Phase::Closing) { _progress.state = State::Completed; return; }
    _codec.reset();
    if (_request.direction == Direction::Receive) {
        if (retry()) sendControl({_phase == Phase::Header ? static_cast<std::uint8_t>('C')
                                      : static_cast<std::uint8_t>(0x15)});
    } else if (_phase == Phase::Header || _phase == Phase::NextHeader) {
        if (retry()) setDeadline(now, _request.config.handshakeRetryMs);
    } else if (_phase == Phase::WaitData) {
        _phase = Phase::HeaderAck; resend();
    } else resend();
}
} // namespace NovaTerm::FileTransfer
