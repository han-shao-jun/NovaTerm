#pragma once
#include <QFile>
#include <QString>
#include <memory>

namespace NovaTerm::Recording::StorageIo {
// Every storage operation validates its private subtree. Windows additionally pins
// directory handles without FILE_SHARE_DELETE for the lifetime of the store.
class DirectoryGuard final {
public:
    DirectoryGuard();
    ~DirectoryGuard();
    bool lock(const QString& path);
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
bool safePath(const QString& path);
bool privateDirectory(const QString& path);
bool safeTree(const QString& path);
bool secureTree(const QString& path);
bool sync(QFile& file);
bool replaceDurable(const QString& path, const QByteArray& bytes);
bool syncDirectory(const QString& path);
bool privateFile(const QString& path);
bool safeFile(const QString& path);
bool removePrivateTree(const QString& path);
bool removePrivateFile(const QString& path);
QString error();
}
