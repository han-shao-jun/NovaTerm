#include "resources/HistoryCapabilities.h"
#include "capabilities/McpCapabilityAdapter.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
using namespace NovaTerm;
namespace R = Recording;
namespace E = Evidence;
namespace C = Capabilities;
namespace {
R::HistoryReadPort source(R::HistoryCaptureId id, QByteArray bytes = "error\n") {
    return {[id](R::HistoryCaptureId requested) -> std::optional<R::HistoryResource> {
        if (requested != id) return {};
        R::HistoryResource resource; resource.capture.id = id; resource.capture.highWaterRecordId = 1; return resource;
    }, [id, bytes](R::HistoryCaptureId, R::HistoryRange, auto done) {
        R::HistoryRecord record; record.captureId = id; record.recordId = record.lastRecordId = 1; record.payload = bytes;
        R::ReadResult result; result.records.push_back(record); done(std::move(result)); return true;
    }};
}
Resources::GrantPersistence memoryGrants() { return {{}, [](const auto&) { return true; }}; }
Resources::EvidenceSearchPage search(Resources::ResourceSharingService& resources, R::HistorySearchRequest query) {
    std::optional<Resources::EvidenceSearchPage> result;
    resources.searchAsync(std::move(query), {"local", 7}, [&](auto value) { result = std::move(value); });
    QElapsedTimer wait; wait.start(); while (!result && wait.elapsed() < 5000) QTest::qWait(1);
    if (result) return std::move(*result);
    Resources::EvidenceSearchPage missing; missing.page.error = "TEST_TIMEOUT"; return missing;
}
}
class ResourceSharingTests final : public QObject {
    Q_OBJECT
private slots:
    void cancelledCachedSearchDoesNotReplayEvidence() {
        Resources::ResourceSharingService resources([](const QString&) { return quint64(7); }, memoryGrants());
        const R::HistoryCaptureId capture{QUuid::createUuid()}; resources.setHistorySource(source(capture, "ab ab ab ab"));
        QVERIFY(resources.share("local", capture, 1, 0, false).isEmpty());
        R::HistorySearchRequest query; query.captureId = capture; query.query = "ab"; query.mode = R::HistorySearchMode::RawBytes; query.maxMatches = 1;
        query.cursor = search(resources, query).page.nextCursor; QVERIFY(!query.cursor.isEmpty());
        const auto page = search(resources, query); QVERIFY(page.page.error.isEmpty()); QCOMPARE(page.page.hits.size(), size_t(1));
        query.cancelled = std::make_shared<std::atomic<bool>>(true);
        const auto cancelled = search(resources, query); QCOMPARE(cancelled.page.error, QString("CANCELLED"));
        QVERIFY(cancelled.page.hits.empty()); QVERIFY(cancelled.evidence.evidenceId.isEmpty());
    }
    void admissionLostDuringEvidenceReadReleasesSearchLease() {
        Resources::ResourceSharingService resources([](const QString&) { return quint64(7); }, memoryGrants());
        const R::HistoryCaptureId capture{QUuid::createUuid()}; auto port = source(capture, "ab ab ab ab");
        const auto original = port.readCommitted; int calls = 0; bool admitted = true;
        port.readCommitted = [&](auto id, auto range, auto done) {
            if (++calls == 2) admitted = false; // Search read succeeds; evidence read invalidates admission.
            return original(id, range, std::move(done));
        };
        resources.setHistorySource(port); QVERIFY(resources.share("local", capture, 1, 0, false).isEmpty());
        for (int i = 0; i < 4; ++i) {
            calls = 0; admitted = true;
            R::HistorySearchRequest query; query.captureId = capture; query.query = "ab"; query.mode = R::HistorySearchMode::RawBytes;
            query.maxMatches = 1; query.admitted = [&] { return admitted; };
            const auto page = search(resources, query);
            QCOMPARE(page.page.error, QString("CANCELLED")); QVERIFY(page.page.hits.empty()); QVERIFY(page.page.nextCursor.isEmpty());
        }
    }
    void twoTrustedEntriesShareOneApplicationAndDisableIndependently() {
        Resources::ResourceSharingService resources([](const QString& id) -> quint64 { return id == "external" || id == "local" ? 7 : 0; }, memoryGrants());
        const R::HistoryCaptureId capture{QUuid::createUuid()}; resources.setHistorySource(source(capture));
        QVERIFY(resources.share("external", capture, 1, 0, false).isEmpty());
        QVERIFY(resources.share("local", capture, 1, 0, false).isEmpty());
        auto registry = C::CapabilityRegistry::implementedCapabilities();
        C::CapabilityAuthorization authorization([&](const C::PrincipalId& principal, C::EntryKind,
            const C::ResourceBinding& binding, const C::Definition&) -> std::optional<C::AuthorizationSnapshot> {
            const auto* history = std::get_if<C::HistoryCaptureBinding>(&binding); if (!history) return {};
            const auto grant = resources.authorize(resources.context(principal.value()), {QUuid(history->id.value())}, history->generation);
            if (!grant) return {};
            return C::AuthorizationSnapshot{binding, grant->accessVersion};
        });
        C::CapabilityDispatcher dispatcher(registry, authorization); Resources::HistoryCapabilities ports(dispatcher, resources);
        C::McpCapabilityAdapter external(authorization, C::PrincipalId{"external"});
        C::McpCapabilityAdapter local(authorization, C::PrincipalId{"local"});
        const auto run = [&](auto& entry, const QString& principal) {
            const auto grant = resources.authorize(resources.context(principal), capture);
            const auto invocation = entry.context(C::HistoryCaptureBinding{C::ResourceId{capture.text()}, 1}, grant->accessVersion,
                C::OperationId{QUuid::createUuid().toString()}, C::Clock::now() + std::chrono::seconds(5));
            R::HistoryReadRequest request; request.captureId = capture;
            std::optional<C::Result<R::HistoryReadPage>> result;
            ports.read.invoke(request, invocation, [&](auto value) { result = std::move(value); });
            QElapsedTimer wait; wait.start(); while (!result && wait.elapsed() < 5000) QTest::qWait(1);
            return result;
        };
        const auto remote = run(external, "external"); QVERIFY(remote); QVERIFY(std::holds_alternative<R::HistoryReadPage>(*remote));
        QCOMPARE(std::get<R::HistoryReadPage>(*remote).byteCount, qsizetype(6));
        external.setEnabled(false);
        const auto denied = run(external, "external"); QVERIFY(denied); QVERIFY(std::holds_alternative<C::Rejection>(*denied));
        const auto trusted = run(local, "local"); QVERIFY(trusted); QVERIFY(std::holds_alternative<R::HistoryReadPage>(*trusted));
        QCOMPARE(std::get<R::HistoryReadPage>(*trusted).byteCount, qsizetype(6));
        resources.setHistorySource(source(capture, QByteArray(65536, 'x')));
        for (int i = 0; i < 70; ++i) {
            external.setEnabled(true);
            const auto grant = resources.authorize(resources.context("external"), capture);
            const auto invocation = external.context(C::HistoryCaptureBinding{C::ResourceId{capture.text()}, 1}, grant->accessVersion,
                C::OperationId{QUuid::createUuid().toString()}, C::Clock::now() + std::chrono::seconds(5));
            R::HistoryReadRequest request; request.captureId = capture; request.maxPayloadBytes = 1;
            std::optional<C::Result<R::HistoryReadPage>> result;
            ports.read.invoke(request, invocation, [&](auto value) { result = std::move(value); });
            external.setEnabled(false); QTRY_VERIFY(result);
            QVERIFY(std::holds_alternative<C::Rejection>(*result));
        }
        external.setEnabled(true); const auto afterClose = run(external, "external");
        QVERIFY(afterClose); QVERIFY(std::holds_alternative<R::HistoryReadPage>(*afterClose));
        QVERIFY(std::get<R::HistoryReadPage>(*afterClose).error.isEmpty());
    }
    void importedEvidenceSurvivesRecordingSourceRemoval() {
        Resources::ResourceSharingService resources([](const QString& id) -> quint64 { return id == "local" ? 7 : 0; }, memoryGrants());
        QTemporaryDir root; QVERIFY(root.isValid()); QFile file(root.filePath("explicit.log")); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("offline contents"), qint64(16)); file.close();
        std::optional<Resources::ImportResult> imported;
        resources.importFile("local", file.fileName(), [&](auto result) { imported = std::move(result); });
        QTRY_VERIFY(imported); QVERIFY2(imported->error.isEmpty(), qPrintable(imported->error));
        E::EvidenceRequest request; request.source = {E::SourceKind::ImportedFile, imported->resourceId, 1, "local"};
        request.fragments.append({0, 0, 16}); std::optional<E::EvidenceResult> created;
        resources.createEvidence(request, {"local", 7}, [&](auto result) { created = std::move(result); });
        QTRY_VERIFY(created); QCOMPARE(created->error, E::Error::None);
        resources.setHistorySource(source({QUuid::createUuid()})); resources.setHistorySource({});
        std::optional<E::EvidenceResult> resolved;
        resources.resolveEvidence(created->reference.evidenceId, {"local", 7}, 16, [&](auto result) { resolved = std::move(result); });
        QTRY_VERIFY(resolved); QCOMPARE(resolved->error, E::Error::None); QCOMPARE(resolved->bytes, QByteArray("offline contents"));
        resources.clearImportedFiles("local"); resolved.reset();
        resources.resolveEvidence(created->reference.evidenceId, {"local", 7}, 16, [&](auto result) { resolved = std::move(result); });
        QTRY_VERIFY(resolved); QVERIFY(resolved->error != E::Error::None); QVERIFY(resolved->bytes.isEmpty());
    }
    void evidenceUsesTheSourceGeneration() {
        Resources::ResourceSharingService resources([](const QString&) { return quint64(7); }, memoryGrants());
        const R::HistoryCaptureId capture{QUuid::createUuid()}; auto port = source(capture);
        const auto describe = port.describe;
        port.describe = [describe](R::HistoryCaptureId id) { auto resource = describe(id); if (resource) resource->generation = 3; return resource; };
        resources.setHistorySource(port); QVERIFY(resources.share("local", capture, 1, 0, false).isEmpty());
        QVERIFY(resources.sharingStatus("local", capture).startsWith("Shared records"));
        R::HistorySearchRequest query; query.captureId = capture; query.query = "error";
        const auto page = search(resources, query); QVERIFY2(page.page.error.isEmpty(), qPrintable(page.page.error));
        QCOMPARE(page.page.hits.size(), size_t(1)); QCOMPARE(page.evidence.source.generation, quint64(3));
    }
    void persistenceRestoresOnlyTheCurrentPrincipalEpoch() {
        std::vector<R::ResourceGrant> saved; quint64 epoch = 7;
        Resources::GrantPersistence persistence{[&] { return saved; }, [&](const auto& grants) { saved = grants; return true; }};
        const R::HistoryCaptureId capture{QUuid::createUuid()};
        { Resources::ResourceSharingService resources([&](const QString&) { return epoch; }, persistence);
          resources.setHistorySource(source(capture)); QVERIFY(resources.share("owner", capture, 1, 0, true).isEmpty()); }
        QCOMPARE(saved.size(), size_t(1));
        { Resources::ResourceSharingService restored([&](const QString&) { return epoch; }, persistence);
          restored.setHistorySource(source(capture)); QVERIFY(restored.authorize({"owner", 7}, capture)); }
        ++epoch; Resources::ResourceSharingService rotated([&](const QString&) { return epoch; }, persistence);
        rotated.setHistorySource(source(capture)); QVERIFY(!rotated.authorize({"owner", epoch}, capture));
    }
};
QTEST_GUILESS_MAIN(ResourceSharingTests)
#include "ResourceSharingTests.moc"
