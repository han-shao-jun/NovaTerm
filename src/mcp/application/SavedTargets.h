#pragma once
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

namespace NovaTerm::Mcp {
// Local composition input only. configurationKey never leaves this application boundary.
struct SavedTargetSource {
    QString id, title, transport, endpoint;
    QByteArray configurationKey;
};
struct SavedTarget {
    QString id, title, transport, endpoint, configVersion;
};
struct SavedTargetPage {
    QList<SavedTarget> targets;
    QString nextAfter, revision, error;
};
// Application-thread metadata directory; does not own connections or inspect credentials.
class SavedTargets final : public QObject {
    Q_OBJECT
public:
    explicit SavedTargets(QObject* parent = nullptr);
    bool replace(const QList<SavedTargetSource>&);
    QList<SavedTarget> localTargets() const;
    bool setShared(const QString& client, quint64 identityVersion, const QString& target,
        const QString& expectedConfigVersion, bool selected);
    bool isShared(const QString& client, quint64 identityVersion, const QString& target) const;
    void revoke(const QString& client);
    SavedTargetPage page(const QString& client, quint64 identityVersion, int limit,
        const QString& after = {}, const QString& expectedRevision = {}) const;
signals:
    void changed();
private:
    struct Entry { SavedTarget value; QByteArray configurationKey; };
    struct Grant { quint64 identityVersion; QString configVersion; };
    void invalidate();
    QMap<QString, Entry> _targets;
    QMap<QString, QMap<QString, Grant>> _grants;
    QString _revision;
};
}
