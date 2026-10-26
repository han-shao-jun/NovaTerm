#pragma once
#include "tasks/ClearOperations.h"
#include <QObject>
#include <QHash>
#include <QJsonObject>
class SessionDirectory;
class TerminalSession;
namespace NovaTerm::Mcp {
class AccessStore;
class TerminalClearAdapter final : public QObject {
public:
    using Completion = std::function<void(QJsonObject)>;
    TerminalClearAdapter(AccessStore&, SessionDirectory&, QString instance, QByteArray key, QObject* parent);
    ~TerminalClearAdapter() override;
    void call(const QString& client, const QString& requestKey, const ClearRequest&, Completion);
    void cancel(const QString& requestKey) { _operations.cancel(requestKey); }
    void revoke(const QString& client, const QString& session) { _operations.cancelSession(client, session); }
private:
    quint64 watch(TerminalSession*);
    void finished(quint64 operation, quint64 generation, int status, quint64 nextLine);
    AccessStore& _access;
    SessionDirectory& _sessions;
    QString _instance;
    QByteArray _key;
    ClearOperations _operations;
    QHash<TerminalSession*, quint64> _watched;
    quint64 _nextSource{0};
};
}
