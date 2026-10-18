/**
 * @file TerminalEvidenceModel.h
 * @brief Session 提示符证据的唯一写入模型（header-only，无 moc 依赖）。
 *
 * 集中保存候选/就绪代际、用户代际与静默窗；消费者（Coordinator 与
 * Controller）只读或通过显式方法写入，不能各自复活独立真源。
 */
#pragma once

#include <QTimer>

#include <algorithm>
#include <functional>

namespace NovaTerm::Session {

class TerminalEvidenceModel
{
public:
    using UserGeneration = std::function<quint64()>;
    using ReadyHandler = std::function<void(quint64 generation)>;

    explicit TerminalEvidenceModel(UserGeneration currentUser = {})
        : _currentUser(std::move(currentUser))
    {
        _silence.setSingleShot(true);
        _silence.connect(&_silence, &QTimer::timeout, [this] { lockCandidate(); });
    }

    void setCurrentUser(UserGeneration currentUser) { _currentUser = std::move(currentUser); }
    void setReadyHandler(ReadyHandler handler) { _onReady = std::move(handler); }
    void configure(int silenceMs)
    {
        _silenceMs = (std::max)(0, silenceMs);
        reset();
    }

    void reset()
    {
        _silence.stop();
        _candidate = 0;
        _candidateUser = currentUser();
        _prompt = 0;
        _promptUser = currentUser();
    }

    void notePromptReady(quint64 generation, quint64 userGeneration)
    {
        _silence.stop();
        _candidate = 0;
        _candidateUser = userGeneration;
        _prompt = generation;
        _promptUser = userGeneration;
    }

    void noteCandidate(quint64 generation)
    {
        _candidate = generation;
        _candidateUser = currentUser();
        _silence.start(_silenceMs);
    }

    /** @brief 非集成 Shell 的字节会取消候选；无活动命令时还清除就绪证据。 */
    void noteBytes(bool shellIntegration, bool commandActive)
    {
        if (shellIntegration)
            return;
        _silence.stop();
        _candidate = 0;
        if (!commandActive) {
            _prompt = 0;
            _promptUser = currentUser();
        }
    }

    void invalidate()
    {
        _silence.stop();
        _candidate = 0;
        _candidateUser = currentUser();
        _prompt = 0;
        _promptUser = currentUser();
    }

    /** @brief 控制器编码失败回滚；不重新验证候选/静默窗。 */
    void restore(quint64 generation, quint64 userGeneration)
    {
        _silence.stop();
        _candidate = 0;
        _candidateUser = userGeneration;
        _prompt = generation;
        _promptUser = userGeneration;
    }

    [[nodiscard]] quint64 promptGeneration() const { return _prompt; }
    [[nodiscard]] quint64 promptUserGeneration() const { return _promptUser; }
    [[nodiscard]] quint64 candidateGeneration() const { return _candidate; }

private:
    [[nodiscard]] quint64 currentUser() const { return _currentUser ? _currentUser() : 0; }

    void lockCandidate()
    {
        if (_candidate == 0)
            return;
        if (_currentUser && _candidateUser != currentUser()) {
            _candidate = 0;
            return;
        }
        const quint64 generation = _candidate;
        _candidate = 0;
        _prompt = generation;
        _promptUser = _candidateUser;
        if (_onReady)
            _onReady(generation);
    }

    UserGeneration _currentUser;
    ReadyHandler _onReady;
    QTimer _silence;
    int _silenceMs{150};
    quint64 _prompt{0}, _promptUser{0};
    quint64 _candidate{0}, _candidateUser{0};
};

} // namespace NovaTerm::Session
