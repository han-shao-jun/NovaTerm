/** @file TextWaitMatcher.h @brief 保留真实换行的有界字面文本匹配。 */
#pragma once
#include "mcp/contracts/WaitTextRequest.h"
#include <QVector>
#include <algorithm>

namespace NovaTerm::Mcp {
/** @brief 有界字面证据匹配器，输入保留真实 LF 边界。 */
class TextWaitMatcher final {
public:
    struct Hit { int index; qsizetype endOffset; };
    TextWaitMatcher(QStringList texts, WaitTextMatch mode, bool sensitive)
        : _texts(std::move(texts)), _mode(mode), _case(sensitive ? Qt::CaseSensitive : Qt::CaseInsensitive),
          _seen(_texts.size(), false) {
        for (const auto& text : _texts) _keep = std::max(_keep, text.size());
    }
    bool feed(const QString& text) {
        const auto joined = _tail + text;
        _hits.clear();
        for (qsizetype i = 0; i < _texts.size(); ++i) {
            if (_seen[i]) continue;
            const auto found = joined.indexOf(_texts[i], 0, _case);
            if (found >= 0) {
                _seen[i] = true;
                _hits.append({int(i), found + _texts[i].size() - _tail.size()});
            }
        }
        _tail = joined.right(std::max(qsizetype{0}, _keep - 1));
        // UTF-16 尾部不能从一个标量中间开始。
        if (!_tail.isEmpty() && _tail.front().isLowSurrogate()) _tail.remove(0, 1);
        return satisfied();
    }
    [[nodiscard]] bool satisfied() const {
        return _mode == WaitTextMatch::Any
            ? std::any_of(_seen.begin(), _seen.end(), [](bool value) { return value; })
            : std::all_of(_seen.begin(), _seen.end(), [](bool value) { return value; });
    }
    [[nodiscard]] QVector<int> matchedIndices() const {
        QVector<int> result;
        for (qsizetype i = 0; i < _seen.size(); ++i) if (_seen[i]) result.append(int(i));
        return result;
    }
    [[nodiscard]] QString tail() const { return _tail; }
    [[nodiscard]] const QVector<Hit>& hits() const { return _hits; }
private:
    QStringList _texts;
    WaitTextMatch _mode;
    Qt::CaseSensitivity _case;
    QVector<bool> _seen;
    qsizetype _keep{0};
    QString _tail;
    QVector<Hit> _hits;
};
}
