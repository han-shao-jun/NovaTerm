#include "serialport_info.h"

#include <QCollator>

SerialPortInfo::SerialPortInfo(QWidget *parent) : QWidget(parent)
{
    setParent(parent);
    hide();
    this->registerEvent();
    portsAvailable = availablePorts();
    portsUsing.clear();
}

SerialPortInfo::~SerialPortInfo()
{
#ifdef Q_OS_LINUX
    if (m_udevMonitor) {
        udev_monitor_unref(m_udevMonitor);
        m_udevMonitor = nullptr;
    }
    if (m_udev) {
        udev_unref(m_udev);
        m_udev = nullptr;
    }
#endif
}

#ifdef Q_OS_WINDOWS

/**
 * @brief 注册windos平台串口插拔事件
 */
void SerialPortInfo::registerEvent()
{
    static const GUID GUID_DEVINTERFACE_LIST[] = {
        // GUID_DEVINTERFACE_USB_DEVICE
        //    {0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } },
        //    // GUID_DEVINTERFACE_DISK
        //    { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } },
        //    // GUID_DEVINTERFACE_HID,
        //    { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } },
        //    // GUID_NDIS_LAN_CLASS
        //    { 0xad498944, 0x762f, 0x11d0, { 0x8d, 0xcb, 0x00, 0xc0, 0x4f, 0xc3, 0x35, 0x8c } },
        //    // GUID_DEVINTERFACE_COMPORT
        {0x86e0d1e0, 0x8089, 0x11d0, {0x9c, 0xe4, 0x08, 0x00, 0x3e, 0x30, 0x1f, 0x73}},
        //    // GUID_DEVINTERFACE_SERENUM_BUS_ENUMERATOR
        //    { 0x4D36E978, 0xE325, 0x11CE, { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 } },
        //    // GUID_DEVINTERFACE_PARALLEL
        //    { 0x97F76EF0, 0xF883, 0x11D0, { 0xAF, 0x1F, 0x00, 0x00, 0xF8, 0x00, 0x84, 0x5C } },
        //    // GUID_DEVINTERFACE_PARCLASS
        //    { 0x811FC6A5, 0xF728, 0x11D0, { 0xA5, 0x37, 0x00, 0x00, 0xF8, 0x75, 0x3E, 0xD1 } },
    };
    // 注册插拔事件
    HDEVNOTIFY hDevNotify;
    DEV_BROADCAST_DEVICEINTERFACE NotifacationFiler;
    ZeroMemory(&NotifacationFiler, sizeof(DEV_BROADCAST_DEVICEINTERFACE));
    NotifacationFiler.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    NotifacationFiler.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    for (auto i : GUID_DEVINTERFACE_LIST) {
        NotifacationFiler.dbcc_classguid = i;
        // GetCurrentUSBGUID();
        hDevNotify = RegisterDeviceNotification(HANDLE(this->winId()), &NotifacationFiler,
                                                DEVICE_NOTIFY_WINDOW_HANDLE);
        if (!hDevNotify) {
            GetLastError();
        }
    }
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))

/**
 * @brief 处理windos平台自定义的事件
 * @param eventType 事件类型
 * @param message 消息
 * @param result 处理结果
 * @return
 */
bool SerialPortInfo::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
#else
bool nativeEvent(const QByteArray &eventType, void *message, long *result)
#endif
{
    MSG *msg = reinterpret_cast<MSG *>(message); // 第一层解算
    UINT msgType = msg->message;
    if (msgType == WM_DEVICECHANGE) {
        auto lParam = PDEV_BROADCAST_HDR(msg->lParam); // 第二层解算
        switch (msg->wParam) {
        case DBT_DEVICEARRIVAL: // 设备插入
            if (lParam->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                qDebug() << "DBT_DEVICEARRIVAL";
                portsAvailable = availablePorts();
                emit update(portsAvailable);
            }
            break;
        case DBT_DEVICEREMOVECOMPLETE: // 设备移除
            if (lParam->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                qDebug() << "DBT_DEVICEREMOVECOMPLETE";
                portsAvailable = availablePorts();
                if (!portsUsing.empty()) {
                    for (const auto &uart : portsUsing) {
                        if (!portsAvailable.contains(uart)) {
                            qDebug() << uart;
                            emit disconnected(uart);
                            this->unregisterUsingSerialPort(uart); // 移除正在被使用串口
                        }
                    }
                }
                emit update(portsAvailable);
            }
            break;
        default:
            break;
        }
    }
    return false;
}

#else  // Q_OS_LINUX

/**
 * @brief 注册linux平台串口插拔事件（通过 udev 监控 tty 设备变化）
 */
void SerialPortInfo::registerEvent()
{
    m_udev = udev_new();
    if (!m_udev) {
        qWarning() << "SerialPortInfo: Failed to create udev context";
        return;
    }

    m_udevMonitor = udev_monitor_new_from_netlink(m_udev, "udev");
    if (!m_udevMonitor) {
        qWarning() << "SerialPortInfo: Failed to create udev monitor";
        udev_unref(m_udev);
        m_udev = nullptr;
        return;
    }

    // 只监听 tty 子系统的设备变化（串口属于 tty 子系统）
    udev_monitor_filter_add_match_subsystem_devtype(m_udevMonitor, "tty", nullptr);
    udev_monitor_enable_receiving(m_udevMonitor);

    int fd = udev_monitor_get_fd(m_udevMonitor);
    m_udevNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_udevNotifier, &QSocketNotifier::activated, this, &SerialPortInfo::onDeviceChanged);
}

/**
 * @brief 处理 udev 设备变化事件
 */
void SerialPortInfo::onDeviceChanged()
{
    struct udev_device *dev = udev_monitor_receive_device(m_udevMonitor);
    if (!dev) {
        return;
    }

    QString action = QString::fromLatin1(udev_device_get_action(dev));
    QString devnode = QString::fromLatin1(udev_device_get_devnode(dev));
    udev_device_unref(dev);

    // 只处理 /dev/tty* 设备（串口设备）
    if (!devnode.startsWith("/dev/tty")) {
        return;
    }

    qDebug() << "Serial port" << action << ":" << devnode;

    portsAvailable = availablePorts();

    if (action == "remove") {
        if (!portsUsing.empty()) {
            for (const auto &uart : portsUsing) {
                if (!portsAvailable.contains(uart)) {
                    qDebug() << uart;
                    emit disconnected(uart);
                    this->unregisterUsingSerialPort(uart); // 移除正在被使用串口
                }
            }
        }
    }
    emit update(portsAvailable);
}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))

/**
 * @brief 处理linux平台自定义的事件（预留，linux下不需要处理原生事件）
 * @param eventType 事件类型
 * @param message 消息
 * @param result 处理结果
 * @return
 */
bool SerialPortInfo::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
#else
bool SerialPortInfo::nativeEvent(const QByteArray &eventType, void *message, long *result)
#endif
{
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
}

#endif

/**
 * @brief 注册要使用的串口
 * @param comport 串口名带描述符，用":"隔开
 */
void SerialPortInfo::registerUsingSerialPort(const QString &comport) { portsUsing.append(comport); }

/**
 * @brief 取消注册使用的串口
 * @param comport 串口名带描述符，用":"隔开
 * @return 是否成功取消注册
 */
bool SerialPortInfo::unregisterUsingSerialPort(const QString &comport)
{
    return portsUsing.removeOne(comport);
}

/**
 * @brief 获取可用的端口
 * @return 可用的端口的列表
 */
QStringList SerialPortInfo::availablePorts()
{
    QStringList portList;

    for (const auto &info : QSerialPortInfo::availablePorts()) {
#ifdef Q_OS_WINDOWS
        const QString portName = info.portName();
#else
        // Linux 的 tty 子系统包含控制台、虚拟终端以及未实际连接的
        // ttyS 设备。串口助手默认只显示常见的 USB 串口设备。
        const QString portName = info.portName();
        if (!portName.startsWith(QLatin1String("ttyUSB")) &&
            !portName.startsWith(QLatin1String("ttyACM"))) {
            continue;
        }
#endif

        QString displayName = portName;
        if (!info.description().isEmpty()) {
            displayName.append(QLatin1Char(':')).append(info.description());
        }

        qDebug() << "Available port:" << displayName;
        portList.append(displayName);
    }

    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    std::sort(portList.begin(), portList.end(),
              [&collator](const QString &left, const QString &right) {
                  return collator.compare(left, right) < 0;
              });

    return portList;
}
