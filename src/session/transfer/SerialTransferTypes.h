/** @file SerialTransferTypes.h @brief 串口文件传输门面的请求与进度值。 */
#pragma once
#include "filetransfer/TransferTypes.h"
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <optional>

enum class SerialTransferProtocol { XmodemChecksum, XmodemCrc, Xmodem1K, Ymodem, Zmodem };
struct SerialTransferRequest {
    SerialTransferProtocol protocol{SerialTransferProtocol::Zmodem};
    NovaTerm::FileTransfer::Direction direction{NovaTerm::FileTransfer::Direction::Send};
    QStringList files; ///< 上传源路径；X 单文件，Y/Z 可多文件。
    QString destination; ///< 接收 X 的完整目标路径，Y/Z 的接收目录。
    std::optional<quint64> expectedSize; ///< 仅 X 接收的已知长度，不猜测填充。
};
struct SerialTransferProgress {
    bool active{false};
    bool preparing{false};
    quint64 transferId{0};
    NovaTerm::FileTransfer::Direction direction{NovaTerm::FileTransfer::Direction::Send};
    QString currentFile;
    QString status;
    quint64 transferredBytes{0};
    std::optional<quint64> totalBytes;
    quint64 completedFiles{0};
    quint64 elapsedMs{0};
};
Q_DECLARE_METATYPE(SerialTransferProgress)
