/**
 * @file InteractiveStreamFramer.cpp
 * @brief 隐藏 OSC marker 的增量解析、校验与有界回退。
 */
#include "InteractiveStreamFramer.h"

#include <QRegularExpression>
#include <utility>

namespace {
constexpr char OscTerminator = '\x07';
}

void InteractiveStreamFramer::configure(InteractiveCommandProfile profile)
{
    if (!profile.isValid())
        profile = InteractiveCommandProfile{};
    _profile = std::move(profile);
    _pending.clear();
    _echoSuffix.clear();
    _echoCandidate.clear();
    _suppressEchoUntilLineFeed = false;
    _deviceLine.clear();
    _deviceControlTail.clear();
    _alternateScreen = false;
}

void InteractiveStreamFramer::reset(quint64 generation,
                                    QByteArray executionNonce)
{
    _sessionGeneration = generation;
    _executionNonce = std::move(executionNonce);
    _promptGeneration = 0;
    _pending.clear();
    _echoSuffix.clear();
    _echoCandidate.clear();
    _suppressEchoUntilLineFeed = false;
    _deviceLine.clear();
    _deviceControlTail.clear();
    _alternateScreen = false;
}

void InteractiveStreamFramer::beginTransaction(QByteArray executionNonce,
                                               QByteArray echoSuffix)
{
    _executionNonce = std::move(executionNonce);
    _echoSuffix = std::move(echoSuffix);
    _echoCandidate.clear();
    _suppressEchoUntilLineFeed = false;
}

QByteArray InteractiveStreamFramer::endTransaction()
{
    QByteArray remainder = std::move(_echoCandidate);
    _executionNonce.clear();
    _echoSuffix.clear();
    _echoCandidate.clear();
    _suppressEchoUntilLineFeed = false;
    return remainder;
}

QByteArray InteractiveStreamFramer::filterEcho(const QByteArray& bytes)
{
    if (_echoSuffix.isEmpty() && !_suppressEchoUntilLineFeed)
        return bytes;
    static constexpr qsizetype EchoDetectionPrefixBytes = 16;
    static constexpr qsizetype MaxEchoEditBytes = 16;
    const auto comparableCandidate = [this] {
        QByteArray comparable;
        comparable.reserve(_echoCandidate.size());
        // 窄 PTY 的行编辑器会用回车/退格重绘，比较终端语义而非原始字节。
        for (const char candidate : _echoCandidate) {
            if (candidate == '\r')
                continue;
            if (candidate == '\b') {
                if (!comparable.isEmpty())
                    comparable.chop(1);
                continue;
            }
            comparable.append(candidate);
        }
        return comparable;
    };
    QByteArray filtered;
    filtered.reserve(bytes.size());
    for (const char byte : bytes) {
        if (_suppressEchoUntilLineFeed) {
            if (byte == '\n') {
                // 远端输入行已结束；补足 CR，避免下一行沿用命令末尾列号。
                filtered.append(QByteArrayLiteral("\r\n"));
                _suppressEchoUntilLineFeed = false;
            }
            continue;
        }
        if (_echoSuffix.isEmpty()) {
            filtered.append(byte);
            continue;
        }
        _echoCandidate.append(byte);
        QByteArray comparable = comparableCandidate();
        if (_echoSuffix.size() >= EchoDetectionPrefixBytes
            && comparable.startsWith(_echoSuffix.left(EchoDetectionPrefixBytes))) {
            // 辅助命令前缀已经唯一匹配，屏蔽当前输入行余下的回显。
            // 窄 PTY 的整行重绘会破坏后缀剩余部分的字节匹配。
            _echoCandidate.clear();
            _echoSuffix.clear();
            _suppressEchoUntilLineFeed = true;
            continue;
        }
        while (!_echoCandidate.isEmpty()
            && (_echoCandidate.size() > _echoSuffix.size() + MaxEchoEditBytes
                || (!_echoSuffix.startsWith(comparable)
                    && !comparable.isEmpty()))) {
            filtered.append(_echoCandidate.at(0));
            _echoCandidate.remove(0, 1);
            comparable = comparableCandidate();
        }
        if (comparable == _echoSuffix) {
            _echoCandidate.clear();
            _echoSuffix.clear();
        }
    }
    return filtered;
}

InteractiveFrameResult InteractiveStreamFramer::consume(
    const QByteArray& bytes)
{
    InteractiveFrameResult result;
    if (bytes.isEmpty())
        return result;
    const QByteArray filtered = filterEcho(bytes);
    if (filtered.isEmpty())
        return result;

    if (!_profile.shellIntegration && _profile.promptPattern.isEmpty()) {
        result.visibleBytes = filtered;
        return result;
    }

    if (!_profile.shellIntegration && !_profile.promptPattern.isEmpty()) {
        result.visibleBytes = filtered;
        const QByteArray controls = _deviceControlTail + filtered;
        _deviceControlTail = controls.right(7);
        if (controls.contains(QByteArrayLiteral("\x1b[?1049h"))) {
            _alternateScreen = true;
            _promptGeneration = 0;
            result.events.append({InteractiveStreamEventKind::ShellReset,
                                  0, std::nullopt, filtered.size()});
        }
        if (controls.contains(QByteArrayLiteral("\x1b[?1049l"))) {
            _alternateScreen = false;
            _promptGeneration = 0;
            result.events.append({InteractiveStreamEventKind::ShellReset,
                                  0, std::nullopt, filtered.size()});
        }
        const qsizetype lineBreak = qMax(filtered.lastIndexOf('\r'),
                                         filtered.lastIndexOf('\n'));
        if (lineBreak >= 0)
            _deviceLine = filtered.mid(lineBreak + 1);
        else
            _deviceLine.append(filtered);
        if (_deviceLine.size() > 512)
            _deviceLine = _deviceLine.right(512);
        const QString line = QString::fromUtf8(_deviceLine);
        const QRegularExpression passwordPrompt(
            QStringLiteral("(?i)(password|passphrase):\\s*$"));
        if (!_alternateScreen && !passwordPrompt.match(line).hasMatch()) {
            const QRegularExpression prompt(_profile.promptPattern);
            const auto match = prompt.match(line);
            if (match.hasMatch() && match.capturedEnd() == line.size()) {
                ++_promptGeneration;
                result.events.append({
                    InteractiveStreamEventKind::PromptCandidate,
                    _promptGeneration, std::nullopt, filtered.size()});
            }
        }
        return result;
    }

    _pending.append(filtered);
    const QByteArray& prefix = _profile.markerPrefix;
    while (!_pending.isEmpty()) {
        const qsizetype markerStart = _pending.indexOf(prefix);
        if (markerStart < 0) {
            const qsizetype held = partialPrefixLength();
            const qsizetype visibleSize = _pending.size() - held;
            if (visibleSize > 0) {
                result.visibleBytes.append(_pending.constData(), visibleSize);
                _pending.remove(0, visibleSize);
            }
            break;
        }
        if (markerStart > 0) {
            result.visibleBytes.append(_pending.constData(), markerStart);
            _pending.remove(0, markerStart);
        }

        const qsizetype terminator = _pending.indexOf(OscTerminator,
                                                       prefix.size());
        if (terminator < 0) {
            if (_pending.size() > _profile.maxMarkerBytes) {
                result.visibleBytes.append(_pending);
                _pending.clear();
                result.events.append({InteractiveStreamEventKind::FramingError,
                                      _promptGeneration, std::nullopt,
                                      result.visibleBytes.size()});
            }
            break;
        }

        const qsizetype markerSize = terminator + 1;
        const QByteArray marker = _pending.left(markerSize);
        const QByteArray body = marker.mid(prefix.size(),
                                           terminator - prefix.size());
        const auto event = parseMarker(body);
        if (event) {
            auto positioned = *event;
            positioned.visibleOffset = result.visibleBytes.size();
            result.events.append(positioned);
        } else {
            result.visibleBytes.append(marker);
        }
        _pending.remove(0, markerSize);
    }
    return result;
}

qsizetype InteractiveStreamFramer::partialPrefixLength() const
{
    const QByteArray& prefix = _profile.markerPrefix;
    const qsizetype maximum = qMin(_pending.size(), prefix.size() - 1);
    for (qsizetype size = maximum; size > 0; --size) {
        if (_pending.endsWith(prefix.left(size)))
            return size;
    }
    return 0;
}

std::optional<InteractiveStreamEvent> InteractiveStreamFramer::parseMarker(
    const QByteArray& body)
{
    const QList<QByteArray> fields = body.split(';');
    if ((fields.size() == 2 || fields.size() == 3)
        && fields.front() == QByteArrayLiteral("PROMPT")) {
        bool ok = false;
        const quint64 generation = fields.at(1).toULongLong(&ok);
        if (!ok || generation == 0)
            return std::nullopt;
        std::optional<int> exitCode;
        if (fields.size() == 3) {
            const int parsed = fields.at(2).toInt(&ok);
            if (!ok)
                return std::nullopt;
            exitCode = parsed;
        }
        const bool completesCommand = !_executionNonce.isEmpty()
            && generation > _promptGeneration && exitCode.has_value();
        _promptGeneration = generation;
        if (completesCommand)
            _executionNonce.clear();
        return InteractiveStreamEvent{
            completesCommand
                ? InteractiveStreamEventKind::CommandFinishedAtPrompt
                : InteractiveStreamEventKind::PromptReady,
            generation, exitCode};
    }
    if (fields.size() == 2 && fields.front() == QByteArrayLiteral("START")
        && !_executionNonce.isEmpty() && fields.at(1) == _executionNonce) {
        return InteractiveStreamEvent{InteractiveStreamEventKind::CommandStarted,
                                      _promptGeneration, std::nullopt};
    }
    if (fields.size() == 3 && fields.front() == QByteArrayLiteral("END")
        && !_executionNonce.isEmpty() && fields.at(1) == _executionNonce) {
        bool ok = false;
        const int exitCode = fields.at(2).toInt(&ok);
        if (!ok)
            return std::nullopt;
        return InteractiveStreamEvent{InteractiveStreamEventKind::CommandFinished,
                                      _promptGeneration, exitCode};
    }
    if (fields.size() == 1 && fields.front() == QByteArrayLiteral("RESET")) {
        _promptGeneration = 0;
        return InteractiveStreamEvent{InteractiveStreamEventKind::ShellReset,
                                      0, std::nullopt};
    }
    return std::nullopt;
}
