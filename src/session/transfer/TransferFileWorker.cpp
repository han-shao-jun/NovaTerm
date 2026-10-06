/** @file TransferFileWorker.cpp @brief 异步源准备、临时接收及无覆盖原子发布。 */
#include "TransferFileWorker.h"
#include "core/ThreadNaming.h"
#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <filesystem>
#include <memory>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace FT = NovaTerm::FileTransfer;
namespace {
constexpr char FileThreadName[] = "nvterm-file";
static_assert(sizeof(FileThreadName) - 1 <= NovaTerm::MaxThreadNameBytes);
/** @brief 同时拒绝 POSIX/Windows 路径、设备名与不可见控制字符。 */
bool safeName(const std::string& utf8)
{
    if (!FT::validFileNameUtf8(utf8)) return false;
    const auto name = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
    if (name == "." || name == ".." || name.endsWith('.') || name.endsWith(' ')) return false;
    for (const auto c : name) {
        if (c.unicode() < 32 || c.unicode() == 127 || QStringLiteral("/\\:<>\"|?*").contains(c)) return false;
    }
    const auto stem = name.section('.', 0, 0).toUpper();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    if (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT"))
        && stem.at(3) >= QChar('1') && stem.at(3) <= QChar('9')) return false;
    return true;
}
std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    const auto bytes = QFile::encodeName(path);
    return std::filesystem::path(bytes.constData());
#endif
}
}
TransferFileWorker::TransferFileWorker(quint64 epoch, SerialTransferRequest request, LinkOperation linkOperation, LinkOperation moveOperation)
    : _epoch(epoch), _request(std::move(request)), _linkOperation(std::move(linkOperation)),
      _moveOperation(std::move(moveOperation))
{
    setObjectName(QString::fromLatin1(FileThreadName));
    qRegisterMetaType<TransferFileResult>();
}
void TransferFileWorker::requestStop()
{
    _stopped.store(true); _wake.notify_one();
}
bool TransferFileWorker::submit(FT::TransferAction action)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_stopped.load() || _busy || action.bytes.size() > 256 * 1024 || action.count > 256 * 1024) return false;
    _busy = true; _job = std::move(action); _wake.notify_one(); return true;
}
void TransferFileWorker::run()
{
    NovaTerm::setCurrentThreadName(FileThreadName);
    if (_stopped.load()) return;
    std::vector<std::unique_ptr<QFile>> sources;
    std::unique_ptr<QTemporaryFile> temporary;
    QString destinationDirectory, explicitName, target;
    TransferFileResult prepared; prepared.epoch = _epoch; prepared.preparation = true;
    prepared.request.direction = _request.direction;
    prepared.request.expectedSize = _request.expectedSize;
    if (_request.direction == FT::Direction::Send) {
        for (const auto& path : _request.files) {
            if (_stopped.load()) return;
            const QFileInfo info(path);
            const auto name = info.fileName().toUtf8().toStdString();
            auto file = std::make_unique<QFile>(path);
            if (!safeName(name) || !info.isFile() || !file->open(QIODevice::ReadOnly)
                || file->size() < 0 || quint64(file->size()) > FT::MaxFileSize) {
                prepared.error = QCoreApplication::translate("SerialFileTransferController", "Cannot open source file or unsafe file name: %1").arg(path); break;
            }
            prepared.request.files.push_back({name, quint64(file->size())});
            sources.push_back(std::move(file));
        }
    } else {
        QFileInfo location(_request.destination);
        if (_request.protocol <= SerialTransferProtocol::Xmodem1K) {
            explicitName = location.fileName();
            destinationDirectory = location.dir().canonicalPath();
            prepared.request.receiveName = explicitName.toUtf8().toStdString();
            if (!safeName(prepared.request.receiveName) || location.exists() || location.isSymLink())
                prepared.error = QCoreApplication::translate("SerialFileTransferController", "Receive target already exists or its name is unsafe");
        } else {
            if (!location.isDir() || location.isSymLink()) prepared.error = QCoreApplication::translate("SerialFileTransferController", "Receive directory does not exist or is a symbolic link");
            destinationDirectory = location.canonicalFilePath();
        }
        if (destinationDirectory.isEmpty()) prepared.error = QCoreApplication::translate("SerialFileTransferController", "Receive directory does not exist");
    }
    { std::lock_guard<std::mutex> lock(_mutex); _busy = false; }
    if (!_stopped.load()) emit resultReady(prepared);
    if (!prepared.error.isEmpty()) return;
    while (!_stopped.load()) {
        FT::TransferAction action;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _wake.wait(lock, [&] { return _stopped.load() || _job.has_value(); });
            if (_stopped.load()) break;
            action = std::move(*_job); _job.reset();
        }
        TransferFileResult result; result.epoch = _epoch; result.operation = action.id;
        auto fail = [&](const QString& message) { result.result.success = false; result.error = message; };
        switch (action.kind) {
        case FT::ActionKind::OfferFile: {
            const auto name = explicitName.isEmpty()
                ? QString::fromUtf8(action.file.name.data(), qsizetype(action.file.name.size())) : explicitName;
            const auto encodedName = name.toUtf8().toStdString();
            target = QDir(destinationDirectory).filePath(name);
            const QFileInfo info(target);
            if (!safeName(encodedName) || info.exists() || info.isSymLink() || temporary) {
                result.result.accepted = false; fail(QCoreApplication::translate("SerialFileTransferController", "Unsafe receive name or target already exists: %1").arg(name)); break;
            }
            temporary = std::make_unique<QTemporaryFile>(QDir(destinationDirectory).filePath(".novaterm-XXXXXX.part"));
            if (!temporary->open()) fail(QCoreApplication::translate("SerialFileTransferController", "Cannot create temporary receive file"));
            break;
        }
        case FT::ActionKind::ReadAt: {
            if (action.fileIndex >= sources.size() || action.offset > FT::MaxFileSize) {
                fail(QCoreApplication::translate("SerialFileTransferController", "Invalid source file index or offset")); break;
            }
            auto& file = *sources[action.fileIndex];
            if (!file.seek(qint64(action.offset))) { fail(file.errorString()); break; }
            const auto bytes = file.read(qint64(action.count));
            if (bytes.size() != qsizetype(action.count)) { fail(QCoreApplication::translate("SerialFileTransferController", "Source file was truncated or changed")); break; }
            result.result.bytes.assign(reinterpret_cast<const quint8*>(bytes.constData()),
                                       reinterpret_cast<const quint8*>(bytes.constData()) + bytes.size());
            break;
        }
        case FT::ActionKind::WriteAt:
            if (!temporary || !temporary->seek(qint64(action.offset))
                || temporary->write(reinterpret_cast<const char*>(action.bytes.data()), qint64(action.bytes.size())) != qint64(action.bytes.size()))
                fail(QCoreApplication::translate("SerialFileTransferController", "Temporary receive file write failed"));
            break;
        case FT::ActionKind::FinishFile:
            if (_request.direction == FT::Direction::Receive) {
                if (!temporary || !temporary->flush()) { fail(QCoreApplication::translate("SerialFileTransferController", "Receive file flush failed")); break; }
                temporary->close();
                // 硬链接创建对已存在目标原子失败；不打开或覆盖目标及其符号链接。
                std::error_code error;
                const auto sourcePath = nativePath(temporary->fileName());
                const auto targetPath = nativePath(target);
                bool linked = false;
                if (!_stopped.load()) {
                    if (_linkOperation) _linkOperation(sourcePath, targetPath, error);
                    else std::filesystem::create_hard_link(sourcePath, targetPath, error);
                    linked = !error;
                }
                // 发布后的停止检查是提交线性化点；此前取消只回滚本次新建硬链接。
                // 临时文件仍保留身份锚点，不跟随目标符号链接或删除其他批次文件。
                const bool stoppedDuringPublication = _stopped.load();
                if (linked && stoppedDuringPublication) {
                    std::error_code identityError;
                    const auto status = std::filesystem::symlink_status(targetPath, identityError);
                    if (!identityError && std::filesystem::is_regular_file(status)
                        && std::filesystem::equivalent(sourcePath, targetPath, identityError)
                        && !identityError) {
                        // 先原子移入线程独有的隔离名再核对身份，避免检查后直接
                        // unlink 用户目标时，误删检查与删除之间替换进来的文件。
                        // QTemporaryFile::close() 并不释放原生句柄（引擎仅回绕到开头），
                        // Windows 上占位文件句柄未关闭时 MoveFileExW 替换会报
                        // ERROR_ACCESS_DENIED；因此只借它原子占名，随即析构释放句柄。
                        QString quarantineName;
                        {
                            QTemporaryFile quarantine(QDir(destinationDirectory).filePath(".novaterm-rollback-XXXXXX.part"));
                            if (quarantine.open()) { quarantine.setAutoRemove(false); quarantineName = quarantine.fileName(); }
                        }
                        if (quarantineName.isEmpty()) {
                            error = std::make_error_code(std::errc::io_error);
                        } else {
                            const auto quarantinePath = nativePath(quarantineName);
                            if (_moveOperation) _moveOperation(targetPath, quarantinePath, error);
                            else {
#ifdef Q_OS_WIN
                                if (!MoveFileExW(targetPath.c_str(), quarantinePath.c_str(), MOVEFILE_REPLACE_EXISTING))
                                    error = std::error_code(int(GetLastError()), std::system_category());
#else
                                std::filesystem::rename(targetPath, quarantinePath, error);
#endif
                            }
                            if (error) {
                                // 移动失败时此名称仍是本线程新建的空隔离文件。
                                QFile::remove(quarantineName);
                            } else {
                                identityError.clear();
                                const auto movedStatus = std::filesystem::symlink_status(quarantinePath, identityError);
                                const bool ownFile = !identityError && std::filesystem::is_regular_file(movedStatus)
                                    && std::filesystem::equivalent(sourcePath, quarantinePath, identityError) && !identityError;
                                if (ownFile) std::filesystem::remove(quarantinePath, error);
                                else {
                                    // 若移动捕获了外来替换，恢复也不能覆盖新的目标。
                                    // 恢复被再次竞争时保留隔离文件，绝不删除外来内容。
                                    if (std::filesystem::is_symlink(movedStatus)) {
                                        const auto linkTarget = std::filesystem::read_symlink(quarantinePath, error);
                                        if (!error) std::filesystem::create_symlink(linkTarget, targetPath, error);
                                    } else std::filesystem::create_hard_link(quarantinePath, targetPath, error);
                                    if (!error) std::filesystem::remove(quarantinePath, error);
                                }
                            }
                        }
                    }
                }
                if (stoppedDuringPublication || error) fail(QCoreApplication::translate("SerialFileTransferController", "Cannot publish the received file without overwriting. Choose another receiving folder or check its permissions: %1").arg(QString::fromStdString(error.message())));
                temporary.reset();
            }
            break;
        }
        { std::lock_guard<std::mutex> lock(_mutex); _busy = false; }
        if (!_stopped.load()) emit resultReady(result);
    }
}
