#pragma once
#include <QByteArray>
#include <QString>
#include <QJsonObject>
#include <functional>
#include <vector>
#include <optional>

namespace NovaTerm::Evidence {
struct Context { QString principal; QByteArray credential; };
enum class SourceKind { TerminalCapture, DiagnosticResult, ImportedFile };
struct Range { quint64 record{0}; quint32 offset{0},length{0}; };
struct Source {
    SourceKind kind{SourceKind::TerminalCapture};
    QString resource;
    quint64 generation{1};
    std::vector<Range> ranges;
};
struct Reference {
    QString id;
    Source source;
    QByteArray hash;
    qint64 collectedMs{0},expiresAtMs{0};
    bool persistent{false};
    QString sourceCompleteness;
    int projectionVersion{0};
    QJsonObject provenance;
};
struct SourceResult { QByteArray bytes; QString error,completeness; QJsonObject provenance; };
struct Result { std::optional<Reference> reference; QByteArray bytes; QString error; };
using Done=std::function<void(SourceResult)>;
}
