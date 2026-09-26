/**
 * @file SessionInputArbiter.h
 * @brief Session 用户输入与 MCP 输入的单通路仲裁器。
 */
#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>

class ITransport;

/** @brief 输入来源；后续统计和审计不得用布尔量混淆来源。 */
enum class SessionInputOrigin
{
    User,
    Mcp,
};

/**
 * @brief 保证用户输入优先的 Session 出站字节仲裁器。
 *
 * 同一时刻最多持有一个 MCP Lease。用户产生非空输入时，先使 Lease
 * 失效并发布抢占结果，再把用户字节写入当前 Transport。
 */
class SessionInputArbiter final : public QObject
{
    Q_OBJECT
public:
    explicit SessionInputArbiter(QObject* parent = nullptr);

    /** @brief 绑定当前 Transport 与 Session 世代，并清除旧 Lease。 */
    void bind(ITransport* transport, quint64 generation);
    /** @brief 推进 Session 世代并清除旧 Lease，保留当前 Transport。 */
    void reset(quint64 generation);

    /** @brief 为当前世代获取唯一 MCP 输入 Lease。 */
    [[nodiscard]] bool acquireMcpLease(quint64 executionId,
                                       quint64 generation);
    /** @brief 在有效 Lease 下写入 MCP 字节。 */
    [[nodiscard]] bool submitMcpInput(quint64 executionId,
                                      const QByteArray& data);
    /** @brief 写入用户字节；非空输入会先抢占 MCP Lease。 */
    void submitUserInput(const QByteArray& data);
    /** @brief 仅在 executionId 匹配时释放 MCP Lease。 */
    void releaseMcpLease(quint64 executionId);

    /** @brief 当前 Session 世代中用户提交非空终端输入的单调代际。 */
    [[nodiscard]] quint64 userInputGeneration() const noexcept
    {
        return _userInputGeneration;
    }

    /** @brief 当前是否存在 MCP Lease。 */
    [[nodiscard]] bool hasMcpLease() const noexcept
    {
        return _executionId != 0;
    }

signals:
    /** @brief 用户即将写入终端；MCP 可先取消尚未完成的主机写入。 */
    void userInputStarted(quint64 userInputGeneration);
    /**
     * @brief 用户输入抢占了 MCP Lease。
     * @param executionId 被抢占的执行 ID。
     * @param executionMayHaveStarted 是否已经向 Transport 写过 MCP 字节。
     */
    void mcpPreempted(quint64 executionId, bool executionMayHaveStarted);

private:
    void clearLease() noexcept;

    QPointer<ITransport> _transport;
    quint64 _generation{0};
    quint64 _executionId{0};
    bool _mcpBytesWritten{false};
    quint64 _userInputGeneration{0};
};
