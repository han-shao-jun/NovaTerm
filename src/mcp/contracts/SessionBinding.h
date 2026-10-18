/**
 * @file SessionBinding.h
 * @brief 会话引用、绑定与授权上下文值对象。
 *
 * `SessionReference` 是模型可提供的标识；`SessionBinding` 由 SessionGateway
 * 解析出 attachment/epoch/generation 与可信目标指纹，模型不能自证这些事实。
 */
#pragma once

#include "Identifiers.h"

#include <QString>

#include <optional>

namespace NovaTerm::Mcp {

/** @brief 请求中由模型给出的会话身份；不含 attachment 或目标指纹。 */
struct SessionReference
{
    SessionId session;
    Epoch epoch;
};

/** @brief 授权与执行使用的可信会话绑定。 */
struct SessionBinding
{
    SessionId session;
    AttachmentId attachment;
    Epoch epoch;
    Generation generation;
    /** @brief 缺失表示目标身份未验证；不得用端口名或空串代替。 */
    std::optional<QString> targetFingerprint;
};

/** @brief 一次业务调用的授权上下文；身份来自连接，不来自模型参数。 */
struct AuthorizationContext
{
    ClientId client;
    ClientAccessVersion accessVersion;
    SessionBinding binding;
};

} // namespace NovaTerm::Mcp
