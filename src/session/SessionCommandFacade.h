/**
 * @file SessionCommandFacade.h
 * @brief Session 级受限命令门面与 Executor 生命周期隔离。
 */
#pragma once

#include "CommandExecutionTypes.h"
#include "CommandPlatformProfile.h"

#include <QObject>
#include <QSet>

#include <memory>

class ISessionCommandExecutor;

/** @brief 向 MCP 等上层隐藏具体 Transport 的命令执行门面。 */
class SessionCommandFacade final : public QObject
{
    Q_OBJECT
public:
    explicit SessionCommandFacade(QObject* parent = nullptr);
    ~SessionCommandFacade() override;

    /** @brief 使旧 Executor 与迟到结果失效。 */
    void reset(quint64 sessionGeneration);
    /** @brief 为当前 Session 世代安装经过选择的 Executor。 */
    void installExecutor(std::unique_ptr<ISessionCommandExecutor> executor,
                         quint64 sessionGeneration);

    [[nodiscard]] bool isAvailable() const;
    [[nodiscard]] CommandExecutorCapabilities capabilities() const;
    [[nodiscard]] CommandPlatformProfile profile() const;
    [[nodiscard]] QString targetFingerprint() const;
    [[nodiscard]] bool execute(const CommandExecutionRequest& request);
    void cancel(quint64 requestId);

signals:
    void finished(const CommandExecutionResult& result);

private:
    std::unique_ptr<ISessionCommandExecutor> _executor;
    QSet<quint64> _activeRequests;
    quint64 _sessionGeneration{0};
    quint64 _bindingSerial{0};
};
