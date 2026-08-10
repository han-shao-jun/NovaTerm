/**
 * @file   RendererCapabilities.h
 * @brief  渲染后端能力探测。
 *
 * 不同 QRhi 后端（D3D11/D3D12/Vulkan/Metal/OpenGL）支持的能力不同。
 * RendererCapabilities 在设备初始化时探测一次，渲染器据此选择
 * 快速路径或退化路径（如不支持 texture array 时按页单独绘制）。
 */
#pragma once

#include <QString>
#include <QtGlobal>

class QRhi;

namespace NovaTerm {

// 渲染后端能力集合。不可变值类型，设备创建后只读。
struct RendererCapabilities
{
    bool partialTextureUploads{true};     // 是否支持纹理子区域上传
    bool instancing{true};                 // 是否支持实例化绘制
    bool persistentBaseTexture{false};   // 是否支持持久化基础纹理（保留渲染目标）
    bool textureArrays{false};            // 是否支持纹理数组（atlas 多页合并提交）
    int maximumTextureSize{2048};         // 单张纹理最大尺寸
    int framesInFlight{3};                 // 在途帧数
    QString backend;                       // 后端名称（D3D11/Vulkan/Metal/...）
    QString fallbackReason;                // 退化原因说明，用于诊断日志

    /**
     * @brief 探测指定 QRhi 实例的能力。
     * @param rhi QRhi 实例，nullptr 时所有能力置为不可用。
     * @return 能力集合。
     */
    static RendererCapabilities detect(QRhi* rhi);
};

} // namespace NovaTerm
