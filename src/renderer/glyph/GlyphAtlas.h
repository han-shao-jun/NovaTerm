/**
 * @file   GlyphAtlas.h
 * @brief  字形纹理 atlas：把多个字形位图打包到大尺寸纹理页中。
 *
 * GlyphAtlas 维护若干固定尺寸的纹理页（Page），按 shelf-pack（行式打包）
 * 算法分配矩形区域给每个字形。页按像素格式（Alpha8 / Rgba8）分类，
 * 互不混用。当页耗尽且未达内存上限时创建新页；达上限时回收最久未用
 * 的页。每个页有独立的 generation，回收或重置后递增，使旧 GlyphLocation
 * 失效，避免采样到错误纹理。
 *
 * 上传模型：写入新字形时把对应矩形加入页的 dirtyRects，takeUploads()
 * 按 byteBudget 取出本帧需上传的区域，超出预算的延后到下一帧。
 */
#pragma once

#include "GlyphTypes.h"

#include <QHash>
#include <QImage>
#include <QRect>
#include <QVector>

#include <optional>

namespace NovaTerm {

// atlas 配置。在构造时固定，运行期不可变。
struct GlyphAtlasConfig
{
    QSize pageSize{2048, 2048};                  // 单页纹理尺寸
    quint64 byteBudget{64ull * 1024ull * 1024ull};  // atlas 总内存上限
    quint64 uploadBudgetPerFrame{4ull * 1024ull * 1024ull};  // 每帧上传字节预算
    int padding{1};                              // 字形间留白，避免采样串色
    int framesInFlight{3};                       // 在途帧数，决定页的 retireFrame
};

// atlas 运行期统计信息，用于诊断内存占用与上传吞吐。
struct GlyphAtlasStatistics
{
    quint64 allocations{0};          // 成功分配次数
    quint64 allocationFailures{0};  // 分配失败次数（页满且无法回收）
    quint64 pageCreations{0};        // 新建页次数
    quint64 pageEvictions{0};        // 回收页次数
    quint64 generationChanges{0};   // 整体 generation 递增次数
    quint64 requestedUploadBytes{0};  // 累计请求上传字节
    quint64 uploadedBytes{0};        // 累计已上传字节
    quint64 deferredUploadBytes{0};  // 累计延后上传字节
    quint64 fullPageUploads{0};      // 整页上传次数
    quint64 dirtyRectsBeforeMerge{0};  // 合并前脏矩形数
    quint64 dirtyRectsAfterMerge{0};    // 合并后脏矩形数
    quint64 currentBytes{0};         // 当前占用字节数
    quint64 peakBytes{0};            // 峰值字节数
};

// 一次纹理上传请求：把指定页的某个矩形区域上传到 GPU。
struct GlyphAtlasUpload
{
    int pageId{-1};
    quint64 pageGeneration{0};
    QRect rect;
    QImage image;
    bool fullPage{false};  // 是否为整页上传（用于选择更高效的更新路径）
    quint64 bytes() const { return quint64(rect.width()) * rect.height() * 4; }
};

// 字形纹理 atlas。单线程（GUI 线程）独占使用。
class GlyphAtlas
{
public:
    explicit GlyphAtlas(GlyphAtlasConfig config = {});

    /**
     * @brief 把栅格化后的字形位图写入 atlas。
     *        优先尝试现有同格式页；都放不下时创建或回收一页再分配。
     * @param bitmap 栅格化结果。
     * @param frameNumber 当前帧号，更新页的 lastUsedFrame / retireFrame。
     * @return 成功返回 GlyphLocation；位图为空或无可用页返回 std::nullopt。
     */
    std::optional<GlyphLocation> insert(const GlyphBitmap& bitmap,
                                        quint64 frameNumber);

    /**
     * @brief 检查 GlyphLocation 是否仍有效（页存在且 generation 匹配）。
     *        回收或重置页后旧 location 会失效。
     */
    bool isValid(const GlyphLocation& location) const;

    /**
     * @brief 标记某 location 所在页最近被使用，影响 LRU 回收决策。
     */
    void touch(const GlyphLocation& location, quint64 frameNumber);

    /**
     * @brief 把页的 retireFrame 提升至指定值，避免在帧期间被回收。
     *        用于保护当前可见字形所在页不被淘汰。
     */
    void pin(const GlyphLocation& location, quint64 retireFrame);

    /**
     * @brief 取出本帧需要上传到 GPU 的纹理更新。超出 byteBudget 的脏矩形
     *        保留在页的 dirtyRects 中延后到后续帧。
     * @param byteBudget 本帧上传字节上限，0 表示使用 config 默认值。
     * @return 本帧的上传请求列表。
     */
    QVector<GlyphAtlasUpload> takeUploads(quint64 byteBudget = 0);

    bool hasPendingUploads() const;
    bool hasFullPageUploads() const;

    /**
     * @brief 把所有页标记为整页脏。用于 GPU 纹理丢失（如设备重置）后
     *        强制全量重传。
     */
    void markAllDirty();
    void clear();

    int pageCount() const { return _pages.size(); }
    quint64 generation() const { return _generation; }
    const GlyphAtlasConfig& config() const { return _config; }
    const GlyphAtlasStatistics& statistics() const { return _statistics; }
    const QImage* pageImage(int pageId) const;
    quint64 pageGeneration(int pageId) const;

private:
    // 单个纹理页。采用 shelf-pack：x/y 为当前光标，rowHeight 为本行最高
    // 字形高度；放不下时换行（x 归 1，y += rowHeight）。
    struct Page {
        int id{-1};
        GlyphPixelFormat format{GlyphPixelFormat::Alpha8};
        quint64 generation{1};   // 页代际，重置后递增使旧 location 失效
        QImage image;
        int x{1};                // 当前光标 x
        int y{1};                // 当前光标 y
        int rowHeight{0};        // 当前行已分配的最大高度
        quint64 lastUsedFrame{0};  // 最近被 touch 的帧号（LRU 依据）
        quint64 retireFrame{0};   // 在此帧前不可回收（framesInFlight 保护）
        QVector<QRect> dirtyRects;  // 待上传的脏矩形
        bool fullDirty{true};      // 是否整页脏（首次创建或设备重置后）
    };

    static quint64 pageBytes(const QSize& size);
    static QVector<QRect> mergeDirtyRects(QVector<QRect> rects,
                                           const QRect& bounds);
    int createOrRecyclePage(GlyphPixelFormat format, quint64 frameNumber);
    std::optional<QRect> allocate(Page& page, const QSize& size);
    void resetPage(Page& page, GlyphPixelFormat format, quint64 frameNumber);

    GlyphAtlasConfig _config;
    QVector<Page> _pages;
    quint64 _generation{1};   // 全局代际，任何页变更都递增
    GlyphAtlasStatistics _statistics;
};

} // namespace NovaTerm
