/**
 * @file  MessagePrompts.h
 * @brief Ela 风格的确认对话框与警告提示。
 *
 * ElaWidgetTools 没有与 QMessageBox 对应的控件：模态确认要用 `ElaContentDialog`
 * 自行拼装，纯告知类提示则用 `ElaMessageBar` 浮层。两者都有「parent 不能为空、
 * 且必须是所在窗口」的隐式要求（见实现文件里的注释），把这些约束收在一处，
 * 各面板只管传内容。
 */
#pragma once

#include <QCoreApplication>
#include <QString>

class QWidget;

namespace NovaTerm::Ui
{

/**
 * @brief 仅用于给下面两个自由函数提供翻译上下文。
 *
 * 自由函数没有 `tr()`。`Q_DECLARE_TR_FUNCTIONS` 让 lupdate 以本类名收集字符串，
 * 从而与项目其余部分一样按类名作上下文，`.ts` 里能正常查到译文。
 */
class Prompts
{
    Q_DECLARE_TR_FUNCTIONS(Prompts)
};


/**
 * @brief  弹出 Ela 风格模态确认框，阻塞至用户作出选择。
 * @param  parent 归属控件。遮罩与居中以其**所在窗口**为基准；传空返回 false。
 * @param  title  标题，以 ElaTextType::Title 显示在内容区顶部。
 * @param  text   正文，以 ElaTextType::Body 显示，自动换行。
 * @return 点击确认返回 true；点击取消、按 Esc 或关闭窗口返回 false。
 * @note   语义与被它替换的 `QMessageBox::question(..., Yes | No, No)` 一致 ——
 *         默认不执行，只有明确确认才返回 true。
 */
[[nodiscard]] bool confirm(QWidget* parent, const QString& title,
                           const QString& text);

/**
 * @brief 在所在窗口右上角弹出 Ela 风格警告浮层，数秒后自动消失。
 * @param parent 归属控件；浮层实际挂到其**所在窗口**上。传空则不显示。
 * @param title  浮层标题。
 * @param text   浮层正文。
 * @note  取代原先的 `QMessageBox::warning()`。与之不同的是**不阻塞**，因此仅
 *        适用于「告知后调用方自行 return」的场景；需要用户答复请用 confirm()。
 */
void warn(QWidget* parent, const QString& title, const QString& text);

} // namespace NovaTerm::Ui
