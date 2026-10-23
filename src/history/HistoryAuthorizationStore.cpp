#include "HistoryAuthorizationStore.h"
#include <QJsonArray>
#include <limits>
namespace NovaTerm::Recording {
bool HistoryAuthorizationStore::share(ResourceGrant grant) {
    if (grant.principal.isEmpty() || grant.principal.size()>128 || grant.credential.isEmpty()
        || grant.credential.size()>128 || !grant.capture.valid() || !grant.first
        || (grant.last && grant.last<grant.first) || grant.fromMs<0 || grant.toMs<0
        || (grant.toMs && grant.toMs<grant.fromMs) || _revision==std::numeric_limits<quint64>::max()) return false;
    const auto key=std::make_pair(grant.principal,grant.capture.text());
    if (!_grants.count(key) && _grants.size()>=MaxGrants) return false;
    grant.version=++_revision; _grants[key]=std::move(grant); return true;
}
void HistoryAuthorizationStore::revoke(const QString& principal, HistoryCaptureId id) {
    _grants.erase({principal,id.text()});
}
std::optional<ResourceGrant> HistoryAuthorizationStore::authorize(const ResourceContext& context, HistoryCaptureId id) const {
    const auto it=_grants.find({context.principal,id.text()});
    if (it==_grants.end() || it->second.credential!=context.credential) return std::nullopt;
    return it->second;
}
QJsonObject HistoryAuthorizationStore::persistentState() const {
    QJsonArray rows;
    for (const auto& item:_grants) { const auto& g=item.second; if (!g.persistent) continue;
        rows.append(QJsonObject{{"principal",g.principal},{"credential",QString::fromLatin1(g.credential.toHex())},
            {"capture",g.capture.text()},{"first",QString::number(g.first)},{"last",QString::number(g.last)},
            {"fromMs",QString::number(g.fromMs)},{"toMs",QString::number(g.toMs)}});
    }
    return {{"version",1},{"grants",rows}};
}
bool HistoryAuthorizationStore::restore(const QJsonObject& object) {
    if (object.value("version")!=1 || !object.value("grants").isArray()
        || object.value("grants").toArray().size()>int(MaxGrants)) return false;
    HistoryAuthorizationStore restored; restored._revision=_revision;
    for (const auto& row:object.value("grants").toArray()) {
        const auto o=row.toObject(); bool a,b,c,d;
        ResourceGrant g; g.principal=o.value("principal").toString();
        g.credential=QByteArray::fromHex(o.value("credential").toString().toLatin1());
        g.capture={QUuid(o.value("capture").toString())};
        g.first=o.value("first").toString().toULongLong(&a); g.last=o.value("last").toString().toULongLong(&b);
        g.fromMs=o.value("fromMs").toString().toLongLong(&c); g.toMs=o.value("toMs").toString().toLongLong(&d);
        g.persistent=true; if (!a||!b||!c||!d||!restored.share(std::move(g))) return false;
    }
    *this=std::move(restored); return true;
}
}
