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

// Immutable creation snapshot for an SSH session. UI/Profile code produces
// this value; the transport consumes it without reading widgets or JSON.
// Passwords and key passphrases are deliberately never persisted anywhere.
struct SshConfig
{
    QString host;
    QString username;
    quint16 port{22};

    // "password" | "publickey"
    QString authMethod{QStringLiteral("password")};
    QString password;            // only valid when authMethod == "password"
    QString privateKeyPath;      // only valid when authMethod == "publickey"
    QString keyPassphrase;       // optional, decrypts the private key

    QString terminalType{QStringLiteral("xterm-256color")};
    int keepAliveSeconds{30};    // 0 = disabled
    QString label;

    [[nodiscard]] bool isValid() const
    {
        if (host.trimmed().isEmpty() || username.trimmed().isEmpty())
            return false;
        if (port == 0)
            return false;
        if (authMethod == QStringLiteral("password"))
            return !password.isEmpty();
        if (authMethod == QStringLiteral("publickey"))
            return !privateKeyPath.trimmed().isEmpty();
        return false;
    }
};

Q_DECLARE_METATYPE(SshConfig)

// Host key verification result presented to the UI before the first trust.
// Transport computes this without any UI dependency; a dialog decides.
enum class SshHostKeyStatus { New, Changed };

struct SshHostKeyInfo
{
    QString host;
    quint16 port{0};
    QString keyType;        // e.g. "ssh-ed25519"
    QString fingerprint;    // colon-separated SHA-256 hex
    SshHostKeyStatus status{SshHostKeyStatus::New};
};

Q_DECLARE_METATYPE(SshHostKeyInfo)
