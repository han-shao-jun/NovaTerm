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
    quint64 size() const noexcept;
private:
    struct Impl;
    explicit LocalFileReader(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> _impl;
};

class LocalFileWriter final {
public:
    static FileIoResult<std::unique_ptr<LocalFileWriter>> create(
        const QString& trustedRoot, const QString& relativePath, quint64 expectedBytes);
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
