#pragma once
#include <QObject>
#include <QJsonObject>
#include <functional>
#include <memory>

class SessionDirectory;
namespace NovaTerm::Mcp {
class AccessStore;
class TerminalInteraction final : public QObject {
public:
    using Completion = std::function<void(QJsonObject)>;
    TerminalInteraction(AccessStore*, SessionDirectory*, QObject* parent);
    ~TerminalInteraction() override;
    void call(const QString& client, const QString& request, const QString& tool,
              const QJsonObject& arguments, Completion completion);
    void cancel(const QString& request);
    void revoke(const QString& client, const QString& session);
    QJsonObject capabilities(const QString& client, const QString& session);
    bool ownsCapture(const QString& client, const QString& capture) const;
    std::function<void()> onChanged;
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
}
