#include "ConfigManager.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDebug>
#include <QSaveFile>

#include <cmath>

namespace {

constexpr qint64 MaximumConfigFileSize = 1024 * 1024;
constexpr qsizetype MaximumDockStateTextSize = 256 * 1024;

bool validateKnownValueTypes(const QJsonObject& schema,
                             QJsonObject& target)
{
    bool repaired = false;
    for (auto it = schema.constBegin(); it != schema.constEnd(); ++it) {
        if (!target.contains(it.key()))
            continue;

        const QJsonValue expected = it.value();
        const QJsonValue actual = target.value(it.key());
        if (expected.isObject() && actual.isObject()) {
            QJsonObject child = actual.toObject();
            repaired |= validateKnownValueTypes(expected.toObject(), child);
            target[it.key()] = child;
            continue;
        }

        if (expected.isArray() && actual.isArray()) {
            const QJsonArray expectedArray = expected.toArray();
            const QJsonArray actualArray = actual.toArray();
            bool arrayValid = expectedArray.size() == actualArray.size();
            for (qsizetype index = 0;
                 arrayValid && index < expectedArray.size(); ++index) {
                arrayValid = expectedArray.at(index).type()
                    == actualArray.at(index).type();
            }
            if (!arrayValid) {
                target[it.key()] = expected;
                repaired = true;
            }
            continue;
        }

        // 已知字段必须与默认配置具有相同 JSON 类型，避免字符串被宽松
        // 转换成数字或布尔值后产生不可预期的窗口和终端参数。
        if (expected.type() != actual.type()) {
            target[it.key()] = expected;
            repaired = true;
        }
    }
    return repaired;
}

bool repairIntegerRange(QJsonObject& object, const QString& key,
                        int minimum, int maximum, int defaultValue)
{
    const double number = object.value(key).toDouble();
    if (std::isfinite(number) && std::floor(number) == number
        && number >= minimum && number <= maximum) {
        return false;
    }

    object[key] = defaultValue;
    return true;
}

bool validateKnownValueRanges(QJsonObject& root,
                              const QJsonObject& defaults)
{
    bool repaired = false;

    QJsonObject ui = root.value(QStringLiteral("ui")).toObject();
    const QJsonObject defaultUi = defaults.value(
        QStringLiteral("ui")).toObject();
    const QString language = ui.value(QStringLiteral("language")).toString();
    if (language != QStringLiteral("en")
        && language != QStringLiteral("zh_CN")) {
        ui[QStringLiteral("language")] = defaultUi.value(
            QStringLiteral("language"));
        repaired = true;
    }
    const QString theme = ui.value(QStringLiteral("theme")).toString();
    if (theme != QStringLiteral("auto")
        && theme != QStringLiteral("light")
        && theme != QStringLiteral("dark")) {
        ui[QStringLiteral("theme")] = defaultUi.value(
            QStringLiteral("theme"));
        repaired = true;
    }
    root[QStringLiteral("ui")] = ui;

    QJsonObject terminal = root.value(QStringLiteral("terminal")).toObject();
    const QJsonObject defaultTerminal = defaults.value(
        QStringLiteral("terminal")).toObject();
    repaired |= repairIntegerRange(
        terminal, QStringLiteral("fontSize"), 6, 96,
        defaultTerminal.value(QStringLiteral("fontSize")).toInt());
    repaired |= repairIntegerRange(
        terminal, QStringLiteral("scrollbackLines"), 100, 1000000,
        defaultTerminal.value(QStringLiteral("scrollbackLines")).toInt());
    if (terminal.value(QStringLiteral("fontFamily")).toString().trimmed()
        .isEmpty()) {
        terminal[QStringLiteral("fontFamily")] = defaultTerminal.value(
            QStringLiteral("fontFamily"));
        repaired = true;
    }
    root[QStringLiteral("terminal")] = terminal;

    QJsonObject window = root.value(QStringLiteral("window")).toObject();
    const QJsonObject defaultWindow = defaults.value(
        QStringLiteral("window")).toObject();
    repaired |= repairIntegerRange(
        window, QStringLiteral("width"), 640, 16384,
        defaultWindow.value(QStringLiteral("width")).toInt());
    repaired |= repairIntegerRange(
        window, QStringLiteral("height"), 480, 16384,
        defaultWindow.value(QStringLiteral("height")).toInt());
    repaired |= repairIntegerRange(
        window, QStringLiteral("sessionPanelExpandedWidth"), 160, 4096,
        defaultWindow.value(
            QStringLiteral("sessionPanelExpandedWidth")).toInt());
    if (window.value(QStringLiteral("dockState")).toString().size()
        > MaximumDockStateTextSize) {
        window[QStringLiteral("dockState")] = QString{};
        repaired = true;
    }
    root[QStringLiteral("window")] = window;

    return repaired;
}

} // namespace

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
            {"colorScheme", "windowsTerminalCampbell"},
            {"colors", QJsonObject{
                {"foreground", "#CCCCCC"},
                {"background", "#0C0C0C"},
                {"cursor", "#FFFFFF"},
                {"selection", "#40FFFFFF"},
                {"palette", QJsonArray{
                    "#0C0C0C", "#C50F1F", "#13A10E", "#C19C00",
                    "#0037DA", "#881798", "#3A96DD", "#CCCCCC",
                    "#767676", "#E74856", "#16C60C", "#F9F1A5",
                    "#3B78FF", "#B4009E", "#61D6D6", "#F2F2F2"
                }}
            }},
            {"scrollbackLines", 10000}
        }},
        {"window", QJsonObject{
            {"width", 1280},
            {"height", 800},
            {"maximized", false},
            {"dockState", ""},
            {"sessionPanelCollapsed", false},
            {"sessionPanelExpandedWidth", 260}
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
        QByteArray raw;
        if (file.size() <= MaximumConfigFileSize)
            raw = file.readAll();
        file.close();

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (!raw.isEmpty() && err.error == QJsonParseError::NoError
            && doc.isObject()) {
            _root = doc.object();
            qDebug() << "配置已加载：" << _filePath;
        } else {
            qWarning() << "配置文件为空、过大或解析失败："
                       << err.errorString() << "— 使用默认值";
            _root = QJsonObject{};
        }
    } else {
        qDebug() << "未找到配置文件 — 使用默认值创建";
        _root = QJsonObject{};
    }

    // 先修复已知字段的类型，再填充缺失字段并校验关键取值范围。
    // 未识别的扩展字段原样保留，兼顾向前兼容与损坏配置恢复。
    const QJsonObject def = defaults();
    bool repaired = validateKnownValueTypes(def, _root);
    applyDefaults(def, _root);
    repaired |= validateKnownValueRanges(_root, def);
    if (repaired)
        qWarning() << "配置包含无效字段，已使用安全默认值修复";
    save();
}

void ConfigManager::save()
{
    if (_filePath.isEmpty())
        return;

    // QSaveFile 先写临时文件再原子替换，避免断电或异常退出留下半个 JSON。
    QSaveFile file(_filePath);
    if (file.open(QIODevice::WriteOnly)) {
        const QByteArray data = QJsonDocument(_root).toJson(
            QJsonDocument::Indented);
        if (file.write(data) != data.size() || !file.commit())
            qWarning() << "提交配置失败：" << _filePath;
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

void ConfigManager::setValues(const QVariantMap& values)
{
    ConfigManager& manager = instance();
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
        manager.setValueAt(it.key(), it.value());

    // 一组相关状态只落盘一次，保证关闭时保存的窗口和面板布局相互一致。
    manager.save();
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
        emit manager.configChanged(it.key());
}
