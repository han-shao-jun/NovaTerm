#include "evidence/EvidenceStore.h"
#include "evidence/ImportedResourceStore.h"
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QThread>
#include <QThreadPool>
#include <QtTest>
#include <optional>

using namespace NovaTerm::Evidence;
namespace {
const ResourceContext Owner{QStringLiteral("owner"), 7};
class FakeSource final : public EvidenceSourceReader {
public:
    mutable bool alive = true;
    mutable quint64 generation = 1;
    mutable QByteArray content = "abcdef";
    mutable std::function<void()> duringRead;
    mutable bool deferred = false;
    mutable std::function<void()> complete;
    void readAsync(const ResourceBinding&, const QVector<RawFragment>& ranges, qsizetype,
                   std::function<void(SourceReadResult)> callback) const override {
        SourceReadResult result;
        result.generation = generation;
        result.sourceCompleteness = QStringLiteral("unknown_tail");
        result.projectionVersion = 3;
        if (!alive) result.error = Error::EvidenceExpired;
        for (const auto& range : ranges)
            result.bytes += content.mid(static_cast<qsizetype>(range.byteOffset), static_cast<qsizetype>(range.byteCount));
        if (duringRead) duringRead();
        auto finish = [callback = std::move(callback), result] { callback(result); };
        if (deferred) complete = std::move(finish); else finish();
    }
};
EvidenceRequest request(SourceKind kind = SourceKind::TerminalCapture) {
    EvidenceRequest r;
    r.source = {kind, QStringLiteral("resource"), 1, Owner.principalId};
    r.fragments = {{1, 0, 2}, {1, 4, 2}};
    return r;
}
EvidenceResult create(EvidenceStore& store, EvidenceRequest r = request(), ResourceContext context = Owner) {
    EvidenceResult result;
    store.createAsync(std::move(r), std::move(context), [&result](auto value) { result = std::move(value); });
    return result;
}
EvidenceResult resolve(EvidenceStore& store, const QString& id, ResourceContext context = Owner,
                       qsizetype maxBytes = EvidenceStore::MaxBytes) {
    EvidenceResult result;
    store.resolveAsync(id, std::move(context), maxBytes, [&result](auto value) { result = std::move(value); });
    return result;
}
bool authorize(const ResourceContext& context, const ResourceBinding& binding,
               const QVector<RawFragment>&, AuthorizationPurpose purpose) {
    return context.tokenGeneration == 7 && context.principalId == binding.ownerPrincipalId
        && purpose != AuthorizationPurpose::CrossPrincipal;
}
// Hold both worker slots so asynchronous admission bounds are deterministic.
class PausedWorkers final {
public:
    PausedWorkers() : _previous(QThreadPool::globalInstance()->maxThreadCount()) {
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(2);
        for (int i = 0; i < 2; ++i)
            QThreadPool::globalInstance()->start([this] { _started.release(); _release.acquire(); });
        _started.acquire(2);
    }
    ~PausedWorkers() {
        _release.release(2);
        QThreadPool::globalInstance()->waitForDone();
        QThreadPool::globalInstance()->setMaxThreadCount(_previous);
    }
private:
    QSemaphore _started;
    QSemaphore _release;
    int _previous;
};
}
class EvidenceTests final : public QObject {
    Q_OBJECT
private slots:
    void concurrentResolutionsHaveBoundedAdmission() {
        class Deferred final : public EvidenceSourceReader {
        public:
            mutable QVector<std::function<void(SourceReadResult)>> callbacks;
            void readAsync(const ResourceBinding&, const QVector<RawFragment>&, qsizetype,
                std::function<void(SourceReadResult)> callback) const override { callbacks.push_back(std::move(callback)); }
        };
        EvidenceStore store(authorize); auto immediate = std::make_shared<FakeSource>();
        store.registerSource(SourceKind::TerminalCapture, immediate); const auto made = create(store);
        auto deferred = std::make_shared<Deferred>(); store.registerSource(SourceKind::TerminalCapture, deferred);
        int denied = 0;
        for (int i = 0; i < 33; ++i) store.resolveAsync(made.reference.evidenceId, Owner, 1024, [&](auto result) {
            if (result.error == Error::LimitExceeded) ++denied;
        });
        QCOMPARE(denied, 1); QCOMPARE(deferred->callbacks.size(), 32);
        auto callbacks = std::move(deferred->callbacks);
        for (auto& finish : callbacks) finish({Error::EvidenceExpired});
    }
    void threeSourcesShareHashPermissionsAndExpiry() {
        qint64 now = 0;
        EvidenceStore store(authorize, [&] { return now; });
        for (auto kind : {SourceKind::TerminalCapture, SourceKind::DiagnosticResult, SourceKind::ImportedFile}) {
            auto source = std::make_shared<FakeSource>();
            store.registerSource(kind, source);
            const auto made = create(store, request(kind));
            QCOMPARE(made.error, Error::None);
            QCOMPARE(made.reference.contentHash, QCryptographicHash::hash("abef", QCryptographicHash::Sha256).toHex());
            QCOMPARE(made.reference.fragments.size(), 2);
            QCOMPARE(made.reference.sourceCompleteness, QStringLiteral("unknown_tail"));
            QCOMPARE(made.reference.projectionVersion, quint64(3));
            QCOMPARE(made.reference.persistence, QStringLiteral("memory"));
            QVERIFY(made.reference.expiresAt.isValid());
            QCOMPARE(resolve(store, made.reference.evidenceId).bytes, QByteArray("abef"));
            QCOMPARE(resolve(store, made.reference.evidenceId, {"other", 7}).error, Error::PermissionDenied);
            QCOMPARE(resolve(store, made.reference.evidenceId, {"owner", 8}).error, Error::PermissionDenied);
            now += 60001;
            QCOMPARE(resolve(store, made.reference.evidenceId).error, Error::EvidenceExpired);
        }
    }
    void expiredSourceDoesNotClaimVerifiable() {
        EvidenceStore store(authorize);
        auto source = std::make_shared<FakeSource>(); store.registerSource(SourceKind::TerminalCapture, source);
        const auto made = create(store);
        QVERIFY(made.verifiable);
        source->alive = false;
        const auto result = resolve(store, made.reference.evidenceId);
        QCOMPARE(result.error, Error::EvidenceExpired); QVERIFY(!result.verifiable); QVERIFY(result.bytes.isEmpty());
    }
    void memoryEvidenceGoneAfterRestart() {
        auto source = std::make_shared<FakeSource>();
        QString id;
        { EvidenceStore store(authorize); store.registerSource(SourceKind::TerminalCapture, source);
          id = create(store).reference.evidenceId; QVERIFY(!id.isEmpty()); }
        EvidenceStore restarted(authorize); restarted.registerSource(SourceKind::TerminalCapture, source);
        QCOMPARE(resolve(restarted, id).error, Error::EvidenceExpired);
    }
    void diagnosticEvidenceRequiresOwner() {
        EvidenceStore store(authorize); auto source = std::make_shared<FakeSource>();
        store.registerSource(SourceKind::DiagnosticResult, source);
        auto r = request(SourceKind::DiagnosticResult);
        r.diagnostic = {"recipe", 4, 5, 6, "shared_terminal", true, true, true};
        const auto result = create(store, r);
        QVERIFY(result.verifiable); QVERIFY(result.reference.diagnostic.sharedTerminal);
        QCOMPARE(result.reference.diagnostic.recipeVersion, quint64(4));
        QCOMPARE(resolve(store, result.reference.evidenceId, {"other", 7}).error, Error::PermissionDenied);
    }
    void importedPathNotAcceptedFromModel() {
        ImportedResourceStore imports;
        std::optional<SourceReadResult> result;
        imports.reader()->readAsync({SourceKind::ImportedFile, QStringLiteral("C:/Windows/win.ini"), 1, "owner"},
            {{0, 0, 1}}, 1024, [&](auto value) { result = std::move(value); });
        QTRY_VERIFY(result.has_value());
        QCOMPARE(result->error, Error::EvidenceExpired); QVERIFY(result->bytes.isEmpty());
    }
    void revokeDuringDeliveryDiscardsBytes() {
        bool allowed = true;
        EvidenceStore store([&](const auto&, const auto&, const auto&, auto) { return allowed; });
        auto source = std::make_shared<FakeSource>(); store.registerSource(SourceKind::TerminalCapture, source);
        const auto made = create(store);
        source->duringRead = [&] { allowed = false; };
        const auto result = resolve(store, made.reference.evidenceId);
        QCOMPARE(result.error, Error::PermissionDenied); QVERIFY(result.bytes.isEmpty()); QVERIFY(!result.verifiable);
    }
    void sourceChangeAndBoundedResult() {
        EvidenceStore store(authorize); auto source = std::make_shared<FakeSource>();
        store.registerSource(SourceKind::TerminalCapture, source); const auto made = create(store);
        QCOMPARE(resolve(store, made.reference.evidenceId, Owner, 3).error, Error::LimitExceeded);
        source->content = "bbcdef";
        QCOMPARE(resolve(store, made.reference.evidenceId).error, Error::EvidenceExpired);
        source->content = "abcdef"; source->generation = 2;
        QCOMPARE(resolve(store, made.reference.evidenceId).error, Error::EvidenceExpired);
    }
    void referencesAndFragmentsStayBounded() {
        EvidenceStore store(authorize); auto source = std::make_shared<FakeSource>();
        store.registerSource(SourceKind::TerminalCapture, source);
        auto invalid = request(); invalid.fragments = {{1, 0, quint64(EvidenceStore::MaxBytes + 1)}};
        QCOMPARE(create(store, invalid).error, Error::LimitExceeded);
        invalid = request(); invalid.fragments.fill({1, 0, 0}, EvidenceStore::MaxFragments + 1);
        QCOMPARE(create(store, invalid).error, Error::LimitExceeded);
        for (qsizetype i = 0; i < EvidenceStore::MaxReferences; ++i)
            QCOMPARE(create(store).error, Error::None);
        QCOMPARE(create(store).error, Error::LimitExceeded);
        store.clear(); QCOMPARE(store.size(), qsizetype(0));
    }
    void removedDuringReadAndOwnerDestroyedAreSafe() {
        auto store = std::make_unique<EvidenceStore>(authorize); auto source = std::make_shared<FakeSource>();
        store->registerSource(SourceKind::TerminalCapture, source); const auto made = create(*store);
        source->deferred = true; std::optional<EvidenceResult> result;
        store->resolveAsync(made.reference.evidenceId, Owner, 1024, [&](auto value) { result = std::move(value); });
        QVERIFY(bool(source->complete));
        store->remove(made.reference.evidenceId); source->complete();
        QVERIFY(result.has_value()); QCOMPARE(result->error, Error::EvidenceExpired);
        result.reset();
        store->createAsync(request(), Owner, [&](auto value) { result = std::move(value); });
        store.reset(); source->complete();
        QVERIFY(result.has_value()); QCOMPARE(result->error, Error::EvidenceExpired);
    }
    void persistenceIsExplicitlyUnsupportedAndEmptyBytesValid() {
        EvidenceStore store(authorize); auto source = std::make_shared<FakeSource>();
        store.registerSource(SourceKind::TerminalCapture, source); auto r = request(); r.persist = true;
        QCOMPARE(create(store, r).error, Error::PersistenceUnavailable);
        r.persist = false; r.fragments = {{1, 0, 0}};
        const auto result = create(store, r); QCOMPARE(result.error, Error::None); QVERIFY(result.verifiable);
        QVERIFY(resolve(store, result.reference.evidenceId).verifiable);
    }
    void importedFileChangesDeletionAndRemovalExpire() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); QFile file(directory.filePath("log.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("abcdef"), qint64(6)); file.close();
        ImportedResourceStore imports;
        const auto shared = imports.shareLocalFile({file.fileName()}, "owner"); QCOMPARE(shared.error, Error::None);
        QCOMPARE(shared.byteCount, quint64(6));
        std::optional<SourceReadResult> result;
        const auto read = [&] { result.reset(); imports.reader()->readAsync(shared.binding, {{0, 1, 3}}, 1024,
            [&](auto value) { result = std::move(value); }); };
        read(); QTRY_VERIFY(result.has_value()); QCOMPARE(result->bytes, QByteArray("bcd"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("uvwxyz"); file.close();
        read(); QTRY_VERIFY(result.has_value()); QCOMPARE(result->error, Error::EvidenceExpired);
        QVERIFY(file.remove()); read(); QTRY_VERIFY(result.has_value()); QCOMPARE(result->error, Error::EvidenceExpired);
        imports.remove(shared.binding.resourceId); read(); QTRY_VERIFY(result.has_value());
        QCOMPARE(result->error, Error::EvidenceExpired);
    }
    void selectedLocalFileImportsAsynchronouslyOnOwnerThread() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile file(directory.filePath("selected.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("selected bytes"), qint64(14)); file.close();
        ImportedResourceStore imports;
        const auto ownerThread = QThread::currentThread();
        std::optional<ImportedResource> imported;
        QThread* callbackThread = nullptr;
        imports.shareLocalFileAsync({file.fileName()}, "owner", [&](ImportedResource result) {
            callbackThread = QThread::currentThread(); imported = std::move(result);
        });
        QVERIFY(!imported.has_value());
        QTRY_VERIFY(imported.has_value());
        QCOMPARE(callbackThread, ownerThread);
        QCOMPARE(imported->error, Error::None);
        QCOMPARE(imported->binding.kind, SourceKind::ImportedFile);
        QCOMPARE(imported->binding.ownerPrincipalId, QStringLiteral("owner"));
        QCOMPARE(imported->byteCount, quint64(14)); QVERIFY(!imported->binding.resourceId.isEmpty());
        QCOMPARE(imported->contentHash, QCryptographicHash::hash("selected bytes", QCryptographicHash::Sha256).toHex());
        std::optional<SourceReadResult> resolved;
        imports.reader()->readAsync(imported->binding, {{0, 0, 14}}, 1024,
            [&](SourceReadResult result) { resolved = std::move(result); });
        QTRY_VERIFY(resolved.has_value()); QCOMPARE(resolved->bytes, QByteArray("selected bytes"));
    }
    void queuedImportDestructionSuppressesDelivery() {
        bool called = false;
        {
            PausedWorkers paused;
            auto imports = std::make_unique<ImportedResourceStore>();
            imports->shareLocalFileAsync({QStringLiteral("missing-explicit-selection")}, "owner",
                [&](ImportedResource) { called = true; });
            QVERIFY(!called);
            imports.reset();
        }
        QCoreApplication::processEvents();
        QVERIFY(!called);
    }
    void pendingImportsShareWorkerAndRegistryBounds() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); QFile file(directory.filePath("selected.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("data"); file.close();
        ImportedResourceStore imports;
        auto existing = imports.shareLocalFile({file.fileName()}, "owner"); QCOMPARE(existing.error, Error::None);
        std::optional<ImportedResource> first, second, third;
        std::optional<SourceReadResult> read;
        {
            PausedWorkers paused;
            imports.shareLocalFileAsync({file.fileName()}, "owner", [&](auto value) { first = std::move(value); });
            imports.shareLocalFileAsync({file.fileName()}, "owner", [&](auto value) { second = std::move(value); });
            imports.shareLocalFileAsync({file.fileName()}, "owner", [&](auto value) { third = std::move(value); });
            QVERIFY(third.has_value()); QCOMPARE(third->error, Error::LimitExceeded);
            imports.reader()->readAsync(existing.binding, {{0, 0, 1}}, 1, [&](auto value) { read = std::move(value); });
            QVERIFY(read.has_value()); QCOMPARE(read->error, Error::LimitExceeded);
        }
        QTRY_VERIFY(first.has_value() && second.has_value());
        QCOMPARE(first->error, Error::None); QCOMPARE(second->error, Error::None);
        // 3 entries now; reserve the 32nd slot while the import is still queued.
        for (int i = 3; i < ImportedResourceStore::MaxResources - 1; ++i)
            QCOMPARE(imports.shareLocalFile({file.fileName()}, "owner").error, Error::None);
        first.reset(); second.reset();
        {
            PausedWorkers paused;
            imports.shareLocalFileAsync({file.fileName()}, "owner", [&](auto value) { first = std::move(value); });
            imports.shareLocalFileAsync({file.fileName()}, "owner", [&](auto value) { second = std::move(value); });
            QVERIFY(second.has_value()); QCOMPARE(second->error, Error::LimitExceeded);
            QCOMPARE(imports.shareLocalFile({file.fileName()}, "owner").error, Error::LimitExceeded);
        }
        QTRY_VERIFY(first.has_value()); QCOMPARE(first->error, Error::None);
    }
};
QTEST_GUILESS_MAIN(EvidenceTests)
#include "EvidenceTests.moc"
