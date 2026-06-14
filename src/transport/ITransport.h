#pragma once
#include <QObject>
#include <QByteArray>

// ITransport 仅用于远程协议（SSH/串口/Telnet）。
// 本地终端由 QTermWidget 内置的 KPty 直接处理
// （Windows 上为 ConPTY，Unix 上为 pty）—— 不经过本接口。
class ITransport : public QObject
{
    Q_OBJECT
public:
    explicit ITransport(QObject* parent = nullptr) : QObject(parent) {}
    ~ITransport() override = default;

    virtual bool connectToHost() = 0;            // 开始连接；完成后发射 connected() / errorOccurred()
    virtual void disconnect() = 0;               // 断开链路；发射 disconnected()
    virtual void write(const QByteArray& data) = 0;   // 向对端发送原始字节（终端按键）
    virtual void resizeTerminal(int cols, int rows) = 0; // 转发窗口尺寸变更（如 SSH PTY 大小调整）
    virtual bool isConnected() const = 0;
    virtual QString errorString() const = 0;     // 最近一次错误信息，供 UI 显示

signals:
    void connected();
    void disconnected();
    void readyRead(const QByteArray& data);      // 对端数据到达 → 送入终端显示
    void errorOccurred(const QString& error);
};
