/**
 * @file TerminalSchemeStore.cpp
 * @brief Windows Terminal 格式的配色数据与配置解析。
 */
#include "TerminalSchemeStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {

constexpr const char* PaletteKeys[] = {
    "black", "red", "green", "yellow", "blue", "purple", "cyan", "white",
    "brightBlack", "brightRed", "brightGreen", "brightYellow",
    "brightBlue", "brightPurple", "brightCyan", "brightWhite"
};

bool fail(QString* error, const QString& message)
{
    if (error)
        *error = message;
    return false;
}

std::optional<QColor> parseColor(const QJsonValue& value, bool alpha = false)
{
    static const QRegularExpression rgb(QStringLiteral("^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$"));
    static const QRegularExpression argb(QStringLiteral("^#[0-9a-fA-F]{8}$"));
    if (!value.isString())
        return std::nullopt;
    const QString text = value.toString();
    if (!rgb.match(text).hasMatch() && !(alpha && argb.match(text).hasMatch()))
        return std::nullopt;
    const QColor color(text);
    return color.isValid() ? std::optional<QColor>(color) : std::nullopt;
}


QString defaultName(bool dark)
{
    return dark ? QStringLiteral("Campbell") : QStringLiteral("One Half Light");
}

void replaceReferences(QJsonObject& config, const QString& oldName,
                       const QString& newName)
{
    auto terminal = config.value(QStringLiteral("terminal")).toObject();
    auto reference = terminal.value(QStringLiteral("colorScheme"));
    if (reference.isString() && reference.toString() == oldName) {
        reference = newName.isEmpty() ? defaultName(true) : newName;
    } else if (reference.isObject()) {
        auto pair = reference.toObject();
        for (const auto& key : {QStringLiteral("light"), QStringLiteral("dark")}) {
            if (pair.value(key).toString() == oldName)
                pair[key] = newName.isEmpty() ? defaultName(key == QStringLiteral("dark")) : newName;
        }
        reference = pair;
    }
    terminal[QStringLiteral("colorScheme")] = reference;
    config[QStringLiteral("terminal")] = terminal;
}

} // namespace

QVector<TerminalColorScheme> TerminalSchemeStore::builtins()
{
    // 内置 JSON 仅作为首次初始化与“恢复内置”的模板；运行配置保存完整方案库。
    static const QVector<TerminalColorScheme> presets = [] {
        QFile file(QStringLiteral(":/novaterm/terminal-color-schemes.json"));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Bundled terminal color schemes are missing");
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        const auto root = document.object();
        if (error.error != QJsonParseError::NoError
            || root.value(QStringLiteral("schemaVersion")).toInt() != 1)
            qFatal("Bundled terminal color schemes have an invalid schema");
        QVector<TerminalColorScheme> result;
        QSet<QString> names;
        for (const auto& value : root.value(QStringLiteral("schemes")).toArray()) {
            const auto scheme = fromJson(value.toObject());
            if (!scheme || names.contains(scheme->name))
                qFatal("Bundled terminal color scheme is invalid or duplicated");
            result.append(*scheme);
            names.insert(scheme->name);
        }
        if (result.isEmpty())
            qFatal("Bundled terminal color schemes are empty");
        return result;
    }();
    return presets;
}

void TerminalSchemeStore::materialize(QJsonObject& config)
{
    auto catalog = config.value(QStringLiteral("schemes")).toArray();
    QSet<QString> names;
    for (const auto& value : catalog) {
        if (const auto scheme = fromJson(value.toObject()))
            names.insert(scheme->name);
    }
    // 只补缺失模板，不覆盖磁盘里的有效颜色和未知扩展字段。
    for (const auto& scheme : builtins()) {
        if (!names.contains(scheme.name))
            catalog.append(toJson(scheme));
    }
    config[QStringLiteral("schemes")] = catalog;
}

bool TerminalSchemeStore::isBuiltin(const QString& name)
{
    const auto list = builtins();
    return std::any_of(list.cbegin(), list.cend(), [&name](const auto& item) {
        return item.name == name;
    });
}

bool TerminalSchemeStore::isDark(const TerminalColorScheme& scheme)
{
    // 用背景感知亮度分类；同一方案不会因为程序主题切换而跨分类。
    const auto& color = scheme.background;
    return 0.2126 * color.redF() + 0.7152 * color.greenF()
        + 0.0722 * color.blueF() < 0.5;
}

std::optional<TerminalColorScheme> TerminalSchemeStore::fromJson(
    const QJsonObject& json, QString* error)
{
    if (error)
        error->clear();
    auto result = TerminalColorScheme::windowsTerminalCampbell();
    result.name = json.value(QStringLiteral("name")).toString().trimmed();
    if (result.name.isEmpty() || result.name.size() > 128) {
        fail(error, QStringLiteral("name: expected 1–128 characters"));
        return std::nullopt;
    }
    for (int i = 0; i < 16; ++i) {
        const QString key = QString::fromLatin1(PaletteKeys[i]);
        auto value = json.value(key);
        if (value.isUndefined() && (i == 5 || i == 13))
            value = json.value(i == 5 ? QStringLiteral("magenta") : QStringLiteral("brightMagenta"));
        const auto color = parseColor(value);
        if (!color) {
            fail(error, key + QStringLiteral(": expected #RGB or #RRGGBB"));
            return std::nullopt;
        }
        result.palette[i] = *color;
    }
    const std::pair<const char*, QColor*> roles[] = {
        {"foreground", &result.foreground}, {"background", &result.background},
        {"cursorColor", &result.cursorColor}, {"selectionBackground", &result.selectionColor}
    };
    for (const auto& role : roles) {
        const QString key = QString::fromLatin1(role.first);
        if (!json.contains(key))
            continue;
        const bool selection = key == QStringLiteral("selectionBackground");
        const auto color = parseColor(json.value(key), selection);
        if (!color) {
            fail(error, key + QStringLiteral(": invalid hexadecimal color"));
            return std::nullopt;
        }
        *role.second = *color;
        // 选区在文字上叠加；WT 的 RGB 选区值映射为 25% 覆盖，避免遮住文字。
        // 八位 ARGB 是兼容 NovaTerm 旧配置的扩展，显式 alpha 原样保留。
        if (selection && json.value(key).toString().size() != 9)
            role.second->setAlpha(64);
    }
    return result;
}

QJsonObject TerminalSchemeStore::toJson(const TerminalColorScheme& scheme)
{
    QJsonObject json{
        {QStringLiteral("name"), scheme.name.trimmed()},
        {QStringLiteral("foreground"), scheme.foreground.name()},
        {QStringLiteral("background"), scheme.background.name()},
        {QStringLiteral("cursorColor"), scheme.cursorColor.name()},
        {QStringLiteral("selectionBackground"), scheme.selectionColor.name(
            scheme.selectionColor.alpha() == 64 ? QColor::HexRgb : QColor::HexArgb)}
    };
    for (int i = 0; i < 16; ++i)
        json[QString::fromLatin1(PaletteKeys[i])] = scheme.palette[i].name();
    return json;
}

QVector<TerminalColorScheme> TerminalSchemeStore::schemes(const QJsonObject& config)
{
    auto result = builtins();
    const auto users = config.value(QStringLiteral("schemes")).toArray();
    for (const auto& value : users) {
        const auto scheme = fromJson(value.toObject());
        if (!scheme)
            continue;
        auto found = std::find_if(result.begin(), result.end(), [&scheme](const auto& item) {
            return item.name == scheme->name;
        });
        if (found == result.end())
            result.append(*scheme);
        else
            *found = *scheme;
    }
    return result;
}

std::optional<TerminalColorScheme> TerminalSchemeStore::find(
    const QJsonObject& config, const QString& name)
{
    const auto list = schemes(config);
    const auto found = std::find_if(list.cbegin(), list.cend(), [&name](const auto& item) {
        return item.name == name;
    });
    return found == list.cend() ? std::nullopt : std::optional<TerminalColorScheme>(*found);
}

TerminalColorScheme TerminalSchemeStore::resolve(
    const QJsonObject& config, bool dark, QString* error)
{
    if (error)
        error->clear();
    const auto reference = config.value(QStringLiteral("terminal")).toObject()
        .value(QStringLiteral("colorScheme"));
    const QString name = reference.isObject()
        ? reference.toObject().value(dark ? QStringLiteral("dark") : QStringLiteral("light")).toString()
        : reference.toString();
    if (const auto found = find(config, name))
        return *found;
    if (!name.isEmpty())
        fail(error, QStringLiteral("terminal.colorScheme: unknown scheme '%1'").arg(name));
    // 固定方案缺失统一回退 Campbell，不随应用主题悄悄变色。
    return *find({}, defaultName(reference.isObject() ? dark : true));
}

bool TerminalSchemeStore::save(QJsonObject& config, const TerminalColorScheme& scheme,
                               const QString& previousName, QString* error)
{
    if (!scheme.foreground.isValid() || !scheme.background.isValid()
        || !scheme.cursorColor.isValid() || !scheme.selectionColor.isValid())
        return fail(error, QStringLiteral("scheme: invalid color"));
    for (const auto& color : scheme.palette) {
        if (!color.isValid())
            return fail(error, QStringLiteral("scheme: invalid palette color"));
    }
    const auto json = toJson(scheme);
    if (!fromJson(json, error))
        return false;
    const QString name = json.value(QStringLiteral("name")).toString();
    if (name != previousName && find(config, name))
        return fail(error, QStringLiteral("name: a scheme with this name already exists"));
    auto users = config.value(QStringLiteral("schemes")).toArray();
    QJsonArray updated;
    QJsonObject previous;
    for (const auto& value : users) {
        const auto item = value.toObject();
        if (item.value(QStringLiteral("name")).toString() == previousName)
            previous = item;
        else
            updated.append(value);
    }
    // 编辑时保留未知扩展字段。
    for (auto it = json.constBegin(); it != json.constEnd(); ++it)
        previous[it.key()] = it.value();
    updated.append(previous);
    config[QStringLiteral("schemes")] = updated;
    if (!previousName.isEmpty() && previousName != name)
        replaceReferences(config, previousName, name);
    return true;
}

bool TerminalSchemeStore::remove(QJsonObject& config, const QString& name)
{
    const auto users = config.value(QStringLiteral("schemes")).toArray();
    QJsonArray updated;
    for (const auto& value : users) {
        if (value.toObject().value(QStringLiteral("name")).toString() != name)
            updated.append(value);
    }
    if (updated.size() == users.size())
        return false;
    config[QStringLiteral("schemes")] = updated;
    if (!isBuiltin(name))
        replaceReferences(config, name, {});
    return true;
}

void TerminalSchemeStore::migrate(QJsonObject& config)
{
    auto terminal = config.value(QStringLiteral("terminal")).toObject();
    const auto reference = terminal.value(QStringLiteral("colorScheme"));
    const QString oldName = reference.toString();
    if (oldName.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0) {
        terminal[QStringLiteral("colorScheme")] = QJsonObject{
            {QStringLiteral("light"), QStringLiteral("Black on White")},
            {QStringLiteral("dark"), QStringLiteral("Campbell")}
        };
    } else if (terminal.contains(QStringLiteral("colors"))) {
        auto legacy = TerminalColorScheme::windowsTerminalCampbell();
        const auto colors = terminal.value(QStringLiteral("colors")).toObject();
        const std::pair<const char*, QColor*> roles[] = {
            {"foreground", &legacy.foreground}, {"background", &legacy.background},
            {"cursor", &legacy.cursorColor}, {"selection", &legacy.selectionColor}
        };
        for (const auto& role : roles) {
            const QColor color(colors.value(QString::fromLatin1(role.first)).toString());
            if (color.isValid())
                *role.second = color;
        }
        const auto palette = colors.value(QStringLiteral("palette")).toArray();
        if (palette.size() == 16) {
            for (int i = 0; i < 16; ++i) {
                const QColor color(palette.at(i).toString());
                if (color.isValid())
                    legacy.palette[i] = color;
            }
        }
        legacy.name = QStringLiteral("Campbell");
        if (toJson(legacy) == toJson(*find(config, legacy.name))) {
            terminal[QStringLiteral("colorScheme")] = legacy.name;
        } else {
            QString base = oldName;
            if (base.isEmpty() || base == QStringLiteral("windowsTerminalCampbell"))
                base = QStringLiteral("Imported colors");
            legacy.name = base.left(110);
            int suffix = 2;
            while (find(config, legacy.name))
                legacy.name = base.left(110) + QStringLiteral(" (%1)").arg(suffix++);
            save(config, legacy);
            terminal[QStringLiteral("colorScheme")] = legacy.name;
        }
    } else if (oldName == QStringLiteral("windowsTerminalCampbell")) {
        terminal[QStringLiteral("colorScheme")] = QStringLiteral("Campbell");
    }
    terminal.remove(QStringLiteral("colors"));
    if (!terminal.contains(QStringLiteral("appearance"))) {
        config[QStringLiteral("terminal")] = terminal;
        const auto referenceName = terminal.value(QStringLiteral("colorScheme")).toString();
        const auto selected = find(config, referenceName);
        terminal[QStringLiteral("appearance")] = selected && !isDark(*selected)
            ? QStringLiteral("light") : QStringLiteral("dark");
    }
    config[QStringLiteral("terminal")] = terminal;
}
