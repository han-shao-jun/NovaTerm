/**
 * @file InteractiveCommandProfile.h
 * @brief 交互命令提示符与隐藏 framing 的可信 Profile 数据。
 */
#pragma once

#include <QByteArray>
#include <QtTypes>

/** @brief Session 交互命令使用的 framing 基础配置。 */
struct InteractiveCommandProfile
{
    QByteArray markerPrefix{QByteArrayLiteral("\x1b]633;NT;")};
    QByteArray lineEnding{QByteArrayLiteral("\r")};
    qsizetype maxMarkerBytes{4096};

    /** @brief 配置是否满足有界解析的最低要求。 */
    [[nodiscard]] bool isValid() const noexcept;
};
