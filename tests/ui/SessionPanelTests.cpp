/**
 * @file   SessionPanelTests.cpp
 * @brief  会话历史面板的选区与状态保持回归测试。
 *
 * 覆盖 `SessionPanel::showItemContextMenu` 的多选保持：在
 * `ExtendedSelection` 下 `setCurrentItem` 等价于 ClearAndSelect，无修饰键的
 * 右键会清掉已有的多选。右键点中**未选中**条目时才应切换选区，点在已选条目上
 * 必须保持原选区 —— 否则「Ctrl+A 选中一批 → 右键其中一项 → 编辑」会把整批选区
 * 静默销毁，且同一控件上按 Delete 键删除整批、右键删除却只有一条，两条路径互相
 * 矛盾。
 */
#include "ui/widgets/SessionPanel.h"

#include "ElaTreeWidget.h"
#include "ui/widgets/MessagePrompts.h"

#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QtTest>

#include <memory>

namespace {

// 面板把历史写到 AppData。测试必须隔离，否则会污染真实用户数据并让断言
// 依赖机器上已有的历史条目。
struct ScopedAppDataIsolation
{
    ScopedAppDataIsolation()
    {
        _previous = qgetenv("XDG_DATA_HOME");
        if (_dir.isValid())
            qputenv("XDG_DATA_HOME", _dir.path().toUtf8());
        QStandardPaths::setTestModeEnabled(true);
    }
    ~ScopedAppDataIsolation()
    {
        QStandardPaths::setTestModeEnabled(false);
        if (_previous.isEmpty())
            qunsetenv("XDG_DATA_HOME");
        else
            qputenv("XDG_DATA_HOME", _previous);
    }
    QTemporaryDir _dir;
    QByteArray _previous;
};

} // namespace

class SessionPanelTests final : public QObject
{
    Q_OBJECT

private slots:
    void contextMenuOnSelectedRowKeepsMultiSelection();
    void contextMenuOnUnselectedRowMovesCurrent();
    void contextMenuOnGroupRowDoesNothing();

private:
    // 在树里造出 group -> leaf 的结构，并返回按顺序排列的叶子节点。
    static QList<QTreeWidgetItem*> populate(QTreeWidget* tree)
    {
        auto* group = new QTreeWidgetItem(tree);
        group->setText(0, QStringLiteral("SSH"));
        group->setData(0, Qt::UserRole, QString()); // 分组不带会话 UUID
        QList<QTreeWidgetItem*> leaves;
        for (int i = 0; i < 3; ++i) {
            auto* leaf = new QTreeWidgetItem(group);
            leaf->setText(0, QStringLiteral("session-%1").arg(i));
            leaf->setData(0, Qt::UserRole,
                          QStringLiteral("00000000-0000-4000-8000-00000000000%1")
                              .arg(i));
            leaves.append(leaf);
        }
        group->setExpanded(true);
        return leaves;
    }
};

void SessionPanelTests::contextMenuOnSelectedRowKeepsMultiSelection()
{
    ScopedAppDataIsolation isolation;
    auto panel = std::make_unique<SessionPanel>();
    auto* tree = panel->findChild<ElaTreeWidget*>();
    QVERIFY(tree != nullptr);
    QCOMPARE(tree->selectionMode(), QAbstractItemView::ExtendedSelection);

    const auto leaves = populate(tree);
    // 注意：只能用 setSelected 建立多选。setCurrentItem 在 ExtendedSelection 下
    // 本身就是 ClearAndSelect —— 用它来「准备」多选会把选区打回 1，那正是本
    // 用例要检的失效模式本身，不能自己踩进去造出假阳性。
    for (QTreeWidgetItem* leaf : leaves)
        leaf->setSelected(true);
    QCOMPARE(tree->selectedItems().size(), 3);

    // 右键命中**已选中**的第二行：选区必须原样保留。
    const QPoint position = tree->visualItemRect(leaves.at(1)).center();
    tree->customContextMenuRequested(position);

    QCOMPARE(tree->selectedItems().size(), 3);

    // 弹出的菜单是 _tree 的子对象，随面板一起销毁，不留悬挂窗口。
    panel.reset();
}

void SessionPanelTests::contextMenuOnUnselectedRowMovesCurrent()
{
    ScopedAppDataIsolation isolation;
    auto panel = std::make_unique<SessionPanel>();
    auto* tree = panel->findChild<ElaTreeWidget*>();
    QVERIFY(tree != nullptr);

    const auto leaves = populate(tree);
    // 只选中第一行，右键点第三行。
    leaves.at(0)->setSelected(true);
    QCOMPARE(tree->selectedItems().size(), 1);

    const QPoint position = tree->visualItemRect(leaves.at(2)).center();
    tree->customContextMenuRequested(position);

    // 未选中时必须切换选区到被点中的那一行。
    QCOMPARE(tree->selectedItems().size(), 1);
    QCOMPARE(tree->currentItem(), leaves.at(2));

    panel.reset();
}

void SessionPanelTests::contextMenuOnGroupRowDoesNothing()
{
    ScopedAppDataIsolation isolation;
    auto panel = std::make_unique<SessionPanel>();
    auto* tree = panel->findChild<ElaTreeWidget*>();
    QVERIFY(tree != nullptr);

    const auto leaves = populate(tree);
    for (QTreeWidgetItem* leaf : leaves)
        leaf->setSelected(true);
    QTreeWidgetItem* group = tree->topLevelItem(0);
    QVERIFY(group != nullptr);

    // 分组行不带会话 UUID，必须被跳过：不弹菜单、不动当前项、不清选区。
    const QPoint position = tree->visualItemRect(group).center();
    tree->customContextMenuRequested(position);

    QCOMPARE(tree->selectedItems().size(), 3);
    QVERIFY(tree->currentItem() == nullptr || tree->currentItem() != group);

    panel.reset();
}

QTEST_MAIN(SessionPanelTests)

#include "SessionPanelTests.moc"