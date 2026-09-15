/**
 * @file TerminalSchemeSettings.cpp
 * @brief Ela 风格配色编辑器，支持终端深浅分类、命名方案和保存后热更新。
 */
#include "TerminalSchemeSettings.h"

#include "ElaColorDialog.h"
#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "service/ConfigManager.h"
#include "service/LanguageManager.h"
#include "service/TerminalSchemeStore.h"

#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

QColor& schemeColor(TerminalColorScheme& scheme, int index)
{
    switch (index) {
    case 0: return scheme.foreground;
    case 1: return scheme.background;
    case 2: return scheme.cursorColor;
    case 3: return scheme.selectionColor;
    default: return scheme.palette[index - 4];
    }
}

bool hasUserScheme(const QJsonObject& config, const QString& name)
{
    for (const auto& builtin : TerminalSchemeStore::builtins()) {
        if (builtin.name == name) {
            const auto current = TerminalSchemeStore::find(config, name);
            return current && TerminalSchemeStore::toJson(*current)
                != TerminalSchemeStore::toJson(builtin);
        }
    }
    const auto users = config.value(QStringLiteral("schemes")).toArray();
    for (const auto& value : users) {
        if (value.toObject().value(QStringLiteral("name")).toString() == name)
            return true;
    }
    return false;
}

} // namespace

/** @brief 无进程、无 GPU 资源的静态终端预览，使用实际方案颜色。 */
class TerminalSchemePreview final : public QWidget
{
public:
    explicit TerminalSchemePreview(QWidget* parent) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumHeight(190);
    }

    void setScheme(const TerminalColorScheme& scheme)
    {
        _scheme = scheme;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), 8, 8);
        painter.setClipPath(clip);
        painter.fillRect(rect(), _scheme.background);
        auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
                          QStringLiteral("Menlo"), QStringLiteral("DejaVu Sans Mono")});
        font.setPixelSize(14);
        painter.setFont(font);
        const int line = painter.fontMetrics().height() + 5;
        int y = 16 + painter.fontMetrics().ascent();
        painter.setPen(_scheme.palette[2]);
        painter.drawText(16, y, QStringLiteral("user@novaterm"));
        const int promptWidth = painter.fontMetrics().horizontalAdvance(QStringLiteral("user@novaterm "));
        painter.setPen(_scheme.palette[4]);
        painter.drawText(16 + promptWidth, y, QStringLiteral("~/project"));
        y += line;
        painter.setPen(_scheme.foreground);
        painter.drawText(16, y, QStringLiteral("$ git status --short"));
        y += line;
        painter.setPen(_scheme.palette[2]);
        painter.drawText(16, y, QStringLiteral("A  src/main.cpp"));
        painter.setPen(_scheme.palette[1]);
        painter.drawText(width() / 2, y, QStringLiteral("M  README.md"));
        y += line;
        const QString selection = QStringLiteral("Selected text / 选中文本");
        painter.setPen(_scheme.foreground);
        painter.drawText(16, y, selection);
        painter.fillRect(QRect(14, y - painter.fontMetrics().ascent(),
            painter.fontMetrics().horizontalAdvance(selection) + 4, line - 4), _scheme.selectionColor);
        painter.fillRect(QRect(width() - 30, y - painter.fontMetrics().ascent(), 9, line - 4), _scheme.cursorColor);
        const int swatchWidth = qMax(1, (width() - 32) / 16);
        const int swatchY = qMax(y + 12, height() - 38);
        for (int i = 0; i < 16; ++i)
            painter.fillRect(16 + i * swatchWidth, swatchY, swatchWidth - 2, 22, _scheme.palette[i]);
    }

private:
    TerminalColorScheme _scheme{TerminalColorScheme::defaultDark()};
};

TerminalSchemeSettings::TerminalSchemeSettings(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    _title = new ElaText(this);
    _title->setTextPixelSize(18);
    _description = new ElaText(this);
    _description->setTextPixelSize(13);
    _description->setWordWrap(true);
    layout->addWidget(_title);
    layout->addWidget(_description);

    auto addRow = [this, layout](ElaText*& label, QWidget* control) {
        auto* row = new QWidget(this);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        label = new ElaText(row);
        label->setTextPixelSize(15);
        label->setWordWrap(true);
        rowLayout->addWidget(label, 1);
        rowLayout->addWidget(control, 1);
        layout->addWidget(row);
        return row;
    };
    _mode = new ElaComboBox(this);
    _mode->setObjectName(QStringLiteral("terminalSchemeMode"));
    _mode->addItems({QString{}, QString{}});
    addRow(_modeLabel, _mode);
    _dark = new ElaComboBox(this);
    _dark->setObjectName(QStringLiteral("terminalDarkScheme"));
    _darkRow = addRow(_darkLabel, _dark);
    _light = new ElaComboBox(this);
    _light->setObjectName(QStringLiteral("terminalLightScheme"));
    _lightRow = addRow(_lightLabel, _light);
    _preview = new TerminalSchemePreview(this);
    layout->addWidget(_preview);
    _name = new ElaLineEdit(this);
    _name->setObjectName(QStringLiteral("terminalSchemeName"));
    _name->setMaxLength(128);
    addRow(_nameLabel, _name);

    auto* actions = new QHBoxLayout;
    _duplicate = new ElaPushButton(this);
    _duplicate->setObjectName(QStringLiteral("duplicateTerminalScheme"));
    _delete = new ElaPushButton(this);
    _delete->setObjectName(QStringLiteral("deleteTerminalScheme"));
    actions->addWidget(_duplicate);
    actions->addWidget(_delete);
    actions->addStretch();
    layout->addLayout(actions);

    auto* colors = new QGridLayout;
    colors->setHorizontalSpacing(12);
    colors->setVerticalSpacing(10);
    for (int i = 0; i < 20; ++i) {
        auto* cell = new QVBoxLayout;
        auto* label = new ElaText(this);
        label->setTextPixelSize(13);
        label->setWordWrap(true);
        auto* button = new ElaPushButton(this);
        button->setMinimumHeight(32);
        button->setObjectName(QStringLiteral("terminalColor%1").arg(i));
        _colorLabels[i] = label;
        _colorButtons[i] = button;
        cell->addWidget(label);
        cell->addWidget(button);
        colors->addLayout(cell, i / 4, i % 4);
        colors->setColumnStretch(i % 4, 1);
        connect(button, &ElaPushButton::clicked, this, [this, i] { editColor(i); });
    }
    layout->addLayout(colors);
    _status = new ElaText(this);
    _status->setTextPixelSize(13);
    _status->setWordWrap(true);
    layout->addWidget(_status);
    auto* footer = new QHBoxLayout;
    _revert = new ElaPushButton(this);
    _revert->setObjectName(QStringLiteral("discardTerminalSchemeChanges"));
    _apply = new ElaPushButton(this);
    _apply->setObjectName(QStringLiteral("applyTerminalSchemes"));
    footer->addStretch();
    footer->addWidget(_revert);
    footer->addWidget(_apply);
    layout->addLayout(footer);

    connect(_mode, &ElaComboBox::currentIndexChanged, this, [this] { updateSelection(); });
    connect(_dark, &ElaComboBox::currentIndexChanged, this, [this] { updateSelection(); });
    connect(_light, &ElaComboBox::currentIndexChanged, this, [this] { updateSelection(); });
    connect(_name, &ElaLineEdit::editingFinished, this, [this] { renameScheme(); });
    connect(_duplicate, &ElaPushButton::clicked, this, &TerminalSchemeSettings::duplicateScheme);
    connect(_delete, &ElaPushButton::clicked, this, &TerminalSchemeSettings::deleteScheme);
    connect(_apply, &ElaPushButton::clicked, this, &TerminalSchemeSettings::apply);
    connect(_revert, &ElaPushButton::clicked, this, &TerminalSchemeSettings::reload);
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this] { retranslateUi(); });
    reload();
}

void TerminalSchemeSettings::reload()
{
    _draft = ConfigManager::instance().root();
    TerminalSchemeStore::migrate(_draft);
    const bool dark = _draft.value(QStringLiteral("terminal")).toObject()
        .value(QStringLiteral("appearance")).toString() != QStringLiteral("light");
    refreshLists(TerminalSchemeStore::resolve(_draft, dark).name);
    _apply->setEnabled(false);
    _revert->setEnabled(false);
    _status->clear();
}

void TerminalSchemeSettings::refreshLists(const QString& editingName)
{
    _refreshing = true;
    const auto reference = _draft.value(QStringLiteral("terminal")).toObject()
        .value(QStringLiteral("colorScheme"));
    const auto requested = TerminalSchemeStore::find(_draft, editingName);
    const bool dark = !requested || TerminalSchemeStore::isDark(*requested);
    _mode->setCurrentIndex(dark ? 0 : 1);
    const auto list = TerminalSchemeStore::schemes(_draft);
    for (auto* combo : {_dark, _light}) {
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (const auto& scheme : list) {
            if (TerminalSchemeStore::isDark(scheme) == (combo == _dark))
                combo->addItem(scheme.name);
        }
    }
    _dark->setCurrentText(TerminalSchemeStore::resolve(_draft, true).name);
    _light->setCurrentText(reference.isObject()
        ? TerminalSchemeStore::resolve(_draft, false).name : QStringLiteral("One Half Light"));
    (dark ? _dark : _light)->setCurrentText(editingName);
    _editingName = (dark ? _dark : _light)->currentText();
    _darkRow->setVisible(dark);
    _lightRow->setVisible(!dark);
    _refreshing = false;
    retranslateUi();
    refreshEditor();
}

void TerminalSchemeSettings::refreshEditor()
{
    auto scheme = TerminalSchemeStore::find(_draft, _editingName);
    if (!scheme)
        return;
    _name->setText(_editingName);
    _name->setReadOnly(TerminalSchemeStore::isBuiltin(_editingName));
    _delete->setEnabled(hasUserScheme(_draft, _editingName));
    _delete->setText(TerminalSchemeStore::isBuiltin(_editingName)
        ? tr("Restore built-in") : tr("Delete scheme"));
    _preview->setScheme(*scheme);
    for (int i = 0; i < 20; ++i) {
        QColor color = schemeColor(*scheme, i);
        // 按钮展示原始 RGB 色值，透明度效果在终端预览里呈现。
        color.setAlpha(255);
        auto* button = _colorButtons[i];
        button->setText(color.name().toUpper());
        button->setLightDefaultColor(color);
        button->setDarkDefaultColor(color);
        button->setLightHoverColor(color.lighter(115));
        button->setDarkHoverColor(color.lighter(115));
        button->setLightPressColor(color.darker(115));
        button->setDarkPressColor(color.darker(115));
        const QColor text = color.lightnessF() > 0.55 ? Qt::black : Qt::white;
        button->setLightTextColor(text);
        button->setDarkTextColor(text);
        button->setAccessibleName(_colorLabels[i]->text() + QLatin1Char(' ') + button->text());
        button->update();
    }
}

void TerminalSchemeSettings::updateSelection()
{
    if (_refreshing)
        return;
    auto terminal = _draft.value(QStringLiteral("terminal")).toObject();
    const bool dark = _mode->currentIndex() == 0;
    terminal[QStringLiteral("appearance")] = dark ? QStringLiteral("dark") : QStringLiteral("light");
    terminal[QStringLiteral("colorScheme")] = QJsonObject{
        {QStringLiteral("dark"), _dark->currentText()},
        {QStringLiteral("light"), _light->currentText()}};
    _draft[QStringLiteral("terminal")] = terminal;
    _darkRow->setVisible(dark);
    _lightRow->setVisible(!dark);
    _editingName = (dark ? _dark : _light)->currentText();
    refreshEditor();
    markDirty();
}

void TerminalSchemeSettings::markDirty()
{
    _apply->setEnabled(true);
    _revert->setEnabled(true);
    _status->setText(tr("Preview only. Save to apply to all open terminals and new tabs."));
}

bool TerminalSchemeSettings::renameScheme()
{
    if (_refreshing || _name->isReadOnly() || _name->text().trimmed() == _editingName)
        return true;
    auto scheme = TerminalSchemeStore::find(_draft, _editingName);
    if (!scheme)
        return false;
    scheme->name = _name->text().trimmed();
    QString error;
    if (!TerminalSchemeStore::save(_draft, *scheme, _editingName, &error)) {
        _status->setText(tr("Use a unique, non-empty scheme name (up to 128 characters)."));
        _name->setText(_editingName);
        return false;
    }
    refreshLists(scheme->name);
    updateSelection();
    return true;
}

void TerminalSchemeSettings::duplicateScheme()
{
    auto scheme = TerminalSchemeStore::find(_draft, _editingName);
    if (!scheme)
        return;
    const QString base = _editingName.left(100) + tr(" (copy)");
    scheme->name = base;
    int suffix = 2;
    while (TerminalSchemeStore::find(_draft, scheme->name))
        scheme->name = base + QStringLiteral(" %1").arg(suffix++);
    if (!TerminalSchemeStore::save(_draft, *scheme))
        return;
    refreshLists(scheme->name);
    updateSelection();
    _name->setFocus();
    _name->selectAll();
}

void TerminalSchemeSettings::deleteScheme()
{
    if (!TerminalSchemeStore::remove(_draft, _editingName))
        return;
    refreshLists(TerminalSchemeStore::resolve(_draft, _mode->currentIndex() == 0).name);
    // 删除仅修改草稿；保存前随时可以撤销。
    updateSelection();
}

void TerminalSchemeSettings::editColor(int index)
{
    const auto scheme = TerminalSchemeStore::find(_draft, _editingName);
    if (!scheme)
        return;
    auto copy = *scheme;
    auto* dialog = new ElaColorDialog(window());
    dialog->setWindowTitle(_colorLabels[index]->text());
    dialog->setCurrentColor(schemeColor(copy, index));
    const QString name = _editingName;
    connect(dialog, &ElaColorDialog::colorSelected, this, [this, name, index](QColor color) {
        auto edited = TerminalSchemeStore::find(_draft, name);
        if (!edited)
            return;
        if (index == 3)
            color.setAlpha(edited->selectionColor.alpha());
        else
            color.setAlpha(255);
        schemeColor(*edited, index) = color;
        if (TerminalSchemeStore::save(_draft, *edited, name)) {
            refreshLists(name);
            updateSelection();
        }
    });
    connect(dialog, &QDialog::finished, dialog, &QObject::deleteLater);
    dialog->open();
}

void TerminalSchemeSettings::apply()
{
    if (!renameScheme())
        return;
    updateSelection();
    const auto reference = _draft.value(QStringLiteral("terminal")).toObject()
        .value(QStringLiteral("colorScheme"));
    const bool saved = ConfigManager::setValues({
        {QStringLiteral("schemes"), _draft.value(QStringLiteral("schemes")).toArray().toVariantList()},
        {QStringLiteral("terminal.appearance"), _draft.value(QStringLiteral("terminal")).toObject()
            .value(QStringLiteral("appearance")).toVariant()},
        {QStringLiteral("terminal.colorScheme"), reference.toVariant()}
    });
    if (!saved) {
        _status->setText(tr("Could not save the configuration. Your changes remain in the preview."));
        return;
    }
    _draft = ConfigManager::instance().root();
    _apply->setEnabled(false);
    _revert->setEnabled(false);
    _status->setText(tr("Saved and applied to all open terminals."));
}

void TerminalSchemeSettings::retranslateUi()
{
    const QSignalBlocker blocker(_mode);
    _title->setText(tr("Terminal color schemes"));
    _description->setText(tr("Choose dark or light terminal colors, independently of the application theme."));
    _modeLabel->setText(tr("Terminal appearance"));
    _mode->setItemText(0, tr("Dark"));
    _mode->setItemText(1, tr("Light"));
    _darkLabel->setText(tr("Dark color scheme"));
    _lightLabel->setText(tr("Light color scheme"));
    _preview->setAccessibleName(tr("Terminal color preview"));
    _nameLabel->setText(tr("Scheme name"));
    _duplicate->setText(tr("Duplicate scheme"));
    _apply->setText(tr("Save and apply"));
    _revert->setText(tr("Discard changes"));
    const QString labels[] = {
        tr("Foreground"), tr("Background"), tr("Cursor"), tr("Selection"),
        tr("Black"), tr("Red"), tr("Green"), tr("Yellow"),
        tr("Blue"), tr("Purple"), tr("Cyan"), tr("White"),
        tr("Bright black"), tr("Bright red"), tr("Bright green"), tr("Bright yellow"),
        tr("Bright blue"), tr("Bright purple"), tr("Bright cyan"), tr("Bright white")
    };
    for (int i = 0; i < 20; ++i)
        _colorLabels[i]->setText(labels[i]);
    refreshEditor();
}
