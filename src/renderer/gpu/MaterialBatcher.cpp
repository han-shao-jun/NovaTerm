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
    append(RenderLayer::Background, backgrounds);
    append(RenderLayer::Content, contents);
    append(RenderLayer::Overlay, overlays);
    return result;
}

} // namespace NovaTerm
