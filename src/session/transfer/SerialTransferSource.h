/** @file SerialTransferSource.h @brief 串口 worker 内创建和持有的只读文件源。 */
#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>

/** @brief 仅在文件 worker 线程使用；失败通过返回值和 error 交付。 */
class SerialTransferSource {
public:
    virtual ~SerialTransferSource() = default;
    [[nodiscard]] virtual quint64 size() const noexcept = 0;
    [[nodiscard]] virtual bool readAt(quint64 offset, qsizetype count,
                                      QByteArray& bytes, QString& error) = 0;
};

struct SerialTransferSourceSpec {
    QString name; ///< 单个协议文件名，不能包含路径。
    /// worker 内调用一次，返回的源也在该线程销毁；工厂不得依赖 GUI 对象。
    std::function<std::unique_ptr<SerialTransferSource>(QString& error)> open;
};

/** @brief 校验单个 UTF-8 协议文件名，拒绝路径、设备名和控制字符。 */
[[nodiscard]] bool validSerialTransferSourceName(const QString& name);
