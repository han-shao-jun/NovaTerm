/** @file StdioChannel.h
 *  @brief 有界 stdio 通道；阻塞管道操作只在可停止的专用线程执行。
 */
#pragma once
#include <QByteArray>
#include <QObject>
#include <memory>

class StdioChannel final : public QObject
{
    Q_OBJECT
public:
    explicit StdioChannel(QObject* parent = nullptr);
    ~StdioChannel() override;
    void start();
    void stop();
    bool write(const QByteArray& message);
signals:
    void line(const QByteArray& message);
    void ended();
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
    void drain();
};
