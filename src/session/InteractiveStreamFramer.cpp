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
    // 提示符正则来自配置，但生命周期与本 framer 相同，因此在 profile 落地
    // 时一次性编译成成员：consume() 每个输入分片都会用到它，放在那里编译
    // 等于每个分片付两次 PCRE2 编译。
    _promptMatcher = QRegularExpression(_profile.promptPattern);
    _pending.clear();
    _deviceLine.clear();
    _deviceControlTail.clear();
    _alternateScreen = false;
}

void InteractiveStreamFramer::reset()
{
    _transactionActive = false;
    _promptGeneration = 0;
    // _profile 不由 reset() 改动，但重连/重新接线都会走到这里，顺带让
    // 编译结果与 profile 保持同步，避免将来新增 profile 写入点时漏掉。
    _promptMatcher = QRegularExpression(_profile.promptPattern);
    _pending.clear();
    _deviceLine.clear();
    _deviceControlTail.clear();
    _alternateScreen = false;
}

void InteractiveStreamFramer::beginTransaction()
{
    _transactionActive = true;
}

void InteractiveStreamFramer::endTransaction()
{
    _transactionActive = false;
}

InteractiveFrameResult InteractiveStreamFramer::consume(
    const QByteArray& bytes)
{
    InteractiveFrameResult result;
    if (bytes.isEmpty())
        return result;
    const auto& filtered = bytes;

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
        // 密码提示符模式是常量，编译一次即可；用户配置的提示符模式见
        // _promptMatcher（由 configure()/reset() 编译）。
        static const QRegularExpression passwordPrompt(
            QStringLiteral("(?i)(password|passphrase):\\s*$"));
        if (!_alternateScreen && !passwordPrompt.match(line).hasMatch()) {
            const auto match = _promptMatcher.match(line);
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
        const bool completesCommand = _transactionActive
            && generation > _promptGeneration && exitCode.has_value();
        _promptGeneration = generation;
        if (completesCommand)
            _transactionActive = false;
        return InteractiveStreamEvent{
            completesCommand
                ? InteractiveStreamEventKind::CommandFinishedAtPrompt
                : InteractiveStreamEventKind::PromptReady,
            generation, exitCode};
    }
    if (fields.size() == 1 && fields.front() == QByteArrayLiteral("RESET")) {
        _promptGeneration = 0;
        return InteractiveStreamEvent{InteractiveStreamEventKind::ShellReset,
                                      0, std::nullopt};
    }
    return std::nullopt;
}
