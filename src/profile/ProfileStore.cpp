#include "ProfileStore.h"

#include <QSet>

namespace {
bool containsSecret(const QVariantMap& values)
{
    static const QSet<QString> forbidden{
        QStringLiteral("password"), QStringLiteral("token"),
        QStringLiteral("passphrase"), QStringLiteral("keypassphrase"),
        QStringLiteral("privatekey")};
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (forbidden.contains(it.key().toLower()))
            return true;
        if (it.value().metaType().id() == QMetaType::QVariantMap
            && containsSecret(it.value().toMap())) {
            return true;
        }
    }
    return false;
}
}

bool ProfileStore::containsSensitiveValues(const QVariantMap& values)
{
    return containsSecret(values);
}

bool MemoryProfileStore::save(ConnectionProfile profile, QString* error)
{
    if (profile.id.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("profile id is empty");
        return false;
    }
    if (containsSensitiveValues(profile.settings)) {
        if (error)
            *error = QStringLiteral("profile contains sensitive credential data");
        return false;
    }
    _profiles.insert(profile.id, std::move(profile));
    return true;
}

std::optional<ConnectionProfile> MemoryProfileStore::find(const QString& id) const
{
    const auto it = _profiles.constFind(id);
    return it == _profiles.cend() ? std::nullopt
                                  : std::optional<ConnectionProfile>(*it);
}

QList<ConnectionProfile> MemoryProfileStore::list() const
{
    return _profiles.values();
}

bool MemoryProfileStore::remove(const QString& id)
{
    return _profiles.remove(id);
}
