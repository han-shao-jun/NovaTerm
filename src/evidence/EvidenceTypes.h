#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>
#include <functional>

namespace NovaTerm::Evidence {
enum class SourceKind { TerminalCapture, DiagnosticResult, ImportedFile };
enum class Error { None, InvalidRequest, PermissionDenied, EvidenceExpired, LimitExceeded, PersistenceUnavailable };
QString errorCode(Error);

// Created by the trusted application entry, never decoded from model arguments.
struct ResourceContext { QString principalId; quint64 tokenGeneration = 0; };
struct ResourceBinding {
    SourceKind kind = SourceKind::TerminalCapture;
    QString resourceId;
    quint64 generation = 0;
    QString ownerPrincipalId;
};
struct RawFragment { quint64 recordId = 0; quint64 byteOffset = 0; quint64 byteCount = 0; };
struct DiagnosticProvenance {
    QString recipeId;
    quint64 recipeVersion = 0;
    quint64 targetGeneration = 0;
    quint64 environmentGeneration = 0;
    QString executionRoute;
    bool completed = false;
    bool truncated = false;
    bool sharedTerminal = false;
};
struct EvidenceRef {
    QString evidenceId;
    ResourceBinding source;
    QVector<RawFragment> fragments;
    QByteArray contentHash; // SHA-256, hexadecimal; concatenated ordered raw fragments.
    QDateTime collectedAt;
    QDateTime expiresAt;
    QString persistence = QStringLiteral("memory");
    QString sourceCompleteness;
    quint64 projectionVersion = 0;
    DiagnosticProvenance diagnostic;
};
struct EvidenceRequest {
    ResourceBinding source;
    QVector<RawFragment> fragments;
    qint64 ttlMs = 60000;
    bool persist = false;
    DiagnosticProvenance diagnostic;
};
struct EvidenceResult {
    Error error = Error::None;
    EvidenceRef reference;
    QByteArray bytes;
    bool verifiable = false;
};
using EvidenceCallback = std::function<void(EvidenceResult)>;
}
