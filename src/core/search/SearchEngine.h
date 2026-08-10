/**
 * @file   SearchEngine.h
 * @brief  滚动历史全文搜索引擎。
 *
 * 在独立的 worker 线程中对 ScrollbackSnapshot 执行正则或纯文本匹配，
 * 命中结果按批通过 resultsReady 信号投递回 GUI 线程。引擎内置 generation
 * 取消机制：每次新搜索生成递增的 generation，旧 generation 的搜索会被
 * 快速取消，避免用户快速输入时累积无用结果。
 */
#pragma once

#include "core/scrollback/ScrollbackSnapshot.h"

#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QVector>

#include <memory>

namespace NovaTerm {

// 一次搜索请求。query 为空表示清空当前结果集。
struct SearchRequest
{
    QString query;
    bool caseSensitive{false};
    bool regularExpression{false};
    bool wholeWord{false};
    // 限定搜索范围的起止逻辑行 ID；0 表示不限制。
    LineId firstLine{0};
    LineId lastLine{0};
    // 搜索代际：调用方递增分配。引擎据此判断搜索是否已被新搜索取代。
    quint64 generation{0};
    // 单批次最大命中数，达到后即先发布一批。
    qsizetype resultBatchSize{128};
    // 命中总数上限，达到后停止扫描，防止超长输出撑爆 UI。
    qsizetype maximumResults{100'000};
};

// 一次命中。坐标以 Cell 为单位，便于 UI 直接高亮。
struct SearchMatch
{
    LineId lineId{0};
    qsizetype startCell{0};
    qsizetype endCell{0};
};

// 一批搜索结果。completed=true 表示搜索已结束（无论是否扫完）。
// cancelled=true 表示被更高 generation 的搜索取代而提前终止。
struct SearchBatch
{
    quint64 generation{0};
    // 触发搜索时的 ScrollbackSnapshot 版本，UI 可据此丢弃过期结果。
    quint64 sourceVersion{0};
    QVector<SearchMatch> matches;
    qsizetype scannedLines{0};
    qsizetype totalLines{0};
    bool completed{false};
    bool cancelled{false};
    QString error;
};

// 滚动历史搜索引擎。不可拷贝，仅可在 GUI 线程创建/销毁。
class SearchEngine final : public QObject
{
    Q_OBJECT
public:
    explicit SearchEngine(QObject* parent = nullptr);
    ~SearchEngine() override;

    /**
     * @brief 提交一次搜索。snapshot 会被移动到 worker 线程中处理，
     *        调用方应在调用前自行 snapshot()。
     * @param snapshot 搜索源数据快照。
     * @param request 搜索请求，generation 由调用方保证单调递增。
     */
    void search(ScrollbackSnapshot snapshot, SearchRequest request);

    /**
     * @brief 取消指定 generation 及更早的搜索。
     *        worker 线程在下一个检查点检测到后立即终止并发布 cancelled 批次。
     * @param generation 取消代际。
     */
    void cancel(quint64 generation);

signals:
    void resultsReady(const NovaTerm::SearchBatch& batch);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace NovaTerm

Q_DECLARE_METATYPE(NovaTerm::SearchRequest)
Q_DECLARE_METATYPE(NovaTerm::SearchMatch)
Q_DECLARE_METATYPE(NovaTerm::SearchBatch)
