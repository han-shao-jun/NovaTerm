#pragma once
#include "HistoryTypes.h"
#include "HistoryRetentionPolicy.h"
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>
#include <map>
#include <thread>

namespace NovaTerm::Recording {
class HistoryRecorder;
// Application-owned local recording use cases. Only the composition root feeds
// receive bytes. No MCP adapter and no automatic history-read grant is installed.
class HistoryService final : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(RecordingResult)>;
    using ReadDone = std::function<void(ReadResult)>;
    explicit HistoryService(QString privateRoot, QObject* parent = nullptr, IoProbe = {});
    ~HistoryService() override;
    bool start(RecordingIntent, Done = {});
    void stop(RecordingIntent, QString reason = QStringLiteral("user_stop"));
    SubmitResult submit(QUuid session, quint64 generation, const QByteArray&);
    void omit(QUuid session, quint64 generation, quint64 bytes, const QString& reason);
    void boundary(QUuid session, quint64 generation, const QString& reason);
    bool remove(HistoryCaptureId, Done = {});
    bool retain(HistoryCaptureId, bool keep, Done = {});
    bool configure(RetentionSettings, Done = {});
    bool readCommitted(HistoryCaptureId, HistoryRange, ReadDone);
    std::vector<CaptureDescriptor> captures() const { return _captures; }
    RetentionSettings settings() const { return _settings; }
    QString error() const { return _error; }
    bool ready() const { return _ready; }
signals:
    void changed();
private:
    struct Worker;
    struct Producer { quint64 generation; std::shared_ptr<HistoryRecorder> recorder; };
    std::unique_ptr<Worker> _worker;
    std::thread _thread;
    std::map<QString, Producer> _producers;
    std::map<quint64, Done> _callbacks;
    std::map<quint64, ReadDone> _readCallbacks;
    struct PendingStart { RecordingIntent intent; bool cancelled{false}; };
    std::map<quint64, PendingStart> _starts;
    std::vector<CaptureDescriptor> _captures;
    RetentionSettings _settings;
    QTimer _poll;
    quint64 _request{0};
    QString _error;
    bool _ready{false};
    void poll();
};
}
