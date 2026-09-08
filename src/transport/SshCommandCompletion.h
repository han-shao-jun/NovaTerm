/**
 * @file   SshCommandCompletion.h
 * @brief  SSH 单次命令的异步完成状态聚合。
 *
 * SSH 的 EOF、exit-status 与 close 是彼此独立的消息。尤其不能把先到达的 EOF
 * 当作命令已经具有退出状态，否则 libssh 会以 -1 表示“尚未收到状态”。
 */
#pragma once

#include <optional>

class SshCommandCompletion final
{
public:
    enum class Result
    {
        Pending,
        Exited,
        MissingExitStatus
    };

    void reset() noexcept
    {
        _outputEnded = false;
        _remoteClosed = false;
        _exitStatus.reset();
    }

    void observeOutputEnd() noexcept { _outputEnded = true; }
    void observeRemoteClose() noexcept { _remoteClosed = true; }
    void observeExitStatus(int status) noexcept { _exitStatus = status; }

    [[nodiscard]] Result result() const noexcept
    {
        if (!_outputEnded)
            return Result::Pending;
        if (_exitStatus.has_value())
            return Result::Exited;
        return _remoteClosed ? Result::MissingExitStatus : Result::Pending;
    }

    [[nodiscard]] const std::optional<int>& exitStatus() const noexcept
    {
        return _exitStatus;
    }

private:
    bool _outputEnded{false};
    bool _remoteClosed{false};
    std::optional<int> _exitStatus;
};
