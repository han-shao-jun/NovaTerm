#pragma once

#include "renderer/RenderCommandBuffer.h"

#include <QVector>

namespace NovaTerm {

enum class RenderLayer : quint8 { Background, Content, Overlay };

struct MaterialKey
{
    RenderLayer layer{RenderLayer::Background};
    int pipelineId{0};
    int atlasPage{-1};
    int textureClass{0};
    int blendMode{0};
    int clipId{0};

    friend bool operator==(const MaterialKey& a, const MaterialKey& b)
    {
        return a.layer == b.layer && a.pipelineId == b.pipelineId
            && a.atlasPage == b.atlasPage
            && a.textureClass == b.textureClass
            && a.blendMode == b.blendMode && a.clipId == b.clipId;
    }
};

struct MaterialBatch
{
    MaterialKey key;
    QVector<RenderCommand> commands;
};

class MaterialBatcher
{
public:
    static QVector<MaterialBatch> build(
        const QVector<RenderCommand>& backgrounds,
        const QVector<RenderCommand>& contents,
        const QVector<RenderCommand>& overlays);
};

} // namespace NovaTerm
