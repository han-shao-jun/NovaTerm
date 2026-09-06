/**
 * @file SshMonitorProtocol.h
 * @brief SSH 常驻资源采集通道的数据帧解析器。
 */
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

/** 常驻采集脚本返回的一帧完整数据。 */
struct SshMonitorFrame
{
    quint64 requestId{0};
    QByteArray payload;
};

/**
 * @brief 增量解析带请求序号和起止标记的资源采集帧。
 *
 * 解析器支持任意分片及一次输入多帧，并对行长、单帧、累计缓冲和条目数
 * 设置硬上限。协议错误后会丢弃当前状态，调用方应重建远端通道。
 */
class SshMonitorFrameParser final
{
public:
    enum class Error {
        None,
        BufferLimit,
        LineLimit,
        InvalidBegin,
        MissingBegin,
        NestedBegin,
        MismatchedEnd,
        EntryLimit,
        FrameLimit
    };

    struct Result
    {
        QVector<SshMonitorFrame> frames;
        Error error{Error::None};
    };

    [[nodiscard]] Result append(const QByteArray& data);
    void reset() noexcept;

    static constexpr qsizetype MaxFrameBytes = 128 * 1024;
    static constexpr qsizetype MaxBufferedBytes = 256 * 1024;
    static constexpr qsizetype MaxLineBytes = 16 * 1024;
    static constexpr int MaxFrameEntries = 256;

private:
    [[nodiscard]] Result fail(Error error);

    QByteArray _buffer;
    QByteArray _payload;
    quint64 _requestId{0};
    int _entryCount{0};
};
