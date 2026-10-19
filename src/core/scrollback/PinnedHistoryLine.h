/** @file PinnedHistoryLine.h
 * @brief 拥有型历史读取结果：句柄存活时行有效，不依赖缓存驻留。
 */
#pragma once
#include "ScrollbackData.h"
#include <string>
#include <utility>

namespace NovaTerm {
enum class HistoryReadStatus { Found, NotFound, Pending, Busy, DecodeError };
/** @brief 不隐式转换为裸指针。Active 行须复制后再交出句柄。 */
class PinnedHistoryLine {
public:
    PinnedHistoryLine() = default;
    PinnedHistoryLine(std::shared_ptr<const void> owner, const LogicalLine* line)
        : _owner(std::move(owner)), _line(line), _status(line ? HistoryReadStatus::Found : HistoryReadStatus::NotFound) {}
    explicit PinnedHistoryLine(HistoryReadStatus status, std::string error = {})
        : _status(status), _error(std::move(error)) {}
    [[nodiscard]] HistoryReadStatus status() const { return _status; }
    [[nodiscard]] const std::string& error() const { return _error; }
    explicit operator bool() const { return _status == HistoryReadStatus::Found; }
    const LogicalLine* operator->() const { return _line; }
    const LogicalLine& operator*() const { return *_line; }
private:
    friend class HistoryReadCursor;
    std::shared_ptr<const void> _owner;
    const LogicalLine* _line{nullptr};
    HistoryReadStatus _status{HistoryReadStatus::NotFound};
    std::string _error;
};
/** @brief 兼容 Cell 数组查询的拥有型句柄，生命周期同 PinnedHistoryLine。 */
class PinnedHistoryCells {
public:
    explicit PinnedHistoryCells(PinnedHistoryLine line) : _line(std::move(line)) {}
    explicit operator bool() const { return bool(_line); }
    const std::vector<Cell>* operator->() const { return &_line->cells; }
    const Cell& operator[](std::size_t n) const { return _line->cells[n]; }
    [[nodiscard]] HistoryReadStatus status() const { return _line.status(); }
private:
    PinnedHistoryLine _line;
};
} // namespace NovaTerm
