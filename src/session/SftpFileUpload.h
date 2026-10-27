#pragma once
#include <QMetaType>
#include <QString>
#include <atomic>
#include <memory>

struct SftpFileUploadControl {
    enum class State { Active, AwaitingPublication, Approved, Cancelled, Publishing };
    std::atomic<State> state{State::Active};
    bool cancel() noexcept
    {
        auto current = state.load(std::memory_order_acquire);
        while (current != State::Publishing) {
            if (current == State::Cancelled) return true;
            if (state.compare_exchange_weak(current, State::Cancelled, std::memory_order_acq_rel)) return true;
        }
        return false;
    }
    void approve(bool allowed) noexcept
    {
        auto expected = State::AwaitingPublication;
        state.compare_exchange_strong(expected, allowed ? State::Approved : State::Cancelled, std::memory_order_acq_rel);
    }
};
struct SftpFileUploadRequest {
    quint64 requestId{0};
    QString localRoot, localRelativePath, remoteRoot, remoteName;
    std::shared_ptr<SftpFileUploadControl> control;
};
struct SftpFileUploadResult {
    quint64 requestId{0}, transferredBytes{0}, totalBytes{0};
    bool published{false};
    QString publication{QStringLiteral("not_started")}, temporary{QStringLiteral("none")}, errorCode;
};
Q_DECLARE_METATYPE(SftpFileUploadResult)
