/**
 * @file Identifiers.h
 * @brief MCP 应用层的强类型标识与单调计数器。
 *
 * 设计约束（2026-10-18 运行期重构设计 §4.1）：不同语义的标识不得因同为
 * `QString` 而隐式互换；RPC 原始 ID 与路由编码只属于协议适配，不进入业务
 * 组件。构造必须显式，外部文本只能经受控解析函数进入。
 */
#pragma once

#include <QHash>
#include <QString>

#include <optional>
#include <utility>

namespace NovaTerm::Mcp {

/**
 * @brief 带标签的小值类型包装，防止不同标识互相赋值。
 * @tparam Tag 只用于区分类型的标签结构体，没有运行时语义。
 *
 * 值为空表示「未设置」；业务组件不得把空值当作合法目标。
 */
template <class Tag>
class StrongId
{
public:
    StrongId() = default;
    explicit StrongId(QString value) : _value(std::move(value)) {}

    [[nodiscard]] bool isEmpty() const noexcept { return _value.isEmpty(); }
    /** @brief 只读访问内部文本；用于协议编码、日志键与持久化。 */
    [[nodiscard]] const QString& str() const noexcept { return _value; }

    friend bool operator==(const StrongId& left, const StrongId& right)
    {
        return left._value == right._value;
    }
    friend bool operator!=(const StrongId& left, const StrongId& right)
    {
        return !(left == right);
    }

private:
    QString _value;
};

template <class Tag>
size_t qHash(const StrongId<Tag>& id, size_t seed = 0) noexcept
{
    return qHash(id.str(), seed);
}

struct ClientIdTag;
struct PeerIdTag;
struct RequestIdTag;
struct TaskIdTag;
struct SessionIdTag;
struct AttachmentIdTag;
struct EpochTag;
struct OperationIdTag;
struct CaptureIdTag;
struct DiagnosticIdTag;
struct ExecutionIdTag;

using ClientId = StrongId<ClientIdTag>;
using PeerId = StrongId<PeerIdTag>;
using RequestId = StrongId<RequestIdTag>;
using TaskId = StrongId<TaskIdTag>;
using SessionId = StrongId<SessionIdTag>;
using AttachmentId = StrongId<AttachmentIdTag>;
using Epoch = StrongId<EpochTag>;
using OperationId = StrongId<OperationIdTag>;
using CaptureId = StrongId<CaptureIdTag>;
using DiagnosticId = StrongId<DiagnosticIdTag>;
using ExecutionId = StrongId<ExecutionIdTag>;

/**
 * @brief 带标签的单调计数器，避免授权、观察与输入序号互相顶替。
 *
 * 只提供显式构造与比较，不做算术；自增由所有者服务在同一事务内完成。
 */
template <class Tag>
class Revision
{
public:
    constexpr Revision() = default;
    explicit constexpr Revision(quint64 value) : _value(value) {}

    [[nodiscard]] constexpr quint64 value() const noexcept { return _value; }

    friend constexpr bool operator==(Revision left, Revision right)
    {
        return left._value == right._value;
    }
    friend constexpr bool operator!=(Revision left, Revision right)
    {
        return !(left == right);
    }
    friend constexpr bool operator<(Revision left, Revision right)
    {
        return left._value < right._value;
    }

private:
    quint64 _value{0};
};

struct GrantRevisionTag;
struct ClientAccessVersionTag;
struct GenerationTag;
struct ModelRevisionTag;
struct EventRevisionTag;
struct InputRevisionTag;
struct PromptRevisionTag;

using GrantRevision = Revision<GrantRevisionTag>;
using ClientAccessVersion = Revision<ClientAccessVersionTag>;
using Generation = Revision<GenerationTag>;
using ModelRevision = Revision<ModelRevisionTag>;
using EventRevision = Revision<EventRevisionTag>;
using InputRevision = Revision<InputRevisionTag>;
using PromptRevision = Revision<PromptRevisionTag>;

/** @brief 是否为规范的无花括号 UUID 文本。 */
[[nodiscard]] bool isCanonicalUuid(const QString& text);

/**
 * @brief 受控解析 UUID 标识。
 * @return 非法文本返回 `std::nullopt`，不产生半有效 ID。
 */
template <class Id>
[[nodiscard]] std::optional<Id> parseUuidId(const QString& text)
{
    if (!isCanonicalUuid(text))
        return std::nullopt;
    return Id{text};
}

/**
 * @brief 受控解析有界文本标识（epoch、capture、诊断 ID 等）。
 * @param text 原始文本。
 * @param maxLength 允许的最大 UTF-16 长度，与 Schema 上限一致。
 */
template <class Id>
[[nodiscard]] std::optional<Id> parseBoundedId(const QString& text, qsizetype maxLength)
{
    if (text.isEmpty() || text.size() > maxLength || text.contains(QChar::Null))
        return std::nullopt;
    return Id{text};
}

} // namespace NovaTerm::Mcp
