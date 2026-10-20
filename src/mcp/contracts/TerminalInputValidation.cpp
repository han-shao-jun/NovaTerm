#include "TerminalInputValidation.h"
#include "Budget.h"
#include <QSet>
#include <QStringList>
namespace NovaTerm::Mcp {
namespace {
bool validText(const QString& text)
{
    if (text.size() > Budget::MaxTerminalActionTextBytes || text.contains(QChar::Null)) return false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isHighSurrogate()) {
            if (++i >= text.size() || !text[i].isLowSurrogate()) return false;
        } else if (text[i].isLowSurrogate()) return false;
    }
    return text.toUtf8().size() <= Budget::MaxTerminalActionTextBytes;
}
bool controls(const QString& text)
{
    for (const auto character : text) if (character.unicode() < 32 || character.unicode() == 127) return true;
    return false;
}
bool validKey(const QString& key)
{
    static const QStringList named{"Enter", "Tab", "Backspace", "Escape", "Up", "Down", "Left", "Right",
        "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    if (named.contains(key)) return true;
    const auto scalars = key.toUcs4();
    return scalars.size() == 1 && QChar::isPrint(scalars.front());
}
}
QString validateTerminalActions(const std::vector<TerminalAction>& actions)
{
    if (actions.empty() || actions.size() > size_t(Budget::MaxTerminalActions)) return "Invalid argument: actions";
    qsizetype total = 0;
    for (const auto& action : actions) {
        const auto error = std::visit([&total, count = actions.size()](const auto& value) -> QString {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, TextAction> || std::is_same_v<T, PasteAction> || std::is_same_v<T, CommandAction>) {
                const auto& text = [&value]() -> const QString& {
                    if constexpr (std::is_same_v<T, CommandAction>) return value.command; else return value.text;
                }();
                if (!validText(text) || (std::is_same_v<T, CommandAction> && text.isEmpty())) return "Invalid argument: actions";
                if constexpr (std::is_same_v<T, CommandAction>) if (count != 1) return "command and release must be the only action.";
                if constexpr (!std::is_same_v<T, PasteAction>) if (controls(text)) return "text and command cannot contain C0 or DEL characters.";
                total += text.toUtf8().size();
            } else if constexpr (std::is_same_v<T, KeyAction>) {
                if (!validText(value.key) || value.key.toUtf8().size() > 16 || !validKey(value.key)) return "Invalid terminal key.";
                QSet<int> modifiers;
                for (const auto modifier : value.modifiers) {
                    const auto number = int(modifier);
                    if (number < int(KeyModifier::Shift) || number > int(KeyModifier::Ctrl) || modifiers.contains(number)) return "Invalid argument: actions";
                    modifiers.insert(number);
                }
            } else if constexpr (std::is_same_v<T, ReleaseAction>) if (count != 1) return "command and release must be the only action.";
            return {};
        }, action);
        if (!error.isEmpty()) return error;
    }
    return total > Budget::MaxTerminalActionTextBytes ? QString("Combined action text exceeds 16 KiB.") : QString{};
}
}
