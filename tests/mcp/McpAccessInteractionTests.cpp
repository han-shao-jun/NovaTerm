#include "mcp/McpAccess.h"
#include "core/terminal/TerminalCore.h"
#include "session/SessionDirectory.h"
#include "TestGrantSupport.h"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QSemaphore>
#include <QElapsedTimer>
#include <atomic>
#include <thread>

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
struct BlockedCredentialState {
    QSemaphore entered, release;
    std::atomic<bool> destroyed{false};
};
class BlockingCredentials final : public CredentialStore {
public:
    explicit BlockingCredentials(std::shared_ptr<BlockedCredentialState> state) : state(std::move(state)) {}
    ~BlockingCredentials() override { state->destroyed = true; }
    bool put(const QString&, const QByteArray&) override {
        state->entered.release(); state->release.acquire(); return true;
    }
    std::optional<QByteArray> get(const QString&) const override { return std::nullopt; }
    bool remove(const QString&) override { return false; }
    bool isPersistent() const override { return false; }
    std::shared_ptr<BlockedCredentialState> state;
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
    void clearTerminalGrantStaysIndependentAndConnectionBound();
    void credentialsAreMethodScopedAndNeverUpgradeGrants();
    void credentialMutationRevokesElevationAcrossClients();
    void unsupportedOrStaleCredentialIdentityIsRejected();
    void asyncCredentialsKeepGuiResponsiveAndRejectConcurrentUpdates();
    void destructionDoesNotWaitForCredentialBackend();
    void anotherInstanceCredentialReplacementRevokesManagedGrant();
    void staleCredentialBindingCannotWriteToReattachedSession();
    void asyncPersistentCredentialsReloadOtherInstanceMetadata();
    void clientIdentitySurvivesUnrelatedReloadAndTracksRevocation();
};

void McpAccessInteractionTests::clientIdentitySurvivesUnrelatedReloadAndTracksRevocation()
{
    QTemporaryDir state;
    auto secrets = std::make_shared<QHash<QString, QByteArray>>();
    const auto factory = [secrets] { return std::make_unique<RetainedCredentials>(secrets); };
    NovaTerm::Mcp::AccessStore first(state.path(), factory());
    QVERIFY(first.setEnabled(true));
    const auto client = first.addClient("client");
    QVERIFY(!client.isEmpty());
    const auto original = first.identityVersion(client);
    QVERIFY(original > 0);
    NovaTerm::Mcp::AccessStore other(state.path(), factory());
    QVERIFY(!other.addClient("unrelated").isEmpty());
    QTRY_COMPARE(first.clients().size(), 2);
    QCOMPARE(first.identityVersion(client), original);
    QVERIFY(other.rotateToken(client));
    const auto token = *other.exportToken(client);
    QTRY_COMPARE(first.authenticate(token), client);
    const auto rotated = first.identityVersion(client);
    QVERIFY(rotated > original);
    QVERIFY(other.setEnabled(false));
    QTRY_VERIFY(!first.enabled());
    const auto stopped = first.identityVersion(client);
    QVERIFY(stopped > rotated);
    QVERIFY(other.setEnabled(true));
    QTRY_VERIFY(first.enabled());
    QCOMPARE(first.identityVersion(client), stopped);
    QVERIFY(other.removeClient(client));
    QTRY_COMPARE(first.identityVersion(client), quint64(0));
}

void McpAccessInteractionTests::terminalPermissionRequiresExplicitGrantAndCurrentEpoch()
{
    Fixture f;
    QVERIFY(f.access.setEnabled(true));
    const auto a = f.access.addClient("a"), b = f.access.addClient("b");
    auto entry = f.entry();
    QVERIFY(!NovaTerm::Mcp::TestSupport::setTerminalGrant(f.access, a, entry, true));
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.access, a, entry, true, {}, true, true, true));
    QVERIFY(f.access.canRunCommand(a, entry));
    QVERIFY(!f.access.canControlTerminal(a, entry));
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.access, a, entry, true));
    QVERIFY(f.access.canControlTerminal(a, entry));
    QVERIFY(!f.access.canControlTerminal(b, entry));
    QVERIFY(!f.access.canUseElevation(a, entry));
    // Updating an old permission must not silently drop an explicit new grant.
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.access, a, entry, true));
    QVERIFY(f.access.canControlTerminal(a, entry));
    entry.epoch = "reconnected";
    QVERIFY(!f.access.canControlTerminal(a, entry));
    QVERIFY(!f.access.canUseElevation(a, entry));
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.access, a, entry, true));
    QVERIFY(!f.access.canControlTerminal(a, entry));
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.access, a, entry, true));
    QVERIFY(!f.access.canRunCommand(a, entry));
}

void McpAccessInteractionTests::readRevocationClearsNewPermissions()
{
    Fixture f;
    QVERIFY(f.access.setEnabled(true));
    const auto client = f.access.addClient("client");
    const auto entry = f.entry();
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.access, client, entry, true));
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.access, client, entry, true, true));
    QVERIFY(f.access.canUseElevation(client, entry));
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.access, client, entry, false));
    QVERIFY(!f.access.canControlTerminal(client, entry));
    QVERIFY(!f.access.canUseElevation(client, entry));
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.access, client, entry, true));
    QVERIFY(!f.access.canControlTerminal(client, entry));
}

void McpAccessInteractionTests::clearTerminalGrantStaysIndependentAndConnectionBound()
{
    using namespace NovaTerm::Mcp;
    Fixture f;
    QVERIFY(f.access.setEnabled(true));
    const auto client = f.access.addClient("clear-client");
    const auto other = f.access.addClient("other-client");
    auto entry = f.entry();
    QVERIFY(!f.access.canClearTerminal(client, entry));
    QVERIFY(TestSupport::setGrant(f.access, client, entry, true));
    QVERIFY(TestSupport::setTerminalGrant(f.access, client, entry, true));
    QVERIFY(!f.access.canClearTerminal(client, entry));
    const auto view = f.access.grantSnapshot(ClientId{client}, entry);
    PermissionChange request;
    request.client = ClientId{client};
    request.binding = view.binding;
    request.expectedRevision = view.revision;
    request.kind = PermissionKind::ClearTerminal;
    request.selected = true;
    QVERIFY(!isError(f.access.changePermission(request, entry)));
    QVERIFY(f.access.canClearTerminal(client, entry));
    QVERIFY(!f.access.canClearTerminal(other, entry));
    auto stale = entry;
    ++stale.generation;
    QVERIFY(!f.access.canClearTerminal(client, stale));
    stale = entry;
    stale.targetFingerprint += QStringLiteral("other");
    QVERIFY(!f.access.canClearTerminal(client, stale));
    f.transport.online = false;
    QVERIFY(!f.access.canClearTerminal(client, entry));
    f.transport.online = true;
    QVERIFY(TestSupport::setGrant(f.access, client, entry, false));
    QVERIFY(!f.access.canClearTerminal(client, entry));
    QVERIFY(TestSupport::setGrant(f.access, client, entry, true));
    QVERIFY(!f.access.canClearTerminal(client, entry));
    const auto refreshed = f.access.grantSnapshot(ClientId{client}, entry);
    request.expectedRevision = refreshed.revision;
    QVERIFY(!isError(f.access.changePermission(request, entry)));
    entry.epoch = QStringLiteral("reconnected");
    QVERIFY(!f.access.canClearTerminal(client, entry));
    QVERIFY(TestSupport::setGrant(f.access, client, entry, true));
    QVERIFY(!f.access.canClearTerminal(client, entry));
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
        QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.access, client, entry, true));
        QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.access, client, entry, true, true));
    }
    QSignalSpy changed(&f.access, &NovaTerm::Mcp::AccessStore::changed);
    QVERIFY(f.access.setElevationCredential(entry, "sudo", "secret"));
    QVERIFY(!changed.isEmpty());
    for (const auto& client : {a, b}) {
        QVERIFY(f.access.canControlTerminal(client, entry));
        QVERIFY(!f.access.canUseElevation(client, entry));
        QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.access, client, entry, true, true));
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

void McpAccessInteractionTests::destructionDoesNotWaitForCredentialBackend()
{
    Fixture fixture;
    auto state = std::make_shared<BlockedCredentialState>();
    auto access = std::make_unique<NovaTerm::Mcp::AccessStore>(fixture.root.filePath("blocked"),
        std::make_unique<MemoryCredentialStore>(), nullptr,
        [state] { return std::make_unique<BlockingCredentials>(state); });
    int callbacks = 0;
    QVERIFY(access->configureElevationCredentialAsync(fixture.entry(), "sudo", "private", false,
        [&](bool) { ++callbacks; }));
    QVERIFY(state->entered.tryAcquire(1, 2000));
    // 仅用于让旧的无界wait失败而非挂死；正常路径在释放后端之前即可析构。
    std::thread watchdog([state] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        state->release.release();
    });
    QElapsedTimer timer; timer.start();
    access.reset();
    const auto elapsed = timer.elapsed();
    state->release.release();
    watchdog.join();
    QTRY_VERIFY(state->destroyed.load());
    QCOMPARE(callbacks, 0);
    QVERIFY2(elapsed < 200, qPrintable(QStringLiteral("GUI destruction blocked for %1 ms").arg(elapsed)));
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
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(first, client, entry, true));
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(first, client, entry, true, true));
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
