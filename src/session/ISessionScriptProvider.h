/** @file ISessionScriptProvider.h
 *  @brief 向当前 Session 的本机或远端主机写入有界脚本字节。
 */
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

/** @brief 写入目标主机的脚本请求。 */
struct ScriptWriteRequest
{
    quint64 requestId{0};
    QByteArray content;
    QString targetPath;
    QString workingDirectory;
};

/** @brief 脚本写入完成结果；不含脚本正文或凭据。 */
struct ScriptWriteResult
{
    quint64 requestId{0};
    bool success{false};
    QString errorCode;
    QString resolvedTargetPath;
    QByteArray contentHash;
};

Q_DECLARE_METATYPE(ScriptWriteResult)

/** @brief Session 级目标主机脚本写入接口。 */
class ISessionScriptProvider : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~ISessionScriptProvider() override = default;

    [[nodiscard]] virtual bool isAvailable() const = 0;
    /** @brief 接受一个有界异步写入；false 表示没有接收该请求。 */
    virtual bool writeScript(const ScriptWriteRequest& request) = 0;
    /** @brief 尝试取消尚未完成的写入。 */
    virtual void cancelWrite(quint64 requestId) = 0;

signals:
    void finished(const ScriptWriteResult& result);
};
