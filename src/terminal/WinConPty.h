#pragma once

#ifdef _WIN32

#include <QObject>
#include <QString>
#include <QThread>
#include <atomic>
#include <windows.h>

// Windows ConPTY (CreatePseudoConsole) 封装。
// 用于替代 QTermWidget 内置的 KPty（在 Windows 上 KPty 是空桩实现），
// 提供真正的伪终端支持（Windows 10 1809+）。
//
// 用法：
//   WinConPty* pty = new WinConPty();
//   connect(pty, &WinConPty::receivedData, terminal, &QTermWidget::receiveData);
//   connect(terminal, &QTermWidget::sendData, pty, &WinConPty::writeData);
//   pty->start("powershell.exe");
//
class WinConPty : public QObject
{
    Q_OBJECT
public:
    explicit WinConPty(QObject* parent = nullptr);
    ~WinConPty() override;

    bool start(const QString& shell, const QStringList& args = {});
    void stop();
    bool isRunning() const;

    void writeData(const char* data, int len);
    void resize(int cols, int rows);

signals:
    void receivedData(const char* data, int len);
    void finished(int exitCode);

private:
    bool createPipes();
    bool createConPty(int cols, int rows);
    bool launchProcess(const QString& cmd);

    HPCON        _hPC{nullptr};        // ConPTY handle
    HANDLE       _hInputPipe{nullptr}; // data from application → ConPTY → _hOutputRead
    HANDLE       _hOutputRead{nullptr};// ConPTY output (we read from this)
    HANDLE       _hOutputWrite{nullptr};// our write → ConPTY → application input
    HANDLE       _hInputWrite{nullptr}; // application writes → ConPTY → we read

    HANDLE       _hProcess{nullptr};
    HANDLE       _hThread{nullptr};

    QThread*     _readerThread{nullptr};
    std::atomic<bool> _running{false};
};

#endif // _WIN32
