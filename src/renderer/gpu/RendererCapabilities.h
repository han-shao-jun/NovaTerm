#pragma once

#include <QString>
#include <QtGlobal>

class QRhi;

namespace NovaTerm {

struct RendererCapabilities
{
    bool partialTextureUploads{true};
    bool instancing{true};
    bool persistentBaseTexture{false};
    bool textureArrays{false};
    int maximumTextureSize{2048};
    int framesInFlight{3};
    QString backend;
    QString fallbackReason;

    static RendererCapabilities detect(QRhi* rhi);
};

} // namespace NovaTerm
