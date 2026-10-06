/**
 * @file SshMonitorProtocol.cpp
 * @brief SSH 常驻资源采集通道的数据帧解析实现。
 */
#include "SshMonitorProtocol.h"

#include <utility>

namespace {

const QByteArray BeginMarker =
    QByteArrayLiteral("__NOVATERM_METRICS_BEGIN__\t");
const QByteArray EndMarker =
    QByteArrayLiteral("__NOVATERM_METRICS_END__\t");

bool parseMarkerId(const QByteArray& line, const QByteArray& marker,
                   quint64& requestId)
{
    if (!line.startsWith(marker))
        return false;
    bool ok = false;
    const quint64 value = line.mid(marker.size()).toULongLong(&ok);
    if (!ok || value == 0)
        return false;
    requestId = value;
    return true;
}

} // namespace

SshMonitorFrameParser::Result
SshMonitorFrameParser::append(const QByteArray& data)
{
    Result result;
    if (data.isEmpty())
        return result;
    if (_buffer.size() + _payload.size() + data.size() > MaxBufferedBytes)
        return fail(Error::BufferLimit);
    _buffer.append(data);

    for (;;) {
        const qsizetype newline = _buffer.indexOf('\n');
        if (newline < 0) {
            if (_buffer.size() > MaxLineBytes)
                return fail(Error::LineLimit);
            break;
        }
        // 行长上限对"本次 append 就带换行"的分支同样生效：否则远端一次
        // 吐出整行超长内容就能绕过上面的缓冲检查，要等到单帧 128 KiB
        // 上限才拦住，与 SshTransport 的 16 KiB 提示差 8 倍。
        if (newline > MaxLineBytes)
            return fail(Error::LineLimit);

        QByteArray line = _buffer.left(newline);
        _buffer.remove(0, newline + 1);
        if (line.endsWith('\r'))
            line.chop(1);

        quint64 markerId = 0;
        if (_requestId == 0) {
            if (line.startsWith(BeginMarker)) {
                if (!parseMarkerId(line, BeginMarker, markerId)) {
                    return fail(Error::InvalidBegin);
                }
                _requestId = markerId;
                _payload.clear();
                _entryCount = 0;
            } else if (line.startsWith(EndMarker)) {
                return fail(Error::MissingBegin);
            }
            // 通道建立时远端 Shell 可能输出一行提示；帧外噪声有界丢弃。
            continue;
        }

        if (line.startsWith(BeginMarker))
            return fail(Error::NestedBegin);
        if (line.startsWith(EndMarker)) {
            if (!parseMarkerId(line, EndMarker, markerId)
                || markerId != _requestId) {
                return fail(Error::MismatchedEnd);
            }
            result.frames.append(SshMonitorFrame{
                _requestId, std::move(_payload)});
            _payload.clear();
            _requestId = 0;
            _entryCount = 0;
            continue;
        }

        if (++_entryCount > MaxFrameEntries)
            return fail(Error::EntryLimit);
        if (_payload.size() + line.size() + 1 > MaxFrameBytes)
            return fail(Error::FrameLimit);
        _payload.append(line);
        _payload.append('\n');
    }
    return result;
}

void SshMonitorFrameParser::reset() noexcept
{
    _buffer.clear();
    _payload.clear();
    _requestId = 0;
    _entryCount = 0;
}

SshMonitorFrameParser::Result
SshMonitorFrameParser::fail(Error error)
{
    reset();
    return Result{{}, error};
}
