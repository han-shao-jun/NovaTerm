#pragma once
#include "capabilities/TargetEnvironment.h"
#include <functional>
#include <QByteArrayView>
#include <QByteArray>
namespace NovaTerm::Session {
// Session integration owns this aggregate; capabilities see only value snapshots.
// All entry methods are local/verified adapter APIs, never model tools.
class EnvironmentTracker final
{
public:
    using Now = std::function<Capabilities::TimePoint()>;
    explicit EnvironmentTracker(Now now = Capabilities::Clock::now);
    void newConnection(Capabilities::SessionBinding);
    void invalidate();
    void declareEnvironment(Capabilities::EnvironmentKind);
    void observeOutput(QByteArrayView);
    void observeCandidate(Capabilities::EnvironmentKind);
    void pairAdapter(QString adapter);
    bool acceptAdapter(const Capabilities::AdapterEnvironmentReport&);
    void confirmOnce(Capabilities::EnvironmentKind, Capabilities::PrincipalId,
        Capabilities::CapabilityId recipe, QString planDigest);
    bool consumeReadiness(quint64 revision, const QString& evidence);
    Capabilities::EnvironmentSnapshot snapshot();
    static constexpr auto EvidenceLifetime = std::chrono::seconds(5);
private:
    Capabilities::EnvironmentSnapshot _snapshot;
    Now _now;
    QByteArray _outputTail;
    QString _adapter;
    quint64 _adapterSequence{0}, _evidenceSequence{0};
};
}
