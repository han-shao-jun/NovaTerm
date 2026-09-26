/** @file LocalSessionScriptProvider.h
 *  @brief LocalShell 目标主机脚本写入适配器。
 */
#pragma once

#include "ISessionScriptProvider.h"

class LocalSessionScriptProvider final : public ISessionScriptProvider
{
    Q_OBJECT
public:
    explicit LocalSessionScriptProvider(QObject* parent = nullptr);

    [[nodiscard]] bool isAvailable() const override { return true; }
    bool writeScript(const ScriptWriteRequest& request) override;
    void cancelWrite(quint64 requestId) override;

private:
    [[nodiscard]] static QString resolvePath(const ScriptWriteRequest& request);
};
