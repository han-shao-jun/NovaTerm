#include "HistoryStorageIo.h"
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <vector>
#include <thread>
#include <chrono>
#include <cerrno>
#include <cstdio>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace NovaTerm::Recording::StorageIo {
namespace {
thread_local QString ioError;
#ifdef Q_OS_WIN
constexpr int IoRetryDelaysMs[] = {2, 4, 8, 16, 32, 64, 128, 256};
#endif
bool redirected(const QString& path) {
#ifdef Q_OS_WIN
    const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return QFileInfo(path).isSymbolicLink();
#endif
}
#ifdef Q_OS_WIN
QByteArray privateDescriptor() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD length = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    std::vector<char> info(length);
    const bool ok = GetTokenInformation(token, TokenUser, info.data(), length, &length);
    CloseHandle(token); if (!ok) return {};
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid, &sid)) return {};
    const QString sddl = "D:P(A;OICI;FA;;;" + QString::fromWCharArray(sid) + ")(A;OICI;FA;;;SY)";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &sd, &length)) return {};
    QByteArray result(reinterpret_cast<char*>(sd), int(length)); LocalFree(sd); return result;
}
#endif
}
bool safePath(const QString& path) {
    QString current = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    while (!current.isEmpty()) {
        if (redirected(current)) return false;
        const QString parent = QFileInfo(current).dir().absolutePath();
        if (parent == current) break;
        current = parent;
    }
    return true;
}
bool privateDirectory(const QString& path) {
    if (!safePath(path)) return false;
    if (!QFileInfo::exists(path)) {
        const auto parent = QFileInfo(path).dir().absolutePath();
        if (!QFileInfo::exists(parent) && !privateDirectory(parent)) return false;
#ifdef Q_OS_WIN
        auto sd = privateDescriptor(); if (sd.isEmpty()) return false;
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), sd.data(), FALSE};
        if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16()), &sa)) return false;
#else
        if (::mkdir(QFile::encodeName(path).constData(), 0700) != 0) return false;
#endif
    }
    if (!QFileInfo(path).isDir() || !safePath(path)) return false;
#ifdef Q_OS_WIN
    auto sd = privateDescriptor(); if (sd.isEmpty()) return false;
    return SetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()),
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd.data());
#else
    struct stat st{};
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0 && st.st_uid == ::geteuid()
        && ::chmod(QFile::encodeName(path).constData(), 0700) == 0;
#endif
}
namespace {
bool regularFile(const QString& path, bool secure) {
    if (!safePath(path) || !QFileInfo(path).isFile()) return false;
#ifdef Q_OS_WIN
    const HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info) && info.nNumberOfLinks == 1
        && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(h); if (!ok) return false;
    if (!secure) return true;
    auto sd = privateDescriptor(); if (sd.isEmpty()) return false;
    return SetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()),
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd.data());
#else
    struct stat st{};
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0 && S_ISREG(st.st_mode)
        && st.st_uid == ::geteuid() && st.st_nlink == 1
        && (!secure || ::chmod(QFile::encodeName(path).constData(), 0600) == 0);
#endif
}
}
bool privateFile(const QString& path) { return regularFile(path, true); }
bool safeFile(const QString& path) { return regularFile(path, false); }
bool removePrivateFile(const QString& path) {
    ioError.clear();
    if (!safePath(path)) { ioError = "unsafe file cleanup path"; return false; }
    if (!QFileInfo::exists(path)) return true;
    if (!safeFile(path)) { ioError = "unsafe file cleanup target"; return false; }
    const auto remove = [&] {
        QFile file(path); if (file.remove()) return true; ioError = file.errorString(); return false;
    };
    if (remove()) return true;
#ifdef Q_OS_WIN
    for (const int delay : IoRetryDelaysMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        if (!safePath(path)) { ioError = "unsafe file cleanup path"; return false; }
        if (!QFileInfo::exists(path)) return true;
        if (!safeFile(path)) { ioError = "unsafe file cleanup target"; return false; }
        if (remove()) return true;
    }
#endif
    return false;
}
bool safeTree(const QString& path) {
    if (!safePath(path) || !QFileInfo(path).isDir()) return false;
    const auto entries = QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const auto& entry : entries) {
        if (redirected(entry.absoluteFilePath())) return false;
        if (entry.isDir()) { if (!safeTree(entry.absoluteFilePath())) return false; }
        else if (!regularFile(entry.absoluteFilePath(), false)) return false;
    }
    return true;
}
bool secureTree(const QString& path) {
    if (!privateDirectory(path)) return false;
    for (const auto& entry : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (redirected(entry.absoluteFilePath())) return false;
        if (entry.isDir()) { if (!secureTree(entry.absoluteFilePath())) return false; }
        else if (!privateFile(entry.absoluteFilePath())) return false;
    }
    return true;
}
bool removePrivateTree(const QString& path) {
    ioError.clear();
    if (!safeTree(path)) { ioError = "unsafe capture cleanup path"; return false; }
    if (QDir(path).removeRecursively()) return true;
#ifdef Q_OS_WIN
    // Indexers/readers may briefly deny delete sharing. Retry the remaining
    // private subtree, checking redirection again before every deletion attempt.
    // Active captures and leases are rejected by the owner before this boundary.
    for (const int delay : IoRetryDelaysMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        if (!safePath(path)) { ioError = "unsafe capture cleanup path"; return false; }
        if (!QFileInfo::exists(path)) return true;
        if (!safeTree(path)) { ioError = "unsafe capture cleanup subtree"; return false; }
        if (QDir(path).removeRecursively()) return true;
    }
    ioError = QStringLiteral("capture deletion failed (Win32 %1)").arg(GetLastError());
#else
    ioError = QStringLiteral("capture deletion failed (errno %1)").arg(errno);
#endif
    return false;
}
struct DirectoryGuard::Impl {
#ifdef Q_OS_WIN
    std::vector<HANDLE> handles;
    ~Impl() { for (const auto h : handles) CloseHandle(h); }
#endif
};
DirectoryGuard::DirectoryGuard() : _impl(std::make_unique<Impl>()) {}
DirectoryGuard::~DirectoryGuard() = default;
bool DirectoryGuard::lock(const QString& path) {
    if (!safePath(path)) return false;
#ifdef Q_OS_WIN
    QString current = QFileInfo(path).absoluteFilePath();
    while (!current.isEmpty()) {
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(current.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(h); return false;
        }
        _impl->handles.push_back(h);
        const auto parent = QFileInfo(current).dir().absolutePath();
        if (parent == current) break;
        current = parent;
    }
#endif
    return true;
}
bool sync(QFile& file) {
    if (!file.flush()) return false;
#ifdef Q_OS_WIN
    return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())));
#else
    return ::fsync(file.handle()) == 0;
#endif
}
bool syncDirectory(const QString& path) {
#ifdef Q_OS_WIN
    Q_UNUSED(path);
    // MoveFileEx(WRITE_THROUGH) below is the Windows publication primitive.
    // Filesystem / device power-loss durability is not claimed by unit tests.
    return true;
#else
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0; ::close(fd); return ok;
#endif
}
bool replaceDurable(const QString& path, const QByteArray& bytes) {
    ioError.clear();
    if (!safePath(path)) return false;
    const QString temp = path + "." + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".tmp";
    QFile file(temp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
    if (!privateFile(temp) || file.write(bytes) != bytes.size() || !sync(file)) {
        file.close(); QFile::remove(temp); return false;
    }
    file.close(); bool ok;
#ifdef Q_OS_WIN
    ok = MoveFileExW(reinterpret_cast<LPCWSTR>(temp.utf16()), reinterpret_cast<LPCWSTR>(path.utf16()),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    DWORD replaceError = ok ? ERROR_SUCCESS : GetLastError();
    // NTFS readers without FILE_SHARE_DELETE can transiently block replacement.
    // Bound retries on this I/O worker; a permanent failure must remain unsealed.
    for (const int delay : IoRetryDelaysMs) {
        if (ok || (replaceError != ERROR_ACCESS_DENIED && replaceError != ERROR_SHARING_VIOLATION)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        if (!safePath(path)) break;
        ok = MoveFileExW(reinterpret_cast<LPCWSTR>(temp.utf16()), reinterpret_cast<LPCWSTR>(path.utf16()),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        replaceError = ok ? ERROR_SUCCESS : GetLastError();
    }
#else
    ok = ::rename(QFile::encodeName(temp).constData(), QFile::encodeName(path).constData()) == 0;
#endif
    if (!ok) {
#ifdef Q_OS_WIN
        ioError = QStringLiteral("atomic replacement failed (Win32 %1)").arg(replaceError);
#else
        ioError = QStringLiteral("atomic replacement failed (errno %1)").arg(errno);
#endif
        QFile::remove(temp); return false;
    }
    return syncDirectory(QFileInfo(path).absolutePath());
}
QString error() { return ioError; }
}
