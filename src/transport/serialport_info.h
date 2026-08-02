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

class SerialPortInfo : public QWidget
{
    Q_OBJECT
public:
    explicit SerialPortInfo(QWidget *parent = nullptr);
    ~SerialPortInfo() override;
    void registerUsingSerialPort(const QString &comport);
    bool unregisterUsingSerialPort(const QString &comport);
    static QStringList availablePorts();
Q_SIGNALS:
    void update(const QStringList &ports);
    void disconnected(const QString &port);

protected:
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#else
    bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
#endif

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
