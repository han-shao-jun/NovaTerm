/**
 * @file   MainWindow.cpp
 * @brief  主窗口实现：菜单构建、会话对话框与语言切换。
 *
 * 构建标题栏图标菜单，连接 TerminalPage 的会话请求信号弹出 SessionPage
 * 对话框。changeEvent 处理 LanguageChange 时调用 retranslateUi() 刷新菜单文本。
 */
#include "MainWindow.h"
#include "ElaDialog.h"
#include "ElaIconButton.h"
#include "ElaMenu.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaToolTip.h"
#include "ElaTheme.h"
#include "ElaContentDialog.h"
#include "ElaMessageBar.h"
#include "ElaApplication.h"
#include "ElaTabWidget.h"
#include "ElaTabBar.h"
#include "ui/pages/TerminalPage.h"
#include "ui/pages/SettingsPage.h"
#include "ui/pages/SessionPage.h"
#include "ui/pages/AboutPage.h"
#include "ui/widgets/SessionPanel.h"
#include "ui/widgets/SftpPanel.h"
#include "ui/widgets/SystemMonitorPanel.h"
#include "service/LanguageManager.h"
#include "service/ConfigManager.h"
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QShowEvent>
#include <QTimerEvent>
#include <QVBoxLayout>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <limits>

namespace {

constexpr int DockLayoutStateVersion = 1;

class DockResizeHighlight final : public QWidget
{
public:
    explicit DockResizeHighlight(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.fillRect(
            rect(), ElaThemeColor(eTheme->getThemeMode(), PrimaryNormal));
    }
};

class DockDropOverlay final : public QWidget
{
public:
    explicit DockDropOverlay(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }

    void begin(Qt::DockWidgetAreas allowedAreas)
    {
        _allowedAreas = allowedAreas;
        _activeArea = Qt::NoDockWidgetArea;
        syncGeometry();
        show();
        raise();
        update();
    }

    void updateTarget(const QPoint& globalPosition)
    {
        syncGeometry();
        raise();

        // 将主窗口四条边划为停靠命中区。左右区域优先，避免鼠标位于
        // 窗口角落时同时命中横向和纵向区域而产生抖动。
        Qt::DockWidgetArea area = Qt::NoDockWidgetArea;
        const QPoint position = mapFromGlobal(globalPosition);
        if (rect().contains(position)) {
            if (_allowedAreas.testFlag(Qt::LeftDockWidgetArea)
                && leftTargetRect().contains(position)) {
                area = Qt::LeftDockWidgetArea;
            } else if (_allowedAreas.testFlag(Qt::RightDockWidgetArea)
                       && rightTargetRect().contains(position)) {
                area = Qt::RightDockWidgetArea;
            } else if (_allowedAreas.testFlag(Qt::TopDockWidgetArea)
                       && topTargetRect().contains(position)) {
                area = Qt::TopDockWidgetArea;
            } else if (_allowedAreas.testFlag(Qt::BottomDockWidgetArea)
                       && bottomTargetRect().contains(position)) {
                area = Qt::BottomDockWidgetArea;
            }
        }

        if (_activeArea == area)
            return;

        _activeArea = area;
        update();
    }

    void finish()
    {
        _activeArea = Qt::NoDockWidgetArea;
        hide();
    }

    [[nodiscard]] Qt::DockWidgetArea activeArea() const noexcept
    {
        return _activeArea;
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(0, 0, 0, 42));

        QColor accent = palette().color(QPalette::Highlight);
        if (!accent.isValid())
            accent = QColor(0, 120, 212);

        if (_allowedAreas.testFlag(Qt::LeftDockWidgetArea))
            paintTarget(painter, leftTargetRect(), Qt::LeftDockWidgetArea,
                        accent);
        if (_allowedAreas.testFlag(Qt::RightDockWidgetArea))
            paintTarget(painter, rightTargetRect(), Qt::RightDockWidgetArea,
                        accent);
        if (_allowedAreas.testFlag(Qt::TopDockWidgetArea))
            paintTarget(painter, topTargetRect(), Qt::TopDockWidgetArea,
                        accent);
        if (_allowedAreas.testFlag(Qt::BottomDockWidgetArea))
            paintTarget(painter, bottomTargetRect(), Qt::BottomDockWidgetArea,
                        accent);
    }

private:
    [[nodiscard]] int targetWidth() const noexcept
    {
        constexpr int minimumTargetWidth = 96;
        constexpr int maximumTargetWidth = 360;
        return std::clamp(width() / 4, minimumTargetWidth,
                          maximumTargetWidth);
    }

    [[nodiscard]] QRect leftTargetRect() const noexcept
    {
        return QRect(0, 0, targetWidth(), height());
    }

    [[nodiscard]] QRect rightTargetRect() const noexcept
    {
        const int zoneWidth = targetWidth();
        return QRect(width() - zoneWidth, 0, zoneWidth, height());
    }

    [[nodiscard]] int targetHeight() const noexcept
    {
        // 上下停靠提示随窗口高度缩放，同时限制尺寸，避免过小难以命中
        // 或过大遮挡中央终端内容。
        constexpr int minimumTargetHeight = 72;
        constexpr int maximumTargetHeight = 240;
        return std::clamp(height() / 4, minimumTargetHeight,
                          maximumTargetHeight);
    }

    [[nodiscard]] QRect topTargetRect() const noexcept
    {
        return QRect(0, 0, width(), targetHeight());
    }

    [[nodiscard]] QRect bottomTargetRect() const noexcept
    {
        const int zoneHeight = targetHeight();
        return QRect(0, height() - zoneHeight, width(), zoneHeight);
    }

    void syncGeometry()
    {
        if (const QWidget* const host = parentWidget())
            setGeometry(host->contentsRect());
    }

    void paintTarget(QPainter& painter, const QRect& target,
                     Qt::DockWidgetArea area, const QColor& accent)
    {
        const bool active = _activeArea == area;
        QColor fill = accent;
        fill.setAlpha(active ? 92 : 34);
        painter.fillRect(target, fill);

        QColor outline = accent;
        outline.setAlpha(active ? 230 : 105);
        painter.setPen(QPen(outline, active ? 2.0 : 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(target.adjusted(1, 1, -1, -1));

        const QPoint center = target.center();
        constexpr int arrowHalfHeight = 12;
        constexpr int arrowWidth = 8;
        QPolygon chevron;
        if (area == Qt::LeftDockWidgetArea
            || area == Qt::RightDockWidgetArea) {
            const int direction = area == Qt::LeftDockWidgetArea ? -1 : 1;
            chevron = QPolygon({
                QPoint(center.x() - direction * arrowWidth,
                       center.y() - arrowHalfHeight),
                center,
                QPoint(center.x() - direction * arrowWidth,
                       center.y() + arrowHalfHeight)});
        } else {
            const int direction = area == Qt::TopDockWidgetArea ? -1 : 1;
            chevron = QPolygon({
                QPoint(center.x() - arrowHalfHeight,
                       center.y() - direction * arrowWidth),
                center,
                QPoint(center.x() + arrowHalfHeight,
                       center.y() - direction * arrowWidth)});
        }
        painter.setPen(QPen(outline, active ? 3.0 : 2.0,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolyline(chevron);
    }

    Qt::DockWidgetAreas _allowedAreas{Qt::NoDockWidgetArea};
    Qt::DockWidgetArea _activeArea{Qt::NoDockWidgetArea};
};

class DraggableDockWidget final : public QDockWidget
{
public:
    explicit DraggableDockWidget(const QString& title, QWidget* parent = nullptr)
        : QDockWidget(title, parent),
          _dropOverlay(new DockDropOverlay(parent)),
          _dockHost(qobject_cast<QMainWindow*>(parent))
    {
        _titleBar = new QWidget(this);
        _titleBar->setCursor(Qt::SizeAllCursor);
        _titleBar->setFocusPolicy(Qt::NoFocus);
        _titleBar->setMouseTracking(true);
        _titleBar->setMinimumHeight(
            style()->pixelMetric(QStyle::PM_TitleBarHeight, nullptr, this));
        _titleBar->installEventFilter(this);

        auto* titleLayout = new QHBoxLayout(_titleBar);
        titleLayout->setContentsMargins(4, 0, 8, 0);
        titleLayout->setSpacing(4);

        _titleDragHandle = new ElaIconButton(
            ElaIconType::GripVertical, 12, 16, 16, _titleBar);
        _titleDragHandle->setAttribute(Qt::WA_TransparentForMouseEvents);
        _titleDragHandle->setFocusPolicy(Qt::NoFocus);
        _titleDragHandle->setOpacity(0.65);
        _titleDragHandle->setAccessibleName(title);
        titleLayout->addWidget(_titleDragHandle, 0, Qt::AlignVCenter);

        _titleLabel = new QLabel(title, _titleBar);
        _titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        _titleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        titleLayout->addWidget(_titleLabel, 1);

        // 使用自定义标题栏后，QDockWidget 不再自动绘制关闭按钮，
        // 因此在标题栏中补回关闭入口，并随 DockWidgetClosable 同步显隐。
        _titleCloseButton = new ElaIconButton(
            ElaIconType::Xmark, 11, 24, 24, _titleBar);
        _titleCloseButton->setObjectName(QStringLiteral("dockCloseButton"));
        _titleCloseButton->setAccessibleName(tr("Close panel"));
        _titleCloseButton->setToolTip(tr("Close panel"));
        titleLayout->addWidget(_titleCloseButton, 0, Qt::AlignVCenter);
        setTitleBarWidget(_titleBar);

        connect(_titleCloseButton, &QPushButton::clicked,
                this, &QDockWidget::close);
        connect(this, &QDockWidget::featuresChanged, this,
                [this](QDockWidget::DockWidgetFeatures features) {
            _titleCloseButton->setVisible(
                features.testFlag(QDockWidget::DockWidgetClosable));
        });

        connect(this, &QDockWidget::windowTitleChanged, this,
                [this](const QString& windowTitle) {
            _titleDragHandle->setAccessibleName(windowTitle);
            _titleLabel->setText(windowTitle);
        });
        setMouseTracking(true);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched != _titleBar)
            return QDockWidget::eventFilter(watched, event);

        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick: {
            const auto* const mouseEvent = static_cast<QMouseEvent*>(event);
            const QPoint dockPosition = _titleBar->mapTo(
                this, mouseEvent->position().toPoint());
            QMouseEvent forwardedEvent(
                mouseEvent->type(), QPointF(dockPosition),
                mouseEvent->globalPosition(), mouseEvent->button(),
                mouseEvent->buttons(), mouseEvent->modifiers(),
                mouseEvent->pointingDevice());
            QApplication::sendEvent(this, &forwardedEvent);
            return true;
        }
        default:
            return QDockWidget::eventFilter(watched, event);
        }
    }

    bool event(QEvent* event) override
    {
        bool finishDragAfterDispatch = false;
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            const auto* const mouseEvent = static_cast<QMouseEvent*>(event);
            _dragPending = mouseEvent->button() == Qt::LeftButton
                && isTitleBarPosition(mouseEvent->position().toPoint());
            break;
        }
        case QEvent::MouseMove: {
            const auto* const mouseEvent = static_cast<QMouseEvent*>(event);
            if (_dragPending
                && mouseEvent->buttons().testFlag(Qt::LeftButton)) {
                beginDockDrag();
            }
            if (_dragging && _dropOverlay) {
                _dropOverlay->updateTarget(
                    mouseEvent->globalPosition().toPoint());
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            const auto* const mouseEvent = static_cast<QMouseEvent*>(event);
            finishDragAfterDispatch =
                mouseEvent->button() == Qt::LeftButton;
            break;
        }
        default:
            break;
        }

        const bool handled = QDockWidget::event(event);
        if (finishDragAfterDispatch) {
            endDockDrag();
        } else if (_dragging && _dropOverlay) {
            // Qt may re-stack the main-window children while unplugging the
            // dock. Re-raise the visual hint after native dock processing.
            _dropOverlay->updateTarget(QCursor::pos());
        }
        return handled;
    }

    void timerEvent(QTimerEvent* event) override
    {
        if (event->timerId() != _dragTimerId) {
            QDockWidget::timerEvent(event);
            return;
        }

        if (!QApplication::mouseButtons().testFlag(Qt::LeftButton)) {
            endDockDrag();
            return;
        }

        if (_dropOverlay)
            _dropOverlay->updateTarget(QCursor::pos());
    }

private:
    [[nodiscard]] bool isTitleBarPosition(const QPoint& position) const
    {
        if (!_titleBar || !_titleBar->isVisible())
            return false;

        // 直接使用自定义标题栏的真实几何范围，避免用内容区顶部坐标估算时
        // 把会话状态行和标题栏下方留白误计入拖动锚点。
        const QRect titleBarGeometry(
            _titleBar->mapTo(this, QPoint{}), _titleBar->size());
        return titleBarGeometry.contains(position);
    }

    void beginDockDrag()
    {
        if (_dragging || !_dropOverlay)
            return;

        _dragging = true;
        _managedDockAreas = allowedAreas();

        // QMainWindow normally paints and applies its own dock preview. Keep
        // QDockWidget for the final layout, but suspend native drop targets
        // while dragging so the custom overlay is the single source of truth.
        setAllowedAreas(Qt::NoDockWidgetArea);
        _dropOverlay->begin(_managedDockAreas);
        _dropOverlay->updateTarget(QCursor::pos());
        _dragTimerId = startTimer(16, Qt::PreciseTimer);
    }

    void endDockDrag()
    {
        _dragPending = false;
        if (!_dragging)
            return;

        _dragging = false;
        if (_dragTimerId != 0) {
            killTimer(_dragTimerId);
            _dragTimerId = 0;
        }

        const Qt::DockWidgetArea targetArea = _dropOverlay
            ? _dropOverlay->activeArea()
            : Qt::NoDockWidgetArea;
        setAllowedAreas(_managedDockAreas);

        if (_dropOverlay)
            _dropOverlay->finish();

        if (_dockHost && targetArea != Qt::NoDockWidgetArea
            && _managedDockAreas.testFlag(targetArea)) {
            _dockHost->addDockWidget(targetArea, this);
            setFloating(false);
        }
    }

    QPointer<DockDropOverlay> _dropOverlay;
    QPointer<QMainWindow> _dockHost;
    QWidget* _titleBar{nullptr};
    ElaIconButton* _titleDragHandle{nullptr};
    QLabel* _titleLabel{nullptr};
    ElaIconButton* _titleCloseButton{nullptr};
    Qt::DockWidgetAreas _managedDockAreas{Qt::NoDockWidgetArea};
    int _dragTimerId{0};
    bool _dragPending{false};
    bool _dragging{false};
};

} // namespace

MainWindow::MainWindow(QWidget* parent) : ElaWindow(parent)
{
    qDebug() << "main id: " << QThread::currentThreadId();
    // 主题切换 → 持久化到配置 + 同步 QPalette（必须在 initContent 之前
    // 连接，因为 TerminalView 在构造时也会连接 themeModeChanged，
    // Qt 按连接顺序调用处理函数；若本处理器后连接，TerminalView
    // 的 applyThemeColorScheme() 会先执行，此时 QApplication::palette()
    // 仍为旧主题色，导致滚动条捕获错误的调色板，呈现白色边框）
    connect(eTheme, &ElaTheme::themeModeChanged, this,
            [this](ElaThemeType::ThemeMode mode) {
        // 配置持久化：仅在用户手动切换时写入；程序化切换（auto 模式 /
        // 跟随系统）不应把配置里的 "auto" 覆盖成具体的 light/dark。
        if (!SettingsPage::s_themeProgrammaticChange) {
            ConfigManager::set("ui.theme",
                               mode == ElaThemeType::Dark ? "dark" : "light");
        }

        // QPalette 同步必须无条件执行（手动与程序化切换都要同步）：终端右侧
        // 的原生 QScrollBar 由 QApplication 调色板绘制，QTermWidget 在
        // setColorScheme() 时执行 _scrollBar->setPalette(QApplication::palette())。
        // 此前该同步与 ConfigManager 写入一起被 s_themeProgrammaticChange 的
        // return 跳过，导致 auto / 跟随系统切换时右侧滚动条残留旧主题色。

        // 同步 QPalette — 必须以干净的 QPalette() 为基准构造，
        // 不能从 app->palette() 复制，否则在上一次已切换为对端
        // 主题的调色板基础上修改，未覆盖的角色（Mid / Dark / Shadow
        // 等，恰为 QScrollBar 所使用）会残留旧主题色，表现为右侧黑边
        // 或白边。
        auto* app = static_cast<QApplication*>(QCoreApplication::instance());
        if (mode == ElaThemeType::Dark) {
            QPalette p;  // 干净基准（平台无关浅色默认值）
            // ElaTheme 深色 WindowBase: #202020, BasicBase: #343434
            p.setColor(QPalette::Window,          QColor(0x20, 0x20, 0x20));
            p.setColor(QPalette::Base,            QColor(0x34, 0x34, 0x34));
            p.setColor(QPalette::AlternateBase,   QColor(0x2A, 0x2A, 0x2A));
            p.setColor(QPalette::WindowText,      QColor(0xF0, 0xF0, 0xF0));
            p.setColor(QPalette::Text,            QColor(0xF0, 0xF0, 0xF0));
            p.setColor(QPalette::Button,          QColor(0x34, 0x34, 0x34));
            p.setColor(QPalette::ButtonText,      QColor(0xF0, 0xF0, 0xF0));
            // 派生色 — QScrollBar 等原生控件用这些角色绘制
            p.setColor(QPalette::Mid,             QColor(0x40, 0x40, 0x40));
            p.setColor(QPalette::Dark,            QColor(0x18, 0x18, 0x18));
            p.setColor(QPalette::Shadow,          QColor(0x10, 0x10, 0x10));
            p.setColor(QPalette::Light,           QColor(0x48, 0x48, 0x48));
            p.setColor(QPalette::Midlight,        QColor(0x3C, 0x3C, 0x3C));
            p.setColor(QPalette::Highlight,       QColor(0x00, 0x78, 0xD4));
            p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
            p.setColor(QPalette::BrightText,      QColor(0xFF, 0x44, 0x44));
            p.setColor(QPalette::Link,            QColor(0x4D, 0xA6, 0xFF));
            app->setPalette(p);
            setPalette(p); // 同步 MainWindow 独立 palette
        } else {
            QPalette p;  // 干净基准（平台无关浅色默认值）
            // ElaTheme 浅色 WindowBase: #ECECEC
            p.setColor(QPalette::Window, QColor(0xEC, 0xEC, 0xEC));
            p.setColor(QPalette::Base,   QColor(0xFF, 0xFF, 0xFF));
            app->setPalette(p);
            setPalette(p);
        }
    });
    initWindow();

    // 语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });

    // ── 关闭确认对话框 ──────────────────────────────────────────
    auto* closeDialog = new ElaContentDialog(this);
    closeDialog->setWindowTitle(tr("Exit"));
    closeDialog->setLeftButtonText(tr("Cancel"));
    closeDialog->setMiddleButtonText(tr("Minimize"));
    closeDialog->setRightButtonText(tr("Exit"));

    auto* closeCentral = new QWidget(closeDialog);
    auto* closeLayout = new QVBoxLayout(closeCentral);
    closeLayout->setContentsMargins(24, 20, 24, 20);
    auto* closeLabel = new QLabel(tr("Are you sure you want to exit NovaTerm?"), closeCentral);
    closeLabel->setWordWrap(true);
    closeLayout->addWidget(closeLabel);
    closeLayout->addStretch();
    closeDialog->setCentralWidget(closeCentral);
    // setCentralWidget() 内部已调用 adjustSize()，基于当前内容
    // 重新计算正确的 sizeHint，避免 exec() 时触发 QWindowsWindow 几何体警告。

    connect(closeDialog, &ElaContentDialog::rightButtonClicked, this, [closeDialog, this]() {
        closeDialog->done(QDialog::Accepted);
        QTimer::singleShot(0, this, &QWidget::close);
    });
    connect(closeDialog, &ElaContentDialog::middleButtonClicked, this, [=]() {
        closeDialog->done(QDialog::Accepted);
        showMinimized();
    });
    connect(closeDialog, &ElaContentDialog::leftButtonClicked, closeDialog,
            &ElaContentDialog::close);
    setIsDefaultClosed(false);
    connect(this, &MainWindow::closeButtonClicked, this, [=]() {
        closeDialog->exec();
    });
}

MainWindow::~MainWindow()
{
    // 正常关闭会在 closeEvent 中保存；这里作为应用直接销毁窗口时的兜底。
    saveWindowLayout();
}

void MainWindow::saveWindowLayout()
{
    if (_windowLayoutSaved)
        return;

    // 停靠状态包含各面板的停靠边、尺寸、顺序和可见性；使用固定版本号
    // 保存，后续若布局格式调整可递增版本并安全忽略旧数据。
    QVariantMap windowState{
        {QStringLiteral("window.width"), width()},
        {QStringLiteral("window.height"), height()},
        {QStringLiteral("window.dockState"),
         QString::fromLatin1(
             saveState(DockLayoutStateVersion).toBase64())}
    };
    // QMainWindow 只保存 Dock 的几何与可见性；快捷连接面板内部的折叠状态
    // 和折叠前宽度需要单独持久化，才能在下次启动时完整恢复。
    if (_sessionPanel) {
        windowState.insert(QStringLiteral("window.sessionPanelCollapsed"),
                           _sessionPanel->isCollapsed());
        windowState.insert(QStringLiteral("window.sessionPanelExpandedWidth"),
                           _sessionPanel->expandedWidth());
    }
    // 关闭时统一写入一次，防止多个字段分别保存造成布局状态不一致。
    ConfigManager::setValues(windowState);
    _windowLayoutSaved = true;
}

bool MainWindow::event(QEvent* event)
{
    const bool handled = ElaWindow::event(event);

    switch (event->type()) {
    case QEvent::HoverMove:
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::LayoutRequest:
    case QEvent::Resize:
        updateDockResizeHighlight(mapFromGlobal(QCursor::pos()));
        break;
    case QEvent::WindowDeactivate:
        _activeDockResizeKind = DockResizeKind::None;
        if (_dockResizeHighlight)
            _dockResizeHighlight->hide();
        break;
    default:
        break;
    }

    return handled;
}

void MainWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    ElaWindow::changeEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    ElaWindow::closeEvent(event);
    if (event->isAccepted()) {
        // 在窗口及 Dock 仍保持最终可见几何时保存，避免析构阶段取得已隐藏
        // 或已被 Qt 清理后的布局状态。
        saveWindowLayout();
    }
}

void MainWindow::showEvent(QShowEvent* event)
{
    ElaWindow::showEvent(event);
    if (_dockStateRestoredAfterShow || _dockStateForFirstShow.isEmpty())
        return;

    _dockStateRestoredAfterShow = true;
    QTimer::singleShot(0, this, [this]() {
        // ElaWindow/平台窗口在首次 show 后才完成最终中央区域几何。此时再
        // 恢复一次可防止构造阶段已经生效的 Dock 布局被晚期布局覆盖。
        if (!restoreState(_dockStateForFirstShow,
                          DockLayoutStateVersion)) {
            _dockStateForFirstShow.clear();
            ConfigManager::set(QStringLiteral("window.dockState"),
                               QString{});
            qWarning() << "首次显示后恢复面板布局失败，已清除无效状态";
            return;
        }

        // 快捷连接固定存在于左侧；旧状态中的隐藏标记不能覆盖该约束。
        if (_sessionDock)
            _sessionDock->setVisible(true);
    });
}

void MainWindow::retranslateUi()
{
    setWindowTitle(tr("NovaTerm"));

    if (_menuTip) _menuTip->setToolTip(tr("Menu"));
    if (_newSessionTip) _newSessionTip->setToolTip(tr("New session"));
    if (_newSessionButton) {
        _newSessionButton->setAccessibleName(tr("New session"));
        _newSessionButton->setToolTip(tr("New session"));
    }

    // 重新翻译弹出菜单项
    if (_actSession)  _actSession->setText(tr("Session"));
    if (_actSettings) _actSettings->setText(tr("Settings"));
    if (_actAbout)    _actAbout->setText(tr("About"));
    if (_localSessionAction)  _localSessionAction->setText(tr("Local"));
    if (_sshSessionAction)    _sshSessionAction->setText(tr("SSH"));
    if (_serialSessionAction) _serialSessionAction->setText(tr("Serial"));
    if (_telnetSessionAction) _telnetSessionAction->setText(tr("Telnet"));
    if (_sessionDock) _sessionDock->setWindowTitle(tr("Sessions"));
    if (_sftpDock) _sftpDock->setWindowTitle(tr("SFTP transfer"));
    if (_systemMonitorDock)
        _systemMonitorDock->setWindowTitle(tr("System resources"));
    for (QDockWidget* dock : {_sftpDock, _systemMonitorDock}) {
        if (!dock)
            continue;
        if (auto* closeButton = dock->findChild<ElaIconButton*>(
                QStringLiteral("dockCloseButton"))) {
            closeButton->setAccessibleName(tr("Close panel"));
            closeButton->setToolTip(tr("Close panel"));
        }
    }
    if (_toggleSftpPanelAction)
        _toggleSftpPanelAction->setText(tr("SFTP panel"));
    if (_toggleSystemMonitorAction)
        _toggleSystemMonitorAction->setText(tr("System resources panel"));
}

void MainWindow::initWindow()
{
    setWindowTitle(tr("NovaTerm"));
    setAttribute(Qt::WA_Hover);

    int w = ConfigManager::get<int>("window.width", 1280);
    int h = ConfigManager::get<int>("window.height", 800);
    resize(w, h);

    setAppBarHeight(32);

    // 隐藏用户信息卡片
    setUserInfoCardVisible(false);

    //隐藏导航栏
    setIsNavigationBarEnable(false);
    // 开启嵌套后，SFTP 与资源监视面板才能在同一侧横向或纵向组合。
    setDockNestingEnabled(true);
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    // ── 标题栏左侧：仅显示应用 Logo + 菜单按钮 ──
    // 保留窗口图标用于任务栏 / Alt-Tab；标题栏上可见的 Logo 由下方的
    // QLabel 自行绘制，以便控制其尺寸。
    setWindowIcon(QIcon(":/icons/app_logo.svg"));

    // 移除默认的左侧控件：
    //   • 导航切换按钮（ElaWindow 默认启用，但我们的导航栏已隐藏）
    //   • 路由前进/后退按钮
    //   • AppBar 内置图标槽（18px，太小）和标题文本
    //     （两者都是 AppBar 的直接子 QLabel；窗口标题本身保留给任务栏）
    setWindowButtonFlag(ElaAppBarType::NavigationButtonHint, false);
    setWindowButtonFlag(ElaAppBarType::RouteBackButtonHint, false);
    setWindowButtonFlag(ElaAppBarType::RouteForwardButtonHint, false);
    if (auto* appBar = findChild<ElaAppBar*>()) {
        auto hideAppBarLabels = [appBar]() {
            // ElaText 继承自 QLabel，因此同时覆盖图标标签和标题标签。
            const auto labels = appBar->findChildren<QLabel*>(
                QString(), Qt::FindDirectChildrenOnly);
            for (auto* label : labels)
                label->hide();
        };
        hideAppBarLabels();
        // AppBar 在 windowTitleChanged 时会重新显示标题标签；需要重新隐藏。
        connect(this, &QWidget::windowTitleChanged, appBar,
                [hideAppBarLabels](const QString&) { hideAppBarLabels(); });
    }

    // ═══════════════════════════════════════════════════════════════
    // 菜单图标，放置在 LeftArea 中使其位于 Logo 右侧。
    //   • 悬停  → ElaToolTip "Menu"
    //   • 点击  → 弹出 ElaMenu，包含会话、侧栏显隐、设置与关于
    // ═══════════════════════════════════════════════════════════════
    _menuButton = new ElaIconButton(ElaIconType::Bars, 18, 32, 32, this);
    // ElaToolTip（非原生 QToolTip）：其背景使用主题的 PopupBase/PopupBorder
    // 绘制，并已绑定 eTheme->themeModeChanged，因此自动跟随全局亮色/暗色主题。
    _menuTip = new ElaToolTip(_menuButton);
    _menuTip->setToolTip(tr("Menu"));

    buildMainMenu();
    connect(_menuButton, &QPushButton::clicked, this, [this]() {
        _mainMenu->popup(_menuButton->mapToGlobal(QPoint(0, _menuButton->height() + 2)));
    });

    _newSessionButton = new ElaIconButton(
        ElaIconType::Plus, 14, 32, 32, this);
    _newSessionButton->setAccessibleName(tr("New session"));
    _newSessionButton->setToolTip(tr("New session"));
    _newSessionTip = new ElaToolTip(_newSessionButton);
    _newSessionTip->setToolTip(tr("New session"));
    buildNewSessionMenu();
    _newSessionButton->setMenu(_newSessionMenu);

    QWidget* customWidget = new QWidget(this);
    QHBoxLayout* customLayout = new QHBoxLayout(customWidget);
    // 左边距为零，使 Logo 紧贴窗口左边缘
    // （AppBar 自身左侧布局为空 —— 其所有控件均已隐藏）。
    customLayout->setContentsMargins(0, 0, 0, 0);
    customLayout->setSpacing(6);

    // 应用 Logo，尺寸与 36px 菜单按钮匹配（26px 图标放在 26 宽的槽中，
    // 使其左对齐，并由标题栏布局保持垂直居中）。
    auto* logoLabel = new QLabel(customWidget);
    logoLabel->setPixmap(QIcon(":/icons/app_logo.svg").pixmap(26, 26));
    logoLabel->setFixedSize(26, 32);
    logoLabel->setAlignment(Qt::AlignCenter);
    customLayout->addWidget(logoLabel);

    customLayout->addWidget(_menuButton);
    customLayout->addWidget(_newSessionButton);
    customLayout->addStretch();
    setCustomWidget(ElaAppBarType::LeftArea, customWidget, this, "processHitTest");

    _terminalPage = new TerminalPage(this);

    // 1 号区域：会话面板固定在左侧。标题已由面板内部绘制，因此隐藏
    // QDockWidget 标题栏，并禁止用户将它拖离快捷连接区域。
    _sessionDock = new QDockWidget(tr("Sessions"), this);
    _sessionDock->setObjectName(QStringLiteral("sessionDock"));
    _sessionDock->setAllowedAreas(Qt::LeftDockWidgetArea);
    _sessionDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    auto* hiddenSessionTitleBar = new QWidget(_sessionDock);
    hiddenSessionTitleBar->setFixedHeight(0);
    _sessionDock->setTitleBarWidget(hiddenSessionTitleBar);

    _sessionPanel = new SessionPanel(_sessionDock);
    _sessionPanel->setCursor(Qt::ArrowCursor);
    _sessionDock->setWidget(_sessionPanel);
    addDockWidget(Qt::LeftDockWidgetArea, _sessionDock);

    // 2 号区域：SFTP 是工具面板，允许停靠到窗口任意一边，也允许关闭。
    _sftpDock = new DraggableDockWidget(tr("SFTP transfer"), this);
    _sftpDock->setObjectName(QStringLiteral("sftpDock"));
    _sftpDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    _sftpDock->setFeatures(QDockWidget::DockWidgetClosable
                           | QDockWidget::DockWidgetMovable);
    _sftpPanel = new SftpPanel(_sftpDock);
    _sftpDock->setWidget(_sftpPanel);
    addDockWidget(Qt::LeftDockWidgetArea, _sftpDock);
    // 默认将 SFTP 放在会话区与中央终端之间，对应参考图的初始顺序。
    splitDockWidget(_sessionDock, _sftpDock, Qt::Horizontal);

    // 4 号区域：系统资源监视默认停靠右侧，并与 SFTP 使用相同的
    // 四边停靠和关闭规则；3 号终端始终由中央页面承载，不参与拖动。
    _systemMonitorDock = new DraggableDockWidget(
        tr("System resources"), this);
    _systemMonitorDock->setObjectName(QStringLiteral("systemMonitorDock"));
    _systemMonitorDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    _systemMonitorDock->setFeatures(QDockWidget::DockWidgetClosable
                                    | QDockWidget::DockWidgetMovable);
    _systemMonitorPanel = new SystemMonitorPanel(_systemMonitorDock);
    _systemMonitorDock->setWidget(_systemMonitorPanel);
    addDockWidget(Qt::RightDockWidgetArea, _systemMonitorDock);

    resizeDocks({_sessionDock, _sftpDock, _systemMonitorDock},
                {260, 260, 290}, Qt::Horizontal);

    _dockResizeHighlight = new DockResizeHighlight(this);

    // 鼠标移动事件通常由面板内部子控件接收，MainWindow 不一定能收到。
    // 使用低频轮询统一检查全局指针位置，确保离开可调整边框后及时取消高亮。
    auto* resizeHoverTimer = new QTimer(this);
    resizeHoverTimer->setInterval(40);
    connect(resizeHoverTimer, &QTimer::timeout, this, [this]() {
        const QPoint globalPosition = QCursor::pos();
        if (!isVisible() || isMinimized() || !isActiveWindow()
            || !frameGeometry().contains(globalPosition)) {
            if (!QApplication::mouseButtons().testFlag(Qt::LeftButton))
                _activeDockResizeKind = DockResizeKind::None;
            if (_dockResizeHighlight)
                _dockResizeHighlight->hide();
            return;
        }
        updateDockResizeHighlight(mapFromGlobal(globalPosition));
    });
    resizeHoverTimer->start();

    connect(_sessionPanel, &SessionPanel::newSessionRequested,
            this, qOverload<>(&MainWindow::showSessionDialog));
    connect(_sessionPanel, &SessionPanel::panelWidthChangeRequested, this,
            [this](int width) {
        // 面板内部折叠按钮只负责状态切换，实际 Dock 宽度由 MainWindow
        // 统一调整，避免直接修改 Dock 几何破坏 QMainWindow 布局。
        resizeDocks({_sessionDock}, {width}, Qt::Horizontal);
    });
    connect(_sessionPanel, &SessionPanel::editSessionRequested, this,
            &MainWindow::editSession);
    connect(_sessionPanel, &SessionPanel::localReconnectRequested, this,
            [this](TerminalView::LocalShellType type) {
        _terminalPage->addTerminalTab(tr("Terminal"), type);
    });
    connect(_sessionPanel, &SessionPanel::serialReconnectRequested, this,
            [this](const SerialConfig& config) {
        _terminalPage->addSerialTerminalTab(config);
    });
    connect(_sessionPanel, &SessionPanel::sshReconnectRequested, this,
            [this](const SshConfig& config) {
        _terminalPage->addSshTerminalTab(config);
    });
    connect(_sessionPanel, &SessionPanel::reconnectUnavailable, this,
            [this](const QString& message) {
        QMessageBox::warning(this, tr("Reconnect session"), message);
    });

    connect(_terminalPage, &TerminalPage::currentSftpContextChanged,
            _sftpPanel, &SftpPanel::setSessionContext);
    connect(_terminalPage, &TerminalPage::currentSessionContextChanged,
            _systemMonitorPanel, &SystemMonitorPanel::setSessionContext);

    // 先应用快捷面板内部状态。setExpandedWidth()/setCollapsed() 会触发
    // resizeDocks，因此必须放在 restoreState() 之前，不能覆盖恢复后的尺寸。
    _sessionPanel->setExpandedWidth(ConfigManager::get<int>(
        QStringLiteral("window.sessionPanelExpandedWidth"), 260));
    _sessionPanel->setCollapsed(ConfigManager::get<bool>(
        QStringLiteral("window.sessionPanelCollapsed"), false));

    // 先完成中央页面注册，确保恢复布局之后不再有初始化操作重排主窗口。
    addPageNode(tr("Terminal"), _terminalPage, ElaIconType::Terminal);

    // 所有 Dock、中央页面及尺寸约束都准备完成后，最后恢复面板位置。
    const QByteArray defaultDockState = saveState(DockLayoutStateVersion);
    const QString savedDockState = ConfigManager::get<QString>(
        QStringLiteral("window.dockState"));
    if (!savedDockState.isEmpty()) {
        const auto decodedState = QByteArray::fromBase64Encoding(
            savedDockState.toLatin1(),
            QByteArray::AbortOnBase64DecodingErrors);
        const bool restored = decodedState && !decodedState.decoded.isEmpty()
            && restoreState(decodedState.decoded, DockLayoutStateVersion);
        if (restored) {
            // 首次 show 后 ElaWindow 还会完成内部几何调整，因此保留一份
            // 已校验状态，待窗口真正显示后再进行最终恢复。
            _dockStateForFirstShow = decodedState.decoded;
        } else {
            // Base64 损坏、Qt 状态版本不匹配或内容不完整时，回退到创建
            // Dock 后保存的默认布局，并清除无效值，避免每次启动重复失败。
            restoreState(defaultDockState, DockLayoutStateVersion);
            ConfigManager::set(QStringLiteral("window.dockState"),
                               QString{});
            qWarning() << "面板布局配置无效，已恢复默认布局";
        }
    }

    // 快捷连接面板不再由顶栏菜单呼出，因此旧配置即使将其隐藏，启动后
    // 也必须恢复可见；该操作只改变显隐，不再调整已恢复的停靠结构。
    _sessionDock->setVisible(true);

    // 顶栏菜单只管理可关闭的工具面板；快捷连接使用自身标题栏按钮折叠。
    const auto bindPanelAction = [](QAction* action, QDockWidget* dock) {
        QObject::connect(action, &QAction::toggled, dock,
                         &QWidget::setVisible);
        QObject::connect(dock, &QDockWidget::visibilityChanged, action,
                         &QAction::setChecked);
        action->setChecked(dock->isVisible());
    };
    bindPanelAction(_toggleSftpPanelAction, _sftpDock);
    bindPanelAction(_toggleSystemMonitorAction, _systemMonitorDock);

}

void MainWindow::updateDockResizeHighlight(const QPoint& position)
{
    if (!_dockResizeHighlight) {
        return;
    }

    // 只使用当前可见、非浮动的 Dock 与中央区域计算相邻关系。旧实现直接
    // 取会话面板和中央区域的中点，二者之间插入 SFTP 后会把提示画进面板内部。
    QList<QWidget*> layoutWidgets;
    if (QWidget* const central = QMainWindow::centralWidget();
        central && central->isVisible()) {
        layoutWidgets.append(central);
    }
    const auto docks = findChildren<QDockWidget*>(
        QString(), Qt::FindDirectChildrenOnly);
    for (QDockWidget* dock : docks) {
        if (dock->isVisible() && !dock->isFloating())
            layoutWidgets.append(dock);
    }

    if (layoutWidgets.size() < 2) {
        _dockResizeHighlight->hide();
        return;
    }

    const int separatorExtent = std::max(
        1, style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent,
                                nullptr, this));
    const int maximumSeparatorGap = std::max(12, separatorExtent * 2);
    const int hitHalfWidth = std::max(2, (separatorExtent + 1) / 2);
    constexpr int minimumSharedExtent = 24;
    constexpr int highlightWidth = 3;

    struct ResizeAnchor {
        DockResizeKind kind{DockResizeKind::None};
        int coordinate = 0;
        int rangeStart = 0;
        int rangeEnd = 0;
        QRect hoverRect;
    };
    QList<ResizeAnchor> resizeAnchors;

    // 分别检查左右相邻与上下相邻关系：前者调整宽度，后者调整高度。
    // 两类锚点独立收集，不能因其中一个方向不相邻而跳过另一个方向。
    for (qsizetype first = 0; first < layoutWidgets.size(); ++first) {
        const QWidget* const firstWidget = layoutWidgets.at(first);
        const QRect firstRect(firstWidget->mapTo(this, QPoint()),
                              firstWidget->size());

        for (qsizetype second = first + 1;
             second < layoutWidgets.size(); ++second) {
            const QWidget* const secondWidget = layoutWidgets.at(second);
            const QRect secondRect(secondWidget->mapTo(this, QPoint()),
                                   secondWidget->size());

            const QRect& leftRect = firstRect.center().x()
                <= secondRect.center().x() ? firstRect : secondRect;
            const QRect& rightRect = firstRect.center().x()
                <= secondRect.center().x() ? secondRect : firstRect;
            const int gap = rightRect.left() - leftRect.right() - 1;
            if (gap >= -1 && gap <= maximumSeparatorGap) {
                const int sharedTop = std::max(leftRect.top(),
                                               rightRect.top());
                const int sharedBottom = std::min(leftRect.bottom(),
                                                  rightRect.bottom());
                const int sharedHeight = sharedBottom - sharedTop + 1;
                if (sharedHeight >= minimumSharedExtent) {
                    const int separatorX = (leftRect.right()
                                            + rightRect.left()) / 2;
                    const QRect hoverRect(
                        separatorX - hitHalfWidth, sharedTop,
                        hitHalfWidth * 2 + 1, sharedHeight);
                    resizeAnchors.append({DockResizeKind::Width,
                                          separatorX, sharedTop,
                                          sharedBottom, hoverRect});
                }
            }

            const QRect& upperRect = firstRect.center().y()
                <= secondRect.center().y() ? firstRect : secondRect;
            const QRect& lowerRect = firstRect.center().y()
                <= secondRect.center().y() ? secondRect : firstRect;
            const int verticalGap = lowerRect.top() - upperRect.bottom() - 1;
            if (verticalGap >= -1
                && verticalGap <= maximumSeparatorGap) {
                const int sharedLeft = std::max(upperRect.left(),
                                                lowerRect.left());
                const int sharedRight = std::min(upperRect.right(),
                                                 lowerRect.right());
                const int sharedWidth = sharedRight - sharedLeft + 1;
                if (sharedWidth >= minimumSharedExtent) {
                    const int separatorY = (upperRect.bottom()
                                            + lowerRect.top()) / 2;
                    const QRect horizontalHoverRect(
                        sharedLeft, separatorY - hitHalfWidth,
                        sharedWidth, hitHalfWidth * 2 + 1);
                    resizeAnchors.append({DockResizeKind::Height,
                                          separatorY, sharedLeft,
                                          sharedRight,
                                          horizontalHoverRect});
                }
            }
        }
    }

    const bool leftButtonDown = QApplication::mouseButtons().testFlag(
        Qt::LeftButton);
    if (!leftButtonDown)
        _activeDockResizeKind = DockResizeKind::None;

    const ResizeAnchor* selectedAnchor = nullptr;
    int selectedDistance = std::numeric_limits<int>::max();
    for (const ResizeAnchor& anchor : resizeAnchors) {
        if (!anchor.hoverRect.contains(position))
            continue;

        const int distance = anchor.kind == DockResizeKind::Width
            ? qAbs(position.x() - anchor.coordinate)
            : qAbs(position.y() - anchor.coordinate);
        if (distance < selectedDistance) {
            selectedAnchor = &anchor;
            selectedDistance = distance;
        }
    }

    // 开始拖动后，Qt 会持续改变 Dock 几何；即使指针短暂偏离窄小的
    // 悬浮命中区，也选择同方向且最近的分隔线，使提示在拖动期间不中断。
    if (leftButtonDown && _activeDockResizeKind != DockResizeKind::None) {
        selectedAnchor = nullptr;
        selectedDistance = std::numeric_limits<int>::max();
        for (const ResizeAnchor& anchor : resizeAnchors) {
            if (anchor.kind != _activeDockResizeKind)
                continue;

            const int rangePosition = anchor.kind == DockResizeKind::Width
                ? position.y() : position.x();
            if (rangePosition < anchor.rangeStart - hitHalfWidth
                || rangePosition > anchor.rangeEnd + hitHalfWidth) {
                continue;
            }

            const int distance = anchor.kind == DockResizeKind::Width
                ? qAbs(position.x() - anchor.coordinate)
                : qAbs(position.y() - anchor.coordinate);
            if (distance < selectedDistance) {
                selectedAnchor = &anchor;
                selectedDistance = distance;
            }
        }
    } else if (leftButtonDown && selectedAnchor) {
        _activeDockResizeKind = selectedAnchor->kind;
    }

    if (selectedAnchor) {
        int mergedStart = selectedAnchor->rangeStart;
        int mergedEnd = selectedAnchor->rangeEnd;
        const int sameSeparatorTolerance = std::max(1, separatorExtent / 2);

        // 同一侧的多个 Dock 上下堆叠时，它们和中央区域共享一条竖向分隔线。
        // 水平或竖直的同轴分段均合并，保证提示覆盖完整的可调整边框。
        for (const ResizeAnchor& anchor : resizeAnchors) {
            if (anchor.kind != selectedAnchor->kind
                || qAbs(anchor.coordinate - selectedAnchor->coordinate)
                > sameSeparatorTolerance) {
                continue;
            }
            mergedStart = std::min(mergedStart, anchor.rangeStart);
            mergedEnd = std::max(mergedEnd, anchor.rangeEnd);
        }

        if (selectedAnchor->kind == DockResizeKind::Width) {
            _dockResizeHighlight->setGeometry(
                selectedAnchor->coordinate - highlightWidth / 2,
                mergedStart, highlightWidth,
                mergedEnd - mergedStart + 1);
        } else {
            _dockResizeHighlight->setGeometry(
                mergedStart,
                selectedAnchor->coordinate - highlightWidth / 2,
                mergedEnd - mergedStart + 1, highlightWidth);
        }
        _dockResizeHighlight->show();
        _dockResizeHighlight->raise();
        return;
    }

    // 指针不在任何真实分隔锚点上时必须隐藏，不能保留上一次悬浮状态。
    _dockResizeHighlight->hide();
}

void MainWindow::buildMainMenu()
{
    _mainMenu = new ElaMenu(this);
    _mainMenu->setMenuItemHeight(27);

    // ── 会话：打开会话/连接选择器 ──
    _actSession = _mainMenu->addElaIconAction(ElaIconType::Terminal, tr("Session"));
    connect(_actSession, &QAction::triggered, this,
            [this]() { showSessionDialog(); });

    // 顶栏菜单仅管理可拖动的 SFTP 与资源监视面板。快捷连接面板通过
    // 自身标题栏按钮折叠，中央终端则始终保留。
    _mainMenu->addSeparator();
    _toggleSftpPanelAction = _mainMenu->addElaIconAction(
        ElaIconType::FolderArrowUp, tr("SFTP panel"));
    _toggleSystemMonitorAction = _mainMenu->addElaIconAction(
        ElaIconType::Gauge, tr("System resources panel"));
    _toggleSftpPanelAction->setCheckable(true);
    _toggleSystemMonitorAction->setCheckable(true);
    _toggleSftpPanelAction->setChecked(true);
    _toggleSystemMonitorAction->setChecked(true);

    _mainMenu->addSeparator();

    // ── 设置：ElaDialog 内嵌现有 SettingsPage ──
    _actSettings = _mainMenu->addElaIconAction(ElaIconType::GearComplex, tr("Settings"));
    connect(_actSettings, &QAction::triggered, this, &MainWindow::showSettingsDialog);

    // ── 关于：模态对话框（行为不变）──
    _actAbout = _mainMenu->addElaIconAction(ElaIconType::CircleInfo, tr("About"));
    connect(_actAbout, &QAction::triggered, this, &MainWindow::showAboutDialog);
}

void MainWindow::buildNewSessionMenu()
{
    _newSessionMenu = new ElaMenu(_newSessionButton);
    _newSessionMenu->setMenuItemHeight(32);
    _localSessionAction = _newSessionMenu->addElaIconAction(
        ElaIconType::Terminal, tr("Local"));
    _sshSessionAction = _newSessionMenu->addElaIconAction(
        ElaIconType::NetworkWired, tr("SSH"));
    _serialSessionAction = _newSessionMenu->addElaIconAction(
        ElaIconType::UsbDrive, tr("Serial"));
    _telnetSessionAction = _newSessionMenu->addElaIconAction(
        ElaIconType::Globe, tr("Telnet"));

    connect(_localSessionAction, &QAction::triggered, this,
            [this]() { showSessionDialog(TransportKind::LocalShell); });
    connect(_sshSessionAction, &QAction::triggered, this,
            [this]() { showSessionDialog(TransportKind::Ssh); });
    connect(_serialSessionAction, &QAction::triggered, this,
            [this]() { showSessionDialog(TransportKind::Serial); });
    connect(_telnetSessionAction, &QAction::triggered, this,
            [this]() { showSessionDialog(TransportKind::Telnet); });
}


bool MainWindow::processHitTest()
{
    // 自定义区域中除菜单按钮外均可拖动。
    if (!_menuButton)
        return true;
    return !ElaApplication::containsCursorToItem(_menuButton)
        && (!_newSessionButton
            || !ElaApplication::containsCursorToItem(_newSessionButton));
}

// ═══════════════════════════════════════════════════════════════════
//  会话选择器 — 与 Settings 同模式的 ElaDialog + ElaTabWidget
// ═══════════════════════════════════════════════════════════════════

void MainWindow::showSessionDialog()
{
    showSessionDialog(TransportKind::LocalShell);
}

void MainWindow::showSessionDialog(TransportKind initialKind)
{
    runSessionDialog(initialKind, std::nullopt, std::nullopt, {});
}

void MainWindow::editSession(const SessionId& id,
                             const RuntimeConfig& runtime,
                             const QByteArray& secret)
{
    runSessionDialog(runtime.transportKind, id, runtime, secret);
}

void MainWindow::runSessionDialog(
    TransportKind initialKind,
    const std::optional<SessionId>& editingSessionId,
    const std::optional<RuntimeConfig>& initialConfig,
    const QByteArray& secret)
{
    if (!_sessionDialog) {
        _sessionDialog = new ElaDialog(this);
        _sessionDialog->setWindowTitle(tr("Session"));
        _sessionDialog->resize(1000, 600);
        _sessionDialog->setWindowModality(Qt::ApplicationModal);
        _sessionDialog->setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
        _sessionDialog->setAppBarHeight(30);

        // SessionPage 属于本次对话框调用。对话框在 exec() 返回后、终端/RHI
        // 控件创建前销毁，二者的字体与图形资源清理不会重叠。
        auto* sessionPage = new SessionPage(_sessionDialog);
        sessionPage->setTitleVisible(false);
        sessionPage->selectTransport(initialKind);
        if (initialConfig)
            sessionPage->applyRuntimeConfig(*initialConfig, secret);

        connect(sessionPage, &SessionPage::localSessionRequested, this,
                [this](TerminalView::LocalShellType type,
                       const QString& label) {
            qDebug() << "localSessionRequested, type ="
                     << (type == TerminalView::LocalShellType::PowerShell ? "PowerShell" : "cmd/Clink");
            // 在构造 QRhiWidget 前结束模态事件循环，避免顶层窗口切换到
            // RHI 合成时重入字体布局。
            _pendingLocalSession = LocalSessionParameters{type, label};
            _sessionDialog->accept();
        });
        connect(sessionPage, &SessionPage::serialSessionRequested, this,
                [this](const SerialConfig& config) {
            _pendingSerialSession = config;
            _sessionDialog->accept();
        });
        connect(sessionPage, &SessionPage::sshSessionRequested, this,
                [this](const SshConfig& config) {
            _pendingSshSession = config;
            _sessionDialog->accept();
        });
        connect(sessionPage, &SessionPage::dialogRejected,
                _sessionDialog, &QDialog::reject);

        auto* mainLayout = new QVBoxLayout(_sessionDialog);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->addWidget(sessionPage);
    }

    _pendingLocalSession.reset();
    _pendingSerialSession.reset();
    _pendingSshSession.reset();
    const int result = _sessionDialog->exec();
    // ElaDialog 及其自定义控件在 accept() 后复用不可靠：后续 QDialog::exec()
    // 在递归查找默认按钮时可能遇到残留项。在终端/RHI 初始化前同步销毁已隐藏
    // 的对话框。
    delete _sessionDialog;
    _sessionDialog = nullptr;

    if (result == QDialog::Accepted && _pendingLocalSession) {
        const LocalSessionParameters parameters = *_pendingLocalSession;
        _pendingLocalSession.reset();
        if (editingSessionId) {
            _sessionPanel->updateLocal(*editingSessionId, parameters.type,
                                       parameters.label);
        } else {
            _sessionPanel->recordLocal(parameters.type, parameters.label);
            const QString title = parameters.label.isEmpty()
                ? tr("Terminal") : parameters.label;
            _terminalPage->addTerminalTab(title, parameters.type);
        }
    } else if (result == QDialog::Accepted && _pendingSerialSession) {
        const SerialConfig config = *_pendingSerialSession;
        _pendingSerialSession.reset();
        if (editingSessionId)
            _sessionPanel->updateSerial(*editingSessionId, config);
        else {
            _sessionPanel->recordSerial(config);
            _terminalPage->addSerialTerminalTab(config);
        }
    } else if (result == QDialog::Accepted && _pendingSshSession) {
        const SshConfig config = *_pendingSshSession;
        _pendingSshSession.reset();
        if (editingSessionId)
            _sessionPanel->updateSsh(*editingSessionId, config);
        else {
            _sessionPanel->recordSsh(config);
            _terminalPage->addSshTerminalTab(config);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  设置 — ElaDialog 内嵌现有 SettingsPage
// ═══════════════════════════════════════════════════════════════════

void MainWindow::showSettingsDialog()
{
    auto* dialog = new ElaDialog(this);
    dialog->setWindowTitle(tr("Settings"));
    dialog->resize(900, 640);
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->setWindowButtonFlags(ElaAppBarType::CloseButtonHint);

    // 以 MainWindow 作为父对象构造，使窗口相关控件
    // （绘制模式 / 窗口特效 / 导航栏模式 / 页面切换模式）仍能定位到它；
    // 捕获的窗口指针在控件被移入对话框布局后仍然有效。
    auto* settingsPage = new SettingsPage(this);
    settingsPage->setTitleVisible(false);

    auto* mainLayout = new QVBoxLayout(dialog);
    mainLayout->setContentsMargins(0, 30, 0, 0);
    mainLayout->addWidget(settingsPage);

    dialog->exec();
    dialog->deleteLater();
}

// ═══════════════════════════════════════════════════════════════════
//  关于 — 模态对话框（行为保持原有逻辑）
// ═══════════════════════════════════════════════════════════════════

void MainWindow::showAboutDialog()
{
    // 若已打开，直接将其前置
    if (_aboutDialog)
    {
        _aboutDialog->raise();
        _aboutDialog->activateWindow();
        return;
    }

    auto* dialog = new ElaContentDialog(this);
    dialog->setWindowTitle(tr("About"));
    dialog->setRightButtonText(tr("OK"));

    // 隐藏左侧和中间按钮 — 关于对话框仅需一个确定按钮
    auto buttons = dialog->findChildren<ElaPushButton*>();
    if (buttons.size() >= 3)
    {
        buttons[0]->hide(); // 左侧按钮
        buttons[1]->hide(); // 中间按钮
    }

    // ── 使用 AboutPage 构建关于内容 ──
    auto* aboutPage = new AboutPage(dialog);
    aboutPage->setTitleVisible(false);
    dialog->setCentralWidget(aboutPage);

    // ── 语言切换支持：仅更新对话框外框，内容由 AboutPage 自行管理 ──
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            dialog, [dialog]() {
        dialog->setWindowTitle(MainWindow::tr("About"));
        dialog->setRightButtonText(MainWindow::tr("OK"));
    });

    // ── 确定按钮：使用 QDialog::done() 避免动画冲突 ──
    connect(dialog, &ElaContentDialog::rightButtonClicked, dialog, [dialog]() {
        dialog->done(QDialog::Accepted);
    });

    // ── 跟踪对话框生命周期 ──
    _aboutDialog = dialog;
    connect(dialog, &QObject::destroyed, this, [this]() {
        _aboutDialog = nullptr;
    });

    dialog->exec();
    dialog->deleteLater();
}
