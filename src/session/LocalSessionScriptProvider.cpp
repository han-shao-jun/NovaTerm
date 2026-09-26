/** @file LocalSessionScriptProvider.cpp
 *  @brief LocalShell 上的原子脚本文件写入。
 */
#include "LocalSessionScriptProvider.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QMetaObject>
#include <QSaveFile>

namespace {
constexpr qsizetype MaxScriptBytes = 2 * 1024 * 1024;
}

LocalSessionScriptProvider::LocalSessionScriptProvider(QObject* parent)
    : ISessionScriptProvider(parent)
{
}

QString LocalSessionScriptProvider::resolvePath(
    const ScriptWriteRequest& request)
{
    const QString target = request.targetPath.trimmed();
    const QString directory = request.workingDirectory.trimmed();
    if (target.isEmpty() || directory.isEmpty())
        return {};
    const QFileInfo targetInfo(target);
    const QString path = targetInfo.isAbsolute()
        ? targetInfo.absoluteFilePath()
        : QDir(directory).absoluteFilePath(target);
    return QDir::cleanPath(path);
}

bool LocalSessionScriptProvider::writeScript(
    const ScriptWriteRequest& request)
{
    const QString target = resolvePath(request);
    if (request.requestId == 0 || request.content.isEmpty()
        || request.content.size() > MaxScriptBytes || target.isEmpty()) {
        return false;
    }

    ScriptWriteResult result;
    result.requestId = request.requestId;
    result.resolvedTargetPath = target;
    result.contentHash = QCryptographicHash::hash(request.content,
                                                   QCryptographicHash::Sha256);
    QSaveFile file(target);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        result.errorCode = QStringLiteral("SCRIPT_WRITE_FAILED");
    } else if (!file.setPermissions(QFileDevice::ReadOwner
                                   | QFileDevice::WriteOwner
                                   | QFileDevice::ExeOwner)) {
        result.errorCode = QStringLiteral("SCRIPT_WRITE_FAILED");
        file.cancelWriting();
    } else if (file.write(request.content) != request.content.size()) {
        result.errorCode = QStringLiteral("SCRIPT_WRITE_FAILED");
        file.cancelWriting();
    } else if (!file.commit()) {
        result.errorCode = QStringLiteral("SCRIPT_WRITE_FAILED");
    } else {
        result.success = true;
    }

    QMetaObject::invokeMethod(this, [this, result] { emit finished(result); },
                              Qt::QueuedConnection);
    return true;
}

void LocalSessionScriptProvider::cancelWrite(quint64 requestId)
{
    Q_UNUSED(requestId);
    // 本地原子写入在 GUI 线程中短暂完成，排队的完成通知由 MCP 取消路径忽略。
}
