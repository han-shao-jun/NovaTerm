/**
 * @file TerminalSchemeStore.h
 * @brief 命名终端配色的解析、合并、引用维护与旧配置迁移。
 */
#pragma once

#include "renderer/TerminalColorScheme.h"

#include <QJsonObject>
#include <QStringList>
#include <QVector>
#include <optional>

/**
 * @brief 服务层配色仓库，使用 Windows Terminal 的 schemes 字段格式。
 * @note 所有操作针对传入的配置值；持久化由 ConfigManager 统一负责。
 *       Renderer 仅接收解析后的 TerminalColorScheme，不接触 JSON。
 */
class TerminalSchemeStore final
{
public:
    [[nodiscard]] static QVector<TerminalColorScheme> builtins();
    /** @brief 补齐完整可持久化方案库，保留已保存的颜色和扩展字段。 */
    static void materialize(QJsonObject& config);
    [[nodiscard]] static bool isBuiltin(const QString& name);
    /** @brief 按终端背景亮度归类，与应用窗口主题无关。 */
    [[nodiscard]] static bool isDark(const TerminalColorScheme& scheme);
    [[nodiscard]] static QVector<TerminalColorScheme> schemes(const QJsonObject& config);
    [[nodiscard]] static std::optional<TerminalColorScheme>
    find(const QJsonObject& config, const QString& name);

    /** @brief 解析完整 ANSI 16 色方案，错误定位到字段。 */
    [[nodiscard]] static std::optional<TerminalColorScheme>
    fromJson(const QJsonObject& json, QString* error = nullptr);
    [[nodiscard]] static QJsonObject toJson(const TerminalColorScheme& scheme);

    /** @brief 按终端自身深浅分类解析引用，不读取应用主题。 */
    [[nodiscard]] static TerminalColorScheme resolve(
        const QJsonObject& config, bool dark, QString* error = nullptr);

    /** @brief 保存用户方案；重命名时同步维护全局深浅配色引用。 */
    static bool save(QJsonObject& config, const TerminalColorScheme& scheme,
                     const QString& previousName = {}, QString* error = nullptr);
    /** @brief 删除用户方案；同名内置覆盖回退到内置，其他引用回退到默认。 */
    static bool remove(QJsonObject& config, const QString& name);
    /** @brief 将旧 terminal.colors 迁移为命名方案，保留已有外观。 */
    static void migrate(QJsonObject& config);
};
