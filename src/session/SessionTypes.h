#pragma once

#include <QMetaType>
#include <QSerialPort>
#include <QString>

// Immutable creation snapshot for a serial session. UI/Profile code produces
// this value; the transport consumes it without reading widgets or JSON.
struct SerialConfig
{
    QString portName;
    qint32 baudRate{115200};
    QSerialPort::DataBits dataBits{QSerialPort::Data8};
    QSerialPort::Parity parity{QSerialPort::NoParity};
    QSerialPort::StopBits stopBits{QSerialPort::OneStop};
    QSerialPort::FlowControl flowControl{QSerialPort::NoFlowControl};
    QString label;

    [[nodiscard]] bool isValid() const
    {
        const bool validDataBits = dataBits == QSerialPort::Data5
            || dataBits == QSerialPort::Data6 || dataBits == QSerialPort::Data7
            || dataBits == QSerialPort::Data8;
        const bool validParity = parity == QSerialPort::NoParity
            || parity == QSerialPort::EvenParity || parity == QSerialPort::OddParity
            || parity == QSerialPort::SpaceParity || parity == QSerialPort::MarkParity;
        const bool validStopBits = stopBits == QSerialPort::OneStop
            || stopBits == QSerialPort::OneAndHalfStop
            || stopBits == QSerialPort::TwoStop;
        const bool validFlowControl = flowControl == QSerialPort::NoFlowControl
            || flowControl == QSerialPort::HardwareControl
            || flowControl == QSerialPort::SoftwareControl;
        return !portName.trimmed().isEmpty() && baudRate > 0 && validDataBits
            && validParity && validStopBits && validFlowControl;
    }
};

Q_DECLARE_METATYPE(SerialConfig)
