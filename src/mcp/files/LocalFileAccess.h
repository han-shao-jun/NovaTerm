#pragma once

#include <QByteArray>
#include <QString>
#include <memory>
#include <variant>

namespace NovaTerm::Mcp::Files {

enum class FileIoError {
    InvalidPath, NotAvailable, UnsafeFile, SizeLimit, IoFailure,
    AlreadyExists, SizeMismatch, InvalidState, UnsupportedPlatform
};
template<typename T> using FileIoResult = std::variant<T, FileIoError>;

// Synchronous worker-only boundary. The caller owns authorization and rechecks
// the original resource revision/identity before opening and before publishing.
// Non-Windows platforms fail closed until their native backend is implemented.
class LocalFileReader final {
public:
    static FileIoResult<std::unique_ptr<LocalFileReader>> open(
        const QString& trustedRoot, const QString& relativePath);
    ~LocalFileReader();
    LocalFileReader(const LocalFileReader&) = delete;
    LocalFileReader& operator=(const LocalFileReader&) = delete;
    LocalFileReader(LocalFileReader&&) noexcept;
    LocalFileReader& operator=(LocalFileReader&&) noexcept;
    FileIoResult<QByteArray> read(qsizetype maxBytes);
    // 固定句柄精确读取 1～65536 字节，不推进顺序 read 的逻辑游标。
    FileIoResult<QByteArray> readAt(quint64 offset, qsizetype count);
    quint64 size() const noexcept;
private:
    struct Impl;
    explicit LocalFileReader(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> _impl;
};

class LocalFileWriter final {
public:
    // overwrite 只替换初始存在且发布前身份/属性仍一致的普通文件。
    // 最后复核与替换不是 CAS；目录其他写入者须可信，当前不验证目录 ACL。
    static FileIoResult<std::unique_ptr<LocalFileWriter>> create(
        const QString& trustedRoot, const QString& relativePath, quint64 expectedBytes, bool overwrite = false);
    ~LocalFileWriter();
    LocalFileWriter(const LocalFileWriter&) = delete;
    LocalFileWriter& operator=(const LocalFileWriter&) = delete;
    LocalFileWriter(LocalFileWriter&&) noexcept;
    LocalFileWriter& operator=(LocalFileWriter&&) noexcept;
    FileIoResult<std::monostate> append(const QByteArray&);
    FileIoResult<std::monostate> publish();
    // false retains ownership so a caller can report/retry failed cleanup.
    bool cancel() noexcept;
private:
    struct Impl;
    explicit LocalFileWriter(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> _impl;
};
}
