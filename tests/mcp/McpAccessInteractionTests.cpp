#include "mcp/McpAccess.h"
#include "core/terminal/TerminalCore.h"
#include "session/SessionDirectory.h"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

namespace {
class OfflineTransport final : public ITransport {
public:
    bool connectToHost() override { online = true; emit connected(); return true; }
    void disconnect() override { online = false; emit disconnected(); }
    void write(const QByteArray&) override {}
    void resizeTerminal(int, int) override {}
    bool isConnected() const override { return online; }
    QString errorString() const override { return {}; }
    bool online{false};
};
class RetainedCredentials final : public CredentialStore {
public:
    explicit RetainedCredentials(std::shared_ptr<QHash<QString, QByteArray>> values)
        : values(std::move(values)) {}
    bool put(const QString& ref, const QByteArray& secret) override {
        values->insert(ref, secret); return true;
    }
    std::optional<QByteArray> get(const QString& ref) const override {
        return values->contains(ref) ? std::optional<QByteArray>{values->value(ref)} : std::nullopt;
    }
    bool remove(const QString& ref) override { return values->remove(ref) != 0; }
    bool isPersistent() const override { return true; }
    std::shared_ptr<QHash<QString, QByteArray>> values;
};
struct Fixture {
    QTemporaryDir root;
    TerminalCore core{80, 24};
    OfflineTransport transport;
    TerminalSession session{&core};
    SessionDirectory directory;
    NovaTerm::Mcp::AccessStore access{root.path(), std::make_unique<MemoryCredentialStore>(), nullptr,
        [] { return std::make_unique<MemoryCredentialStore>(); }};
    Fixture() {
        session.attach(&transport, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
        static_cast<void>(session.start());
        directory.add(&session);
    }
    SessionDirectory::Entry entry() { return directory.entries().first(); }
};
}

class McpAccessInteractionTests final : public QObject {
    Q_OBJECT
private slots:
    void terminalPermissionRequiresExplicitGrantAndCurrentEpoch();
    void readRevocationClearsNewPermissions();
    void credentialsAreMethodScopedAndNeverUpgradeGrants();
    void credentialMutationRevokesElevationAcrossClients();
    void unsupportedOrStaleCredentialIdentityIsRejected();
    void asyncCredentialsKeepGuiResponsiveAndRejectConcurrentUpdates();
    void anotherInstanceCredentialReplacementRevokesManagedGrant();
    void staleCredentialBindingCannotWriteToReattachedSession();
    void asyncPersistentCredentialsReloadOtherInstanceMetadata();
};

void McpAccessInteractionTests::terminalPermissionRequiresExplicitGrantAndCurrentEpoch()
{
    Fixture f;
    QVERIFY(f.access.setEnabled(true));
    const auto a = f.access.addClient("a"), b = f.access.addClient("b");
    auto entry = f.entry();
    QVERIFY(!f.access.setTerminalGrant(a, entry, true));
    QVERIFY(f.access.setGrant(a, entry, true, {}, true, true, true));
    QVERIFY(f.access.canRunCommand(a, entry));
    QVERIFY(!f.access.canControlTerminal(a, entry));
    QVERIFY(f.access.setTerminalGrant(a, entry, true));
    QVERIFY(f.access.canControlTerminal(a, entry));
    QVERIFY(!f.access.canControlTerminal(b, entry));
    QVERIFY(!f.access.canUseElevation(a, entry));
    // Updating an old permission must not silently drop an explicit new grant.
    QVERIFY(f.access.setGrant(a, entry, true));
    QVERIFY(f.access.canControlTerminal(a, entry));
    entry.epoch = "reconnected";
    QVERIFY(!f.access.canControlTerminal(a, entry));
    QVERIFY(!f.access.canUseElevation(a, entry));
    QVERIFY(f.access.setGrant(a, entry, true));
    QVERIFY(!f.access.canControlTerminal(a, entry));
    QVERIFY(f.access.setTerminalGrant(a, entry, true));
    QVERIFY(!f.access.canRunCommand(a, entry));
}

void McpAccessInteractionTests::readRevocationClearsNewPermissions()
{
    Fixture f;
    QVERIFY(f.access.setEnabled(true));
    const auto client = f.access.addClient("client");
    const auto entry = f.entry();
    QVERIFY(f.access.setGrant(client, entry, true));
    QVERIFY(f.access.setTerminalGrant(client, entry, true, true));
    QVERIFY(f.access.canUseElevation(client, entry));
    QVERIFY(f.access.setGrant(client, entry, false));
    QVERIFY(!f.access.canControlTerminal(client, entry));
    QVERIFY(!f.access.canUseElevation(client, entry));
    QVERIFY(f.access.setGrant(client, entry, true));
    QVERIFY(!f.access.canControlTerminal(client, entry));
}

void McpAccessInteractionTests::credentialsAreMethodScopedAndNeverUpgradeGrants()
{
    Fixture f;
    QVERIFY(f.access.setEnabled(true));
    const auto client = f.access.addClient("client");
    const auto entry = f.entry();
    QVERIFY(!entry.targetFingerprint.isEmpty());
    QVERIFY(f.access.setElevationCredential(entry, "sudo", "private-sudo-value"));
    QVERIFY(f.access.hasElevationCredential(entry, "sudo"));
    QVERIFY(!f.access.hasElevationCredential(entry, "su"));
    QVERIFY(!f.access.canUseElevation(client, entry));
    QVERIFY(!f.access.elevationCredentialsPersistent());
    QVERIFY(!f.access.setElevationCredential(entry, "sudo", {}));
    QVERIFY(f.access.hasElevationCredential(entry, "sudo"));
    QVERIFY(f.access.setElevationCredential(entry, "su", "private-su-value"));
    QVERIFY(f.access.removeElevationCredential(entry, "sudo"));
    QVERIFY(!f.access.hasElevationCredential(entry, "sudo"));
    QVERIFY(f.access.hasElevationCredential(entry, "su"));
    QFile config(f.root.filePath("access.json"));
    QVERIFY(config.open(QIODevice::ReadOnly));
    const auto saved = config.readAll();
    QVERIFY(!saved.contains("private-sudo-value"));
    QVERIFY(!saved.contains("private-su-value"));
}

void McpAccessInteractionTests::credentialMutationRevokesElevationAcrossClients()
{
    Fixture f;
    QVERIFY(f.access.setEnabled(true));
    const auto entry = f.entry();
    const auto a = f.access.addClient("a"), b = f.access.addClient("b");
    for (const auto& client : {a, b}) {
        QVERIFY(f.access.setGrant(client, entry, true));
        QVERIFY(f.access.setTerminalGrant(client, entry, true, true));
    }
    QSignalSpy changed(&f.access, &NovaTerm::Mcp::AccessStore::changed);
    QVERIFY(f.access.setElevationCredential(entry, "sudo", "secret"));
    QVERIFY(!changed.isEmpty());
    for (const auto& client : {a, b}) {
        QVERIFY(f.access.canControlTerminal(client, entry));
        QVERIFY(!f.access.canUseElevation(client, entry));
        QVERIFY(f.access.setTerminalGrant(client, entry, true, true));
    }
    QVERIFY(f.access.removeElevationCredential(entry, "sudo"));
    QVERIFY(!f.access.canUseElevation(a, entry));
    QVERIFY(!f.access.canUseElevation(b, entry));
}

void McpAccessInteractionTests::unsupportedOrStaleCredentialIdentityIsRejected()
{
    Fixture f;
    auto entry = f.entry();
    QVERIFY(!f.access.setElevationCredential(entry, "other", "secret"));
    entry.targetFingerprint = "wrong-target";
    QVERIFY(!f.access.setElevationCredential(entry, "sudo", "secret"));
    entry = f.entry();
    entry.kind = TransportKind::Telnet;
    QVERIFY(!f.access.setElevationCredential(entry, "sudo", "secret"));
    entry.kind = TransportKind::Serial;
    QVERIFY(!f.access.setElevationCredential(entry, "sudo", "secret"));
    entry = f.entry();
    entry.epoch = "stale-epoch";
    // Entry identity is checked against the live Session generation and binding.
    ++entry.generation;
    QVERIFY(!f.access.setElevationCredential(entry, "sudo", "secret"));
}

void McpAccessInteractionTests::asyncCredentialsKeepGuiResponsiveAndRejectConcurrentUpdates()
{
    Fixture f;
    const auto entry = f.entry();
    bool finished = false;
    bool success = false;
    QThread* callbackThread = nullptr;
    QVERIFY(f.access.configureElevationCredentialAsync(entry, "sudo", "secret", false,
        [&](bool ok) { finished = true; success = ok; callbackThread = QThread::currentThread(); }));
    QVERIFY(!finished); // No inline system-keyring operation or nested GUI loop.
    QVERIFY(!f.access.configureElevationCredentialAsync(entry, "su", "other", false, {}));
    QTRY_VERIFY(finished);
    QVERIFY(success);
    QCOMPARE(callbackThread, f.access.thread());
    QVERIFY(f.access.hasElevationCredential(entry, "sudo"));
    QVERIFY(!f.access.hasElevationCredential(entry, "su"));
    QVERIFY(!f.access.elevationCredentialsPersistent());
    finished = false;
    QVERIFY(f.access.configureElevationCredentialAsync(entry, "sudo", {}, true,
        [&](bool ok) { finished = true; success = ok; }));
    QTRY_VERIFY(finished);
    QVERIFY(success);
    QVERIFY(!f.access.hasElevationCredential(entry, "sudo"));
}

void McpAccessInteractionTests::anotherInstanceCredentialReplacementRevokesManagedGrant()
{
    Fixture f;
    const auto entry = f.entry();
    QTemporaryDir state;
    auto secrets = std::make_shared<QHash<QString, QByteArray>>();
    NovaTerm::Mcp::AccessStore first(state.path(), std::make_unique<RetainedCredentials>(secrets));
    QVERIFY(first.setEnabled(true));
    const auto client = first.addClient("client");
    QVERIFY(!client.isEmpty());
    QVERIFY(first.setGrant(client, entry, true));
    QVERIFY(first.setTerminalGrant(client, entry, true, true));
    NovaTerm::Mcp::AccessStore other(state.path(), std::make_unique<RetainedCredentials>(secrets));
    QVERIFY(other.setElevationCredential(entry, "sudo", "secret"));
    QTRY_VERIFY(!first.canUseElevation(client, entry));
    QVERIFY(first.canControlTerminal(client, entry));
    QVERIFY(first.hasElevationCredential(entry, "sudo"));
}

void McpAccessInteractionTests::staleCredentialBindingCannotWriteToReattachedSession()
{
    Fixture f;
    const auto original = f.entry();
    OfflineTransport replacement;
    replacement.online = true;
    f.session.attach(&replacement, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
    const auto current = f.entry();
    QVERIFY(current.transport != original.transport);
    QVERIFY(current.attachmentId != original.attachmentId);
    bool callbackCalled = false;
    QVERIFY(!f.access.configureElevationCredentialAsync(original, "sudo", "original-target-secret", false,
        [&](bool) { callbackCalled = true; }));
    QVERIFY(!callbackCalled);
    QVERIFY(!f.access.hasElevationCredential(current, "sudo"));
    QVERIFY(!f.access.setElevationCredential(original, "sudo", "original-target-secret"));
}

void McpAccessInteractionTests::asyncPersistentCredentialsReloadOtherInstanceMetadata()
{
    Fixture f;
    const auto entry = f.entry();
    QTemporaryDir state;
    auto secrets = std::make_shared<QHash<QString, QByteArray>>();
    const auto factory = [secrets] { return std::make_unique<RetainedCredentials>(secrets); };
    NovaTerm::Mcp::AccessStore first(state.path(), factory(), nullptr, factory);
    bool finished = false;
    bool success = false;
    QVERIFY(first.configureElevationCredentialAsync(entry, "sudo", "first-secret", false,
        [&](bool ok) { finished = true; success = ok; }));
    QTRY_VERIFY(finished);
    QVERIFY(success);
    QVERIFY(first.elevationCredentialsPersistent());
    NovaTerm::Mcp::AccessStore other(state.path(), factory());
    QVERIFY(other.setElevationCredential(entry, "su", "second-secret"));
    QTRY_VERIFY(first.hasElevationCredential(entry, "su"));
    QVERIFY(first.hasElevationCredential(entry, "sudo"));
    // A subsequent unrelated write must keep the other instance's metadata.
    QVERIFY(first.setEnabled(true));
    NovaTerm::Mcp::AccessStore reloaded(state.path(), factory());
    QVERIFY(reloaded.hasElevationCredential(entry, "sudo"));
    QVERIFY(reloaded.hasElevationCredential(entry, "su"));
}

QTEST_GUILESS_MAIN(McpAccessInteractionTests)
#include "McpAccessInteractionTests.moc"
