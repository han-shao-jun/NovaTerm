/** @file TransferFileWorker.h @brief 单任务有界文件线程，仅在线程内拥有文件。 */
#pragma once
#include "SerialTransferTypes.h"
#include "filetransfer/TransferTypes.h"
#include <QThread>
#include <QString>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <atomic>
#include <functional>
#include <filesystem>

struct TransferFileResult {
    quint64 epoch{0};
    bool preparation{false};
    QString error;
    NovaTerm::FileTransfer::TransferRequest request;
    NovaTerm::FileTransfer::OperationId operation{0};
    NovaTerm::FileTransfer::OperationResult result;
};
Q_DECLARE_METATYPE(TransferFileResult)

/** @brief 请求停止后自行退出；持有方不在 GUI 等待线程。 */
class TransferFileWorker final : public QThread {
    Q_OBJECT
public:
    using LinkOperation = std::function<void(const std::filesystem::path&,
                                             const std::filesystem::path&, std::error_code&)>;
    /** @brief 可注入发布系统调用以确定性测试取消竞态，生产默认使用硬链接。 */
    explicit TransferFileWorker(quint64 epoch, SerialTransferRequest request,
                                LinkOperation linkOperation = {}, LinkOperation moveOperation = {});
    void requestStop();
    [[nodiscard]] bool submit(NovaTerm::FileTransfer::TransferAction action);
signals:
    void resultReady(const TransferFileResult& result);
protected:
    void run() override;
private:
    quint64 _epoch;
    SerialTransferRequest _request;
    LinkOperation _linkOperation;
    LinkOperation _moveOperation;
    std::mutex _mutex;
    std::condition_variable _wake;
    std::optional<NovaTerm::FileTransfer::TransferAction> _job;
    bool _busy{true};
    std::atomic_bool _stopped{false};
};
