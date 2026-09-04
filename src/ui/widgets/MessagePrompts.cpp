/**
 * @file  MessagePrompts.cpp
 * @brief Ela 风格确认对话框与警告提示的实现。
 */
#include "ui/widgets/MessagePrompts.h"

#include "ElaContentDialog.h"
#include "ElaMessageBar.h"
#include "ElaPushButton.h"
#include "ElaText.h"

#include <QVBoxLayout>
#include <QWidget>

namespace NovaTerm::Ui
{

namespace {

// 警告浮层的停留时长。取 2.5s：够读完一句校验提示，又不至于挡住用户随即进行的
// 修改操作。
constexpr int WarningToastMsec = 2500;

} // namespace

bool confirm(QWidget* parent, const QString& title, const QString& text)
{
    // ElaContentDialog 的 showEvent 直接解引用 parentWidget() 计算遮罩尺寸，
    // parent 为空会崩。这里取所在窗口而非 parent 本身，让遮罩覆盖整个窗口 ——
    // 与 MainWindow 的退出确认框表现一致；否则停靠面板里弹出的确认框只会给
    // 面板自身蒙上遮罩。
    QWidget* const host = parent != nullptr ? parent->window() : nullptr;
    if (host == nullptr)
        return false;

    // 堆分配 + deleteLater()：ElaContentDialog 的按钮处理器在关闭动画之后用
    // QTimer::singleShot(0, nullptr, ...) 延迟发信号，且捕获了 this。栈对象在
    // exec() 返回即析构，那个定时器会打在已释放的对象上。沿用 MainWindow 关于
    // 框（MainWindow.cpp:1698）已验证的写法。
    auto* dialog = new ElaContentDialog(host);
    dialog->setWindowTitle(title);
    dialog->setLeftButtonText(Prompts::tr("Cancel"));
    dialog->setRightButtonText(Prompts::tr("Confirm"));

    // 只需取消 / 确认两个按钮。ElaContentDialog 未提供隐藏中间按钮的接口，
    // 按构造顺序（左、中、右）取第二个隐藏 —— 与 MainWindow 关于框做法相同。
    // 必须在 setCentralWidget() 之前取，避免把内容区里的按钮也数进来。
    const QList<ElaPushButton*> buttons = dialog->findChildren<ElaPushButton*>();
    if (buttons.size() >= 3)
        buttons[1]->hide();

    auto* central = new QWidget(dialog);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(6);
    auto* titleText = new ElaText(title, central);
    titleText->setTextStyle(ElaTextType::Title);
    auto* bodyText = new ElaText(text, central);
    bodyText->setTextStyle(ElaTextType::Body);
    bodyText->setWordWrap(true);
    layout->addWidget(titleText);
    layout->addWidget(bodyText);
    layout->addStretch();
    // setCentralWidget() 内部已调用 adjustSize()，不要再 resize()，否则会触发
    // QWindowsWindow 的几何体告警（ElaContentDialog.cpp:144-147 的注释）。
    dialog->setCentralWidget(central);

    // 左右按钮的内建处理器已经分别调用 reject() / accept()
    //（ElaContentDialogPrivate::_doCloseAnimation），Esc 走 reject 分支，
    // 因此无需再接任何信号。
    const bool accepted = dialog->exec() == QDialog::Accepted;
    dialog->deleteLater();
    return accepted;
}

void warn(QWidget* parent, const QString& title, const QString& text)
{
    // parent 必须显式给到窗口一级。ElaMessageBar 构造里直接
    // parent->installEventFilter()，而静态方法在 parent 为空时会去找顶层的
    // ElaWindow（ElaMessageBar.cpp:107-121）—— 对模态对话框里的页面来说那是
    // 主窗口，浮层会落到对话框背后看不见。
    QWidget* const host = parent != nullptr ? parent->window() : nullptr;
    if (host == nullptr)
        return;

    ElaMessageBar::warning(ElaMessageBarType::TopRight, title, text,
                           WarningToastMsec, host);
}

} // namespace NovaTerm::Ui
