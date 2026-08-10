/**
 * @file   RendererCapabilities.cpp
 * @brief  渲染后端能力探测实现。
 *
 * 详见 RendererCapabilities.h。detect() 查询 QRhi 的特性支持位，
 * 并记录退化原因以便诊断为何走慢路径。
 */
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
    // QRhi 提供可移植的纹理子区域上传与实例化绘制 API。纹理数组与
    // 保留渲染目标内容在获得各后端专项验证前刻意禁用，atlas 批次
    // 仍按页单独绘制。
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
