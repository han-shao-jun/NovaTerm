/**
 * @file   serialport_info.h
 * @brief  串口枚举与热插拔监控。
 *
 * 跨平台串口设备发现：
 *   • Windows: RegisterDeviceNotification 监听 DBT_DEVICEARRIVAL /
 *             DBT_DEVICEREMOVECOMPLETE 原生设备变更消息。
 *   • Linux  : udev monitor 监听 tty 子系统设备变化。
 *
 * 本类是一个隐藏的 QWidget，仅借用窗口句柄接收原生设备消息
 * （Windows 平台 RegisterDeviceNotification 需要 HWND）。
 */
#ifndef SERIALPORT_H
#define SERIALPORT_H

#include <QStringList>
#include <QWidget>
#include <QtSerialPort>
#include <algorithm>

#ifdef Q_OS_WINDOWS

#include <Windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <Dbt.h>
#include <devguid.h>

#elif defined(Q_OS_LINUX)

#include <QSocketNotifier>
#include <libudev.h>

#endif

/**
 * @brief 串口设备发现与热插拔监控组件。
 *
 * 维护当前可用串口列表与正在使用的串口列表，设备插拔时通过
 * update()/disconnected() 信号通知上层刷新 UI 或处理断连。
 */
class SerialPortInfo : public QWidget
{
    Q_OBJECT
public:
    explicit SerialPortInfo(QWidget *parent = nullptr);
    ~SerialPortInfo() override;

    /**
     * @brief 登记一个正在使用的串口。
     * @param comport 串口名（可带描述符，用 ':' 隔开）。
     * @note 设备移除时会检查本列表，对仍在使用但已消失的串口发出 disconnected()。
     */
    void registerUsingSerialPort(const QString &comport);

    /**
     * @brief 取消登记一个正在使用的串口。
     * @param comport 串口名（格式同 registerUsingSerialPort）。
     * @return true 表示成功移除；false 表示列表中不存在该串口。
     */
    bool unregisterUsingSerialPort(const QString &comport);

    /**
     * @brief 枚举当前系统可用的串口。
     * @return 串口名列表（Windows 为 "COMx[:描述]"，Linux 仅保留 ttyUSB 与 ttyACM 前缀设备）。
     * @note 结果按自然排序（数字大小敏感）。
     */
    static QStringList availablePorts();

Q_SIGNALS:
    /**
     * @brief 可用串口列表发生变化。
     * @param ports 最新的可用串口列表。
     */
    void update(const QStringList &ports);

    /**
     * @brief 某个正在使用的串口已消失。
     * @param port 消失的串口名。
     */
    void disconnected(const QString &port);

protected:
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#else
    bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
#endif

    /**
     * @brief 注册设备插拔通知（平台相关）。
     */
    void registerEvent();

private:
    QStringList portsAvailable; // 当前可用串口列表
    QStringList portsUsing;     // 正在被使用的串口列表
#ifdef Q_OS_LINUX
    struct udev *m_udev = nullptr;
    struct udev_monitor *m_udevMonitor = nullptr;
    QSocketNotifier *m_udevNotifier = nullptr;
private Q_SLOTS:
    void onDeviceChanged();
#endif
};

#endif // SERIALPORT_H
