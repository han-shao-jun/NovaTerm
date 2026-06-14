// Windows stub implementations for the PTY classes (KPty, KPtyDevice,
// KPtyProcess, Pty). These provide empty/minimal implementations so the
// qtermwidget6 library can link on Windows, where PTY functionality is
// handled by PtyTransport (QProcess-based) in the WindTermQt application.

#ifdef _WIN32

#include "kpty.h"
#include "kptydevice.h"
#include "kptyprocess.h"
#include "Pty.h"
#include "kpty_p.h"

#include <QSize>
#include <QIODevice>

// ============================================================
// KPty stubs
// ============================================================

KPty::KPty() {}
KPty::~KPty() {}
int KPty::slaveFd() const { return -1; }

// ============================================================
// KPtyDevice stubs (inherits QIODevice and KPty)
// ============================================================

KPtyDevice::KPtyDevice(QObject* parent) : QIODevice(parent), KPty() {}
KPtyDevice::~KPtyDevice() = default;

// QIODevice virtual overrides
bool KPtyDevice::atEnd() const { return true; }
bool KPtyDevice::canReadLine() const { return false; }
void KPtyDevice::close() {}
bool KPtyDevice::isSequential() const { return true; }
bool KPtyDevice::open(QIODevice::OpenMode mode) { Q_UNUSED(mode); return false; }
bool KPtyDevice::waitForBytesWritten(int msecs) { Q_UNUSED(msecs); return false; }
bool KPtyDevice::waitForReadyRead(int msecs) { Q_UNUSED(msecs); return false; }
qint64 KPtyDevice::bytesAvailable() const { return 0; }
qint64 KPtyDevice::bytesToWrite() const { return 0; }
qint64 KPtyDevice::readData(char* data, qint64 maxlen) { Q_UNUSED(data); Q_UNUSED(maxlen); return -1; }
qint64 KPtyDevice::readLineData(char* data, qint64 maxlen) { Q_UNUSED(data); Q_UNUSED(maxlen); return -1; }
qint64 KPtyDevice::writeData(const char* data, qint64 len) { Q_UNUSED(data); Q_UNUSED(len); return -1; }

// KPtyDevicePrivate stubs
bool KPtyDevicePrivate::_k_canRead() { return false; }
bool KPtyDevicePrivate::_k_canWrite() { return false; }

// ============================================================
// KPtyProcess stubs (inherits from KProcess)
// ============================================================

KPtyProcess::KPtyProcess(QObject* parent) : KProcess(parent) {}
KPtyProcess::KPtyProcess(int ptyMasterFd, QObject* parent)
    : KProcess(parent) {
    Q_UNUSED(ptyMasterFd);
}
KPtyProcess::~KPtyProcess() = default;
KPtyDevice* KPtyProcess::pty() const { return nullptr; }
void KPtyProcess::setPtyChannels(PtyChannels channels) { Q_UNUSED(channels); }
KPtyProcess::PtyChannels KPtyProcess::ptyChannels() const { return NoChannels; }
void KPtyProcess::setUseUtmp(bool value) { Q_UNUSED(value); }
bool KPtyProcess::isUseUtmp() const { return false; }

// ============================================================
// Konsole::Pty stubs (inherits from KPtyProcess)
// ============================================================

Konsole::Pty::Pty(QObject* parent) : KPtyProcess(parent) {}
Konsole::Pty::~Pty() = default;

int Konsole::Pty::start(const QString& program,
                        const QList<QString>& arguments,
                        const QList<QString>& environment,
                        unsigned long winid,
                        bool addToUtmp) {
    Q_UNUSED(program); Q_UNUSED(arguments); Q_UNUSED(environment);
    Q_UNUSED(winid); Q_UNUSED(addToUtmp);
    return -1;
}

void Konsole::Pty::setEmptyPTYProperties() {}
void Konsole::Pty::setWriteable(bool writeable) { Q_UNUSED(writeable); }
void Konsole::Pty::setFlowControlEnabled(bool enable) { Q_UNUSED(enable); }
bool Konsole::Pty::flowControlEnabled() const { return true; }
void Konsole::Pty::setWindowSize(int columns, int lines) {
    Q_UNUSED(columns); Q_UNUSED(lines);
}
QSize Konsole::Pty::windowSize() const { return QSize(80, 24); }
void Konsole::Pty::setErase(char erase) { Q_UNUSED(erase); }
void Konsole::Pty::setInitialWorkingDirectory(const QString& dir) { Q_UNUSED(dir); }
int Konsole::Pty::foregroundProcessGroup() const { return 0; }
void Konsole::Pty::closePty() {}
void Konsole::Pty::setUtf8Mode(bool enable) { Q_UNUSED(enable); }
void Konsole::Pty::lockPty(bool lock) { Q_UNUSED(lock); }
void Konsole::Pty::sendData(const char* data, int length) {
    Q_UNUSED(data); Q_UNUSED(length);
}
void Konsole::Pty::dataReceived() {}

#endif // _WIN32
