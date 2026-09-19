/**
 * @file ISessionCommandExecutor.h
 * @brief Session 受限命令执行器接口；不接受 MCP 原始 JSON。
 */
#pragma once

#include "CommandExecutionTypes.h"

#include <QObject>

/** @brief Session 命令执行后端的最小异步契约。 */
class ISessionCommandExecutor : public QObject
{
    Q_OBJECT
public:
    explicit ISessionCommandExecutor(QObject* parent = nullptr)
        : QObject(parent)
    {
    }
    ~ISessionCommandExecutor() override = default;

    [[nodiscard]] virtual bool isAvailable() const = 0;
    [[nodiscard]] virtual CommandExecutorCapabilities capabilities() const = 0;
    [[nodiscard]] virtual QString targetFingerprint() const = 0;
    [[nodiscard]] virtual bool execute(const CommandExecutionRequest& request) = 0;
    virtual void cancel(quint64 requestId) = 0;

signals:
    /** @brief 完成结果；发出线程由具体 Executor 保证。 */
    void finished(const CommandExecutionResult& result);
};
