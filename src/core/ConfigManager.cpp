#include "ConfigManager.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QDebug>

ConfigManager& ConfigManager::instance()
{
    static ConfigManager mgr;
    return mgr;
}

QStringList ConfigManager::splitPath(const QString& path)
{
    return path.split('.', Qt::SkipEmptyParts);
}

QJsonValue ConfigManager::valueAt(const QString& path) const
{
    const QStringList keys = splitPath(path);
    QJsonValue cur = _root;
    for (const QString& key : keys) {
        if (!cur.isObject())
            return QJsonValue::Undefined;
        cur = cur[key];
    }
    return cur;
}

// 辅助函数：递归确保嵌套对象路径存在，然后设置叶子节点的值。
// 返回修改后的对象（按值返回 —— QJsonObject 为隐式共享 / CoW）。
static QJsonObject setNested(QJsonObject obj, const QStringList& keys, int depth,
                              const QVariant& value)
{
    if (depth >= keys.size())
        return obj;

    const QString& k = keys[depth];
    if (depth == keys.size() - 1) {
        // 叶子节点 —— 设置值
        obj[k] = QJsonValue::fromVariant(value);
    } else {
        // 中间节点 —— 确保对象存在，递归处理
        QJsonObject child = obj[k].toObject();  // 缺失或非对象时返回空 {}
        child = setNested(child, keys, depth + 1, value);
        obj[k] = child;
    }
    return obj;
}

void ConfigManager::setValueAt(const QString& path, const QVariant& value)
{
    const QStringList keys = splitPath(path);
    if (keys.isEmpty())
        return;
    _root = setNested(_root, keys, 0, value);
}

void ConfigManager::applyDefaults(const QJsonObject& def, QJsonObject& target)
{
    for (auto it = def.begin(); it != def.end(); ++it) {
        if (!target.contains(it.key())) {
            // 键缺失 — 从默认值填充
            target[it.key()] = it.value();
        } else if (it.value().isObject() && target[it.key()].isObject()) {
            // 递归处理嵌套对象
            QJsonObject sub = target[it.key()].toObject();
            applyDefaults(it.value().toObject(), sub);
            target[it.key()] = sub;
        }
        // 否则已有非对象值 — 保留不变
    }
}

QJsonObject ConfigManager::defaults()
{
    return QJsonObject{
        {"ui", QJsonObject{
            {"language", "zh_CN"},
            {"theme", "auto"},
            {"animation", true}
        }},
        {"terminal", QJsonObject{
            {"fontFamily", "Cascadia Code"},
            {"fontSize", 12},
            {"colorScheme", "system"},
            {"scrollbackLines", 10000}
        }},
        {"window", QJsonObject{
            {"width", 1280},
            {"height", 800},
            {"maximized", false}
        }}
    };
}

void ConfigManager::load()
{
    if (_loaded)
        return;
    _loaded = true;

    // 配置文件位于可执行文件同目录
    _filePath = QCoreApplication::applicationDirPath() + "/novaterm.json";

    QFile file(_filePath);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray raw = file.readAll();
        file.close();

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            _root = doc.object();
            qDebug() << "配置已加载：" << _filePath;
        } else {
            qWarning() << "配置解析错误：" << err.errorString() << "— 使用默认值";
            _root = QJsonObject{};
        }
    } else {
        qDebug() << "未找到配置文件 — 使用默认值创建";
        _root = QJsonObject{};
    }

    // 用内置默认值填充所有缺失的键，然后持久化
    QJsonObject def = defaults();
    applyDefaults(def, _root);
    save();
}

void ConfigManager::save()
{
    if (_filePath.isEmpty())
        return;

    QFile file(_filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonDocument doc(_root);
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
    } else {
        qWarning() << "写入配置失败：" << _filePath;
    }
}

void ConfigManager::set(const QString& path, const QVariant& value)
{
    instance().setValueAt(path, value);
    instance().save();
    emit instance().configChanged(path);
}
