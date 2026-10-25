#include "HistoryRecordingBinding.h"
#include "recording/HistoryService.h"
#include "session/TerminalSession.h"

void bindHistoryRecording(TerminalSession* session, NovaTerm::Recording::HistoryService* recording) {
    if (!session || !recording) return;
    QObject::connect(session, &TerminalSession::receiveObserved, recording,
        [recording, session](quint64 generation, const QByteArray& bytes) { recording->submit(session->id(), generation, bytes); });
    QObject::connect(session, &TerminalSession::receiveOmitted, recording,
        [recording, session](quint64 generation, quint64 bytes) { recording->omit(session->id(), generation, bytes, "file_transfer"); });
    QObject::connect(session, &TerminalSession::receiveBoundary, recording,
        [recording, session](quint64 generation, const QString& reason) { recording->boundary(session->id(), generation, reason); });
    QObject::connect(session, &TerminalSession::receiveStreamClosed, recording,
        [recording, session](quint64 generation) { recording->stop({session->id(), generation}, "connection_closed"); });
}
