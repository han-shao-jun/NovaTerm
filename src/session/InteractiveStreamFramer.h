/**
 * @file InteractiveStreamFramer.h
 * @brief 从终端入站流剥离可信交互命令标记并发布事务事件。
 */
#pragma once

#include "InteractiveCommandProfile.h"

#include <QByteArray>
#include <QMetaType>
#include <QVector>

#include <optional>

/** @brief 交互流中识别出的可信事件类型。 */
enum class InteractiveStreamEventKind
{
    PromptReady,
    CommandStarted,
    CommandFinished,
    CommandFinishedAtPrompt,
    ShellReset,
    FramingError,
    PromptCandidate,
};

/** @brief 一条已验证的交互流事件。 */
struct InteractiveStreamEvent
{
    InteractiveStreamEventKind kind{InteractiveStreamEventKind::FramingError};
    quint64 promptGeneration{0};
    std::optional<int> exitCode;
    qsizetype visibleOffset{0};
};

/** @brief 一次 consume 的可见字节与内部事件。 */
struct InteractiveFrameResult
{
    QByteArray visibleBytes;
    QVector<InteractiveStreamEvent> events;
};

Q_DECLARE_METATYPE(InteractiveStreamEvent)

/** @brief 有界、可增量解析的 OSC 交互命令 framing 过滤器。 */
class InteractiveStreamFramer final
{
public:
    void configure(InteractiveCommandProfile profile);
    void reset(quint64 generation, QByteArray executionNonce = {});
    /** @brief 为当前提示符开始一个新事务，不重置提示符状态。 */
    void beginTransaction(QByteArray executionNonce,
                          QByteArray echoSuffix = {});
    [[nodiscard]] QByteArray endTransaction();
    [[nodiscard]] InteractiveFrameResult consume(const QByteArray& bytes);

private:
    [[nodiscard]] qsizetype partialPrefixLength() const;
    [[nodiscard]] QByteArray filterEcho(const QByteArray& bytes);
    [[nodiscard]] std::optional<InteractiveStreamEvent> parseMarker(
        const QByteArray& body);

    InteractiveCommandProfile _profile;
    QByteArray _pending;
    QByteArray _executionNonce;
    QByteArray _echoSuffix;
    QByteArray _echoCandidate;
    bool _suppressEchoUntilLineFeed{false};
    quint64 _sessionGeneration{0};
    quint64 _promptGeneration{0};
    QByteArray _deviceLine;
    QByteArray _deviceControlTail;
    bool _alternateScreen{false};
};
