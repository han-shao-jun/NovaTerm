#pragma once
#include <QObject>
#include <QByteArray>

enum class TransportExitReason
{
    NormalExit,
    FailedExit,
    Crash,
    UserClosed,
    IoError,
    StartFailed,
};
Q_DECLARE_METATYPE(TransportExitReason)

// ITransport — 抽象传输接口，统一本地和远程数据通路。
//   • LocalShellTransport : 本地 PTY（Unix: posix_openpt + fork, Windows: ConPTY）
//   • SSH / Serial / Telnet : 远程协议实现
// 所有实现均通过 ITransport 向 TerminalView 提供字节流，
// 不再区分"本地走 KPty / 远程走 transport"两条路径。
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
    virtual bool hasPendingDisconnect() const { return false; }
    virtual bool setReadPaused(bool paused)
    {
        Q_UNUSED(paused);
        return false;
    }
    virtual QString errorString() const = 0;     // 最近一次错误信息，供 UI 显示

signals:
    void connected();
    void disconnected();
    void readyRead(const QByteArray& data);      // 对端数据到达 → 送入终端显示
    void errorOccurred(const QString& error);
    void exited(quint32 exitCode, TransportExitReason reason);
};
