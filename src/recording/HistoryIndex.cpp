#include "HistoryIndex.h"
#include "HistoryStorageIo.h"
#include "detail/HistoryIndexSql.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QUuid>

namespace NovaTerm::Recording {
struct HistoryIndex::Impl {
    QString name{QUuid::createUuid().toString()};
    QSqlDatabase db;
    QString directory;
    QString error;
    ~Impl() { db.close(); db = {}; QSqlDatabase::removeDatabase(name); }
    bool query(const QString& sql) {
        QSqlQuery q(db); if (q.exec(sql)) return true; error = q.lastError().text(); return false;
    }
};
HistoryIndex::HistoryIndex() : _impl(std::make_unique<Impl>()) {}
HistoryIndex::~HistoryIndex() = default;
bool HistoryIndex::open(const QString& directory) {
    auto& p = *_impl; p.directory = directory;
    if (!StorageIo::safeTree(directory)) { p.error = "unsafe index path"; return false; }
    const auto path = directory + "/index.sqlite";
    p.db = QSqlDatabase::addDatabase("QSQLITE", p.name);
    p.db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=0"); p.db.setDatabaseName(path);
    if (!p.db.open()) { p.error = p.db.lastError().text(); return false; }
    if (!p.query("PRAGMA quick_check")) {
        p.db.close();
        for (const auto& suffix : {"", "-wal", "-shm"}) if (!StorageIo::removePrivateFile(path + suffix)) return false;
        if (!p.db.open()) return false;
    }
    if (!p.query("PRAGMA journal_mode=WAL") || !p.query("PRAGMA synchronous=FULL") || !p.query("PRAGMA cache_size=-128")
        || !p.query("CREATE TABLE IF NOT EXISTS records (id INTEGER PRIMARY KEY, last_id INTEGER, chunk INTEGER, offset INTEGER, time_ms INTEGER, hash BLOB, kind INTEGER)")) return false;
    for (const auto& suffix : {"", "-wal", "-shm"}) {
        const auto file = path + suffix;
        if (QFile::exists(file) && !StorageIo::privateFile(file)) return false;
    }
    return StorageIo::safeTree(directory);
}
bool HistoryIndex::beginRebuild() { return _impl->db.transaction() && _impl->query("DELETE FROM records"); }
bool HistoryIndex::begin() { return _impl->db.transaction(); }
bool HistoryIndex::add(const HistoryRecord& r, int chunk, qint64 offset, const QByteArray& hash) {
    auto& p = *_impl; QSqlQuery q(p.db);
    q.prepare("INSERT OR REPLACE INTO records VALUES (?,?,?,?,?,?,?)");
    q.addBindValue(QVariant::fromValue(r.recordId)); q.addBindValue(QVariant::fromValue(r.lastRecordId));
    q.addBindValue(chunk); q.addBindValue(offset); q.addBindValue(r.wallTimestampMs); q.addBindValue(hash); q.addBindValue(int(r.kind));
    if (q.exec()) return true; p.error = q.lastError().text(); return false;
}
bool HistoryIndex::commit() {
    auto& p = *_impl;
    if (!p.db.commit()) { p.error = p.db.lastError().text(); return false; }
    return StorageIo::safePath(p.directory + "/index.sqlite")
        && StorageIo::safePath(p.directory + "/index.sqlite-wal")
        && StorageIo::safePath(p.directory + "/index.sqlite-shm");
}
QString HistoryIndex::error() const { return _impl->error; }
std::vector<HistoryIndex::Location> HistoryIndex::locate(quint64 first, quint64 last, size_t limit) {
    auto& p = *_impl; std::vector<Location> result;
    if (!limit || first > last) return result;
    QSqlQuery preceding(p.db); preceding.prepare(IndexSql::PrecedingQuery);
    preceding.addBindValue(QVariant::fromValue(first));
    if (!preceding.exec()) { p.error = preceding.lastError().text(); return {}; }
    if (preceding.next() && preceding.value(3).toULongLong() >= first) {
        result.push_back({preceding.value(0).toInt(), preceding.value(1).toLongLong(), preceding.value(2).toULongLong(), preceding.value(3).toULongLong()});
    }
    if (result.size() == limit) return result;
    QSqlQuery q(p.db);
    q.prepare(IndexSql::RangeQuery);
    q.addBindValue(QVariant::fromValue(first)); q.addBindValue(QVariant::fromValue(last));
    q.addBindValue(int(limit - result.size()));
    if (!q.exec()) { p.error = q.lastError().text(); return {}; }
    while (q.next()) result.push_back({q.value(0).toInt(), q.value(1).toLongLong(), q.value(2).toULongLong(), q.value(3).toULongLong()});
    return result;
}
}
