/**
 * @file   SessionStore.h
 * @brief  会话恢复元数据持久化。
 *
 * 将可恢复会话列表（SessionRestoreMetadata）序列化为 JSON 文件，
 * 供应用重启后恢复会话列表。写入时拒绝包含敏感数据（凭据）的元数据。
 */
#pragma once

#include "SessionTypes.h"

#include <QList>
#include <QString>

/**
 * @brief 会话恢复存储：JSON 文件读写。
 *
 * 使用 QSaveFile 原子写入，避免崩溃导致文件损坏。读写均为同步操作，
 * 由上层在合适线程调用。
 */
class SessionStore
{
public:
    explicit SessionStore(QString filePath);

    /**
     * @brief 保存会话恢复列表到 JSON 文件。
     * @param sessions 待持久化的会话元数据列表。
     * @param error    错误信息输出（可选）。
     * @return true 表示保存成功；false 表示失败（含敏感数据或 I/O 错误）。
     */
    bool save(const QList<SessionRestoreMetadata>& sessions,
              QString* error = nullptr) const;

    /**
     * @brief 从 JSON 文件加载会话恢复列表。
     * @param error 错误信息输出（可选）。
     * @return 会话元数据列表（文件不存在或解析失败时返回空列表）。
     */
    [[nodiscard]] QList<SessionRestoreMetadata>
    load(QString* error = nullptr) const;

    /**
     * @brief 获取存储文件路径。
     * @return 文件路径常引用。
     */
    [[nodiscard]] const QString& filePath() const noexcept { return _filePath; }

private:
    QString _filePath;
};
