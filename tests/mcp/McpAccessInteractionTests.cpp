#include "mcp/McpAccess.h"
#include "mcp/McpProtocol.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QLockFile>
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
#include <utility>

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
struct CredentialProbe {
    QHash<QString, QByteArray> values;
    int reads{0}, writes{0}, removals{0}, statusReads{0};
    bool rejectWrites{false};
    bool persistent{true};
    std::function<void()> afterWrite;
    std::function<void()> afterRead;
};
class ProbedCredentials final : public CredentialStore {
public:
    explicit ProbedCredentials(std::shared_ptr<CredentialProbe> state) : state(std::move(state)) {}
    bool put(const QString& ref, const QByteArray& secret) override {
        ++state->writes;
        if (state->rejectWrites) return false;
        state->values.insert(ref, secret);
        if (state->afterWrite) {
            auto callback = std::exchange(state->afterWrite, {});
            callback();
        }
        return true;
    }
    std::optional<QByteArray> get(const QString& ref) const override {
        ++state->reads;
        const auto result = state->values.contains(ref) ? std::optional<QByteArray>{state->values.value(ref)} : std::nullopt;
        if (state->afterRead) {
            auto callback = std::exchange(state->afterRead, {});
            callback();
        }
        return result;
    }
    bool remove(const QString& ref) override { ++state->removals; return state->values.remove(ref) != 0; }
    bool isPersistent() const override { ++state->statusReads; return state->persistent; }
    std::shared_ptr<CredentialProbe> state;
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
    void legacyClientsAreNotAuthenticatedOrEnabled();
    void enablingCreatesOneSharedAccess();
    void statusAndReloadNeverAccessCredentials();
    void elevationPersistenceStartsUnknownWithoutCredentialAccess();
    void elevationPersistenceTracksCompletedSaveAndReload_data();
    void elevationPersistenceTracksCompletedSaveAndReload();
    void sharedAccessCreationAndRotationRollbackOnFailure();
    void concurrentRotationCannotOverwriteCommittedToken();
    void reentrantCreationCannotReplaceCommittedSharedAccess();
    void reentrantRotationCannotReplaceCommittedToken();
    void exportRejectsTokenRotatedDuringCredentialRead();
    void disableDuringCredentialWriteCancelsMutation_data();
    void disableDuringCredentialWriteCancelsMutation();
    void ephemeralAccessDoesNotReviveAfterRestart();
    void rotationPreservesSharedTerminalSelections();
    void terminalPermissionRequiresExplicitGrantAndCurrentEpoch();
    void readRevocationClearsNewPermissions();
    void clearTerminalGrantStaysIndependentAndConnectionBound();
    void credentialsAreMethodScopedAndNeverUpgradeGrants();
    void credentialMutationRevokesSharedElevation();
    void unsupportedOrStaleCredentialIdentityIsRejected();
    void asyncCredentialsKeepGuiResponsiveAndRejectConcurrentUpdates();
    void destructionDoesNotWaitForCredentialBackend();
    void anotherInstanceCredentialReplacementRevokesManagedGrant();
    void staleCredentialBindingCannotWriteToReattachedSession();
    void asyncPersistentCredentialsReloadOtherInstanceMetadata();
    void clientIdentitySurvivesUnrelatedReloadAndTracksRevocation();
};

void McpAccessInteractionTests::legacyClientsAreNotAuthenticatedOrEnabled()
{
    using namespace NovaTerm::Mcp;
    QTemporaryDir state;
    auto secrets = std::make_shared<QHash<QString, QByteArray>>();
    const auto id = newId();
    const QByteArray token(64, 'a');
    secrets->insert("mcp-client-" + id, token);
    QVERIFY(secureDirectory(state.path()));
    QVERIFY(writePrivateJson(state.filePath("access.json"), {{"version", 1},
        {"revision", "41"}, {"enabled", true}, {"clients", QJsonArray{QJsonObject{
            {"id", id}, {"label", "legacy"}, {"persistent", true},
            {"digest", QString::fromLatin1(QCryptographicHash::hash(token, QCryptographicHash::Sha256).toHex())}}}}}));
    AccessStore access(state.path(), std::make_unique<RetainedCredentials>(secrets));
    QVERIFY(access.authenticate(token).isEmpty());
    QVERIFY(!access.enabled());
    QVERIFY(access.accessId().isEmpty());
    QCOMPARE(secrets->value("mcp-client-" + id), token);
    QVERIFY(access.setEnabled(true)); // Retains legacy revision for compare-and-swap migration.
    QVERIFY(access.authenticate(token).isEmpty());
    const auto stored = readJson(state.filePath("access.json"));
    QVERIFY(stored);
    QCOMPARE(stored->value("version").toInt(), 2);
    QVERIFY(!stored->contains("clients"));
    QVERIFY(!access.accessId().isEmpty());
}

void McpAccessInteractionTests::enablingCreatesOneSharedAccess()
{
    Fixture f;
    QVERIFY(f.access.accessId().isEmpty());
    QVERIFY(f.access.setEnabled(true));
    QVERIFY(!f.access.accessId().isEmpty());
    const auto id = f.access.accessId();
    const auto token = f.access.exportToken();
    QVERIFY(token);
    QCOMPARE(f.access.authenticate(*token), id);
    QVERIFY(f.access.setEnabled(true));
    QVERIFY(!f.access.accessId().isEmpty());
    QCOMPARE(f.access.exportToken(), token);
}

void McpAccessInteractionTests::statusAndReloadNeverAccessCredentials()
{
    using namespace NovaTerm::Mcp;
    QTemporaryDir state;
    auto probe = std::make_shared<CredentialProbe>();
    AccessStore access(state.path(), std::make_unique<ProbedCredentials>(probe));
    QVERIFY(access.accessId().isEmpty());
    QVERIFY(!access.enabled());
    QVERIFY(!access.elevationCredentialsPersistent().has_value());
    QVERIFY(!access.exportToken());
    QCOMPARE(probe->reads + probe->writes + probe->removals + probe->statusReads, 0);
    QVERIFY(access.setEnabled(true));
    const auto token = access.exportToken();
    QVERIFY(token);
    const int before = probe->reads + probe->writes + probe->removals + probe->statusReads;
    AccessStore reloaded(state.path(), std::make_unique<ProbedCredentials>(probe));
    QCOMPARE(reloaded.accessId(), access.accessId());
    QCOMPARE(reloaded.authenticate(*token), access.accessId());
    QVERIFY(reloaded.enabled());
    static_cast<void>(reloaded.elevationCredentialsPersistent());
    QCOMPARE(probe->reads + probe->writes + probe->removals + probe->statusReads, before);
}

void McpAccessInteractionTests::elevationPersistenceStartsUnknownWithoutCredentialAccess()
{
    using namespace NovaTerm::Mcp;
    QTemporaryDir state;
    auto probe = std::make_shared<CredentialProbe>();
    AccessStore access(state.path(), std::make_unique<ProbedCredentials>(probe));
    // The optional conversion keeps this a behavioral regression against the old bool API.
    QVERIFY(!std::optional<bool>{access.elevationCredentialsPersistent()}.has_value());
    QCOMPARE(probe->reads + probe->writes + probe->removals + probe->statusReads, 0);
    QVERIFY(access.setEnabled(true));
    const int before = probe->reads + probe->writes + probe->removals + probe->statusReads;
    QVERIFY(!std::optional<bool>{access.elevationCredentialsPersistent()}.has_value());
    AccessStore reloaded(state.path(), std::make_unique<ProbedCredentials>(probe));
    QVERIFY(!std::optional<bool>{reloaded.elevationCredentialsPersistent()}.has_value());
    QCOMPARE(probe->reads + probe->writes + probe->removals + probe->statusReads, before);
}

void McpAccessInteractionTests::elevationPersistenceTracksCompletedSaveAndReload_data()
{
    QTest::addColumn<bool>("persistent");
    QTest::addColumn<bool>("asynchronous");
    QTest::newRow("persistent-sync") << true << false;
    QTest::newRow("volatile-sync") << false << false;
    QTest::newRow("persistent-worker") << true << true;
    QTest::newRow("volatile-worker") << false << true;
}

void McpAccessInteractionTests::elevationPersistenceTracksCompletedSaveAndReload()
{
    using namespace NovaTerm::Mcp;
    QFETCH(bool, persistent);
    QFETCH(bool, asynchronous);
    Fixture fixture;
    const auto entry = fixture.entry();
    QTemporaryDir state;
    auto probe = std::make_shared<CredentialProbe>();
    probe->persistent = persistent;
    const auto factory = [probe] { return std::make_unique<ProbedCredentials>(probe); };
    AccessStore access(state.path(), factory(), nullptr, factory);
    if (asynchronous) {
        bool finished = false;
        bool success = false;
        QVERIFY(access.configureElevationCredentialAsync(entry, "sudo", "secret", false,
            [&](bool ok) { success = ok; finished = true; }));
        QTRY_VERIFY(finished);
        QVERIFY(success);
    } else {
        QVERIFY(access.setElevationCredential(entry, "sudo", "secret"));
    }
    const auto actual = std::optional<bool>{access.elevationCredentialsPersistent()};
    QVERIFY(actual.has_value());
    QCOMPARE(*actual, persistent);
    const int before = probe->reads + probe->writes + probe->removals + probe->statusReads;
    AccessStore reloaded(state.path(), factory());
    const auto restored = std::optional<bool>{reloaded.elevationCredentialsPersistent()};
    if (persistent) {
        QVERIFY(reloaded.hasElevationCredential(entry, "sudo"));
        QVERIFY(restored.has_value());
        QVERIFY(*restored);
    } else {
        QVERIFY(!reloaded.hasElevationCredential(entry, "sudo"));
        QVERIFY(!restored.has_value());
    }
    QCOMPARE(probe->reads + probe->writes + probe->removals + probe->statusReads, before);
}

void McpAccessInteractionTests::sharedAccessCreationAndRotationRollbackOnFailure()
{
    using namespace NovaTerm::Mcp;
    QTemporaryDir state;
    auto probe = std::make_shared<CredentialProbe>();
    AccessStore access(state.path(), std::make_unique<ProbedCredentials>(probe));
    probe->rejectWrites = true;
    QVERIFY(!access.setEnabled(true));
    QVERIFY(!access.enabled());
    QVERIFY(access.accessId().isEmpty());
    QVERIFY(!QFile::exists(state.filePath("access.json")));
    probe->rejectWrites = false;
    QVERIFY(secureDirectory(state.path()));
    QLockFile lock(state.filePath("access.lock"));
    QVERIFY(lock.tryLock(0));
    QVERIFY(!access.setEnabled(true));
    QVERIFY(access.accessId().isEmpty());
    QVERIFY(!access.enabled());
    QVERIFY(probe->values.isEmpty());
    QCOMPARE(access.lastSaveFailure(), AccessStore::SaveFailure::Locked);
    lock.unlock();
    QVERIFY(access.setEnabled(true));
    const auto id = access.accessId();
    const auto token = access.exportToken();
    QVERIFY(token);
    const auto identity = access.identityVersion(id);
    const auto version = access.version(id);
    const auto credentials = probe->values;
    QSignalSpy revoked(&access, &AccessStore::revoked);
    probe->rejectWrites = true;
    QVERIFY(!access.rotateToken());
    probe->rejectWrites = false;
    QVERIFY(lock.tryLock(0));
    QVERIFY(!access.rotateToken());
    QCOMPARE(access.exportToken(), token);
    QCOMPARE(access.authenticate(*token), id);
    QCOMPARE(access.identityVersion(id), identity);
    QCOMPARE(access.version(id), version);
    QCOMPARE(probe->values, credentials);
    QVERIFY(revoked.isEmpty());
    lock.unlock();
    QVERIFY(access.rotateToken());
    QVERIFY(access.authenticate(*token).isEmpty());
    QVERIFY(access.identityVersion(id) > identity);
    QCOMPARE(probe->values.size(), 1);
}

void McpAccessInteractionTests::ephemeralAccessDoesNotReviveAfterRestart()
{
    using namespace NovaTerm::Mcp;
    QTemporaryDir state;
    AccessStore first(state.path(), std::make_unique<MemoryCredentialStore>());
    QVERIFY(first.setEnabled(true));
    const auto id = first.accessId();
    const auto token = first.exportToken();
    QVERIFY(token);
    const auto saved = readJson(state.filePath("access.json"));
    QVERIFY(saved);
    QVERIFY(saved->value("access").toObject().isEmpty());
    AccessStore second(state.path(), std::make_unique<MemoryCredentialStore>());
    QVERIFY(second.accessId().isEmpty());
    QVERIFY(!second.enabled());
    QVERIFY(second.authenticate(*token).isEmpty());
    QVERIFY(second.setEnabled(true));
    QVERIFY(second.accessId() != id);
    QTRY_COMPARE(first.accessId(), id);
    QCOMPARE(first.authenticate(*token), id);
    QCOMPARE(first.ensureAccess(), id);
    QVERIFY(second.setEnabled(false));
    QTRY_VERIFY(!first.enabled());
}

void McpAccessInteractionTests::concurrentRotationCannotOverwriteCommittedToken()
{
    using namespace NovaTerm::Mcp;
    QTemporaryDir state;
    auto secrets = std::make_shared<QHash<QString, QByteArray>>();
    const auto factory = [secrets] { return std::make_unique<RetainedCredentials>(secrets); };
    AccessStore first(state.path(), factory());
    QVERIFY(first.setEnabled(true));
    AccessStore stale(state.path(), factory());
    const auto original = stale.exportToken();
    QVERIFY(original);
    QVERIFY(first.rotateToken());
    const auto committed = first.exportToken();
    QVERIFY(committed);
    const auto savedCredentials = *secrets;
    // No event-loop turn: stale still holds the previous JSON revision.
    QVERIFY(!stale.rotateToken());
    QCOMPARE(stale.lastSaveFailure(), AccessStore::SaveFailure::Revision);
    QCOMPARE(*secrets, savedCredentials);
    QCOMPARE(first.exportToken(), committed);
    QTRY_COMPARE(stale.authenticate(*committed), first.accessId());
    QCOMPARE(stale.exportToken(), committed);
    QVERIFY(stale.authenticate(*original).isEmpty());
}

void McpAccessInteractionTests::reentrantCreationCannotReplaceCommittedSharedAccess()
{
    using namespace NovaTerm::Mcp;
    QTemporaryDir state;
    auto probe = std::make_shared<CredentialProbe>();
    const auto factory = [probe] { return std::make_unique<ProbedCredentials>(probe); };
    AccessStore access(state.path(), factory());
    AccessStore other(state.path(), factory());
    QString nested;
    bool nestedEnabled = true;
    bool committed = false;
    probe->afterWrite = [&] {
        nested = access.ensureAccess();
        nestedEnabled = access.setEnabled(true);
        committed = other.setEnabled(true);
        QTest::qWait(50); // Model a platform keyring's nested event loop and directory refresh.
    };
    const bool result = access.setEnabled(true);
    QVERIFY(nested.isEmpty());
    QVERIFY(!nestedEnabled);
    QVERIFY(committed);
    QVERIFY(!result);
    QCOMPARE(access.accessId(), other.accessId());
    QCOMPARE(access.exportToken(), other.exportToken());
    QCOMPARE(probe->values.size(), 1);
    QVERIFY(access.enabled());
}

void McpAccessInteractionTests::reentrantRotationCannotReplaceCommittedToken()
{
    using namespace NovaTerm::Mcp;
    QTemporaryDir state;
    auto probe = std::make_shared<CredentialProbe>();
    const auto factory = [probe] { return std::make_unique<ProbedCredentials>(probe); };
    AccessStore access(state.path(), factory());
    QVERIFY(access.setEnabled(true));
    AccessStore other(state.path(), factory());
    const auto original = access.exportToken();
    QVERIFY(original);
    bool committed = false;
    probe->afterWrite = [&] {
        committed = other.rotateToken();
        QTest::qWait(50);
    };
    QVERIFY(!access.rotateToken());
    QVERIFY(committed);
    const auto token = other.exportToken();
    QVERIFY(token);
    QCOMPARE(access.exportToken(), token);
    QCOMPARE(access.authenticate(*token), other.accessId());
    QVERIFY(access.authenticate(*original).isEmpty());
    QCOMPARE(probe->values.size(), 1);
}

void McpAccessInteractionTests::exportRejectsTokenRotatedDuringCredentialRead()
{
    using namespace NovaTerm::Mcp;
    QTemporaryDir state;
    auto probe = std::make_shared<CredentialProbe>();
    AccessStore access(state.path(), std::make_unique<ProbedCredentials>(probe));
    QVERIFY(access.setEnabled(true));
    const auto original = access.exportToken();
    QVERIFY(original);
    bool rotated = false;
    probe->afterRead = [&] { rotated = access.rotateToken(); };
    const auto stale = access.exportToken();
    QVERIFY(rotated);
    QVERIFY(!stale);
    const auto token = access.exportToken();
    QVERIFY(token);
    QVERIFY(*token != *original);
    QCOMPARE(access.authenticate(*token), access.accessId());
}

void McpAccessInteractionTests::disableDuringCredentialWriteCancelsMutation_data()
{
    QTest::addColumn<bool>("initialized");
    QTest::addColumn<bool>("lockStorage");
    QTest::newRow("create") << false << false;
    QTest::newRow("rotate") << true << false;
    QTest::newRow("create-save-failure") << false << true;
    QTest::newRow("rotate-save-failure") << true << true;
}

void McpAccessInteractionTests::disableDuringCredentialWriteCancelsMutation()
{
    using namespace NovaTerm::Mcp;
    QFETCH(bool, initialized);
    QFETCH(bool, lockStorage);
    QTemporaryDir state;
    auto probe = std::make_shared<CredentialProbe>();
    AccessStore access(state.path(), std::make_unique<ProbedCredentials>(probe));
    if (initialized) QVERIFY(access.setEnabled(true));
    const auto original = access.exportToken();
    const auto epoch = access.tokenEpoch();
    const auto savedCredentials = probe->values;
    QVERIFY(secureDirectory(state.path()));
    QLockFile lock(state.filePath("access.lock"));
    if (lockStorage) QVERIFY(lock.tryLock(0));
    bool disabledSaved = false;
    probe->afterWrite = [&] { disabledSaved = access.setEnabled(false); };
    QVERIFY(!(initialized ? access.rotateToken() : access.setEnabled(true)));
    QCOMPARE(disabledSaved, !lockStorage);
    QVERIFY(!access.enabled());
    QCOMPARE(access.exportToken(), original);
    QCOMPARE(access.tokenEpoch(), epoch);
    QCOMPARE(probe->values, savedCredentials);
    if (original) QVERIFY(access.authenticate(*original).isEmpty());
    else QVERIFY(access.accessId().isEmpty());
}

void McpAccessInteractionTests::rotationPreservesSharedTerminalSelections()
{
    using namespace NovaTerm::Mcp;
    Fixture f;
    QVERIFY(f.access.setEnabled(true));
    const auto id = f.access.accessId();
    const auto entry = f.entry();
    QVERIFY(TestSupport::setGrant(f.access, id, entry, true));
    QVERIFY(TestSupport::setTerminalGrant(f.access, id, entry, true, false));
    const auto token = f.access.exportToken();
    QVERIFY(token);
    const auto identity = f.access.identityVersion(id);
    QVERIFY(f.access.rotateToken());
    QCOMPARE(f.access.accessId(), id);
    QVERIFY(f.access.identityVersion(id) > identity);
    QVERIFY(f.access.authenticate(*token).isEmpty());
    QVERIFY(f.access.canControlTerminal(id, entry));
    QVERIFY(!f.access.canUseElevation(id, entry));
    QVERIFY(!f.access.canClearTerminal(id, entry));
    QVERIFY(f.access.setEnabled(false));
    QVERIFY(!f.access.canRead(id, entry));
}

void McpAccessInteractionTests::clientIdentitySurvivesUnrelatedReloadAndTracksRevocation()
{
    QTemporaryDir state;
    auto secrets = std::make_shared<QHash<QString, QByteArray>>();
    const auto factory = [secrets] { return std::make_unique<RetainedCredentials>(secrets); };
    NovaTerm::Mcp::AccessStore first(state.path(), factory());
    QVERIFY(first.setEnabled(true));
    const auto client = first.ensureAccess();
    QVERIFY(!client.isEmpty());
    const auto original = first.identityVersion(client);
    QVERIFY(original > 0);
    NovaTerm::Mcp::AccessStore other(state.path(), factory());
    QCOMPARE(other.ensureAccess(), client);
    QVERIFY(other.setEnabled(true));
    QTest::qWait(50);
    QCOMPARE(first.accessId(), client);
    QCOMPARE(first.identityVersion(client), original);
    QVERIFY(other.rotateToken());
    const auto token = *other.exportToken();
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
    QVERIFY(QFile::remove(state.filePath("access.json")));
    QTRY_COMPARE(first.identityVersion(client), quint64(0));
    QVERIFY(first.accessId().isEmpty());
}

void McpAccessInteractionTests::terminalPermissionRequiresExplicitGrantAndCurrentEpoch()
{
    Fixture f;
    QVERIFY(f.access.setEnabled(true));
    const auto a = f.access.ensureAccess(), b = QStringLiteral("forged-access");
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
    const auto client = f.access.ensureAccess();
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
    const auto client = f.access.ensureAccess();
    const auto other = QStringLiteral("forged-access");
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
    const auto client = f.access.ensureAccess();
    const auto entry = f.entry();
    QVERIFY(!entry.targetFingerprint.isEmpty());
    QVERIFY(f.access.setElevationCredential(entry, "sudo", "private-sudo-value"));
    QVERIFY(f.access.hasElevationCredential(entry, "sudo"));
    QVERIFY(!f.access.hasElevationCredential(entry, "su"));
    QVERIFY(!f.access.canUseElevation(client, entry));
    QVERIFY(f.access.elevationCredentialsPersistent() == std::optional<bool>{false});
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

void McpAccessInteractionTests::credentialMutationRevokesSharedElevation()
{
    Fixture f;
    QVERIFY(f.access.setEnabled(true));
    const auto entry = f.entry();
    const auto a = f.access.ensureAccess();
    const auto token = f.access.exportToken();
    QVERIFY(token);
    const auto b = f.access.authenticate(*token);
    QCOMPARE(a, b);
    QVERIFY(NovaTerm::Mcp::TestSupport::setGrant(f.access, a, entry, true));
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.access, a, entry, true, true));
    QSignalSpy changed(&f.access, &NovaTerm::Mcp::AccessStore::changed);
    QVERIFY(f.access.setElevationCredential(entry, "sudo", "secret"));
    QVERIFY(!changed.isEmpty());
    for (const auto& client : {a, b}) {
        QVERIFY(f.access.canControlTerminal(client, entry));
        QVERIFY(!f.access.canUseElevation(client, entry));
    }
    QVERIFY(NovaTerm::Mcp::TestSupport::setTerminalGrant(f.access, a, entry, true, true));
    QVERIFY(f.access.canUseElevation(b, entry));
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
    QVERIFY(f.access.elevationCredentialsPersistent() == std::optional<bool>{false});
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
    const auto client = first.ensureAccess();
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
    QVERIFY(first.elevationCredentialsPersistent() == std::optional<bool>{true});
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
