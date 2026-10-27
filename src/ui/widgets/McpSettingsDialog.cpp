/** @file McpSettingsDialog.cpp
 *  @brief 用户可导出令牌或解除目标暂停；这些操作不暴露给 MCP。
 */
#include "McpSettingsDialog.h"
#include "mcp/McpService.h"
#include "mcp/application/SavedTargets.h"
#include "mcp/CapabilityAdapter.h"
#include "ui/models/SettingsController.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionTerminalController.h"
#include "ElaCheckBox.h"
#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaPlainTextEdit.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaTreeWidget.h"
#include "ElaTheme.h"
#include <QScrollBar>
#include <QApplication>
#include <QClipboard>
#include <QFontMetrics>
#include <QHeaderView>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {

/* 授权列来自 SessionPermissionModel 的具名枚举，委托不写业务列号。
 * 本文件仍避免在匿名命名空间新增类型/枚举声明：Qt lupdate 的 C++ 解析器靠
 * 花括号嵌套推断 tr() 的 context，声明会让既有译文被误判成新条目而清空。
 * 列与权限种类的对应关系由 McpTests 的对话框权限用例逐项守住。 */

class PermissionCellDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        if (!index.data(Qt::CheckStateRole).isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem cell(option);
        initStyleOption(&cell, index);
        auto* style = cell.widget ? cell.widget->style() : QApplication::style();
        const QRect indicator = style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,
            &cell, cell.widget);
        if (!indicator.isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const bool available = cell.state.testFlag(QStyle::State_Enabled)
            && index.flags().testFlag(Qt::ItemIsEnabled)
            && index.flags().testFlag(Qt::ItemIsUserCheckable)
            && ((index.column()
                    != NovaTerm::Ui::SessionPermissionModel::index(
                        NovaTerm::Ui::SessionPermissionModel::Column::Elevation)
                && index.column()
                    != NovaTerm::Ui::SessionPermissionModel::index(
                        NovaTerm::Ui::SessionPermissionModel::Column::ClearTerminal))
                || index.data(Qt::UserRole).toBool());
        if (!available) cell.state &= ~QStyle::State_Enabled;
        const QRect indicatorArea = indicator.adjusted(-2, -2, 2, 2).intersected(cell.rect);

        // 保留 Ela 的行背景、文字、选中与焦点绘制，只替换复选框颜色。
        painter->save();
        painter->setClipRect(indicatorArea, Qt::IntersectClip);
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &cell, painter, cell.widget);
        painter->restore();
        painter->save();
        painter->setClipRegion(QRegion(cell.rect).subtracted(QRegion(indicatorArea)), Qt::IntersectClip);
        QStyledItemDelegate::paint(painter, cell, index);
        painter->restore();

        const auto mode = eTheme->getThemeMode();
        const bool selected = cell.checkState != Qt::Unchecked;
        const bool highlighted = available && (cell.state.testFlag(QStyle::State_MouseOver)
            || cell.state.testFlag(QStyle::State_HasFocus));
        QColor fill = available ? ElaThemeColor(mode, BasicBase) : ElaThemeColor(mode, BasicDisable);
        QColor border = available ? ElaThemeColor(mode, PrimaryNormal) : ElaThemeColor(mode, BasicBorder);
        if (available && selected) {
            fill = ElaThemeColor(mode, PrimaryNormal);
            border = fill;
        } else if (highlighted) {
            border = ElaThemeColor(mode, PrimaryHover);
        }
        painter->save();
        painter->setClipRect(cell.rect, Qt::IntersectClip);
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(indicator).adjusted(0.75, 0.75, -0.75, -0.75);
        painter->setPen(QPen(border, available ? 1.5 : 1.0));
        painter->setBrush(fill);
        painter->drawRoundedRect(box, 3, 3);
        if (selected) {
            const QColor mark = !available ? ElaThemeColor(mode, BasicTextDisable)
                : fill.lightness() > 128 ? ElaThemeColor(ElaThemeType::Light, BasicText)
                : ElaThemeColor(ElaThemeType::Dark, BasicText);
            painter->setPen(QPen(mark, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (cell.checkState == Qt::PartiallyChecked) {
                painter->drawLine(QPointF(box.left() + box.width() * 0.25, box.center().y()),
                    QPointF(box.right() - box.width() * 0.25, box.center().y()));
            } else {
                QPainterPath tick;
                tick.moveTo(box.left() + box.width() * 0.23, box.top() + box.height() * 0.52);
                tick.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.72);
                tick.lineTo(box.left() + box.width() * 0.78, box.top() + box.height() * 0.30);
                painter->drawPath(tick);
            }
        }
        painter->restore();
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override
    {
        // QTreeWidgetItem 的 flags 作用于整行；按列拦截以保留其余权限可编辑。
        if ((index.column()
                == NovaTerm::Ui::SessionPermissionModel::index(
                    NovaTerm::Ui::SessionPermissionModel::Column::Elevation)
                || index.column()
                    == NovaTerm::Ui::SessionPermissionModel::index(
                        NovaTerm::Ui::SessionPermissionModel::Column::ClearTerminal))
            && !index.data(Qt::UserRole).toBool()) {
            return false;
        }
        if (NovaTerm::Ui::SessionPermissionModel::isPermissionColumn(index.column())
            && index.flags().testFlag(Qt::ItemIsEnabled)
            && index.flags().testFlag(Qt::ItemIsUserCheckable)
            && model->data(index, Qt::CheckStateRole).isValid()
            && event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton
                && option.rect.contains(mouse->position().toPoint())) {
                const auto state = model->data(index, Qt::CheckStateRole).toInt();
                return model->setData(index,
                    state == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                    Qt::CheckStateRole);
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};

} // namespace

McpSettingsDialog::McpSettingsDialog(NovaTerm::Mcp::Service* service, QWidget* parent)
    : ElaDialog(parent), _service(service),
      _model(std::make_unique<NovaTerm::Ui::SessionPermissionModel>()),
      _controller(std::make_unique<NovaTerm::Ui::SettingsController>(service))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("AI MCP access"));
    setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
    resize(1180, 800);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->setSpacing(10);
    _enabled = new ElaCheckBox(tr("Enable local MCP access"), this);
    layout->addWidget(_enabled);
    auto* explanation = new ElaText(tr("Share only the sessions you select. Terminal output may contain sensitive information. "
        "Fixed diagnostics stay separate from interactive terminal input. Shared sessions can run ordinary commands; potentially destructive commands and every script require confirmation in the MCP client. "
        "Script contents are written to the selected host path and are never sent to the terminal UI. Risk checks are best-effort, not a sandbox."), this);
    explanation->setWordWrap(true);
    explanation->setTextPixelSize(12);
    layout->addWidget(explanation);
    auto* interactionHelp = new ElaText(tr("允许执行命令授予当前连接的完整终端输入权限，不再逐条确认。旧命令接口的诊断、脚本和交互权限保持独立。托管提权必须另外授权；保存密码不会启用提权，可信认证适配器尚不可用。"), this);
    interactionHelp->setWordWrap(true);
    interactionHelp->setTextPixelSize(12);
    layout->addWidget(interactionHelp);
    auto* clearHelp = new ElaText(tr("允许清空终端须单独授权，默认关闭。清空影响本机屏幕或滚动历史，不向设备发送字节、不停止远端程序，也不删除独立历史录制。重连后需重新授权。"), this);
    clearHelp->setWordWrap(true);
    clearHelp->setTextPixelSize(12);
    layout->addWidget(clearHelp);
    _status = new ElaText(this);
    _status->setWordWrap(true);
    _status->setTextPixelSize(12);
    layout->addWidget(_status);
    auto* clients = new QHBoxLayout;
    _clients = new ElaComboBox(this);
    _clients->setObjectName(QStringLiteral("mcpClientSelector"));
    _clients->setMinimumWidth(180);
    clients->addWidget(_clients, 1);
    _label = new ElaLineEdit(this);
    _label->setPlaceholderText(tr("New client label"));
    _label->setMaxLength(80);
    clients->addWidget(_label, 1);
    auto* add = new ElaPushButton(tr("Add client"), this);
    auto* remove = new ElaPushButton(tr("Remove client"), this);
    clients->addWidget(add);
    clients->addWidget(remove);
    layout->addLayout(clients);
    auto* actions = new QHBoxLayout;
    auto* copy = new ElaPushButton(tr("Copy MCP configuration"), this);
    auto* copyForCcSwitch = new ElaPushButton(tr("Copy for CC Switch"), this);
    auto* rotate = new ElaPushButton(tr("Rotate access token"), this);
    auto* takeOver = new ElaPushButton(tr("接管选中会话"), this);
    actions->addWidget(copy);
    actions->addWidget(copyForCcSwitch);
    actions->addWidget(rotate);
    actions->addWidget(takeOver);
    actions->addStretch();
    layout->addLayout(actions);
    _sessions = new ElaTreeWidget(this);
    _sessions->setColumnCount(NovaTerm::Ui::SessionPermissionModel::ColumnCount);
    _sessions->setHeaderLabels({tr("Session"), tr("State"), tr("Read output"),
        tr("Fixed diagnostics"), tr("Script tasks"), tr("Interactive commands"),
        tr("允许执行命令"), tr("允许托管提权"), tr("允许清空终端"), tr("提权凭据"), tr("当前操作")});
    // 授权列整格均可点击，避免仅命中小尺寸复选框时才能切换状态。
    auto* permissionDelegate = new PermissionCellDelegate(_sessions);
    for (int column = 0; column < NovaTerm::Ui::SessionPermissionModel::ColumnCount; ++column) {
        if (NovaTerm::Ui::SessionPermissionModel::isPermissionColumn(column))
            _sessions->setItemDelegateForColumn(column, permissionDelegate);
    }
    _sessions->setRootIsDecorated(true);
    _sessions->setItemHeight(30);
    connect(eTheme, &ElaTheme::themeModeChanged, _sessions,
        [this] { _sessions->viewport()->update(); });
    auto* sessionHeader = _sessions->header();
    sessionHeader->setStretchLastSection(false);
    sessionHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    // 首列仅预留常见主机名和两级树缩进，避免大窗口把空白都分给会话名。
    sessionHeader->resizeSection(0,
        _sessions->fontMetrics().horizontalAdvance(QStringLiteral("192.168.100.100"))
            + 2 * _sessions->indentation() + _sessions->getItemLeftPadding() + 24);
    for (int column = 1;
         column < NovaTerm::Ui::SessionPermissionModel::index(
             NovaTerm::Ui::SessionPermissionModel::Column::Operation); ++column)
        sessionHeader->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    sessionHeader->setSectionResizeMode(NovaTerm::Ui::SessionPermissionModel::index(
        NovaTerm::Ui::SessionPermissionModel::Column::Operation), QHeaderView::Stretch);
    layout->addWidget(_sessions, 2);
    auto* legacyPermissions = new ElaCheckBox(tr("显示旧命令接口详细权限"), this);
    legacyPermissions->setChecked(true);
    connect(legacyPermissions, &QCheckBox::toggled, this, [this](bool visible) {
        for (int column = 3; column <= 5; ++column) _sessions->setColumnHidden(column, !visible);
    });
    layout->addWidget(legacyPermissions);
    auto* savedHelp = new ElaText(tr("保存目标：共享目录只披露名称与端点摘要；另勾选允许打开后，可复用连接或创建可见终端。新建会话的读取、完整输入和清空须分别预先选择，默认关闭；仅在原连接首次就绪后绑定，复用和重连不重新授权。本次运行有效，修改配置或轮换令牌后需重新选择；文件、脚本和提权仍另行授权。"), this);
    savedHelp->setWordWrap(true);
    savedHelp->setTextPixelSize(12);
    layout->addWidget(savedHelp);
    _savedTargets = new ElaTreeWidget(this);
    _savedTargets->setObjectName(QStringLiteral("mcpSavedTargets"));
    _savedTargets->setColumnCount(7);
    _savedTargets->setHeaderLabels({tr("保存目标"), tr("端点摘要"), tr("共享目录"), tr("允许打开"),
        tr("新建时读取"), tr("新建时完整输入"), tr("新建时允许清空")});
    _savedTargets->setRootIsDecorated(false);
    _savedTargets->setMaximumHeight(140);
    _savedTargets->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(_savedTargets);
    connect(_savedTargets, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (_refreshing || !_service || column < 2 || column > 6) return;
        const auto client = item->data(0, Qt::UserRole + 2).toString();
        const auto identityVersion = item->data(0, Qt::UserRole + 3).toULongLong();
        const bool current = client == _clients->currentData().toString()
            && identityVersion == _service->access().identityVersion(client);
        const auto id = item->data(0, Qt::UserRole).toString();
        const auto version = item->data(0, Qt::UserRole + 1).toString();
        const bool applied = current && (column == 2
            ? _service->savedTargets().setShared(client, identityVersion, id, version, item->checkState(2) == Qt::Checked)
            : column == 3
                ? _service->savedTargets().setOpenAllowed(client, identityVersion, id, version, item->checkState(3) == Qt::Checked)
                : _service->savedTargets().setNewSessionSharing(client, identityVersion, id, version,
                    column == 4 ? NovaTerm::Mcp::NewSessionPermission::Read
                        : column == 5 ? NovaTerm::Mcp::NewSessionPermission::Input
                        : NovaTerm::Mcp::NewSessionPermission::Clear,
                    item->checkState(column) == Qt::Checked));
        QMetaObject::invokeMethod(this, [this, applied] {
            refresh();
            if (!applied) _status->setText(tr("保存目标或客户端已变化，目录共享未提交；请重新选择。"));
        }, Qt::QueuedConnection);
    });
    _records = new ElaComboBox(this);
    layout->addWidget(_records);
    _details = new ElaPlainTextEdit(this);
    _details->setReadOnly(true);
    _details->setMaximumBlockCount(1024);
    _details->setPlaceholderText(tr("Command results appear here. A disconnected or timed-out command may still be running remotely."));
    layout->addWidget(_details, 1);
    _targets = new ElaComboBox(this);
    layout->addWidget(_targets);
    _confirmedStopped = new ElaCheckBox(tr("I have checked that the remote command has stopped"), this);
    auto* acknowledge = new ElaPushButton(tr("Release selected target"), this);
    auto* release = new QHBoxLayout;
    release->addWidget(_confirmedStopped, 1);
    release->addWidget(acknowledge);
    layout->addLayout(release);
    connect(_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!_refreshing && _controller && !_controller->setEnabled(enabled)) { refresh(); reportFailure(); }
    });
    connect(_clients, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { if (!_refreshing) refresh(); });
    connect(add, &QPushButton::clicked, this, [this] {
        if (!_service) return;
        const auto id = _controller->addClient(_label->text());
        if (id.isEmpty()) { reportFailure(); return; }
        _label->clear();
        refresh();
        _clients->setCurrentIndex(_clients->findData(id));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (_controller && !_controller->removeClient(_clients->currentData().toString())) reportFailure();
    });
    connect(copy, &QPushButton::clicked, this, [this] {
        if (!_service) return;
        const auto configuration = _service->clientConfiguration(_clients->currentData().toString());
        if (configuration.isEmpty()) { reportFailure(); return; }
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(configuration).toJson()));
        _status->setText(tr("Configuration copied. It contains an access token; keep it private."));
    });
    connect(copyForCcSwitch, &QPushButton::clicked, this, [this] {
        if (!_service) return;
        const auto configuration = _service->ccSwitchConfiguration(_clients->currentData().toString());
        if (configuration.isEmpty()) { reportFailure(); return; }
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(configuration).toJson()));
        _status->setText(tr("CC Switch configuration copied. It contains an access token; keep it private."));
    });
    connect(rotate, &QPushButton::clicked, this, [this] {
        if (_controller && !_controller->rotateToken(_clients->currentData().toString())) reportFailure();
    });
    connect(takeOver, &QPushButton::clicked, this, [this] {
        if (!_service || !_sessions->currentItem()) return;
        auto* item = _sessions->currentItem();
        if (item->parent()) item = item->parent();
        const auto entry = _service->directory().find(item->data(0, Qt::UserRole).toString());
        if (entry && entry->session)
            entry->session->terminalController()->release({}, QStringLiteral("preempted"));
        refresh();
    });
    connect(_sessions, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (_refreshing || !_service || !_model || !_controller
            || !NovaTerm::Ui::SessionPermissionModel::isPermissionColumn(column)) {
            return;
        }
        using Model = NovaTerm::Ui::SessionPermissionModel;
        const int readColumn = Model::index(Model::Column::ReadOutput);
        const int diagnosticsColumn = Model::index(Model::Column::Diagnostics);
        const int terminalColumn = Model::index(Model::Column::TerminalInput);
        const int elevationColumn = Model::index(Model::Column::Elevation);
        auto* root = item->parent() ? item->parent() : item;
        const auto id = root->data(0, Qt::UserRole).toString();
        const auto* row = _model->find(id);
        if (!row || row->client.str() != _clients->currentData().toString()) {
            // 刷新排队期间可能已重连或换客户端。旧行的“撤权”不能把其他仍勾选
            // 的权限写到新连接；也不能在 Qt setData 发射栈里删除旧行。
            QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
            return;
        }
        {
            // 完整终端输入在现有界面语义里同时选择读取共享；取消输入则撤销提权。
            const QSignalBlocker blocker(_sessions);
            if (column == terminalColumn && root->checkState(terminalColumn) == Qt::Checked)
                root->setCheckState(readColumn, Qt::Checked);
            if (column == terminalColumn && root->checkState(terminalColumn) != Qt::Checked)
                root->setCheckState(elevationColumn, Qt::Unchecked);
        }
        const bool read = root->checkState(readColumn) == Qt::Checked;
        if (!read && column != readColumn) {
            // 读取未共享时执行类授权不可保存；排队刷新复原控件，不提交事务。
            QMetaObject::invokeMethod(this, [this] { refresh(); }, Qt::QueuedConnection);
            return;
        }
        bool applied = true;
        if (column == readColumn) {
            if (!read) {
                const QSignalBlocker blocker(_sessions);
                for (int current = diagnosticsColumn; current < Model::ColumnCount; ++current)
                    if (Model::isPermissionColumn(current))
                        root->setCheckState(current, Qt::Unchecked);
                for (int i = 0; i < root->childCount(); ++i)
                    root->child(i)->setCheckState(diagnosticsColumn, Qt::Unchecked);
            }
            applied = applyPermission(root, *row,
                NovaTerm::Mcp::PermissionKind::ReadOutput, std::nullopt, read);
        } else if (column == diagnosticsColumn) {
            const bool selected = item->checkState(diagnosticsColumn) == Qt::Checked;
            if (!item->parent()) {
                // 根项一次操作整组诊断：仍按单项事务逐条提交，每条都使用最新行版本。
                for (int i = 0; i < root->childCount() && applied; ++i) {
                    const auto* current = _model->find(id);
                    if (!current) {
                        applied = false;
                        break;
                    }
                    const auto diagnostic = NovaTerm::Mcp::DiagnosticId{
                        root->child(i)->data(0, Qt::UserRole).toString()};
                    applied = applyPermission(root, *current,
                        NovaTerm::Mcp::PermissionKind::Diagnostic, diagnostic, selected);
                }
            } else {
                applied = applyPermission(root, *row,
                    NovaTerm::Mcp::PermissionKind::Diagnostic,
                    NovaTerm::Mcp::DiagnosticId{item->data(0, Qt::UserRole).toString()},
                    selected);
            }
        } else {
            const auto kind = NovaTerm::Ui::SessionPermissionModel::permissionForColumn(column);
            applied = kind && applyPermission(root, *row, *kind, std::nullopt,
                root->checkState(column) == Qt::Checked);
        }
        if (!applied) {
            // itemChanged 位于 QTreeWidgetItem::setData 的发射栈内；此处
            // 同步 clear/rebuild 会删除 Qt 尚在访问的 item。退回事件循环后刷新。
            QMetaObject::invokeMethod(this, [this] {
                refresh();
                reportFailure();
            }, Qt::QueuedConnection);
        }
    });

    connect(_records, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { if (!_refreshing) showRecord(); });
    connect(_targets, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (!_refreshing) _confirmedStopped->setChecked(false);
    });
    connect(_confirmedStopped, &QCheckBox::toggled, this, [this](bool checked) {
        _confirmedFingerprint = checked ? _targets->currentData().toString() : QString{};
        _confirmedExecutionId = checked
            ? _targets->currentData(Qt::UserRole + 1).toString() : QString{};
    });
    connect(acknowledge, &QPushButton::clicked, this, [this] {
        if (!_confirmedStopped->isChecked() || !_service) return;
        const bool released = !_confirmedFingerprint.isEmpty()
            && !_confirmedExecutionId.isEmpty()
            && _confirmedFingerprint == _targets->currentData().toString()
            && _confirmedExecutionId == _targets->currentData(Qt::UserRole + 1).toString()
            && _service->acknowledgeTarget(_confirmedFingerprint, _confirmedExecutionId);
        _confirmedStopped->setChecked(false);
        if (!released) reportFailure();
    });
    if (_service) connect(_service, &NovaTerm::Mcp::Service::changed,
        this, &McpSettingsDialog::refresh, Qt::QueuedConnection);
    refresh();
}

McpSettingsDialog::~McpSettingsDialog() = default;

void McpSettingsDialog::editElevationCredentials(const QString& sessionId)
{
    if (!_service) return;
    const auto entry = _service->directory().find(sessionId);
    if (!entry || !entry->session) return;
    const auto binding = *entry;
    const auto matchesBinding = [binding](const SessionDirectory::Entry& current) {
        return binding.session && current.session == binding.session
            && current.id == binding.id && current.attachmentId == binding.attachmentId
            && current.epoch == binding.epoch && current.generation == binding.generation
            && current.targetFingerprint == binding.targetFingerprint
            && current.transport == binding.transport && current.kind == binding.kind
            && current.state == SessionState::Running;
    };
    auto* dialog = new ElaDialog(this);
    dialog->setObjectName(QStringLiteral("mcpElevationCredentialsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("提权凭据 — %1").arg(entry->title));
    dialog->setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
    dialog->resize(560, 330);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 8, 16, 16);
    auto* explanation = new ElaText(tr("sudo 认证密码和 su root 密码分别保存，不使用 SSH 登录密码。保存后不回填。只有明确授权且可信适配器可用时才能托管认证；当前适配器不可用。撤销授权不会退出已经获得的 root Shell。"), dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* status = new ElaText(_controller->elevationCredentialsPersistent()
        ? tr("存储：系统凭据库。密码仅在本机设置，不对 MCP 客户端开放。")
        : tr("存储：仅本次运行。密码仅在本机设置，不对 MCP 客户端开放。"), dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    for (const auto& method : {QStringLiteral("sudo"), QStringLiteral("su")}) {
        auto* title = new ElaText(method == "sudo" ? tr("sudo 认证密码") : tr("su root 密码"), dialog);
        layout->addWidget(title);
        auto* row = new QHBoxLayout;
        auto* password = new ElaLineEdit(dialog);
        password->setObjectName(QStringLiteral("mcpElevationPassword_") + method);
        password->setEchoMode(QLineEdit::Password);
        password->setMaxLength(1024);
        password->setPlaceholderText(tr("输入新密码；留空不会删除"));
        auto* save = new ElaPushButton(tr("保存 / 替换"), dialog);
        auto* remove = new ElaPushButton(tr("删除"), dialog);
        save->setObjectName(QStringLiteral("mcpElevationSave_") + method);
        remove->setObjectName(QStringLiteral("mcpElevationRemove_") + method);
        remove->setEnabled(_controller->hasElevationCredential(*entry, method));
        row->addWidget(password, 1);
        row->addWidget(save);
        row->addWidget(remove);
        layout->addLayout(row);
        const auto update = [this, sessionId, binding, matchesBinding, method, password, save, remove, status,
                             guard = QPointer<ElaDialog>(dialog)](bool deleting) {
            if (!guard || !_service || !_controller) return;
            auto* controller = _controller.get();
            const auto current = _service->directory().find(sessionId);
            if (!current || !matchesBinding(*current)) {
                password->clear();
                save->setEnabled(false);
                remove->setEnabled(false);
                status->setText(tr("会话连接或目标已变化，未提交凭据操作。请关闭并重新打开设置。"));
                return;
            }
            QByteArray secret = deleting ? QByteArray{} : password->text().toUtf8();
            password->clear();
            save->setEnabled(false);
            remove->setEnabled(false);
            status->setText(tr("正在访问本机凭据库；可以关闭此窗口。"));
            const auto completed = [guard, service = _service, controller, sessionId,
                                    matchesBinding, method, save, remove, status](bool ok) {
                if (!guard || !service) return;
                const auto active = service->directory().find(sessionId);
                if (!active || !matchesBinding(*active)) {
                    save->setEnabled(false);
                    remove->setEnabled(false);
                    status->setText(QObject::tr("会话连接或目标已变化；凭据操作针对打开窗口时的原目标。请关闭并重新打开设置。"));
                    return;
                }
                save->setEnabled(true);
                remove->setEnabled(controller->hasElevationCredential(*active, method));
                status->setText(ok
                    ? (controller->elevationCredentialsPersistent()
                        ? QObject::tr("已更新系统凭据库。托管提权授权已撤销，请按需重新授权。")
                        : QObject::tr("已更新，仅本次运行保存。托管提权授权已撤销，请按需重新授权。"))
                    : QObject::tr("凭据操作失败；请检查会话身份、密码是否为空及凭据库权限。"));
            };
            if (!controller->configureElevationCredentialAsync(
                    binding, method, std::move(secret), deleting, completed)) completed(false);
        };
        connect(save, &QPushButton::clicked, dialog, [update] { update(false); });
        connect(remove, &QPushButton::clicked, dialog, [update] { update(true); });
    }
    dialog->open();
}

bool McpSettingsDialog::applyPermission(QTreeWidgetItem* root,
    const NovaTerm::Ui::SessionPermissionRow& row,
    NovaTerm::Mcp::PermissionKind kind,
    std::optional<NovaTerm::Mcp::DiagnosticId> diagnostic, bool selected)
{
    if (!_model || !_controller || !root)
        return false;
    const auto outcome = _controller->submit(row, kind, diagnostic, selected);
    if (outcome.status != NovaTerm::Ui::SettingsController::Status::Applied)
        return false;
    if (!_model->replaceRow(outcome.row))
        return false;
    const auto& updated = outcome.row;
    using Model = NovaTerm::Ui::SessionPermissionModel;
    const int readColumn = Model::index(Model::Column::ReadOutput);
    const int diagnosticsColumn = Model::index(Model::Column::Diagnostics);
    const int scriptColumn = Model::index(Model::Column::Script);
    const int interactiveColumn = Model::index(Model::Column::Interactive);
    const int terminalColumn = Model::index(Model::Column::TerminalInput);
    const int elevationColumn = Model::index(Model::Column::Elevation);
    const int clearColumn = Model::index(Model::Column::ClearTerminal);
    root->setData(0, Qt::UserRole + 4,
        QVariant::fromValue<qulonglong>(updated.revision.value()));
    // 服务端级联结果回写控件；整个过程屏蔽信号，避免在 itemChanged 栈内递归。
    const QSignalBlocker blocker(_sessions);
    root->setCheckState(readColumn, updated.selection.read ? Qt::Checked : Qt::Unchecked);
    int granted = 0;
    for (int i = 0; i < root->childCount(); ++i) {
        const NovaTerm::Mcp::DiagnosticId id{
            root->child(i)->data(0, Qt::UserRole).toString()};
        const bool on = updated.selection.diagnostics.contains(id);
        root->child(i)->setCheckState(diagnosticsColumn, on ? Qt::Checked : Qt::Unchecked);
        if (on)
            ++granted;
    }
    if (root->childCount() > 0) {
        root->setCheckState(diagnosticsColumn, granted == 0 ? Qt::Unchecked
            : granted == root->childCount() ? Qt::Checked : Qt::PartiallyChecked);
    }
    root->setCheckState(scriptColumn,
        updated.selection.script ? Qt::Checked : Qt::Unchecked);
    root->setCheckState(interactiveColumn,
        updated.selection.riskControlledCommand ? Qt::Checked : Qt::Unchecked);
    root->setCheckState(terminalColumn,
        updated.selection.fullTerminalInput ? Qt::Checked : Qt::Unchecked);
    root->setCheckState(elevationColumn,
        updated.selection.elevation ? Qt::Checked : Qt::Unchecked);
    root->setData(elevationColumn, Qt::UserRole, updated.elevationSelectable);
    root->setCheckState(clearColumn,
        updated.selection.clearTerminal ? Qt::Checked : Qt::Unchecked);
    root->setData(clearColumn, Qt::UserRole, updated.clearTerminalSelectable);
    return true;
}

void McpSettingsDialog::reportFailure()
{
    // AccessStore 把最近一次保存失败的原因留在 lastSaveFailure()，这里取用后
    // 立刻清空，避免上一次失败的原因被下一次无关失败误读。目录未加固与被别的
    // 实例持锁占用是两种最常见也最难自察的成因，处置完全不同，因此各自给一条
    // 可操作的提示。
    if (!_service) return;
    using Failure = NovaTerm::Ui::SettingsController::SaveFailure;
    const Failure reason = _controller->lastSaveFailure();
    _controller->clearSaveFailure();
    if (reason == Failure::Locked) {
        _status->setText(tr("Cannot update MCP access settings: another NovaTerm instance is holding the shared access file. Close that instance and try again."));
        return;
    }
    if (reason == Failure::Directory) {
        _status->setText(tr("Cannot update MCP access settings: the MCP state directory is not private to this user. Fix its permissions and owner, then try again."));
        return;
    }
    _status->setText(tr("Operation failed. Check the selected client, storage permissions, and active commands."));
}
void McpSettingsDialog::refresh()
{
    if (_refreshing || !_service) return;
    _refreshing = true;
    const QSignalBlocker clientsBlocker(_clients), sessionsBlocker(_sessions), enabledBlocker(_enabled);
    _enabled->setChecked(_controller->enabled());
    QString state = _service->status();
    if (state == "Disabled") state = tr("Disabled");
    else if (state == "Listening") state = tr("Listening");
    else if (state == "Starting") state = tr("Starting");
    QString statusText = tr("Status: %1; instance: %2").arg(state, _service->instanceId());
    if (!_service->httpEndpoint().isEmpty()) statusText += '\n' + _service->httpEndpoint();
    _status->setText(statusText);
    const auto selected = _clients->currentData().toString();
    _clients->clear();
    for (const auto& client : _controller->clients())
        _clients->addItem(client.label + (client.persistent ? QString() : tr(" (this run only)")), client.id.str());
    if (_clients->findData(selected) >= 0) _clients->setCurrentIndex(_clients->findData(selected));
    const auto clientId = _clients->currentData().toString();
    {
        const QSignalBlocker blocker(_savedTargets);
        _savedTargets->clear();
        const auto identityVersion = _service->access().identityVersion(clientId);
        for (const auto& target : _service->savedTargets().localTargets()) {
            auto* item = new QTreeWidgetItem(_savedTargets, {target.title, target.endpoint, {}, {}});
            item->setData(0, Qt::UserRole, target.id);
            item->setData(0, Qt::UserRole + 1, target.configVersion);
            item->setData(0, Qt::UserRole + 2, clientId);
            item->setData(0, Qt::UserRole + 3, QVariant::fromValue<qulonglong>(identityVersion));
            item->setCheckState(2, _service->savedTargets().isShared(clientId, identityVersion, target.id)
                ? Qt::Checked : Qt::Unchecked);
            item->setCheckState(3, _service->savedTargets().canOpen(clientId, identityVersion, target.id)
                ? Qt::Checked : Qt::Unchecked);
            const auto sharing = _service->savedTargets().newSessionSharing(clientId, identityVersion, target.id);
            item->setCheckState(4, sharing.read ? Qt::Checked : Qt::Unchecked);
            item->setCheckState(5, sharing.input ? Qt::Checked : Qt::Unchecked);
            item->setCheckState(6, sharing.clear ? Qt::Checked : Qt::Unchecked);
            if (!identityVersion) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
    }
    // 本函数每次都重建整棵树，而根项默认折叠：先按会话 ID 记下当前展开的根项，
    // 重建后原样恢复。否则勾选任意一条授权（itemChanged → changePermission → changed →
    // 排队的 refresh）都会把用户刚展开的层级收起来，每次授权都要重新展开。
    QString currentSession;
    if (const auto* current = _sessions->currentItem())
        currentSession = (current->parent() ? current->parent() : current)
            ->data(0, Qt::UserRole).toString();
    const int scrollPosition = _sessions->verticalScrollBar()->value();
    QSet<QString> expandedSessions;
    for (int i = 0; i < _sessions->topLevelItemCount(); ++i) {
        const auto* root = _sessions->topLevelItem(i);
        const auto id = root->data(0, Qt::UserRole).toString();
        if (root->isExpanded() && !id.isEmpty())
            expandedSessions.insert(id);
    }
    _sessions->clear();
    _model->rebuild(_controller->rows(clientId), expandedSessions);
    using Model = NovaTerm::Ui::SessionPermissionModel;
    const int readColumn = Model::index(Model::Column::ReadOutput);
    const int diagnosticsColumn = Model::index(Model::Column::Diagnostics);
    const int scriptColumn = Model::index(Model::Column::Script);
    const int interactiveColumn = Model::index(Model::Column::Interactive);
    const int terminalColumn = Model::index(Model::Column::TerminalInput);
    const int elevationColumn = Model::index(Model::Column::Elevation);
    const int clearColumn = Model::index(Model::Column::ClearTerminal);
    const int credentialsColumn = Model::index(Model::Column::Credentials);
    const int operationColumn = Model::index(Model::Column::Operation);
    for (const auto& row : _model->rows()) {
        auto* item = new QTreeWidgetItem(_sessions, {row.title, row.state, {}, {}, {}, {}});
        item->setData(0, Qt::UserRole, row.key.sessionId);
        item->setData(0, Qt::UserRole + 1, row.key.epoch);
        item->setData(0, Qt::UserRole + 2, row.key.attachmentId);
        item->setData(0, Qt::UserRole + 3, row.client.str());
        item->setData(0, Qt::UserRole + 4,
            QVariant::fromValue<qulonglong>(row.revision.value()));
        item->setData(0, Qt::UserRole + 5, QVariant::fromValue<qulonglong>(row.key.generation));
        item->setData(0, Qt::UserRole + 6, row.key.targetFingerprint);
        item->setToolTip(0, row.title + '\n' + row.key.sessionId);
        if (_model->isExpanded(row.key.sessionId))
            item->setExpanded(true);
        item->setCheckState(readColumn,
            row.selection.read ? Qt::Checked : Qt::Unchecked);
        int granted = 0;
        for (const auto& diagnostic : row.diagnostics)
            if (diagnostic.granted) ++granted;
        item->setCheckState(diagnosticsColumn, granted == 0 ? Qt::Unchecked
            : granted == row.diagnostics.size() ? Qt::Checked : Qt::PartiallyChecked);
        const auto diagnosticHelp = row.localShell
            ? tr("Fixed diagnostics run in an isolated local helper and do not write to the current shell.")
            : tr("仅对已确认的 Linux/POSIX 目标使用固定诊断；授权不代表目标支持这些命令。");
        if (!row.capability.diagnosticsReady)
            item->setText(diagnosticsColumn, tr("后端不可用"));
        item->setToolTip(diagnosticsColumn, row.capability.diagnosticsReady ? diagnosticHelp
            : tr("可以保存诊断授权，但当前没有可用的目标 Profile 或执行后端；不会自动向终端发送诊断命令。")
                + '\n' + diagnosticHelp);
        item->setCheckState(scriptColumn,
            row.selection.script ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(scriptColumn, tr("Allows LocalShell/SSH script tasks. Each script still requires MCP-client confirmation; its body is written to the requested host path and is not shown in the terminal UI."));
        item->setCheckState(interactiveColumn,
            row.selection.riskControlledCommand ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(interactiveColumn, tr("Allows the MCP client to type commands into this session's current terminal. Ordinary low-risk commands run without confirmation; anything potentially destructive or unclassifiable still requires confirmation in the MCP client. Off by default: sharing read output does not imply permission to type."));
        item->setCheckState(terminalColumn,
            row.selection.fullTerminalInput ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(terminalColumn, tr("允许当前客户端在本次连接输入文本、按键并执行命令。重连、撤权或目标变化后需要重新授权。"));
        item->setCheckState(clearColumn,
            row.selection.clearTerminal ? Qt::Checked : Qt::Unchecked);
        item->setData(clearColumn, Qt::UserRole, row.clearTerminalSelectable);
        item->setToolTip(clearColumn, tr("须先共享读取且会话已连接。独立允许清空本机屏幕或滚动历史，影响人的本机显示；不向设备发送字节、不停止远端程序，也不删除独立历史录制。重连后需重新授权。"));
        item->setCheckState(elevationColumn,
            row.selection.elevation ? Qt::Checked : Qt::Unchecked);
        // 已有授权始终允许撤销；不满足条件时仅禁止新勾选，不能锁住整行。
        item->setData(elevationColumn, Qt::UserRole, row.elevationSelectable);
        const auto& elevationDisabled = row.capability.elevationGrantDisabledReason;
        const auto& elevationUnavailable = row.capability.elevationDisabledReason;
        QString elevationStatus, elevationReason;
        if (elevationDisabled == "SESSION_NOT_READY") {
            elevationStatus = tr("未连接");
            elevationReason = tr("会话未连接，暂不能授权托管提权。");
        } else if (elevationDisabled == "TERMINAL_PERMISSION_REQUIRED") {
            elevationStatus = tr("需执行权限");
            elevationReason = tr("请先勾选允许执行命令，再授权托管提权。");
        } else if (elevationDisabled == "CREDENTIAL_UPDATE_IN_PROGRESS") {
            elevationStatus = tr("凭据更新中");
            elevationReason = tr("正在更新提权凭据，请稍后重新授权。");
        } else if (elevationUnavailable == "TARGET_IDENTITY_UNVERIFIED") {
            elevationStatus = tr("身份未验证");
            elevationReason = tr("可以保存提权授权，但目标身份尚未验证，不能绑定凭据或实际提权。");
        } else if (elevationUnavailable == "ELEVATION_ADAPTER_UNAVAILABLE") {
            elevationStatus = tr("适配器不可用");
            elevationReason = tr("可以保存提权授权，但当前没有可信认证适配器，不会读取或发送密码。");
        }
        item->setText(elevationColumn, elevationStatus);
        const auto elevationHelp = tr("须先允许执行命令；勾选仅保存授权。实际提权还需要可信目标身份及认证适配器，保存密码不会自动启用提权。");
        item->setToolTip(elevationColumn, elevationReason.isEmpty() ? elevationHelp
            : elevationReason + '\n' + elevationHelp);
        auto* credentials = new ElaPushButton(row.sudoSaved && row.suSaved ? tr("sudo / su")
            : row.sudoSaved ? tr("sudo") : row.suSaved ? tr("su") : tr("未配置"), _sessions);
        credentials->setEnabled(elevationUnavailable != "TARGET_IDENTITY_UNVERIFIED");
        credentials->setToolTip(row.credentialsPersistent
            ? tr("密码保存在系统凭据库；保存不代表授权使用，也不启用认证适配器。")
            : tr("密码仅本次运行保留；保存不代表授权使用，也不启用认证适配器。"));
        connect(credentials, &QPushButton::clicked, this,
            [this, id = row.key.sessionId] { editElevationCredentials(id); });
        _sessions->setItemWidget(item, credentialsColumn, credentials);
        QString operation = row.terminalOwner.isEmpty() ? tr("空闲") : tr("Agent 操作");
        for (const auto& client : _controller->clients())
            if (client.id.str() == row.terminalOwner)
                operation += " / " + client.label;
        if (row.controlState == "preempted") operation = tr("用户接管");
        if (!row.commandState.isEmpty()) operation += " / " + row.commandState;
        item->setText(operationColumn, operation);
        for (const auto& diagnostic : row.diagnostics) {
            QString title = diagnostic.title;
            if (diagnostic.id == "system.identity") title = tr("System identity");
            else if (diagnostic.id == "system.uptime") title = tr("Uptime and load");
            else if (diagnostic.id == "memory.summary") title = tr("Memory summary");
            else if (diagnostic.id == "filesystem.usage") title = tr("Filesystem capacity");
            auto* child = new QTreeWidgetItem(item, {title});
            child->setData(0, Qt::UserRole, diagnostic.id);
            child->setToolTip(0, diagnostic.preview);
            child->setCheckState(diagnosticsColumn,
                diagnostic.granted ? Qt::Checked : Qt::Unchecked);
        }
    }
    _sessions->setEnabled(!clientId.isEmpty() && _controller->enabled());
    const auto previousRecord = _records->currentData().toString();
    _records->clear();
    _recordData.clear();
    for (const auto& value : _service->executionRecords()) {
        const auto record = value.toObject();
        const auto id = record.value("executionId").toString();
        _records->addItem(record.value("commandId").toString() + " / " + id.left(8), id);
        _recordData.insert(id, record);
    }
    if (_records->findData(previousRecord) >= 0) _records->setCurrentIndex(_records->findData(previousRecord));
    const auto previousTarget = _targets->currentData().toString();
    const QSignalBlocker targetsBlocker(_targets);
    _targets->clear();
    const auto targets = _service->protectedTargets();
    for (auto it = targets.begin(); it != targets.end(); ++it) {
        const auto executionId = it.value().toObject().value("executionId").toString();
        _targets->addItem(tr("Protected target %1 — execution %2")
            .arg(it.key().left(12), executionId.left(8)), it.key());
        _targets->setItemData(_targets->count() - 1, executionId, Qt::UserRole + 1);
    }
    const auto previousTargetIndex = _targets->findData(previousTarget);
    if (previousTargetIndex >= 0) _targets->setCurrentIndex(previousTargetIndex);
    if (_confirmedFingerprint != _targets->currentData().toString()
        || _confirmedExecutionId != _targets->currentData(Qt::UserRole + 1).toString()) {
        _confirmedStopped->setChecked(false);
    }
    if (!currentSession.isEmpty()) {
        for (int i = 0; i < _sessions->topLevelItemCount(); ++i) {
            if (_sessions->topLevelItem(i)->data(0, Qt::UserRole).toString()
                == currentSession) {
                _sessions->setCurrentItem(_sessions->topLevelItem(i));
                break;
            }
        }
    }
    _sessions->verticalScrollBar()->setValue(scrollPosition);
    _refreshing = false;
    showRecord();
}
void McpSettingsDialog::showRecord()
{
    const auto record = _recordData.value(_records->currentData().toString());
    if (record.isEmpty()) { _details->clear(); return; }
    if (!record.value("complete").toBool()) { _details->setPlainText(tr("Command is running.")); return; }
    const auto payload = record.value("result").toObject();
    const auto details = payload.value("ok").toBool() ? payload.value("data").toObject()
        : payload.value("error").toObject().value("details").toObject();
    _details->setPlainText(tr("Status: %1\nExit code: %2\nTermination confirmed: %3\n\n%4\n%5")
        .arg(details.value("status").toString(), details.value("exitCode").isNull()
            ? tr("Unknown") : QString::number(details.value("exitCode").toInt()),
            details.value("terminationConfirmed").toBool() ? tr("Yes") : tr("No"),
            details.value("stdout").toString(), details.value("stderr").toString()));
}
