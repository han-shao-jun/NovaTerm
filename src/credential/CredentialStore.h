#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <memory>
#include <optional>

class CredentialStore
{
public:
    virtual ~CredentialStore() = default;
    virtual bool put(const QString& reference, const QByteArray& secret) = 0;
    [[nodiscard]] virtual std::optional<QByteArray>
    get(const QString& reference) const = 0;
    virtual bool remove(const QString& reference) = 0;
};

// Volatile fallback used until a platform keychain implementation is wired.
// It intentionally has no serialization API.
class MemoryCredentialStore final : public CredentialStore
{
public:
    ~MemoryCredentialStore() override;
    bool put(const QString& reference, const QByteArray& secret) override;
    [[nodiscard]] std::optional<QByteArray>
    get(const QString& reference) const override;
    bool remove(const QString& reference) override;

private:
    QHash<QString, QByteArray> _secrets;
};

// Uses the platform credential vault where available and falls back to the
// volatile store on unsupported platforms.
[[nodiscard]] std::unique_ptr<CredentialStore> createCredentialStore();
