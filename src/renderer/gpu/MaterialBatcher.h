/**
 * @file   MaterialBatcher.h
 * @brief  渲染命令按材质分批。
 *
 * GPU 绘制以 batch 为单位，同一 batch 内所有命令共享相同的 pipeline /
 * texture / blend 状态，可一次 draw call 提交。MaterialBatcher 把
 * RenderCommandBuffer 的三层数据（背景/内容/叠加）按 MaterialKey 分组，
 * 输出可直接逐批提交的 MaterialBatch 列表。
 */
#pragma once

#include "renderer/RenderCommandBuffer.h"

#include <QVector>

namespace NovaTerm {

// 渲染层次：背景 → 内容 → 叠加，绘制顺序固定。
enum class RenderLayer : quint8 { Background, Content, Overlay };

// 材质键：相同 MaterialKey 的命令可合并为一个 batch。
struct MaterialKey
{
    RenderLayer layer{RenderLayer::Background};
    int pipelineId{0};       // 着色器管线 ID
    int atlasPage{-1};       // 字形所在 atlas 页（-1 表示无纹理）
    int textureClass{0};     // 纹理类别：0=无纹理，1=atlas 纹理
    int blendMode{0};         // 混合模式：0=覆盖（背景），1=alpha 混合
    int clipId{0};            // 裁剪区域 ID

    friend bool operator==(const MaterialKey& a, const MaterialKey& b)
    {
        return a.layer == b.layer && a.pipelineId == b.pipelineId
            && a.atlasPage == b.atlasPage
            && a.textureClass == b.textureClass
            && a.blendMode == b.blendMode && a.clipId == b.clipId;
    }
};

// 一个材质批次：共享 MaterialKey 的命令集合，对应一次 draw call。
struct MaterialBatch
{
    MaterialKey key;
    QVector<RenderCommand> commands;
};

// 渲染命令分批器。无状态，全部为静态方法。
class MaterialBatcher
{
public:
    /**
     * @brief 把三层渲染命令按 MaterialKey 分组为批次。
     *        输出顺序：背景层批次 → 内容层批次 → 叠加层批次，
     *        保证绘制顺序正确。
     * @param backgrounds 背景层命令（背景矩形）。
     * @param contents    内容层命令（字形/下划线/删除线）。
     * @param overlays    叠加层命令（光标/选区/搜索高亮）。
     * @return 分批后的列表。
     */
    static QVector<MaterialBatch> build(
        const QVector<RenderCommand>& backgrounds,
        const QVector<RenderCommand>& contents,
        const QVector<RenderCommand>& overlays);
};

} // namespace NovaTerm
