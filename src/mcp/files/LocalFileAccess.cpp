#include "LocalFileAccess.h"
#include "mcp/contracts/Budget.h"
#include <QDir>
#include <QStringList>
#include <QUuid>
#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#endif

namespace NovaTerm::Mcp::Files {
namespace {
#ifdef Q_OS_WIN
class Handle final {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) noexcept : _handle(handle) {}
    ~Handle() { if (valid()) CloseHandle(_handle); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : _handle(std::exchange(other._handle, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            if (valid()) CloseHandle(_handle);
            _handle = std::exchange(other._handle, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    bool valid() const noexcept { return _handle != INVALID_HANDLE_VALUE; }
    HANDLE get() const noexcept { return _handle; }
private:
    HANDLE _handle{INVALID_HANDLE_VALUE};
};

bool validText(const QString& text)
{
    if (text.isEmpty() || text.toUtf8().size() > Budget::MaxFileRootPathBytes
        || QString::fromUtf8(text.toUtf8()) != text) return false;
    for (const auto ch : text) if (ch.unicode() < 32 || ch.unicode() == 127) return false;
    return true;
}
bool validPart(const QString& part)
{
    if (part.isEmpty() || part.endsWith('.') || part.endsWith(' ')) return false;
    for (const auto ch : part) if (QStringLiteral("<>:\"|?*\\").contains(ch)) return false;
    const auto base = part.section('.', 0, 0).toUpper();
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") return false;
    if (base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT"))) {
        const auto digit = base[3];
        if ((digit >= '1' && digit <= '9') || digit == QChar(0xb9)
            || digit == QChar(0xb2) || digit == QChar(0xb3)) return false;
    }
    return true;
}
struct Path {
    QString volume;
    QStringList directories;
    QString leaf;
};
std::optional<Path> preparePath(const QString& root, const QString& relative)
{
    if (!validText(root) || root.startsWith("\\\\") || root.startsWith("//")
        || !validText(relative) || relative.contains('\\')) return std::nullopt;
    const auto normalized = QDir::fromNativeSeparators(root);
    if (normalized.size() <= 3 || normalized[1] != ':' || normalized[2] != '/'
        || !((normalized[0] >= 'a' && normalized[0] <= 'z')
            || (normalized[0] >= 'A' && normalized[0] <= 'Z'))) return std::nullopt;
    const auto rootParts = normalized.mid(3).split('/');
    auto fileParts = relative.split('/');
    if (rootParts.size() > Budget::MaxLocalTransferPathParts
        || fileParts.size() > Budget::MaxLocalTransferPathParts) return std::nullopt;
    for (const auto& part : rootParts) if (!validPart(part)) return std::nullopt;
    for (const auto& part : fileParts) if (!validPart(part)) return std::nullopt;
    const auto leaf = fileParts.takeLast();
    return Path{normalized.left(3), rootParts + fileParts, leaf};
}
QString native(const QString& path) { return QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(path); }
const wchar_t* wide(const QString& value) { return reinterpret_cast<const wchar_t*>(value.utf16()); }
FileIoResult<Handle> openRelative(HANDLE parent, const QString& name, ACCESS_MASK access,
    ULONG sharing, ULONG disposition, ULONG options, ULONG attributes = FILE_ATTRIBUTE_NORMAL,
    NTSTATUS* nativeStatus = nullptr)
{
    if (nativeStatus) *nativeStatus = 0;
    const auto create = reinterpret_cast<decltype(&NtCreateFile)>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    if (!create) return FileIoError::UnsupportedPlatform;
    UNICODE_STRING string{};
    string.Buffer = const_cast<wchar_t*>(wide(name));
    string.Length = USHORT(name.size() * sizeof(wchar_t));
    string.MaximumLength = string.Length;
    OBJECT_ATTRIBUTES object{};
    object.Length = sizeof(object);
    object.RootDirectory = parent;
    object.ObjectName = &string;
    object.Attributes = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK io{};
    HANDLE file = nullptr;
    const auto status = create(&file, access | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &object, &io, nullptr, attributes,
        sharing, disposition, options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    if (nativeStatus) *nativeStatus = status;
    if (status < 0) return quint32(status) == 0xc0000035
        ? FileIoError::AlreadyExists : FileIoError::NotAvailable;
    return Handle(file);
}
struct PinnedPath {
    std::vector<Handle> directories;
    QString parent;
    QString leaf;
};
FileIoResult<PinnedPath> pinPath(const Path& path)
{
    const auto volume = QDir::toNativeSeparators(path.volume);
    const auto type = GetDriveTypeW(wide(volume));
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK) return FileIoError::UnsafeFile;
    PinnedPath pinned;
    pinned.leaf = path.leaf;
    QString current = path.volume;
    // Lock the volume and every ancestor in order, before resolving children.
    for (qsizetype i = 0; i <= path.directories.size(); ++i) {
        Handle handle;
        if (i == 0) {
            const auto encoded = native(current);
            handle = Handle(CreateFileW(wide(encoded), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        } else {
            auto opened = openRelative(pinned.directories.back().get(), path.directories[i - 1],
                FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                FILE_OPEN, FILE_DIRECTORY_FILE);
            if (const auto* error = std::get_if<FileIoError>(&opened)) return *error;
            handle = std::move(std::get<Handle>(opened));
        }
        if (!handle.valid()) return FileIoError::NotAvailable;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.get(), &info)) return FileIoError::IoFailure;
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return FileIoError::UnsafeFile;
        pinned.directories.push_back(std::move(handle));
        if (i < path.directories.size()) {
            if (!current.endsWith('/')) current += '/';
            current += path.directories[i];
        }
    }
    pinned.parent = current;
    return pinned;
}
quint64 fileSize(const BY_HANDLE_FILE_INFORMATION& info)
{
    return (quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}
bool regular(HANDLE handle, const BY_HANDLE_FILE_INFORMATION& info)
{
    return GetFileType(handle) == FILE_TYPE_DISK && info.nNumberOfLinks == 1
        && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
#endif
#ifdef Q_OS_LINUX
class PosixFd final {
public:
    explicit PosixFd(int fd = -1) noexcept : _fd(fd) {}
    ~PosixFd() { if (_fd >= 0) ::close(_fd); }
    PosixFd(const PosixFd&) = delete;
    PosixFd& operator=(const PosixFd&) = delete;
    PosixFd(PosixFd&& other) noexcept : _fd(std::exchange(other._fd, -1)) {}
    PosixFd& operator=(PosixFd&& other) noexcept {
        if (this != &other) { if (_fd >= 0) ::close(_fd); _fd = std::exchange(other._fd, -1); }
        return *this;
    }
    int get() const noexcept { return _fd; }
    bool valid() const noexcept { return _fd >= 0; }
private:
    int _fd;
};
bool posixText(const QString& text)
{
    if (text.isEmpty() || text.toUtf8().size() > Budget::MaxFileRootPathBytes
        || QString::fromUtf8(text.toUtf8()) != text || text.contains('\\')) return false;
    for (const auto ch : text) if (ch.unicode() < 32 || ch.unicode() == 127) return false;
    return true;
}
bool posixRegular(const struct stat& info)
{
    return S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_size >= 0;
}
bool sameSource(const struct stat& a, const struct stat& b)
{
    return posixRegular(b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode
        && a.st_size == b.st_size && a.st_nlink == b.st_nlink
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
FileIoError posixOpenError()
{
    return errno == ELOOP || errno == ENOTDIR ? FileIoError::UnsafeFile : FileIoError::NotAvailable;
}
struct PosixPinnedFile {
    std::vector<PosixFd> directories;
    PosixFd file;
    struct stat snapshot{};
};
FileIoResult<PosixPinnedFile> openPosixSource(const QString& root, const QString& relative)
{
    if (!posixText(root) || root == "/" || !root.startsWith('/') || root.startsWith("//")
        || QDir::cleanPath(root) != root || !posixText(relative) || relative.startsWith('/')
        || relative.contains(':') || QDir::cleanPath(relative) != relative) return FileIoError::InvalidPath;
    auto rootParts = root.mid(1).split('/');
    auto fileParts = relative.split('/');
    if (rootParts.size() > Budget::MaxLocalTransferPathParts || fileParts.size() > Budget::MaxLocalTransferPathParts)
        return FileIoError::InvalidPath;
    for (const auto& part : rootParts + fileParts)
        if (part.isEmpty() || part == "." || part == "..") return FileIoError::InvalidPath;
    const auto leaf = fileParts.takeLast().toUtf8();
    const auto parents = rootParts + fileParts;
    PosixPinnedFile pinned;
    pinned.directories.emplace_back(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!pinned.directories.back().valid()) return posixOpenError();
    for (const auto& part : parents) {
        const auto bytes = part.toUtf8();
        PosixFd directory(::openat(pinned.directories.back().get(), bytes.constData(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!directory.valid()) return posixOpenError();
        pinned.directories.push_back(std::move(directory));
    }
    // O_PATH inspects and pins the leaf before opening it for I/O: a device/FIFO
    // is rejected without ever opening that object in read mode.
    PosixFd identity(::openat(pinned.directories.back().get(), leaf.constData(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (!identity.valid()) return posixOpenError();
    if (::fstat(identity.get(), &pinned.snapshot) != 0) return FileIoError::IoFailure;
    if (!posixRegular(pinned.snapshot)) return FileIoError::UnsafeFile;
    if (quint64(pinned.snapshot.st_size) > Budget::MaxLocalTransferFileBytes) return FileIoError::SizeLimit;
    // /proc/self/fd references this still-owned descriptor, never a mutable
    // pathname supplied by the caller. Recheck identity after the reopen.
    const auto descriptorPath = QByteArray("/proc/self/fd/") + QByteArray::number(identity.get());
    pinned.file = PosixFd(::open(descriptorPath.constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (!pinned.file.valid()) return posixOpenError();
    struct stat opened{};
    if (::fstat(pinned.file.get(), &opened) != 0) return FileIoError::IoFailure;
    if (!sameSource(pinned.snapshot, opened)) return FileIoError::UnsafeFile;
    if (::flock(pinned.file.get(), LOCK_SH | LOCK_NB) != 0) return FileIoError::NotAvailable;
    return pinned;
}
#endif
}
struct LocalFileReader::Impl {
#ifdef Q_OS_WIN
    PinnedPath path;
    Handle file;
#elif defined(Q_OS_LINUX)
    PosixPinnedFile source;
#endif
    quint64 bytes{0};
    quint64 consumed{0};
    bool failed{false};
};
struct LocalFileWriter::Impl {
#ifdef Q_OS_WIN
    PinnedPath path;
    Handle file;
    std::optional<BY_HANDLE_FILE_INFORMATION> originalTarget;
    bool discard() noexcept
    {
        if (file.valid() && !published) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(file.get(), &info)) return false;
            if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                FILE_BASIC_INFO basic{};
                basic.FileAttributes = info.dwFileAttributes & ~DWORD(FILE_ATTRIBUTE_READONLY);
                if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                if (!SetFileInformationByHandle(file.get(), FileBasicInfo, &basic, sizeof(basic))) return false;
            }
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition))) return false;
            file = Handle{};
        }
        return true;
    }
    ~Impl() { discard(); }
#endif
    quint64 expected{0};
    quint64 written{0};
    bool failed{false};
    bool published{false};
};
LocalFileReader::LocalFileReader(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
LocalFileReader::~LocalFileReader() = default;
LocalFileReader::LocalFileReader(LocalFileReader&&) noexcept = default;
LocalFileReader& LocalFileReader::operator=(LocalFileReader&&) noexcept = default;
LocalFileWriter::LocalFileWriter(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
LocalFileWriter::~LocalFileWriter() = default;
LocalFileWriter::LocalFileWriter(LocalFileWriter&&) noexcept = default;
LocalFileWriter& LocalFileWriter::operator=(LocalFileWriter&&) noexcept = default;
FileIoResult<std::unique_ptr<LocalFileReader>> LocalFileReader::open(const QString& root, const QString& relative)
{
#ifdef Q_OS_WIN
    const auto path = preparePath(root, relative);
    if (!path) return FileIoError::InvalidPath;
    auto pins = pinPath(*path);
    if (const auto* error = std::get_if<FileIoError>(&pins)) return *error;
    auto impl = std::make_unique<Impl>();
    impl->path = std::move(std::get<PinnedPath>(pins));
    auto opened = openRelative(impl->path.directories.back().get(), impl->path.leaf,
        GENERIC_READ, FILE_SHARE_READ, FILE_OPEN, 0);
    if (const auto* error = std::get_if<FileIoError>(&opened)) return *error;
    impl->file = std::move(std::get<Handle>(opened));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(impl->file.get(), &info)) return FileIoError::IoFailure;
    if (!regular(impl->file.get(), info)) return FileIoError::UnsafeFile;
    impl->bytes = fileSize(info);
    if (impl->bytes > Budget::MaxLocalTransferFileBytes) return FileIoError::SizeLimit;
    return std::unique_ptr<LocalFileReader>(new LocalFileReader(std::move(impl)));
#elif defined(Q_OS_LINUX)
    auto source = openPosixSource(root, relative);
    if (const auto* error = std::get_if<FileIoError>(&source)) return *error;
    auto impl = std::make_unique<Impl>();
    impl->source = std::move(std::get<PosixPinnedFile>(source));
    impl->bytes = quint64(impl->source.snapshot.st_size);
    return std::unique_ptr<LocalFileReader>(new LocalFileReader(std::move(impl)));
#else
    Q_UNUSED(root); Q_UNUSED(relative);
    return FileIoError::UnsupportedPlatform;
#endif
}
FileIoResult<QByteArray> LocalFileReader::read(qsizetype maxBytes)
{
    if (!_impl || _impl->failed) return FileIoError::InvalidState;
    if (maxBytes <= 0 || maxBytes > Budget::LocalTransferChunkBytes) return FileIoError::SizeLimit;
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    const auto count = qMin(quint64(maxBytes), _impl->bytes - _impl->consumed);
    if (!count) return QByteArray{};
    auto bytes = readAt(_impl->consumed, qsizetype(count));
    if (std::holds_alternative<QByteArray>(bytes)) _impl->consumed += count;
    return bytes;
#else
    return FileIoError::UnsupportedPlatform;
#endif
}
FileIoResult<QByteArray> LocalFileReader::readAt(quint64 offset, qsizetype count)
{
    if (!_impl || _impl->failed) return FileIoError::InvalidState;
    if (count <= 0 || count > Budget::LocalTransferChunkBytes
        || offset > _impl->bytes || quint64(count) > _impl->bytes - offset) return FileIoError::SizeLimit;
#ifdef Q_OS_WIN
    LARGE_INTEGER position{};
    position.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(_impl->file.get(), position, nullptr, FILE_BEGIN)) {
        _impl->failed = true;
        return FileIoError::IoFailure;
    }
    QByteArray bytes(count, Qt::Uninitialized);
    DWORD received = 0;
    if (!ReadFile(_impl->file.get(), bytes.data(), DWORD(count), &received, nullptr)
        || received != quint64(count)) {
        _impl->failed = true;
        return FileIoError::IoFailure;
    }
    return bytes;
#elif defined(Q_OS_LINUX)
    struct stat before{};
    if (::fstat(_impl->source.file.get(), &before) != 0 || !sameSource(_impl->source.snapshot, before)) {
        _impl->failed = true; return FileIoError::UnsafeFile;
    }
    QByteArray bytes(count, Qt::Uninitialized);
    qsizetype received = 0;
    while (received < count) {
        const auto n = ::pread(_impl->source.file.get(), bytes.data() + received,
            size_t(count - received), off_t(offset + quint64(received)));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { _impl->failed = true; return FileIoError::IoFailure; }
        received += qsizetype(n);
    }
    struct stat after{};
    if (::fstat(_impl->source.file.get(), &after) != 0 || !sameSource(_impl->source.snapshot, after)) {
        _impl->failed = true; return FileIoError::UnsafeFile;
    }
    return bytes;
#else
    return FileIoError::UnsupportedPlatform;
#endif
}
quint64 LocalFileReader::size() const noexcept { return _impl ? _impl->bytes : 0; }
FileIoResult<std::unique_ptr<LocalFileWriter>> LocalFileWriter::create(
    const QString& root, const QString& relative, quint64 expectedBytes, bool overwrite)
{
#ifdef Q_OS_WIN
    if (expectedBytes > Budget::MaxLocalTransferFileBytes) return FileIoError::SizeLimit;
    const auto path = preparePath(root, relative);
    if (!path) return FileIoError::InvalidPath;
    auto pins = pinPath(*path);
    if (const auto* error = std::get_if<FileIoError>(&pins)) return *error;
    auto impl = std::make_unique<Impl>();
    impl->path = std::move(std::get<PinnedPath>(pins));
    impl->expected = expectedBytes;
    // 只有明确缺失才允许创建新目标；其他查询失败不得当作不存在。
    NTSTATUS targetStatus = 0;
    auto existing = openRelative(impl->path.directories.back().get(), impl->path.leaf,
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN, 0, FILE_ATTRIBUTE_NORMAL, &targetStatus);
    if (auto* handle = std::get_if<Handle>(&existing)) {
        if (!overwrite) return FileIoError::AlreadyExists;
        BY_HANDLE_FILE_INFORMATION original{};
        if (!GetFileInformationByHandle(handle->get(), &original)) return FileIoError::IoFailure;
        if (!regular(handle->get(), original)
            || (original.dwFileAttributes & FILE_ATTRIBUTE_READONLY)) return FileIoError::UnsafeFile;
        impl->originalTarget = original;
        *handle = Handle{};
    } else if (quint32(targetStatus) != 0xc0000034) { // STATUS_OBJECT_NAME_NOT_FOUND
        return std::get<FileIoError>(existing);
    }
    const auto temporary = QStringLiteral(".novaterm-transfer-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".tmp");
    auto opened = openRelative(impl->path.directories.back().get(), temporary,
        GENERIC_WRITE | DELETE, FILE_SHARE_READ, FILE_CREATE, FILE_NON_DIRECTORY_FILE, FILE_ATTRIBUTE_TEMPORARY);
    if (const auto* error = std::get_if<FileIoError>(&opened)) return *error;
    impl->file = std::move(std::get<Handle>(opened));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(impl->file.get(), &info)) return FileIoError::IoFailure;
    if (!regular(impl->file.get(), info)) return FileIoError::UnsafeFile;
    return std::unique_ptr<LocalFileWriter>(new LocalFileWriter(std::move(impl)));
#else
    Q_UNUSED(root); Q_UNUSED(relative); Q_UNUSED(expectedBytes); Q_UNUSED(overwrite);
    return FileIoError::UnsupportedPlatform;
#endif
}
FileIoResult<std::monostate> LocalFileWriter::append(const QByteArray& bytes)
{
    if (!_impl || _impl->failed || _impl->published) return FileIoError::InvalidState;
    if (bytes.size() > Budget::LocalTransferChunkBytes
        || quint64(bytes.size()) > _impl->expected - _impl->written) {
        _impl->failed = true;
        return FileIoError::SizeLimit;
    }
#ifdef Q_OS_WIN
    DWORD written = 0;
    if (!WriteFile(_impl->file.get(), bytes.constData(), DWORD(bytes.size()), &written, nullptr)
        || written != quint64(bytes.size())) {
        _impl->failed = true;
        return FileIoError::IoFailure;
    }
    _impl->written += written;
    return std::monostate{};
#else
    return FileIoError::UnsupportedPlatform;
#endif
}
FileIoResult<std::monostate> LocalFileWriter::publish()
{
    if (!_impl || _impl->failed || _impl->published) return FileIoError::InvalidState;
    _impl->failed = true; // Every failed publication is terminal; do not retry with a changed target.
    if (_impl->written != _impl->expected) return FileIoError::SizeMismatch;
#ifdef Q_OS_WIN
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(_impl->file.get(), &info)) return FileIoError::IoFailure;
    if (!regular(_impl->file.get(), info)) return FileIoError::UnsafeFile;
    if (fileSize(info) != _impl->expected) return FileIoError::SizeMismatch;
    if (!FlushFileBuffers(_impl->file.get())) return FileIoError::IoFailure;
    if (_impl->originalTarget) {
        auto existing = openRelative(_impl->path.directories.back().get(), _impl->path.leaf,
            FILE_READ_ATTRIBUTES, FILE_SHARE_READ, FILE_OPEN, 0);
        if (const auto* error = std::get_if<FileIoError>(&existing)) return *error;
        const auto& handle = std::get<Handle>(existing);
        BY_HANDLE_FILE_INFORMATION current{};
        if (!GetFileInformationByHandle(handle.get(), &current)) return FileIoError::IoFailure;
        const auto& original = *_impl->originalTarget;
        if (!regular(handle.get(), current)
            || current.dwVolumeSerialNumber != original.dwVolumeSerialNumber
            || current.nFileIndexHigh != original.nFileIndexHigh
            || current.nFileIndexLow != original.nFileIndexLow
            || fileSize(current) != fileSize(original)
            || CompareFileTime(&current.ftLastWriteTime, &original.ftLastWriteTime) != 0
            || current.dwFileAttributes != original.dwFileAttributes) return FileIoError::UnsafeFile;
        // 经典 rename 前必须关闭目标检查句柄。此后仍有竞争窗口，不承诺 CAS；
        // 调用方只能对其他写入者可信的目录启用覆盖，pinPath 不检查目录 ACL。
    }
    const auto target = _impl->path.leaf;
    const auto nameBytes = target.size() * sizeof(wchar_t);
    const auto size = offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);
    // quint64 storage supplies the native struct's alignment as well as ownership.
    std::vector<quint64> storage((size + sizeof(quint64) - 1) / sizeof(quint64));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = _impl->originalTarget ? TRUE : FALSE;
    rename->RootDirectory = _impl->path.directories.back().get();
    rename->FileNameLength = DWORD(nameBytes);
    memcpy(rename->FileName, target.utf16(), size_t(nameBytes));
    rename->FileName[target.size()] = L'\0';
    // The Win32 wrapper requires a null RootDirectory and reopens the parent by
    // path. Use the native relative-handle operation to retain the directory lock.
    using SetInformation = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    const auto setInformation = reinterpret_cast<SetInformation>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
    if (!setInformation) return FileIoError::UnsupportedPlatform;
    IO_STATUS_BLOCK io{};
    constexpr auto renameInformation = static_cast<FILE_INFORMATION_CLASS>(10);
    const auto status = setInformation(_impl->file.get(), &io, rename, ULONG(size), renameInformation);
    if (status < 0) return quint32(status) == 0xc0000035 // STATUS_OBJECT_NAME_COLLISION
        ? FileIoError::AlreadyExists : FileIoError::IoFailure;
    _impl->published = true; // No later failure or cancellation may delete the published target.
    _impl->file = Handle{};
    _impl->path.directories.clear();
    return std::monostate{};
#else
    return FileIoError::UnsupportedPlatform;
#endif
}
bool LocalFileWriter::cancel() noexcept
{
#ifdef Q_OS_WIN
    if (_impl && !_impl->discard()) { _impl->failed = true; return false; }
#endif
    _impl.reset();
    return true;
}
}
