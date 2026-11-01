#include "SerialFileTargetAdapter.h"
#include "transport/SerialTransport.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
namespace NovaTerm::Mcp {
SerialFileTargetAdapter::SerialFileTargetAdapter(SessionDirectory& sessions, QObject* parent)
    : QObject(parent), _sessions(sessions) {}
std::optional<SerialFileBinding> SerialFileTargetAdapter::liveBinding(const QString& id)
{
    const auto entry = _sessions.find(id);
    if (!entry || !entry->session || entry->kind != TransportKind::Serial
        || entry->state != SessionState::Running || entry->session->state() != SessionState::Running
        || entry->session->transport() != entry->transport || entry->session->statistics().generation != entry->generation)
        return std::nullopt;
    const auto* serial = qobject_cast<SerialTransport*>(entry->transport.data());
    if (!serial || !serial->isConnected() || serial->hasPendingDisconnect()
        || !serial->config().isValid() || serial->config().dataBits != QSerialPort::Data8) return std::nullopt;
    const auto io = serial->ioStatistics();
    if (!io.generation || !io.connected || !io.desiredOpen || io.closePending || io.stopping) return std::nullopt;
    const auto& config = serial->config();
    const QJsonArray fields{config.portName, config.baudRate, int(config.dataBits), int(config.parity),
        int(config.stopBits), int(config.flowControl)};
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(fields).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    std::optional<QString> fingerprint;
    if (!entry->targetFingerprint.isEmpty()) fingerprint = entry->targetFingerprint;
    return SerialFileBinding{{SessionId{entry->id}, AttachmentId{entry->attachmentId}, Epoch{entry->epoch},
        Generation{entry->generation}, fingerprint}, io.generation, digest};
}
}
