/**
 * @file   ConfigManager.h
 * @brief  持久化 JSON 配置管理（单例）。
 *
 * 读写可执行文件同目录下的 novaterm.json。通过点分隔的键路径
 * （如 "ui.language"）公开嵌套键，并提供带默认值回退的类型化 getter。
 */
#pragma once
#include <QObject>
#include <QJsonObject>
#include <QVariant>
#include <QString>

// 持久化 JSON 配置（单例）。读写可执行文件同目录下的 novaterm.json。
// 通过点分隔的键路径（如 "ui.language"）公开嵌套键，并提供带默认值回退的
// 类型化 getter。
//
// 用法：
//   ConfigManager::instance().load();                          // 启动时调用一次
//   QString lang = ConfigManager::get<QString>("ui.language");
//   ConfigManager::set("ui.theme", "dark");                    // 自动保存

class ConfigManager : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 获取单例实例。
     * @return 配置管理器单例引用。
     */
    static ConfigManager& instance();

    /**
     * @brief 从磁盘读取配置，用内置默认值填充缺失键，并回写保存。
     * @note 仅首次调用生效（内部 _loaded 标志）。
     */
    void load();

    /**
     * @brief 将当前配置状态写入磁盘。
     */
    void save();

    // ── 类型化 getter（键缺失或类型错误时返回 defaultValue）──
    /**
     * @brief 按点分隔键路径读取类型化值。
     * @tparam T 目标类型（如 QString、int、bool）。
     * @param path         键路径（如 "ui.language"）。
     * @param defaultValue 键缺失或类型不匹配时的回退值。
     * @return 读取到的值或默认值。
     */
    template <typename T>
    static T get(const QString& path, const T& defaultValue = T{});

    // ── 设置值，自动保存 ──
    /**
     * @brief 设置键路径的值并自动保存。
     * @param path  键路径。
     * @param value 待设置的值。
     * @note 设置后立即写盘并发 configChanged 信号。
     */
    static void set(const QString& path, const QVariant& value);

    // ── 原始访问 ──
    /**
     * @brief 获取原始 JSON 根对象。
     * @return 根对象（含所有已合并默认值的键）。
     */
    QJsonObject root() const { return _root; }

    // ── 内置默认值（公开以便调用方引用）──
    /**
     * @brief 获取内置默认配置。
     * @return 默认 JSON 对象（ui/terminal/window 三大分组）。
     */
    static QJsonObject defaults();

signals:
    /**
     * @brief 配置项变更。
     * @param path 变更的键路径。
     */
    void configChanged(const QString& path);

private:
    ConfigManager() = default;

    // 将 "ui.language" 分解为 _root["ui"]["language"]，
    // 当 createMissing 为 true 时创建中间对象。
    static QStringList splitPath(const QString& path);
    QJsonValue valueAt(const QString& path) const;
    void setValueAt(const QString& path, const QVariant& value);

    QJsonObject _root;
    QString _filePath;
    bool _loaded = false;

    // 将默认值合并到 _root：_root 中缺失的键从 def 填充。
    void applyDefaults(const QJsonObject& def, QJsonObject& target);
};

// ── 模板实现（必须在头文件中）──

template <typename T>
T ConfigManager::get(const QString& path, const T& defaultValue)
{
    QVariant v = instance().valueAt(path).toVariant();
    if (!v.isValid() || !v.canConvert<T>())
        return defaultValue;
    return v.value<T>();
}
