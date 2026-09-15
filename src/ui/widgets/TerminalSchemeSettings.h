/**
 * @file TerminalSchemeSettings.h
 * @brief 终端命名配色的选择、编辑与独立预览。
 */
#pragma once

#include <QJsonObject>
#include <QWidget>
#include <array>

class ElaComboBox;
class ElaLineEdit;
class ElaPushButton;
class ElaText;
class TerminalSchemePreview;

/** @brief 编辑草稿，用户明确保存后才更新已打开的终端。 */
class TerminalSchemeSettings final : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalSchemeSettings(QWidget* parent = nullptr);

private:
    void retranslateUi();
    void reload();
    void refreshLists(const QString& editingName);
    void refreshEditor();
    void updateSelection();
    void markDirty();
    bool renameScheme();
    void duplicateScheme();
    void deleteScheme();
    void editColor(int index);
    void apply();

    QJsonObject _draft;
    QString _editingName;
    bool _refreshing{false};
    ElaText* _title{nullptr};
    ElaText* _description{nullptr};
    ElaText* _modeLabel{nullptr};
    ElaText* _darkLabel{nullptr};
    ElaText* _lightLabel{nullptr};
    ElaText* _nameLabel{nullptr};
    ElaText* _status{nullptr};
    ElaComboBox* _mode{nullptr};
    ElaComboBox* _dark{nullptr};
    ElaComboBox* _light{nullptr};
    QWidget* _darkRow{nullptr};
    QWidget* _lightRow{nullptr};
    ElaLineEdit* _name{nullptr};
    ElaPushButton* _duplicate{nullptr};
    ElaPushButton* _delete{nullptr};
    ElaPushButton* _apply{nullptr};
    ElaPushButton* _revert{nullptr};
    TerminalSchemePreview* _preview{nullptr};
    std::array<ElaText*, 20> _colorLabels{};
    std::array<ElaPushButton*, 20> _colorButtons{};
};
