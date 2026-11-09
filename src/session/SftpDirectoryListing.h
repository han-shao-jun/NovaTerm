#pragma once
#include <QString>
#include <QVector>
#include <QMetaType>
#include <QDir>
#include <QFileInfo>
#include <optional>

/** @brief 远端名称只能是单个可安全用于本机路径的文件名。 */
inline bool isSafeSftpEntryName(const QString& name)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
        || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    for (const auto ch : name)
        if (ch.unicode() < 32 || ch.unicode() == 127) return false;
#ifdef Q_OS_WIN
    for (const auto ch : QStringLiteral(":<>\"|?*"))
        if (name.contains(ch)) return false;
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) return false;
    const auto stem = name.section(QLatin1Char('.'), 0, 0).toUpper();
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    if (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT"))
        && (QStringLiteral("123456789¹²³").contains(stem.back()))) return false;
#endif
    return true;
}

/** @brief 拼接下载子路径并复核词法边界，不接受绝对路径或目录跳转。 */
inline std::optional<QString> sftpDownloadChildPath(const QString& root, const QString& relative)
{
    const auto components = relative.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const auto& component : components)
        if (!isSafeSftpEntryName(component)) return std::nullopt;
    const QDir directory(root);
    const QString result = QDir::cleanPath(directory.absoluteFilePath(relative));
    const QString relativeResult = directory.relativeFilePath(result);
    if (QDir::isAbsolutePath(relativeResult) || relativeResult == QStringLiteral("..")
        || relativeResult.startsWith(QStringLiteral("../"))) return std::nullopt;
    return result;
}

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
