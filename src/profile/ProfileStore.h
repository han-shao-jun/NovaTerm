#pragma once

#include "session/SessionTypes.h"

#include <QHash>
#include <QList>
#include <QString>
#include <optional>

struct ConnectionProfile
{
    QString id;
    QString name;
    TransportKind transportKind{TransportKind::LocalShell};
    QVariantMap settings;
    QString credentialRef;
};

// Storage-format-independent profile boundary. Secrets are rejected from the
// settings map; callers must put them in CredentialStore and retain a ref.
class ProfileStore
{
public:
    virtual ~ProfileStore() = default;
    virtual bool save(ConnectionProfile profile, QString* error = nullptr) = 0;
    [[nodiscard]] virtual std::optional<ConnectionProfile>
    find(const QString& id) const = 0;
    [[nodiscard]] virtual QList<ConnectionProfile> list() const = 0;
    virtual bool remove(const QString& id) = 0;

    [[nodiscard]] static bool containsSensitiveValues(const QVariantMap& values);
};

class MemoryProfileStore final : public ProfileStore
{
public:
    bool save(ConnectionProfile profile, QString* error = nullptr) override;
    [[nodiscard]] std::optional<ConnectionProfile>
    find(const QString& id) const override;
    [[nodiscard]] QList<ConnectionProfile> list() const override;
    bool remove(const QString& id) override;

private:
    QHash<QString, ConnectionProfile> _profiles;
};
