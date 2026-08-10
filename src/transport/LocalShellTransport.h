/**
 * @file   LocalShellTransport.h
 * @brief  本地 shell 传输：ITransport 实现，封装本地 PTY 进程。
 *
 * 统一本地与远程的字节流通路：本地 shell 不再依赖 QTermWidget 内置 KPty，
 * 而是通过本类提供与 SSH/Serial 完全一致的 ITransport 接口。
 *   • Unix   : posix_openpt() + fork() + QSocketNotifier
 *   • Windows: CreatePseudoConsole (ConPTY) + 读取线程
 */
#pragma once
#include "ITransport.h"
#include "session/LocalShellProfile.h"
#include <QStringList>
#include <QPointer>
#include <atomic>
#include <memory>
#include <mutex>

class QThread;
namespace NovaTerm::Windows { class ConPtySession; }
namespace NovaTerm::Linux { class PtySession; }

// LocalShellTransport — ITransport 实现，封装本地 PTY shell 进程。
//
// 统一本地和远程的字节流通路：本地 shell 不再走 QTermWidget 内置 KPty，
// 而是通过本类提供与 SSH/Serial/Telnet 完全一致的 ITransport 接口。
//
//   • Unix   : posix_openpt() + fork() + QSocketNotifier
//   • Windows: CreatePseudoConsole (ConPTY) + 读取线程
//
// 用法:
//   auto* t = new LocalShellTransport(parent);
//   t->setShellProgram("/bin/bash");
//   t->setShellArgs({"-l"});
//   t->connectToHost();          // 打开 PTY，启动 shell
//   ...
//   t->write(data);              // 写入 stdin
//   // readyRead() 信号携带 shell 输出
//   t->disconnect();             // 关闭 PTY，终止子进程
class LocalShellTransport : public ITransport
{
    Q_OBJECT
public:
    enum class LifecycleState { Idle, Starting, Running, Closing, Closed };
    Q_ENUM(LifecycleState)

    explicit LocalShellTransport(QObject* parent = nullptr);
    ~LocalShellTransport() override;

    // ── 配置（必须在 connectToHost() 之前调用）──
    void setShellProgram(const QString& program);
    void setShellArgs(const QStringList& args);
    void setWorkingDirectory(const QString& dir);
    void setEnvironment(const QStringList& env);
    void setShellProfile(const LocalShellProfile& profile);
    void setSessionConfig(const LocalShellConfig& config);
    LifecycleState lifecycleState() const { return _state; }

    // ── ITransport 接口 ──
    bool connectToHost() override;
    void disconnect() override;
    void write(const QByteArray& data) override;
    void resizeTerminal(int cols, int rows) override;
    bool isConnected() const override;
    bool hasPendingDisconnect() const override
    {
        return _state == LifecycleState::Closing;
    }
    QString errorString() const override;
    bool setReadPaused(bool paused) override;
    [[nodiscard]] TransportCapabilities capabilities() const override
    {
        return TransportCapability::PauseReads
            | TransportCapability::ResizeTerminal
            | TransportCapability::Reconnect;
    }

signals:
    void lifecycleStateChanged(LocalShellTransport::LifecycleState state);

private:
    QString _shellProgram;
    QStringList _shellArgs;
    QString _workingDir;
    QStringList _environment;
    QString _errorString;
    bool _connected{false};
    std::atomic<bool> _readPaused{false};
    LocalShellConfig _config;
    LifecycleState _state{LifecycleState::Idle};

    void setLifecycleState(LifecycleState state);

    // ── Linux 路径 ────────────────────────────────
#ifndef _WIN32
    QPointer<NovaTerm::Linux::PtySession> _linuxSession;
    quint64 _linuxGeneration{0};
    int _cols{80};
    int _rows{24};

    // ── Windows 路径 (ConPTY) ─────────────────────
#else
    struct ResizeDispatchState;
    QPointer<NovaTerm::Windows::ConPtySession> _windowsSession;
    QThread* _windowsThread{nullptr};
    std::shared_ptr<ResizeDispatchState> _resizeDispatch;
    quint64 _windowsGeneration{0};
    int _cols{80};
    int _rows{24};
#endif
};
