#pragma once
#include <QString>
#include <QVector>
#include <QMetaType>

/** 远端目录中的一个条目。 */
struct SftpFileInfo
{
    QString name;
    QString path;
    QString linkTarget;
    quint64 size{0};
    qint64 modifiedSeconds{0};
    quint32 permissions{0};
    bool directory{false};
    bool symbolicLink{false};
    bool hardLink{false};
    bool linkTargetDirectory{false};
    bool brokenSymbolicLink{false};
};

struct SftpDirectoryResult
{
    QString path;
    QVector<SftpFileInfo> entries;
    QString error;
    bool truncated{false};
};
Q_DECLARE_METATYPE(SftpDirectoryResult)
