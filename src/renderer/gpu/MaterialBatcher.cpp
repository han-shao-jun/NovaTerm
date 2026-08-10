/**
 * @file   MaterialBatcher.cpp
 * @brief  渲染命令分批实现。
 *
 * 详见 MaterialBatcher.h。build() 顺序遍历三层命令，按 MaterialKey
 * 线性查找已有批次命中则追加，否则新建一个批次。批次数通常很少，
 * O(n*m) 线性查找足够。
 */
#include "MaterialBatcher.h"

namespace NovaTerm {

QVector<MaterialBatch> MaterialBatcher::build(
    const QVector<RenderCommand>& backgrounds,
    const QVector<RenderCommand>& contents,
    const QVector<RenderCommand>& overlays)
{
    QVector<MaterialBatch> result;
    auto append = [&result](RenderLayer layer,
                            const QVector<RenderCommand>& commands) {
        for (const RenderCommand& command : commands) {
            // 由命令属性派生 MaterialKey：atlasPage<0 表示无纹理（背景矩形），
            // blendMode 由层次决定（背景覆盖，内容/叠加 alpha 混合）。
            const MaterialKey key{layer, 0, command.atlasPage,
                                  command.atlasPage < 0 ? 0 : 1,
                                  layer == RenderLayer::Background ? 0 : 1, 0};
            int target = -1;
            for (int index = 0; index < result.size(); ++index) {
                if (result[index].key == key) {
                    target = index;
                    break;
                }
            }
            if (target < 0) {
                target = result.size();
                result.push_back({key, {}});
            }
            result[target].commands.push_back(command);
        }
    };
    // 顺序固定：背景 → 内容 → 叠加，保证后绘制的层覆盖先绘制的层。
    append(RenderLayer::Background, backgrounds);
    append(RenderLayer::Content, contents);
    append(RenderLayer::Overlay, overlays);
    return result;
}

} // namespace NovaTerm
