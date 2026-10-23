#pragma once
#include "HistoryTypes.h"
#include <QJsonObject>
#include <map>

namespace NovaTerm::Recording {
// Supplied by a trusted application entry. Never decoded from model arguments.
struct ResourceContext { QString principal; QByteArray credential; };
struct ResourceGrant {
    QString principal;
    QByteArray credential;
    HistoryCaptureId capture;
    quint64 first{1}, last{0};
    qint64 fromMs{0}, toMs{0};
    quint64 version{0};
    bool persistent{false};
};
class HistoryAuthorizationStore final {
public:
    bool share(ResourceGrant);
    void revoke(const QString& principal, HistoryCaptureId);
    std::optional<ResourceGrant> authorize(const ResourceContext&, HistoryCaptureId) const;
    QJsonObject persistentState() const;
    bool restore(const QJsonObject&);
    static constexpr size_t MaxGrants = 1024;
private:
    std::map<std::pair<QString, QString>, ResourceGrant> _grants;
    quint64 _revision{0};
};
}
