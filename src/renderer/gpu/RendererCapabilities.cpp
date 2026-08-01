#include "RendererCapabilities.h"

#include <rhi/qrhi.h>

namespace NovaTerm {

RendererCapabilities RendererCapabilities::detect(QRhi* rhi)
{
    RendererCapabilities result;
    if (!rhi) {
        result.partialTextureUploads = false;
        result.instancing = false;
        result.fallbackReason = QStringLiteral("QRhi unavailable");
        return result;
    }
    result.maximumTextureSize = rhi->resourceLimit(QRhi::TextureSizeMax);
    switch (rhi->backend()) {
    case QRhi::Vulkan: result.backend = QStringLiteral("Vulkan"); break;
    case QRhi::OpenGLES2: result.backend = QStringLiteral("OpenGL"); break;
    case QRhi::D3D11: result.backend = QStringLiteral("D3D11"); break;
    case QRhi::D3D12: result.backend = QStringLiteral("D3D12"); break;
    case QRhi::Metal: result.backend = QStringLiteral("Metal"); break;
    case QRhi::Null: result.backend = QStringLiteral("Null"); break;
    }
    // QRhi exposes portable partial upload and instanced draw APIs. Texture
    // arrays and preserved render-target contents are intentionally disabled
    // until a backend-specific proof is available; batching remains per page.
    result.partialTextureUploads = true;
    result.instancing = rhi->isFeatureSupported(QRhi::Instancing);
    result.textureArrays = rhi->isFeatureSupported(QRhi::TextureArrays);
    result.persistentBaseTexture = false;
    result.fallbackReason = QStringLiteral(
        "persistent base disabled: no portable preserved-load contract");
    if (!result.instancing || !result.textureArrays) {
        result.fallbackReason += QStringLiteral(
            "; compact vertex/per-page fallback required");
    }
    return result;
}

} // namespace NovaTerm
