#include "mcp/files/LocalFileAccess.h"
#include "mcp/contracts/Budget.h"
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>
#include <type_traits>
#ifdef Q_OS_LINUX
#include <unistd.h>
#include <sys/stat.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <vector>
#endif

using namespace NovaTerm::Mcp;
using namespace NovaTerm::Mcp::Files;

namespace {
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
bool put(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
#endif
#ifdef Q_OS_WIN
QByteArray contents(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "fixture read failed" << file.errorString() << QFile::exists(path)
                   << QDir(QFileInfo(path).absolutePath()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        return {};
    }
    return file.readAll();
}
const wchar_t* wide(const QString& path) { return reinterpret_cast<const wchar_t*>(path.utf16()); }
bool junction(const QString& path, const QString& target)
{
    if (!QDir().mkdir(path)) return false;
    const auto substitute = QStringLiteral("\\??\\") + QDir::toNativeSeparators(target);
    const auto display = QDir::toNativeSeparators(target);
    struct MountPoint {
        DWORD tag;
        WORD length, reserved, subOffset, subLength, printOffset, printLength;
        wchar_t names[1];
    };
    const auto nameBytes = (substitute.size() + display.size() + 2) * sizeof(wchar_t);
    const auto bytes = offsetof(MountPoint, names) + nameBytes;
    std::vector<quint64> buffer((bytes + 7) / 8);
    auto* data = reinterpret_cast<MountPoint*>(buffer.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->length = WORD(bytes - 8);
    data->subLength = WORD(substitute.size() * sizeof(wchar_t));
    data->printOffset = WORD(data->subLength + sizeof(wchar_t));
    data->printLength = WORD(display.size() * sizeof(wchar_t));
    memcpy(data->names, substitute.utf16(), data->subLength);
    memcpy(reinterpret_cast<char*>(data->names) + data->printOffset, display.utf16(), data->printLength);
    const HANDLE handle = CreateFileW(wide(path), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, DWORD(bytes), nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    return ok;
}
#endif
}

class LocalFileAccessTests : public QObject {
    Q_OBJECT
private slots:
#ifdef Q_OS_LINUX
    void linuxReaderUsesPinnedBoundedSource() {
        QTemporaryDir root; QVERIFY(root.isValid());
        QVERIFY(QDir(root.path()).mkdir("nested"));
        const auto path = root.filePath("nested/source");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray original("abc\0def\r\nxyz", 12);
        QCOMPARE(file.write(original), original.size()); file.close();
        auto opened = LocalFileReader::open(root.path(), "nested/source");
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileReader>>(opened));
        auto reader = std::move(std::get<std::unique_ptr<LocalFileReader>>(opened));
        QCOMPARE(reader->size(), quint64(12));
        QCOMPARE(std::get<QByteArray>(reader->readAt(4, 3)), original.mid(4, 3));
        QCOMPARE(std::get<QByteArray>(reader->read(4)), original.left(4));
        QCOMPARE(std::get<QByteArray>(reader->read(65536)), original.mid(4));
        QCOMPARE(std::get<QByteArray>(reader->read(1)), QByteArray{});
        QCOMPARE(std::get<FileIoError>(reader->readAt(12, 1)), FileIoError::SizeLimit);
        QVERIFY(QFile::rename(path, root.filePath("renamed")));
        QFile replacement(path); QVERIFY(replacement.open(QIODevice::WriteOnly));
        QCOMPARE(replacement.write("replacement!"), qint64(12)); replacement.close();
        const auto pinned = reader->readAt(0, 12);
        if (const auto* bytes = std::get_if<QByteArray>(&pinned)) QCOMPARE(*bytes, original);
    }
    void linuxReaderRejectsUnsafeObjects_data() {
        QTest::addColumn<QString>("kind");
        for (const auto* kind : {"symlink", "parent-symlink", "root-symlink", "hardlink", "fifo", "directory", "oversize"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void linuxReaderRejectsUnsafeObjects() {
        QFETCH(QString, kind); QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkdir("inside"));
        QVERIFY(QDir(root.path()).mkdir("outside"));
        auto selected = root.filePath("inside"); QString relative = "source";
        QFile file(root.filePath("outside/data")); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("outside"), qint64(7)); file.close();
        if (kind == "symlink") QVERIFY(QFile::link(file.fileName(), selected + "/source"));
        else if (kind == "parent-symlink") {
            QVERIFY(QFile::link(root.filePath("outside"), selected + "/link")); relative = "link/data";
        } else if (kind == "root-symlink") {
            QVERIFY(QFile::link(root.filePath("outside"), root.filePath("linked-root")));
            selected = root.filePath("linked-root"); relative = "data";
        } else if (kind == "hardlink") {
            QCOMPARE(::link(QFile::encodeName(file.fileName()).constData(), QFile::encodeName(selected + "/source").constData()), 0);
        } else if (kind == "fifo") QCOMPARE(::mkfifo(QFile::encodeName(selected + "/source").constData(), 0600), 0);
        else if (kind == "directory") QVERIFY(QDir().mkdir(selected + "/source"));
        else { QFile large(selected + "/source"); QVERIFY(large.open(QIODevice::WriteOnly)); QVERIFY(large.resize(Budget::MaxLocalTransferFileBytes + 1)); }
        const auto opened = LocalFileReader::open(selected, relative);
        QVERIFY(std::holds_alternative<FileIoError>(opened));
        QCOMPARE(std::get<FileIoError>(opened), kind == "oversize" ? FileIoError::SizeLimit : FileIoError::UnsafeFile);
    }
    void linuxReaderRefusesMutationAndInvalidPaths() {
        QTemporaryDir root;
        const auto path = root.filePath("source");
        QFile source(path); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("original"); source.close();
        auto opened = LocalFileReader::open(root.path(), "source");
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileReader>>(opened));
        auto reader = std::move(std::get<std::unique_ptr<LocalFileReader>>(opened));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("changed-size"); source.close();
        QVERIFY(std::holds_alternative<FileIoError>(reader->readAt(0, 2)));
        QCOMPARE(std::get<FileIoError>(reader->readAt(0, 2)), FileIoError::InvalidState);
        for (const auto& relative : QStringList{"../source", "/source", "a//b", "a/./b", "a\\b", "a:b", QString(5000, 'x')})
            QCOMPARE(std::get<FileIoError>(LocalFileReader::open(root.path(), relative)), FileIoError::InvalidPath);
        QCOMPARE(std::get<FileIoError>(LocalFileReader::open("/", "source")), FileIoError::InvalidPath);
    }
#endif
    void readsBinaryFileWithBoundedChunks()
    {
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
        auto result = LocalFileReader::open("/tmp/unused", "file");
        QCOMPARE(std::get<FileIoError>(result), FileIoError::UnsupportedPlatform);
#else
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("子目录")));
        const auto relative = QStringLiteral("子目录/固件.bin");
        const QByteArray bytes("abc\0def\r\nxyz", 12);
        QVERIFY(put(directory.filePath(relative), bytes));
        auto result = LocalFileReader::open(directory.path(), relative);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileReader>>(result));
        auto reader = std::move(std::get<std::unique_ptr<LocalFileReader>>(result));
        QCOMPARE(reader->size(), quint64(12));
        QCOMPARE(std::get<FileIoError>(reader->read(0)), FileIoError::SizeLimit);
        QCOMPARE(std::get<FileIoError>(reader->read(65537)), FileIoError::SizeLimit);
        QCOMPARE(std::get<QByteArray>(reader->read(4)), QByteArray("abc\0", 4));
        QCOMPARE(std::get<QByteArray>(reader->read(64)), QByteArray("def\r\nxyz", 8));
        QCOMPARE(std::get<QByteArray>(reader->read(64)), QByteArray{});
#endif
    }
    void rereadsBinaryRangesAtForwardAndBackwardOffsets()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(put(directory.filePath("source"), QByteArray("abc\0def\r\nxyz", 12)));
        auto opened = LocalFileReader::open(directory.path(), "source");
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileReader>>(opened));
        auto& reader = std::get<std::unique_ptr<LocalFileReader>>(opened);
        for (const auto offset : {quint64(9), quint64(0), quint64(4), quint64(0), quint64(9)}) {
            auto result = reader->readAt(offset, 3);
            QVERIFY(std::holds_alternative<QByteArray>(result));
            const auto expected = offset == 9 ? QByteArray("xyz")
                : offset == 4 ? QByteArray("def") : QByteArray("abc");
            QCOMPARE(std::get<QByteArray>(result), expected);
        }
        auto binary = reader->readAt(2, 7);
        QVERIFY(std::holds_alternative<QByteArray>(binary));
        QCOMPARE(std::get<QByteArray>(binary), QByteArray("c\0def\r\n", 7));
#endif
    }
    void randomReadsPreserveSequentialCursor()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(put(directory.filePath("source"), "abcdefghijkl"));
        auto opened = LocalFileReader::open(directory.path(), "source");
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileReader>>(opened));
        auto& reader = std::get<std::unique_ptr<LocalFileReader>>(opened);
        QCOMPARE(std::get<QByteArray>(reader->read(3)), QByteArray("abc"));
        auto forward = reader->readAt(8, 2);
        QVERIFY(std::holds_alternative<QByteArray>(forward));
        QCOMPARE(std::get<QByteArray>(forward), QByteArray("ij"));
        auto middle = reader->read(3);
        QVERIFY(std::holds_alternative<QByteArray>(middle));
        QCOMPARE(std::get<QByteArray>(middle), QByteArray("def"));
        auto backward = reader->readAt(1, 2);
        QVERIFY(std::holds_alternative<QByteArray>(backward));
        QCOMPARE(std::get<QByteArray>(backward), QByteArray("bc"));
        auto tail = reader->read(64);
        QVERIFY(std::holds_alternative<QByteArray>(tail));
        QCOMPARE(std::get<QByteArray>(tail), QByteArray("ghijkl"));
        auto afterEnd = reader->readAt(0, 3);
        QVERIFY(std::holds_alternative<QByteArray>(afterEnd));
        QCOMPARE(std::get<QByteArray>(afterEnd), QByteArray("abc"));
        QCOMPARE(std::get<QByteArray>(reader->read(1)), QByteArray{});
#endif
    }
    void rejectsRandomReadRangesAndInvalidCounts()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(put(directory.filePath("source"), "abcd"));
        auto opened = LocalFileReader::open(directory.path(), "source");
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileReader>>(opened));
        auto& reader = std::get<std::unique_ptr<LocalFileReader>>(opened);
        struct Range { quint64 offset; qsizetype count; };
        const Range invalid[]{{0, -1}, {0, 0}, {0, 65537}, {0, std::numeric_limits<qsizetype>::max()},
            {3, 2}, {4, 1}, {5, 1}, {std::numeric_limits<quint64>::max(), 1},
            {std::numeric_limits<quint64>::max() - 1, 4}};
        for (const auto& range : invalid) {
            auto result = reader->readAt(range.offset, range.count);
            QVERIFY(std::holds_alternative<FileIoError>(result));
            QCOMPARE(std::get<FileIoError>(result), FileIoError::SizeLimit);
        }
        auto lastByte = reader->readAt(3, 1);
        QVERIFY(std::holds_alternative<QByteArray>(lastByte));
        QCOMPARE(std::get<QByteArray>(lastByte), QByteArray("d"));
        auto wholeFile = reader->readAt(0, 4);
        QVERIFY(std::holds_alternative<QByteArray>(wholeFile));
        QCOMPARE(std::get<QByteArray>(wholeFile), QByteArray("abcd"));
        QCOMPARE(std::get<QByteArray>(reader->read(1)), QByteArray("a"));
#endif
    }
    void randomReadsHonorChunkAndFileBudgets()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(put(directory.filePath("source"), QByteArray(65536, 'x') + 'z'));
        auto opened = LocalFileReader::open(directory.path(), "source");
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileReader>>(opened));
        auto& reader = std::get<std::unique_ptr<LocalFileReader>>(opened);
        auto first = reader->readAt(0, 65536);
        QVERIFY(std::holds_alternative<QByteArray>(first));
        QCOMPARE(std::get<QByteArray>(first), QByteArray(65536, 'x'));
        auto last = reader->readAt(1, 65536);
        QVERIFY(std::holds_alternative<QByteArray>(last));
        QCOMPARE(std::get<QByteArray>(last), QByteArray(65535, 'x') + 'z');
        QCOMPARE(std::get<FileIoError>(reader->readAt(0, 65537)), FileIoError::SizeLimit);
        QFile large(directory.filePath("large"));
        QVERIFY(large.open(QIODevice::WriteOnly));
        QVERIFY(large.resize(64 * 1024 * 1024));
        QVERIFY(large.seek(64 * 1024 * 1024 - 1));
        QCOMPARE(large.write("Z", 1), qint64(1));
        large.close();
        auto largeOpened = LocalFileReader::open(directory.path(), "large");
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileReader>>(largeOpened));
        auto& largeReader = std::get<std::unique_ptr<LocalFileReader>>(largeOpened);
        auto finalByte = largeReader->readAt(64 * 1024 * 1024 - 1, 1);
        QVERIFY(std::holds_alternative<QByteArray>(finalByte));
        QCOMPARE(std::get<QByteArray>(finalByte), QByteArray("Z"));
        QCOMPARE(std::get<FileIoError>(largeReader->readAt(64 * 1024 * 1024 - 1, 2)), FileIoError::SizeLimit);
#endif
    }
    void rejectsAmbiguousAndEscapingPaths()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QStringList paths{"", ".", "..", "../outside", "a/../file", "a/./file", "/absolute", "C:/file",
            "a//file", "a/", "a\\file", "file:stream", "file.", "file ", "CON", "NUL.txt", "com1.bin", "LPT9",
            "a\nfile", "a?file", "a*file", QString(1025, 'a'), QString(1, QChar(0xd800))};
        for (const auto& path : paths) {
            auto result = LocalFileReader::open(directory.path(), path);
            QVERIFY2(std::holds_alternative<FileIoError>(result), qPrintable(path));
            QCOMPARE(std::get<FileIoError>(result), FileIoError::InvalidPath);
        }
        for (const auto& root : QStringList{"relative", "C:/", "//server/share", "\\\\?\\C:\\folder",
                directory.path() + "/..", directory.path() + "/."}) {
            auto result = LocalFileReader::open(root, "file");
            QVERIFY(std::holds_alternative<FileIoError>(result));
            QCOMPARE(std::get<FileIoError>(result), FileIoError::InvalidPath);
        }
#endif
    }
    void rejectsHardLinksDirectoriesAndOversizeSources()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("source");
        const auto link = directory.filePath("alias");
        QVERIFY(put(path, "original"));
        QVERIFY(CreateHardLinkW(wide(link), wide(path), nullptr));
        QCOMPARE(std::get<FileIoError>(LocalFileReader::open(directory.path(), "source")), FileIoError::UnsafeFile);
        QCOMPARE(std::get<FileIoError>(LocalFileReader::open(directory.path(), "alias")), FileIoError::UnsafeFile);
        QVERIFY(QDir(directory.path()).mkdir("nested"));
        QCOMPARE(std::get<FileIoError>(LocalFileReader::open(directory.path(), "nested")), FileIoError::UnsafeFile);
        QFile large(directory.filePath("large"));
        QVERIFY(large.open(QIODevice::WriteOnly));
        QVERIFY(large.resize(64 * 1024 * 1024 + 1));
        large.close();
        QCOMPARE(std::get<FileIoError>(LocalFileReader::open(directory.path(), "large")), FileIoError::SizeLimit);
        QCOMPARE(std::get<FileIoError>(LocalFileReader::open(directory.path(), "missing")), FileIoError::NotAvailable);
#endif
    }
    void pinsEveryAncestorAndSourceAgainstReplacement()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkpath("root/nested"));
        const auto root = directory.filePath("root");
        const auto path = directory.filePath("root/nested/source");
        QVERIFY(put(path, "original"));
        auto result = LocalFileReader::open(root, "nested/source");
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileReader>>(result));
        auto reader = std::move(std::get<std::unique_ptr<LocalFileReader>>(result));
        QVERIFY(!QDir().rename(root, directory.filePath("moved")));
        QVERIFY(!QDir().rename(root + "/nested", root + "/replaced"));
        QVERIFY(!QFile::rename(path, path + ".old"));
        QVERIFY(!put(path, "changed"));
        auto suffix = reader->readAt(4, 4);
        QVERIFY(std::holds_alternative<QByteArray>(suffix));
        QCOMPARE(std::get<QByteArray>(suffix), QByteArray("inal"));
        QCOMPARE(std::get<QByteArray>(reader->read(64)), QByteArray("original"));
        auto repeated = reader->readAt(0, 8);
        QVERIFY(std::holds_alternative<QByteArray>(repeated));
        QCOMPARE(std::get<QByteArray>(repeated), QByteArray("original"));
        QVERIFY(!QFile::remove(path));
        QVERIFY(!put(path, "changed"));
        reader.reset();
        QVERIFY(QFile::rename(path, path + ".old"));
        bool renamed = false;
        QTRY_VERIFY_WITH_TIMEOUT(renamed || (renamed = QDir().rename(root, directory.filePath("moved"))), 1000);
#endif
    }
    void rejectsJunctionsAtRootAndBelowRoot()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir("outside"));
        QVERIFY(put(directory.filePath("outside/secret"), "secret"));
        const auto link = directory.filePath("redirect");
        QVERIFY(junction(link, directory.filePath("outside")));
        auto rootResult = LocalFileReader::open(link, "secret");
        auto nestedResult = LocalFileReader::open(directory.path(), "redirect/secret");
        QVERIFY(RemoveDirectoryW(wide(link)));
        QVERIFY(std::holds_alternative<FileIoError>(rootResult));
        QCOMPARE(std::get<FileIoError>(rootResult), FileIoError::UnsafeFile);
        QVERIFY(std::holds_alternative<FileIoError>(nestedResult));
        QCOMPARE(std::get<FileIoError>(nestedResult), FileIoError::UnsafeFile);
        QCOMPARE(contents(directory.filePath("outside/secret")), QByteArray("secret"));
#endif
    }
    void keepsWriterParentPinnedUntilPublication()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkpath("root/nested"));
        const auto root = directory.filePath("root");
        auto result = LocalFileWriter::create(root, "nested/result", 1);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
        auto& writer = std::get<std::unique_ptr<LocalFileWriter>>(result);
        QVERIFY(!QDir().rename(root, directory.filePath("moved")));
        QVERIFY(!QDir().rename(root + "/nested", root + "/replaced"));
        QVERIFY(std::holds_alternative<std::monostate>(writer->append("x")));
        QVERIFY(std::holds_alternative<std::monostate>(writer->publish()));
        QCOMPARE(contents(root + "/nested/result"), QByteArray("x"));
        writer.reset();
        bool renamed = false;
        QTRY_VERIFY_WITH_TIMEOUT(renamed || (renamed = QDir().rename(root, directory.filePath("moved"))), 1000);
#endif
    }
    void stagesBinaryDownloadThenPublishesExactlyOnce()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir("nested"));
        const auto target = directory.filePath("nested/result.bin");
        auto result = LocalFileWriter::create(directory.path(), "nested/result.bin", 5);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
        auto writer = std::move(std::get<std::unique_ptr<LocalFileWriter>>(result));
        QVERIFY(!QFile::exists(target));
        QVERIFY(std::holds_alternative<std::monostate>(writer->append(QByteArray("a\0", 2))));
        QVERIFY(!QFile::exists(target));
        QVERIFY(std::holds_alternative<std::monostate>(writer->append("xyz")));
        QVERIFY(std::holds_alternative<std::monostate>(writer->publish()));
        QCOMPARE(contents(target), QByteArray("a\0xyz", 5));
        QCOMPARE(std::get<FileIoError>(writer->publish()), FileIoError::InvalidState);
        QCOMPARE(std::get<FileIoError>(writer->append("!")), FileIoError::InvalidState);
        writer->cancel();
        writer.reset();
        QCOMPARE(contents(target), QByteArray("a\0xyz", 5));
        QCOMPARE(QDir(directory.filePath("nested")).entryList(QDir::Files | QDir::Hidden).size(), 1);
#else
        auto result = LocalFileWriter::create("/tmp/unused", "file", 5);
        QCOMPARE(std::get<FileIoError>(result), FileIoError::UnsupportedPlatform);
#endif
    }
    void refusesExistingAndRacingTargetsWithoutOverwriting()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto target = directory.filePath("result");
        QVERIFY(put(target, "original"));
        QCOMPARE(std::get<FileIoError>(LocalFileWriter::create(directory.path(), "result", 3)), FileIoError::AlreadyExists);
        QCOMPARE(contents(target), QByteArray("original"));
        QVERIFY(QFile::remove(target));
        auto result = LocalFileWriter::create(directory.path(), "result", 3);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
        auto writer = std::move(std::get<std::unique_ptr<LocalFileWriter>>(result));
        QVERIFY(std::holds_alternative<std::monostate>(writer->append("new")));
        QVERIFY(put(target, "competitor"));
        QCOMPARE(std::get<FileIoError>(writer->publish()), FileIoError::AlreadyExists);
        writer->cancel();
        QCOMPARE(contents(target), QByteArray("competitor"));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
#endif
    }
    void explicitlyOverwritesExistingFileAndCancellationPreservesPublishedResult()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto target = directory.filePath("result");
        QVERIFY(put(target, "original"));
        QCOMPARE(std::get<FileIoError>(LocalFileWriter::create(directory.path(), "result", 3, false)), FileIoError::AlreadyExists);
        auto result = LocalFileWriter::create(directory.path(), "result", 3, true);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
        auto& writer = std::get<std::unique_ptr<LocalFileWriter>>(result);
        QVERIFY(std::holds_alternative<std::monostate>(writer->append("new")));
        QCOMPARE(contents(target), QByteArray("original"));
        QVERIFY(std::holds_alternative<std::monostate>(writer->publish()));
        QCOMPARE(contents(target), QByteArray("new"));
        QVERIFY(writer->cancel());
        writer.reset();
        QCOMPARE(contents(target), QByteArray("new"));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
#endif
    }
    void cancellingOrFailingOverwritePreservesOriginal_data()
    {
        QTest::addColumn<bool>("failPublication");
        QTest::newRow("cancel") << false;
        QTest::newRow("incomplete-publication") << true;
    }
    void cancellingOrFailingOverwritePreservesOriginal()
    {
#ifdef Q_OS_WIN
        QFETCH(bool, failPublication);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto target = directory.filePath("result");
        QVERIFY(put(target, "original"));
        auto result = LocalFileWriter::create(directory.path(), "result", 3, true);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
        auto& writer = std::get<std::unique_ptr<LocalFileWriter>>(result);
        QVERIFY(std::holds_alternative<std::monostate>(writer->append("n")));
        if (failPublication)
            QCOMPARE(std::get<FileIoError>(writer->publish()), FileIoError::SizeMismatch);
        QVERIFY(writer->cancel());
        QCOMPARE(contents(target), QByteArray("original"));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
#endif
    }
    void overwriteOfInitiallyAbsentFileStillRefusesRacingCreation()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto target = directory.filePath("result");
        auto result = LocalFileWriter::create(directory.path(), "result", 3, true);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
        auto& writer = std::get<std::unique_ptr<LocalFileWriter>>(result);
        QVERIFY(std::holds_alternative<std::monostate>(writer->append("new")));
        QVERIFY(put(target, "competitor"));
        QCOMPARE(std::get<FileIoError>(writer->publish()), FileIoError::AlreadyExists);
        QVERIFY(writer->cancel());
        QCOMPARE(contents(target), QByteArray("competitor"));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
#endif
    }
    void overwriteRejectsTargetChangesBeforePublication_data()
    {
        QTest::addColumn<QString>("change");
        for (const auto& change : QStringList{"identity", "size", "mtime", "attributes", "deleted", "directory", "hardlink", "junction"})
            QTest::newRow(qPrintable(change)) << change;
    }
    void overwriteRejectsTargetChangesBeforePublication()
    {
#ifdef Q_OS_WIN
        QFETCH(QString, change);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto target = directory.filePath("result");
        QVERIFY(put(target, "original"));
        auto result = LocalFileWriter::create(directory.path(), "result", 3, true);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
        auto& writer = std::get<std::unique_ptr<LocalFileWriter>>(result);
        QVERIFY(std::holds_alternative<std::monostate>(writer->append("new")));
        auto cleanup = qScopeGuard([&] {
            if (change == "junction") RemoveDirectoryW(wide(target));
            else if (change == "attributes") SetFileAttributesW(wide(target), FILE_ATTRIBUTE_NORMAL);
        });
        if (change == "identity") {
            QVERIFY(QFile::rename(target, target + ".old"));
            QVERIFY(put(target, "original"));
        } else if (change == "size") {
            QVERIFY(put(target, "competitor-content"));
        } else if (change == "mtime") {
            const HANDLE handle = CreateFileW(wide(target), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(handle != INVALID_HANDLE_VALUE);
            auto close = qScopeGuard([&] { CloseHandle(handle); });
            FILETIME time{};
            QVERIFY(GetFileTime(handle, nullptr, nullptr, &time));
            time.dwHighDateTime += 1;
            QVERIFY(SetFileTime(handle, nullptr, nullptr, &time));
        } else if (change == "attributes") {
            QVERIFY(SetFileAttributesW(wide(target), FILE_ATTRIBUTE_READONLY));
        } else if (change == "hardlink") {
            QVERIFY(CreateHardLinkW(wide(target + ".alias"), wide(target), nullptr));
        } else {
            QVERIFY(QFile::remove(target));
            if (change == "directory") QVERIFY(QDir().mkdir(target));
            if (change == "junction") {
                QVERIFY(QDir(directory.path()).mkdir("outside"));
                QVERIFY(put(directory.filePath("outside/secret"), "secret"));
                QVERIFY(junction(target, directory.filePath("outside")));
            }
        }
        QVERIFY(std::holds_alternative<FileIoError>(writer->publish()));
        QCOMPARE(std::get<FileIoError>(writer->publish()), FileIoError::InvalidState);
        QVERIFY(writer->cancel());
        QVERIFY(QDir(directory.path()).entryList({".novaterm-transfer-*"}, QDir::Files | QDir::Hidden).isEmpty());
        if (change == "deleted") QVERIFY(!QFile::exists(target));
        else if (change == "directory") QVERIFY(QFileInfo(target).isDir());
        else if (change == "junction") QCOMPARE(contents(directory.filePath("outside/secret")), QByteArray("secret"));
        else QCOMPARE(contents(target), change == "size" ? QByteArray("competitor-content") : QByteArray("original"));
#endif
    }
    void overwriteRefusesUnsafeInitialTarget_data()
    {
        QTest::addColumn<QString>("kind");
        for (const auto& kind : QStringList{"directory", "hardlink", "junction", "readonly"})
            QTest::newRow(qPrintable(kind)) << kind;
    }
    void overwriteRefusesUnsafeInitialTarget()
    {
#ifdef Q_OS_WIN
        QFETCH(QString, kind);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto target = directory.filePath("result");
        auto cleanup = qScopeGuard([&] {
            if (kind == "junction") RemoveDirectoryW(wide(target));
            if (kind == "readonly") SetFileAttributesW(wide(target), FILE_ATTRIBUTE_NORMAL);
        });
        if (kind == "directory") QVERIFY(QDir().mkdir(target));
        else if (kind == "junction") {
            QVERIFY(QDir(directory.path()).mkdir("outside"));
            QVERIFY(junction(target, directory.filePath("outside")));
        } else {
            QVERIFY(put(target, "original"));
            if (kind == "hardlink") QVERIFY(CreateHardLinkW(wide(target + ".alias"), wide(target), nullptr));
            if (kind == "readonly") QVERIFY(SetFileAttributesW(wide(target), FILE_ATTRIBUTE_READONLY));
        }
        auto result = LocalFileWriter::create(directory.path(), "result", 3, true);
        QVERIFY(std::holds_alternative<FileIoError>(result));
        if (kind == "hardlink" || kind == "readonly") QCOMPARE(contents(target), QByteArray("original"));
        QVERIFY(QDir(directory.path()).entryList({".novaterm-transfer-*"}, QDir::Files | QDir::Hidden).isEmpty());
#endif
    }
    void overwriteCannotReplaceTargetHeldOpenByAnotherReader()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto target = directory.filePath("result");
        QVERIFY(put(target, "original"));
        auto result = LocalFileWriter::create(directory.path(), "result", 3, true);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
        auto& writer = std::get<std::unique_ptr<LocalFileWriter>>(result);
        QVERIFY(std::holds_alternative<std::monostate>(writer->append("new")));
        const HANDLE reader = CreateFileW(wide(target), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(reader != INVALID_HANDLE_VALUE);
        auto close = qScopeGuard([&] { CloseHandle(reader); });
        QVERIFY(std::holds_alternative<FileIoError>(writer->publish()));
        QVERIFY(writer->cancel());
        QCOMPARE(contents(target), QByteArray("original"));
#endif
    }
    void overwriteDoesNotTreatDeletePendingAsMissing()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto target = directory.filePath("result");
        QVERIFY(put(target, "original"));
        {
            const HANDLE handle = CreateFileW(wide(target), DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(handle != INVALID_HANDLE_VALUE);
            auto close = qScopeGuard([&] { CloseHandle(handle); });
            FILE_DISPOSITION_INFO disposition{TRUE};
            QVERIFY(SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)));
            auto result = LocalFileWriter::create(directory.path(), "result", 3, true);
            disposition.DeleteFile = FALSE;
            QVERIFY(SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)));
            QVERIFY(std::holds_alternative<FileIoError>(result));
        }
        QCOMPARE(contents(target), QByteArray("original"));
        QVERIFY(QDir(directory.path()).entryList({".novaterm-transfer-*"}, QDir::Files | QDir::Hidden).isEmpty());
#endif
    }
    void cancelsOnlyOwnedTemporaryAndMoveAssignmentReleasesOldOwnership()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(put(directory.filePath(".novaterm-transfer-unrelated.tmp"), "unrelated"));
        auto first = LocalFileWriter::create(directory.path(), "first", 3);
        auto second = LocalFileWriter::create(directory.path(), "second", 3);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(first));
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(second));
        auto one = std::move(std::get<std::unique_ptr<LocalFileWriter>>(first));
        auto two = std::move(std::get<std::unique_ptr<LocalFileWriter>>(second));
        QVERIFY(std::holds_alternative<std::monostate>(one->append("one")));
        QVERIFY(std::holds_alternative<std::monostate>(two->append("two")));
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 3);
        *one = std::move(*two);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
        QCOMPARE(std::get<FileIoError>(two->publish()), FileIoError::InvalidState);
        QVERIFY(std::holds_alternative<std::monostate>(one->publish()));
        QVERIFY(!QFile::exists(directory.filePath("first")));
        QCOMPARE(contents(directory.filePath("second")), QByteArray("two"));
        one.reset(); two.reset();
        auto cancelled = LocalFileWriter::create(directory.path(), "cancelled", 3);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(cancelled));
        std::get<std::unique_ptr<LocalFileWriter>>(cancelled)->cancel();
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
        QCOMPARE(contents(directory.filePath(".novaterm-transfer-unrelated.tmp")), QByteArray("unrelated"));
        QVERIFY(!QFile::exists(directory.filePath("cancelled")));
#endif
    }
    void rejectsInvalidLengthsAndPoisonedWritesCannotPublish()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QCOMPARE(std::get<FileIoError>(LocalFileWriter::create(directory.path(), "huge", 67108865)), FileIoError::SizeLimit);
        for (const auto& path : QStringList{"../escape", "a:stream", "NUL", "file.", "a\\file"})
            QCOMPARE(std::get<FileIoError>(LocalFileWriter::create(directory.path(), path, 3)), FileIoError::InvalidPath);
        auto shortFile = LocalFileWriter::create(directory.path(), "short", 3);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(shortFile));
        auto& writer = std::get<std::unique_ptr<LocalFileWriter>>(shortFile);
        QVERIFY(std::holds_alternative<std::monostate>(writer->append("x")));
        QCOMPARE(std::get<FileIoError>(writer->publish()), FileIoError::SizeMismatch);
        QCOMPARE(std::get<FileIoError>(writer->append("yz")), FileIoError::InvalidState);
        writer.reset();
        auto overflow = LocalFileWriter::create(directory.path(), "overflow", 3);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(overflow));
        auto& tooLong = std::get<std::unique_ptr<LocalFileWriter>>(overflow);
        QCOMPARE(std::get<FileIoError>(tooLong->append("four")), FileIoError::SizeLimit);
        QCOMPARE(std::get<FileIoError>(tooLong->publish()), FileIoError::InvalidState);
        tooLong.reset();
        auto chunk = LocalFileWriter::create(directory.path(), "chunk", 65537);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(chunk));
        auto& tooBig = std::get<std::unique_ptr<LocalFileWriter>>(chunk);
        QCOMPARE(std::get<FileIoError>(tooBig->append(QByteArray(65537, 'x'))), FileIoError::SizeLimit);
        QCOMPARE(std::get<FileIoError>(tooBig->publish()), FileIoError::InvalidState);
        tooBig.reset();
        QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
#endif
    }
    void acceptsEmptyFileAndCleansUnpublishedFileOnDestruction()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        {
            auto result = LocalFileWriter::create(directory.path(), "discard", 1);
            QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
            auto& writer = std::get<std::unique_ptr<LocalFileWriter>>(result);
            QVERIFY(std::holds_alternative<std::monostate>(writer->append("x")));
            const auto files = QDir(directory.path()).entryList(QDir::Files | QDir::Hidden);
            QCOMPARE(files.size(), 1);
            QVERIFY(!QFile::remove(directory.filePath(files.first())));
            QVERIFY(!put(directory.filePath(files.first()), "tampered"));
        }
        QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
        auto result = LocalFileWriter::create(directory.path(), "empty", 0);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
        QVERIFY(std::holds_alternative<std::monostate>(std::get<std::unique_ptr<LocalFileWriter>>(result)->publish()));
        auto readResult = LocalFileReader::open(directory.path(), "empty");
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileReader>>(readResult));
        QCOMPARE(std::get<std::unique_ptr<LocalFileReader>>(readResult)->size(), quint64(0));
        QCOMPARE(std::get<QByteArray>(std::get<std::unique_ptr<LocalFileReader>>(readResult)->read(1)), QByteArray{});
        QCOMPARE(std::get<FileIoError>(std::get<std::unique_ptr<LocalFileReader>>(readResult)->readAt(0, 1)), FileIoError::SizeLimit);
        QCOMPARE(std::get<FileIoError>(std::get<std::unique_ptr<LocalFileReader>>(readResult)->readAt(0, 0)), FileIoError::SizeLimit);
#endif
    }
    void preservesExactPublicationNameAtEveryBufferAlignment()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        for (int length = 1; length <= 20; ++length) {
            const auto name = QString(length, 'x') + QStringLiteral("固件.bin");
            auto result = LocalFileWriter::create(directory.path(), name, 1);
            QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
            auto& writer = std::get<std::unique_ptr<LocalFileWriter>>(result);
            QVERIFY(std::holds_alternative<std::monostate>(writer->append("x")));
            QVERIFY(std::holds_alternative<std::monostate>(writer->publish()));
            QCOMPARE(contents(directory.filePath(name)), QByteArray("x"));
        }
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 20);
#endif
    }
    void clearsReadonlyOnOwnedTemporaryBeforeCancellation()
    {
#ifdef Q_OS_WIN
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto result = LocalFileWriter::create(directory.path(), "result", 1);
        QVERIFY(std::holds_alternative<std::unique_ptr<LocalFileWriter>>(result));
        auto& writer = std::get<std::unique_ptr<LocalFileWriter>>(result);
        QVERIFY(std::holds_alternative<std::monostate>(writer->append("x")));
        const auto files = QDir(directory.path()).entryList(QDir::Files | QDir::Hidden);
        QCOMPARE(files.size(), 1);
        const auto path = directory.filePath(files.first());
        const HANDLE attributes = CreateFileW(wide(path), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        QVERIFY(attributes != INVALID_HANDLE_VALUE);
        FILE_BASIC_INFO basic{};
        basic.FileAttributes = FILE_ATTRIBUTE_READONLY;
        const bool changed = SetFileInformationByHandle(attributes, FileBasicInfo, &basic, sizeof(basic));
        CloseHandle(attributes);
        QVERIFY(changed);
        writer->cancel();
        writer.reset();
        QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
#endif
    }
};

static_assert(!std::is_copy_constructible_v<LocalFileReader>);
static_assert(std::is_nothrow_move_constructible_v<LocalFileReader>);
static_assert(!std::is_copy_constructible_v<LocalFileWriter>);
static_assert(std::is_nothrow_move_assignable_v<LocalFileWriter>);
QTEST_GUILESS_MAIN(LocalFileAccessTests)
#include "LocalFileAccessTests.moc"
