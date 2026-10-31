/** @file McpSettingsDialog.cpp
 *  @brief 本机MCP接入、统一接入与独立会话授权。
 */
#include "McpSettingsDialog.h"
#include "mcp/McpService.h"
#include "mcp/CapabilityAdapter.h"
#include "ui/models/SettingsController.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionTerminalController.h"
#include "ElaCheckBox.h"
#include "ElaLineEdit.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaTreeWidget.h"
#include "ElaTheme.h"
#include <QScrollBar>
#include <QApplication>
#include <QClipboard>
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
    resize(1050, 680);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->setSpacing(10);
    _enabled = new ElaCheckBox(tr("Enable local MCP access"), this);
    layout->addWidget(_enabled);
    auto* explanation = new ElaText(tr("所有 AI 共用此接入令牌及授权。会话的读取、输入、托管提权和清空权限默认开启，可取消勾选；终端输出可能包含敏感信息。"), this);
    explanation->setWordWrap(true);
    explanation->setTextPixelSize(12);
    layout->addWidget(explanation);
    _status = new ElaText(this);
    _status->setWordWrap(true);
    _status->setTextPixelSize(12);
    layout->addWidget(_status);
    auto* actions = new QHBoxLayout;
    auto* copy = new ElaPushButton(tr("Copy MCP configuration"), this);
    auto* copyForCcSwitch = new ElaPushButton(tr("Copy for CC Switch"), this);
    auto* rotate = new ElaPushButton(tr("Reset access token"), this);
    rotate->setObjectName("mcpRotateAccessToken");
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
        tr("允许执行命令"), tr("允许托管提权"), tr("允许清空终端"), tr("提权凭据"), tr("当前操作")});
    // 授权列整格均可点击，避免仅命中小尺寸复选框时才能切换状态。
    auto* permissionDelegate = new PermissionCellDelegate(_sessions);
    for (int column = 0; column < NovaTerm::Ui::SessionPermissionModel::ColumnCount; ++column) {
        if (NovaTerm::Ui::SessionPermissionModel::isPermissionColumn(column))
            _sessions->setItemDelegateForColumn(column, permissionDelegate);
    }
    _sessions->setRootIsDecorated(false);
    _sessions->setItemHeight(30);
    connect(eTheme, &ElaTheme::themeModeChanged, _sessions,
        [this] { _sessions->viewport()->update(); });
    auto* sessionHeader = _sessions->header();
    sessionHeader->setStretchLastSection(false);
    sessionHeader->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1;
         column < NovaTerm::Ui::SessionPermissionModel::index(
             NovaTerm::Ui::SessionPermissionModel::Column::Operation); ++column)
        sessionHeader->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    sessionHeader->setSectionResizeMode(NovaTerm::Ui::SessionPermissionModel::index(
        NovaTerm::Ui::SessionPermissionModel::Column::Operation), QHeaderView::ResizeToContents);
    layout->addWidget(_sessions, 2);
    auto* forgetUploads = new ElaPushButton(tr("清除上传确认记忆"), this);
    forgetUploads->setObjectName("mcpClearUploadMemory"); layout->addWidget(forgetUploads);
    connect(forgetUploads, &QPushButton::clicked, this, [this] {
        if (!_service || !_service->clearUploadMemory()) { reportFailure(); return; }
        _status->setText(tr("已清除上传确认记忆，下次上传时会在 AI 对话中确认一次。"));
    });
    connect(_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!_refreshing && _controller && !_controller->setEnabled(enabled)) { refresh(); reportFailure(); }
    });
    connect(rotate, &QPushButton::clicked, this, [this] {
        if (!_controller || !_controller->rotateToken()) { reportFailure(); return; }
        refresh();
        _status->setText(tr("Access token reset. Copy the updated configuration for all AI connections."));
    });
    connect(copy, &QPushButton::clicked, this, [this] {
        if (!_service) return;
        const auto configuration = _service->clientConfiguration();
        if (configuration.isEmpty()) { reportFailure(); return; }
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(configuration).toJson()));
        _status->setText(tr("Configuration copied. It contains an access token; keep it private."));
    });
    connect(copyForCcSwitch, &QPushButton::clicked, this, [this] {
        if (!_service) return;
        const auto configuration = _service->ccSwitchConfiguration();
        if (configuration.isEmpty()) { reportFailure(); return; }
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(configuration).toJson()));
        _status->setText(tr("CC Switch configuration copied. It contains an access token; keep it private."));
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
        const int terminalColumn = Model::index(Model::Column::TerminalInput);
        const int elevationColumn = Model::index(Model::Column::Elevation);
        auto* root = item->parent() ? item->parent() : item;
        const auto id = root->data(0, Qt::UserRole).toString();
        const auto* row = _model->find(id);
        if (!row || row->client.str() != _service->access().accessId()
            || _accessIdentity != _service->access().identityVersion(row->client.str())) {
            // 刷新排队期间可能已重连或重置接入令牌。旧行的“撤权”不能把其他仍勾选
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
                for (int current = terminalColumn; current < Model::ColumnCount; ++current)
                    if (Model::isPermissionColumn(current))
                        root->setCheckState(current, Qt::Unchecked);
            }
            applied = applyPermission(root, *row,
                NovaTerm::Mcp::PermissionKind::ReadOutput, std::nullopt, read);
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
    explanation->setTextPixelSize(12);
    layout->addWidget(explanation);
    const auto storagePersistent = _controller->elevationCredentialsPersistent();
    auto* status = new ElaText(!storagePersistent.has_value()
        ? tr("存储：尚未确认。保存密码后显示实际存储方式。密码仅在本机设置，不对 MCP 客户端开放。")
        : *storagePersistent ? tr("存储：系统凭据库。密码仅在本机设置，不对 MCP 客户端开放。")
        : tr("存储：仅本次运行。密码仅在本机设置，不对 MCP 客户端开放。"), dialog);
    status->setObjectName(QStringLiteral("mcpElevationCredentialStorageStatus"));
    status->setWordWrap(true);
    status->setTextPixelSize(12);
    layout->addWidget(status);
    for (const auto& method : {QStringLiteral("sudo"), QStringLiteral("su")}) {
        auto* title = new ElaText(method == "sudo" ? tr("sudo 认证密码") : tr("su root 密码"), dialog);
        title->setTextPixelSize(13);
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
                const auto persistent = controller->elevationCredentialsPersistent();
                status->setText(ok
                    ? (!persistent.has_value()
                        ? QObject::tr("已更新，存储方式尚未确认。托管提权授权已撤销，请按需重新授权。")
                        : *persistent ? QObject::tr("已更新系统凭据库。托管提权授权已撤销，请按需重新授权。")
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
    const int terminalColumn = Model::index(Model::Column::TerminalInput);
    const int elevationColumn = Model::index(Model::Column::Elevation);
    const int clearColumn = Model::index(Model::Column::ClearTerminal);
    root->setData(0, Qt::UserRole + 4,
        QVariant::fromValue<qulonglong>(updated.revision.value()));
    // 服务端级联结果回写控件；整个过程屏蔽信号，避免在 itemChanged 栈内递归。
    const QSignalBlocker blocker(_sessions);
    root->setCheckState(readColumn, updated.selection.read ? Qt::Checked : Qt::Unchecked);
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
    _status->setText(tr("Operation failed. Check access settings, storage permissions, and active commands."));
}
void McpSettingsDialog::refresh()
{
    if (_refreshing || !_service) return;
    _refreshing = true;
    const QSignalBlocker sessionsBlocker(_sessions), enabledBlocker(_enabled);
    _enabled->setChecked(_controller->enabled());
    QString state = _service->status();
    if (state == "Disabled") state = tr("Disabled");
    else if (state == "Listening") state = tr("Listening");
    else if (state == "Starting") state = tr("Starting");
    QString statusText = tr("Status: %1; instance: %2").arg(state, _service->instanceId());
    if (!_service->httpEndpoint().isEmpty()) statusText += '\n' + _service->httpEndpoint();
    _status->setText(statusText);
    const auto clientId = _service->access().accessId();
    _accessIdentity = _service->access().identityVersion(clientId);
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
        item->setCheckState(terminalColumn,
            row.selection.fullTerminalInput ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(terminalColumn, tr("允许所有使用此令牌的 AI 在本次连接输入文本、按键并执行命令。新建或重连后的连接默认允许，可在此取消授权。"));
        item->setCheckState(clearColumn,
            row.selection.clearTerminal ? Qt::Checked : Qt::Unchecked);
        item->setData(clearColumn, Qt::UserRole, row.clearTerminalSelectable);
        item->setToolTip(clearColumn, tr("须先共享读取且会话已连接。独立允许清空本机屏幕或滚动历史，影响人的本机显示；不向设备发送字节、不停止远端程序，也不删除独立历史录制。新建或重连后的连接默认允许，可在此取消授权。"));
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
        credentials->setToolTip(!row.credentialsPersistent.has_value()
            ? tr("密码存储方式尚未确认；保存后显示实际存储方式。保存不代表授权使用，也不启用认证适配器。")
            : *row.credentialsPersistent ? tr("密码保存在系统凭据库；保存不代表授权使用，也不启用认证适配器。")
            : tr("密码仅本次运行保留；保存不代表授权使用，也不启用认证适配器。"));
        connect(credentials, &QPushButton::clicked, this,
            [this, id = row.key.sessionId] { editElevationCredentials(id); });
        _sessions->setItemWidget(item, credentialsColumn, credentials);
        QString operation = row.terminalOwner.isEmpty() ? tr("空闲") : tr("Agent 操作");
        if (row.controlState == "preempted") operation = tr("用户接管");
        if (!row.commandState.isEmpty()) operation += " / " + row.commandState;
        item->setText(operationColumn, operation);
    }
    _sessions->setEnabled(!clientId.isEmpty() && _controller->enabled());
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
}
