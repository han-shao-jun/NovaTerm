#pragma once

#include "session/LocalShellProfile.h"
#include "transport/ITransport.h"

#include <QByteArray>
#include <QObject>

#include <cstddef>
#include <deque>
#include <mutex>

class QSocketNotifier;
class QTimer;

namespace NovaTerm::Linux {

// Event-driven Linux pseudo-terminal session. Native descriptors and child
// lifecycle remain on the QObject thread; enqueueing input is thread-safe.
class PtySession final : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Starting, Running, Closing, Closed };
    Q_ENUM(State)

    explicit PtySession(LocalShellConfig config, int columns, int rows,
                        QObject* parent = nullptr);
    ~PtySession() override;

    [[nodiscard]] bool tryEnqueueInput(const QByteArray& data);

public slots:
    void start();
    void resize(int columns, int rows);
    void setReadPaused(bool paused);
    void requestClose();

signals:
    void started();
    void dataReady(const QByteArray& data);
    void errorOccurred(const QString& error);
    void exited(quint32 exitCode, TransportExitReason reason);
    void closed();
    void stateChanged(NovaTerm::Linux::PtySession::State state);

private:
    static constexpr std::size_t InputCapacity = 4U * 1024U * 1024U;
    static constexpr std::size_t ReadBufferSize = 64U * 1024U;

    void transition(State state);
    void drainOutput();
    void flushInput();
    void checkChildExit();
    void closeDescriptors();
    void failStart(const QString& error);
    void finish(int status);
    void finalizeClose();
    void reportIoError(const QString& operation, int error);

    LocalShellConfig _config;
    State _state{State::Idle};
    int _columns{80};
    int _rows{24};
    int _masterFd{-1};
    qint64 _childPid{-1};
    QSocketNotifier* _readNotifier{nullptr};
    QSocketNotifier* _writeNotifier{nullptr};
    QTimer* _exitTimer{nullptr};
    bool _readPaused{false};
    bool _userCloseRequested{false};
    bool _exitEmitted{false};
    int _closePolls{0};

    std::mutex _inputMutex;
    std::deque<QByteArray> _inputQueue;
    std::size_t _inputBytes{0};
    qsizetype _inputOffset{0};
    bool _acceptingInput{false};
};

} // namespace NovaTerm::Linux
