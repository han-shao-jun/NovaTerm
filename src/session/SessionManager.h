/**
 * @file   SessionManager.h
 * @brief  会话集合管理器。
 *
 * 维护活跃 TerminalSession 的注册表：添加、查找、关闭。会话进入 Closed
 * 状态后自动从注册表移除并 deleteLater。所有会话以本对象为父级，
 * 析构时统一关闭（Abort 模式）。
 */
#pragma once

#include "SessionTypes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <memory>

class TerminalSession;

/**
 * @brief 会话注册表：管理多个 TerminalSession 的生命周期。
 *
 * 通过 SessionId 索引会话，监听 stateChanged 信号在会话关闭后自动回收。
 * 线程安全性与 QObject 一致（同线程使用）。
 */
class SessionManager final : public QObject
{
    Q_OBJECT
public:
    explicit SessionManager(QObject* parent = nullptr);
    ~SessionManager() override;

    /**
     * @brief 添加并接管一个会话。
     * @param session 待添加的会话（所有权转移）。
     * @param start   是否立即启动会话。
     * @return 会话 ID；若会话为空或 ID 冲突返回空 UUID。
     */
    SessionId add(std::unique_ptr<TerminalSession> session, bool start = true);

    /**
     * @brief 按 ID 查找会话。
     * @return 会话指针（不存在返回 nullptr）。
     */
    [[nodiscard]] TerminalSession* find(const SessionId& id) const;

    /**
     * @brief 获取所有会话 ID。
     * @return ID 列表。
     */
    [[nodiscard]] QList<SessionId> sessionIds() const;

    /**
     * @brief 当前活跃会话数。
     */
    [[nodiscard]] qsizetype size() const noexcept { return _sessions.size(); }

    /**
     * @brief 关闭指定会话。
     * @param id   会话 ID。
     * @param mode 关闭模式。
     * @return true 表示会话存在并已请求关闭。
     */
    bool close(const SessionId& id, CloseMode mode = CloseMode::Graceful);

    /**
     * @brief 按会话 ID 重新连接。
     * @param id 会话 ID。
     * @return 会话存在、类型支持重连且已提交重连请求时返回 true。
     */
    bool reconnect(const SessionId& id);

    /**
     * @brief 关闭所有会话。
     * @param mode 关闭模式。
     */
    void closeAll(CloseMode mode = CloseMode::Graceful);

signals:
    /**
     * @brief 会话已添加到注册表。
     * @param id 会话 ID。
     */
    void sessionAdded(const SessionId& id);

    /**
     * @brief 会话已从注册表移除。
     * @param id 会话 ID。
     */
    void sessionRemoved(const SessionId& id);

private:
    QHash<SessionId, TerminalSession*> _sessions;
};
