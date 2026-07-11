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
    static ConfigManager& instance();

    // 从磁盘读取，用内置默认值填充缺失键，回写保存。
    void load();

    // 将当前状态写入磁盘。
    void save();

    // ── 类型化 getter（键缺失或类型错误时返回 defaultValue）──
    template <typename T>
    static T get(const QString& path, const T& defaultValue = T{});

    // ── 设置值，自动保存 ──
    static void set(const QString& path, const QVariant& value);

    // ── 原始访问 ──
    QJsonObject root() const { return _root; }

    // ── 内置默认值（公开以便调用方引用）──
    static QJsonObject defaults();

signals:
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
