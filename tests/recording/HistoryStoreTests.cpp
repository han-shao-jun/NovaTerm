#include "recording/HistoryChunkStore.h"
#include "recording/HistoryRetentionPolicy.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <thread>
#include "recording/detail/HistoryIndexSql.h"
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

using namespace NovaTerm::Recording;
class HistoryStoreTests : public QObject {
    Q_OBJECT
    static CaptureDescriptor descriptor() {
        CaptureDescriptor d; d.id = {QUuid::createUuid()}; d.sessionId = QUuid::createUuid();
        d.connectionGeneration = 1; d.createdMs = QDateTime::currentMSecsSinceEpoch(); return d;
    }
    static HistoryRecord payload(quint64 id, QByteArray bytes = "raw\r\n") {
        HistoryRecord r; r.recordId = id; r.payload = std::move(bytes); return r;
    }
private slots:
    void transientWindowsIndexReaderDoesNotPreventRecovery() {
#ifdef Q_OS_WIN
        QTemporaryDir dir; const auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY(s.commit()); QVERIFY(s.seal("stop")); }
        const auto path = dir.path()+"/"+d.id.text()+"/index.sqlite";
        HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(file != INVALID_HANDLE_VALUE);
        std::thread release([file] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); CloseHandle(file); });
        HistoryChunkStore recovered(dir.path(), d); const bool success = recovered.recover();
        release.join(); QVERIFY2(success, qPrintable(recovered.error()));
        QCOMPARE(recovered.readCommitted({}).integrity, Integrity::Lossless);
#else
        QSKIP("Windows file-sharing fixture");
#endif
    }
    void transientWindowsReaderDoesNotPreventDeletion() {
#ifdef Q_OS_WIN
        QTemporaryDir dir; const auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY(s.commit()); QVERIFY(s.seal("stop")); }
        const auto path = dir.path()+"/"+d.id.text()+"/chunks/00000001.bin";
        HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(file != INVALID_HANDLE_VALUE);
        std::thread release([file] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); CloseHandle(file); });
        HistoryRetentionPolicy policy; const bool removed = policy.removeCapture(dir.path(), d.id, 0, false);
        release.join(); QVERIFY(removed);
        QVERIFY(!QFileInfo::exists(dir.path()+"/"+d.id.text()));
#else
        QSKIP("Windows file-sharing fixture");
#endif
    }
    void missingIndexRowsCannotAppearLossless_data() {
        QTest::addColumn<int>("missingId");
        QTest::newRow("first") << 1; QTest::newRow("middle") << 2; QTest::newRow("last") << 3;
    }
    void missingIndexRowsCannotAppearLossless() {
        QFETCH(int, missingId); QTemporaryDir dir; const auto d = descriptor();
        HistoryChunkStore s(dir.path(), d); QVERIFY(s.open());
        QVERIFY(s.append({payload(1), payload(2), payload(3)})); QVERIFY(s.commit()); QVERIFY(s.seal("stop"));
        const auto name = QUuid::createUuid().toString();
        { auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(s.directory()+"/index.sqlite"); QVERIFY(db.open());
          QSqlQuery q(db); QVERIFY(q.exec(QString("DELETE FROM records WHERE id=%1").arg(missingId))); }
        QSqlDatabase::removeDatabase(name);
        const auto result = s.readCommitted({});
        QVERIFY(result.missing); QCOMPARE(result.integrity, Integrity::UnknownTail);
        QVERIFY(!result.lostByteCount); QVERIFY(!result.lostRecordCount);
    }
    void readFailureCannotRetainLosslessStatus() {
        QTemporaryDir dir; bool allowRead = true;
        HistoryChunkStore s(dir.path(), descriptor(), {}, [&](IoPoint p) { return p != IoPoint::ReadAccess || allowRead; });
        QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY(s.commit()); QVERIFY(s.seal("stop")); allowRead = false;
        const auto result = s.readCommitted({}); QVERIFY(result.missing);
        QCOMPARE(result.integrity, Integrity::UnknownTail); QVERIFY(!result.lostByteCount); QVERIFY(!result.lostRecordCount);
    }
    void rangeQueryUsesBothIdBounds() {
        const auto name = QUuid::createUuid().toString();
        { auto db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(":memory:"); QVERIFY(db.open()); QSqlQuery q(db);
          QVERIFY(q.exec("CREATE TABLE records (id INTEGER PRIMARY KEY,last_id INTEGER,chunk INTEGER,offset INTEGER)"));
          QVERIFY(q.prepare("EXPLAIN QUERY PLAN " + QString::fromLatin1(IndexSql::RangeQuery)));
          q.addBindValue(9999); q.addBindValue(10000); q.addBindValue(2); QVERIFY(q.exec()); QString plan;
          while (q.next()) plan += q.value(3).toString();
          QVERIFY2(plan.contains("rowid>?") && plan.contains("rowid<?"), qPrintable(plan)); }
        QSqlDatabase::removeDatabase(name);
    }
    void rangeCanStartInsideCoalescedGap() {
        QTemporaryDir dir; HistoryChunkStore s(dir.path(), descriptor()); QVERIFY(s.open());
        auto gap = payload(2, {}); gap.kind = RecordKind::Gap; gap.lastRecordId = 4; gap.lostByteCount = 20; gap.lostRecordCount = 3;
        QVERIFY(s.append({payload(1), gap, payload(5)})); QVERIFY(s.commit()); QVERIFY(s.seal("stop"));
        HistoryRange range; range.firstRecordId = 3; const auto result = s.readCommitted(range);
        QVERIFY(!result.missing); QCOMPARE(result.records.size(), size_t(2)); QCOMPARE(result.records.front().recordId, quint64(2));
    }
    void privateStoragePermissions() {
        QTemporaryDir dir; auto d = descriptor(); HistoryChunkStore s(dir.path(), d);
        QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY2(s.commit(), qPrintable(s.error()));
        const auto base = dir.path() + "/" + d.id.text();
        for (const auto& path : {base, base+"/chunks", base+"/manifest.json", base+"/chunks/00000001.bin", base+"/index.sqlite"}) {
#ifdef Q_OS_WIN
            DWORD needed = 0; GetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
            QVERIFY(needed > 0); std::vector<char> bytes(needed);
            QVERIFY(GetFileSecurityW(reinterpret_cast<LPCWSTR>(path.utf16()), DACL_SECURITY_INFORMATION, bytes.data(), needed, &needed));
            PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
            QVERIFY(GetSecurityDescriptorDacl(bytes.data(), &present, &acl, &defaulted)); QVERIFY(present && acl);
            ACL_SIZE_INFORMATION info{}; QVERIFY(GetAclInformation(acl, &info, sizeof(info), AclSizeInformation));
            QCOMPARE(info.AceCount, DWORD(2));
            for (DWORD i = 0; i < info.AceCount; ++i) {
                void* value = nullptr; QVERIFY(GetAce(acl, i, &value)); auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(value);
                QCOMPARE(ace->Header.AceType, BYTE(ACCESS_ALLOWED_ACE_TYPE));
                QVERIFY(!IsWellKnownSid(&ace->SidStart, WinWorldSid));
                QVERIFY(!IsWellKnownSid(&ace->SidStart, WinBuiltinUsersSid));
                QVERIFY(!IsWellKnownSid(&ace->SidStart, WinBuiltinAdministratorsSid));
            }
#else
            struct stat st{}; QVERIFY(::stat(QFile::encodeName(path).constData(), &st) == 0);
            QCOMPARE(st.st_mode & 0777, mode_t(QFileInfo(path).isDir() ? 0700 : 0600));
#endif
        }
    }
    void unknownGapCannotBecomeZeroLossSeal() {
        QTemporaryDir dir; auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); auto gap = payload(1, {});
          gap.kind = RecordKind::Gap; gap.reason = "unknown_tail";
          QVERIFY(s.append({gap})); QVERIFY2(s.commit(), qPrintable(s.error())); QVERIFY(!s.seal("stop")); }
        HistoryChunkStore s(dir.path(), d); QVERIFY(s.recover()); const auto r = s.readCommitted({});
        QCOMPARE(r.integrity, Integrity::UnknownTail); QVERIFY(!r.lostByteCount); QVERIFY(!r.lostRecordCount);
    }
    void defaultReadAcceptsMaximumPayload() {
        QTemporaryDir dir; HistoryChunkStore s(dir.path(), descriptor()); QVERIFY(s.open());
        const QByteArray bytes(64 * 1024, 'x'); QVERIFY(s.append({payload(1, bytes)})); QVERIFY2(s.commit(), qPrintable(s.error())); QVERIFY2(s.seal("stop"), qPrintable(s.error()));
        const auto r = s.readCommitted({}); QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.records.size(), size_t(1)); QCOMPARE(r.records[0].payload, bytes);
    }
    void sealedUnexpectedTailIsUnknown() {
        QTemporaryDir dir; auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY2(s.commit(), qPrintable(s.error())); QVERIFY2(s.seal("stop"), qPrintable(s.error())); }
        QFile f(dir.path()+"/"+d.id.text()+"/chunks/00000001.bin"); QVERIFY(f.open(QIODevice::Append)); f.write("unexpected"); f.close();
        HistoryChunkStore s(dir.path(), d); QVERIFY(s.recover());
        QCOMPARE(s.readCommitted({}).integrity, Integrity::UnknownTail);
    }
    void partialCommittedRecordRetainsOnlyValidatedPrefix() {
        QTemporaryDir dir; auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); QVERIFY(s.append({payload(1), payload(2)})); QVERIFY2(s.commit(), qPrintable(s.error())); QVERIFY2(s.seal("stop"), qPrintable(s.error())); }
        QFile f(dir.path()+"/"+d.id.text()+"/chunks/00000001.bin"); QVERIFY(f.open(QIODevice::ReadWrite)); QVERIFY(f.resize(f.size()-3)); f.close();
        HistoryChunkStore s(dir.path(), d); QVERIFY(s.recover()); const auto r = s.readCommitted({});
        QCOMPARE(r.highWaterRecordId, quint64(1)); QCOMPARE(r.records.size(), size_t(1)); QCOMPARE(r.integrity, Integrity::UnknownTail);
    }
    void manyChunksAndReadBudget() {
        QTemporaryDir dir; auto d = descriptor(); StorageOptions options; options.chunkBytes = 1024;
        HistoryChunkStore s(dir.path(), d, options); QVERIFY(s.open());
        for (quint64 i = 1; i <= 10; ++i) { QVERIFY(s.append({payload(i, QByteArray(900, char(i)))})); QVERIFY2(s.commit(), qPrintable(s.error())); }
        QVERIFY2(s.seal("stop"), qPrintable(s.error())); HistoryRange range; range.firstRecordId = 5; range.maxBytes = 1500;
        const auto r = s.readCommitted(range); QCOMPARE(r.records.size(), size_t(1)); QCOMPARE(r.records[0].recordId, quint64(5)); QCOMPARE(r.nextRecordId, quint64(6));
    }
    void deleteRemovesWalAndEvidenceDependencies() {
        QTemporaryDir dir; auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY2(s.commit(), qPrintable(s.error())); QVERIFY2(s.seal("stop"), qPrintable(s.error())); }
        const auto path = dir.path()+"/"+d.id.text();
        QVERIFY(QDir().mkpath(path+"/evidence"));
        for (const auto& name : {"index.sqlite-wal", "index.sqlite-shm", "evidence/derived.bin"}) {
            QFile f(path+"/"+name); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("derived");
        }
        HistoryRetentionPolicy p; auto lease = p.acquire(d.id, 0, 10); QVERIFY(lease);
        QVERIFY(!p.removeCapture(dir.path(), d.id, 5, false));
        QVERIFY(p.removeCapture(dir.path(), d.id, 10, false));
        QVERIFY(!QFileInfo::exists(path));
        HistoryChunkStore missing(dir.path(), d); QVERIFY(!missing.recover());
        QVERIFY(!QFileInfo::exists(path));
    }
    void diskFullIsBounded() {
        QTemporaryDir dir; auto d = descriptor(); StorageOptions options; options.maxCaptureBytes = 1000;
        { HistoryChunkStore s(dir.path(), d, options); QVERIFY(s.open());
          QVERIFY(!s.append({payload(1, QByteArray(1024, 'x'))})); QVERIFY(!s.seal("stop")); }
        HistoryChunkStore s(dir.path(), d); QVERIFY(s.recover());
        QCOMPARE(s.readCommitted({}).integrity, Integrity::UnknownTail);
    }
    void committedRoundTrip() {
        QTemporaryDir dir; HistoryChunkStore store(dir.path(), descriptor());
        QVERIFY2(store.open(), qPrintable(store.error()));
        QVERIFY(store.append({payload(1, QByteArray::fromHex("001b5b6dff0d0a")), payload(2)}));
        QVERIFY(store.commit()); QVERIFY(store.seal("user_stop"));
        const auto result = store.readCommitted({});
        QCOMPARE(result.records.size(), size_t(2));
        QCOMPARE(result.records[0].payload, QByteArray::fromHex("001b5b6dff0d0a"));
        QCOMPARE(result.integrity, Integrity::Lossless);
    }
    void writtenBeyondManifestNotPublished() {
        QTemporaryDir dir; auto d = descriptor();
        { HistoryChunkStore store(dir.path(), d); QVERIFY(store.open());
          QVERIFY(store.append({payload(1)})); QVERIFY(store.commit());
          QVERIFY(store.append({payload(2)})); }
        HistoryChunkStore recovered(dir.path(), d); QVERIFY(recovered.recover());
        const auto result = recovered.readCommitted({});
        QCOMPARE(result.highWaterRecordId, quint64(1));
        QCOMPARE(result.records.size(), size_t(1));
        QCOMPARE(result.integrity, Integrity::UnknownTail);
        QVERIFY(!result.lostByteCount.has_value());
        QVERIFY(!recovered.append({payload(2)}));
    }
    void partialRecordTailNotPublished() {
        QTemporaryDir dir; auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY2(s.commit(), qPrintable(s.error())); }
        QFile f(dir.path()+"/"+d.id.text()+"/chunks/00000001.bin");
        QVERIFY(f.open(QIODevice::Append)); f.write("partial"); f.close();
        HistoryChunkStore s(dir.path(), d); QVERIFY(s.recover());
        QCOMPARE(s.readCommitted({}).records.size(), size_t(1));
        QCOMPARE(s.readCommitted({}).integrity, Integrity::UnknownTail);
    }
    void indexLagsChunkRecovers() {
        QTemporaryDir dir; auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY2(s.commit(), qPrintable(s.error())); QVERIFY2(s.seal("stop"), qPrintable(s.error())); }
        QFile::remove(dir.path()+"/"+d.id.text()+"/index.sqlite");
        HistoryChunkStore s(dir.path(), d); QVERIFY2(s.recover(), qPrintable(s.error()));
        QCOMPARE(s.readCommitted({}).integrity, Integrity::Lossless);
        QVERIFY(QFileInfo::exists(dir.path()+"/"+d.id.text()+"/index.sqlite"));
    }
    void corruptIndexRebuilt() {
        QTemporaryDir dir; auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY2(s.commit(), qPrintable(s.error())); }
        QFile f(dir.path()+"/"+d.id.text()+"/index.sqlite"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("broken"); f.close();
        HistoryChunkStore s(dir.path(), d); QVERIFY2(s.recover(), qPrintable(s.error()));
        QCOMPARE(s.readCommitted({}).highWaterRecordId, quint64(1));
    }
    void sealWithGapIsNotLossless() {
        QTemporaryDir dir; HistoryChunkStore s(dir.path(), descriptor()); QVERIFY(s.open());
        auto gap = payload(2, {}); gap.kind = RecordKind::Gap; gap.lastRecordId = 4;
        gap.reason = "queue_limit"; gap.lostByteCount = 100; gap.lostRecordCount = 3;
        QVERIFY(s.append({payload(1), gap})); QVERIFY2(s.commit(), qPrintable(s.error())); QVERIFY2(s.seal("stop"), qPrintable(s.error()));
        QCOMPARE(s.readCommitted({}).integrity, Integrity::KnownGap);
        QCOMPARE(s.readCommitted({}).highWaterRecordId, quint64(4));
    }
    void crashBeforeSealReportsUnknownTail() {
        QTemporaryDir dir; auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY2(s.commit(), qPrintable(s.error())); }
        HistoryChunkStore s(dir.path(), d); QVERIFY(s.recover());
        QCOMPARE(s.readCommitted({}).integrity, Integrity::UnknownTail);
        QVERIFY(!s.readCommitted({}).lostRecordCount.has_value());
    }
    void diskFullWithoutGapWriteStillUnsealed_data() {
        QTest::addColumn<int>("point");
        for (auto point : {IoPoint::DataWrite, IoPoint::DataSync, IoPoint::ManifestReplace, IoPoint::IndexCommit, IoPoint::Seal})
            QTest::newRow(qPrintable(QString::number(int(point)))) << int(point);
    }
    void diskFullWithoutGapWriteStillUnsealed() {
        QFETCH(int, point); QTemporaryDir dir; auto d = descriptor(); bool fail = false;
        { HistoryChunkStore s(dir.path(), d, {}, [&](IoPoint p) { return !(fail && int(p) == point); });
          QVERIFY(s.open()); fail = true;
          const bool appended = s.append({payload(1)}); const bool committed = appended && s.commit();
          const bool sealed = committed && s.seal("stop"); QVERIFY(!sealed); }
        HistoryChunkStore s(dir.path(), d); QVERIFY2(s.recover(), qPrintable(s.error()));
        QCOMPARE(s.readCommitted({}).integrity, Integrity::UnknownTail);
        QVERIFY(!s.readCommitted({}).lostByteCount.has_value());
    }
    void corruptManifestIsUnsealed() {
        QTemporaryDir dir; auto d = descriptor();
        { HistoryChunkStore s(dir.path(), d); QVERIFY(s.open()); QVERIFY(s.append({payload(1)})); QVERIFY2(s.commit(), qPrintable(s.error())); QVERIFY2(s.seal("stop"), qPrintable(s.error())); }
        QFile f(dir.path()+"/"+d.id.text()+"/manifest.json"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("{"); f.close();
        HistoryChunkStore s(dir.path(), d); QVERIFY(s.recover());
        QCOMPARE(s.readCommitted({}).integrity, Integrity::RecoveredUnsealed);
    }
    void leaseExpiresBeforeCleanup() {
        HistoryRetentionPolicy p; const auto id = HistoryCaptureId{QUuid::createUuid()};
        auto lease = p.acquire(id, 100, 10); QVERIFY(lease.has_value());
        QVERIFY(p.leased(id, 109)); QVERIFY(!p.leased(id, 110));
        QVERIFY(!p.renew(*lease, 111, 10));
    }
    void symlinkRejected() {
        QTemporaryDir dir; QTemporaryDir outside; const auto d = descriptor();
#ifdef Q_OS_WIN
        // QFile::link creates .lnk on Windows; use an actual directory junction.
        QProcess process; process.start("cmd", {"/c", "mklink", "/J", QDir::toNativeSeparators(dir.path()+"/redirect"), QDir::toNativeSeparators(outside.path())});
        QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
#else
        QVERIFY(QFile::link(outside.path(), dir.path()+"/redirect"));
#endif
        HistoryChunkStore s(dir.path()+"/redirect", d); QVERIFY(!s.open());
        QVERIFY(QDir(outside.path()).entryList(QDir::NoDotAndDotDot|QDir::AllEntries).isEmpty());
    }
};
QTEST_GUILESS_MAIN(HistoryStoreTests)
#include "HistoryStoreTests.moc"
