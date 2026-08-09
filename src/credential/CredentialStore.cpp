#include "CredentialStore.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {

#ifdef Q_OS_WIN
QString credentialTarget(const QString& reference)
{
    return QStringLiteral("NovaTerm/%1").arg(reference);
}

class WindowsCredentialStore final : public CredentialStore
{
public:
    bool put(const QString& reference, const QByteArray& secret) override
    {
        if (reference.trimmed().isEmpty() || secret.isEmpty())
            return false;

        const QString target = credentialTarget(reference);
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<wchar_t*>(
            reinterpret_cast<const wchar_t*>(target.utf16()));
        credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(
            const_cast<char*>(secret.constData()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        return CredWriteW(&credential, 0) != FALSE;
    }

    [[nodiscard]] std::optional<QByteArray>
    get(const QString& reference) const override
    {
        if (reference.trimmed().isEmpty())
            return std::nullopt;

        const QString target = credentialTarget(reference);
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(reinterpret_cast<const wchar_t*>(target.utf16()),
                       CRED_TYPE_GENERIC, 0, &credential)) {
            return std::nullopt;
        }

        const QByteArray secret(
            reinterpret_cast<const char*>(credential->CredentialBlob),
            static_cast<qsizetype>(credential->CredentialBlobSize));
        CredFree(credential);
        return secret;
    }

    bool remove(const QString& reference) override
    {
        if (reference.trimmed().isEmpty())
            return false;
        const QString target = credentialTarget(reference);
        if (CredDeleteW(reinterpret_cast<const wchar_t*>(target.utf16()),
                        CRED_TYPE_GENERIC, 0)) {
            return true;
        }
        return GetLastError() == ERROR_NOT_FOUND;
    }
};
#endif

} // namespace

MemoryCredentialStore::~MemoryCredentialStore()
{
    for (QByteArray& secret : _secrets)
        secret.fill('\0');
}

bool MemoryCredentialStore::put(const QString& reference,
                                const QByteArray& secret)
{
    if (reference.trimmed().isEmpty() || secret.isEmpty())
        return false;
    auto it = _secrets.find(reference);
    if (it != _secrets.end())
        it->fill('\0');
    _secrets.insert(reference, secret);
    return true;
}

std::optional<QByteArray>
MemoryCredentialStore::get(const QString& reference) const
{
    const auto it = _secrets.constFind(reference);
    return it == _secrets.cend() ? std::nullopt
                                 : std::optional<QByteArray>(*it);
}

bool MemoryCredentialStore::remove(const QString& reference)
{
    auto it = _secrets.find(reference);
    if (it == _secrets.end())
        return false;
    it->fill('\0');
    _secrets.erase(it);
    return true;
}

std::unique_ptr<CredentialStore> createCredentialStore()
{
#ifdef Q_OS_WIN
    return std::make_unique<WindowsCredentialStore>();
#else
    return std::make_unique<MemoryCredentialStore>();
#endif
}
