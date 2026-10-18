/** @file Identifiers.cpp @brief 强类型标识的受控解析实现。 */
#include "Identifiers.h"

#include <QUuid>

namespace NovaTerm::Mcp {

bool isCanonicalUuid(const QString& text)
{
    // 只接受 QUuid 的标准无花括号小写形式，拒绝大小写混写与多余空格。
    const QUuid value(text);
    return !value.isNull()
        && value.toString(QUuid::WithoutBraces) == text;
}

} // namespace NovaTerm::Mcp
