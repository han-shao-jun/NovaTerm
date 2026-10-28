/** @file CommandSafetyTests.cpp @brief 命令风险与脚本精确路径的无外部副作用回归。 */
#include "session/LocalSessionScriptProvider.h"
#include "session/SshSessionScriptProvider.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class CommandSafetyTests : public QObject
{
    Q_OBJECT
private slots:
    void localWritePreservesConfirmedPath_data();
    void localWritePreservesConfirmedPath();
    void localRelativeTargetDoesNotBecomeAbsolute();
    void localRejectsNullPaths();
    void remotePathsPreserveConfirmedSpelling_data();
    void remotePathsPreserveConfirmedSpelling();
    void remoteUploadKeepsPublicationEvidenceAfterIdentityChange_data();
    void remoteUploadKeepsPublicationEvidenceAfterIdentityChange();
};


void CommandSafetyTests::localWritePreservesConfirmedPath_data()
{
    QTest::addColumn<QString>("directoryName");
    QTest::addColumn<QString>("target");
    QTest::newRow("leading-target-space") << QStringLiteral("cwd") << QStringLiteral(" script.sh");
    QTest::newRow("trailing-target-space") << QStringLiteral("cwd") << QStringLiteral("script.sh ");
    QTest::newRow("trailing-cwd-space") << QStringLiteral("cwd ") << QStringLiteral("script.sh");
}

void CommandSafetyTests::localWritePreservesConfirmedPath()
{
    QFETCH(QString, directoryName);
    QFETCH(QString, target);
#ifdef Q_OS_WIN
    if (directoryName.endsWith(QLatin1Char(' ')) || target.endsWith(QLatin1Char(' ')))
        QSKIP("Win32 path handling cannot reliably create distinct trailing-space names");
#endif
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkdir(directoryName));
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("cwd")));
    const QString directory = root.filePath(directoryName);
    const QString confirmed = directory + QLatin1Char('/') + target;
    const QString unconfirmed = directory.trimmed() + QLatin1Char('/') + target.trimmed();
    LocalSessionScriptProvider provider;
    QSignalSpy finished(&provider, &ISessionScriptProvider::finished);
    const ScriptWriteRequest request{1, QByteArrayLiteral("inert test bytes"), target, directory};
    QVERIFY(provider.writeScript(request));
    QTRY_COMPARE(finished.size(), 1);
    const auto result = qvariant_cast<ScriptWriteResult>(finished.first().first());
    QVERIFY(result.success);
    QCOMPARE(result.resolvedTargetPath, confirmed);
    QFile file(confirmed);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), request.content);
    QVERIFY(!QFile::exists(unconfirmed));
}

void CommandSafetyTests::localRejectsNullPaths()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    LocalSessionScriptProvider provider;
    const QString null(1, QChar::Null);
    QVERIFY(!provider.writeScript({1, QByteArrayLiteral("inert"),
        QStringLiteral("script") + null + QStringLiteral(".sh"), root.path()}));
    QVERIFY(!provider.writeScript({2, QByteArrayLiteral("inert"),
        QStringLiteral("script.sh"), root.path() + null}));
    QVERIFY(QDir(root.path()).entryList(QDir::Files).isEmpty());
}

void CommandSafetyTests::localRelativeTargetDoesNotBecomeAbsolute()
{
#ifdef Q_OS_WIN
    QSKIP("A space-prefixed drive path contains a colon in a relative component on Win32");
#else
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString unconfirmed = root.filePath(QStringLiteral("unconfirmed.sh"));
    const QString target = QLatin1Char(' ') + unconfirmed;
    const QString confirmed = root.path() + QLatin1Char('/') + target;
    QVERIFY(QDir().mkpath(QFileInfo(confirmed).absolutePath()));
    LocalSessionScriptProvider provider;
    QSignalSpy finished(&provider, &ISessionScriptProvider::finished);
    QVERIFY(provider.writeScript({1, QByteArrayLiteral("inert bytes"), target, root.path()}));
    QTRY_COMPARE(finished.size(), 1);
    const auto result = qvariant_cast<ScriptWriteResult>(finished.first().first());
    QVERIFY(result.success);
    QCOMPARE(result.resolvedTargetPath, confirmed);
    QVERIFY(QFile::exists(confirmed));
    QVERIFY(!QFile::exists(unconfirmed));
#endif
}

void CommandSafetyTests::remotePathsPreserveConfirmedSpelling_data()
{
    QTest::addColumn<QString>("target");
    QTest::addColumn<QString>("directory");
    QTest::addColumn<QString>("resolved");
    QTest::newRow("relative") << QStringLiteral("script.sh") << QStringLiteral("/workspace")
        << QStringLiteral("/workspace/script.sh");
    QTest::newRow("absolute") << QStringLiteral("/tmp/script.sh") << QStringLiteral("/workspace")
        << QStringLiteral("/tmp/script.sh");
    QTest::newRow("absolute-looking-relative") << QStringLiteral(" /tmp/script.sh")
        << QStringLiteral("/workspace") << QStringLiteral("/workspace/ /tmp/script.sh");
    QTest::newRow("trailing-target-space") << QStringLiteral("script.sh ")
        << QStringLiteral("/workspace") << QStringLiteral("/workspace/script.sh ");
    QTest::newRow("trailing-directory-space") << QStringLiteral("script.sh")
        << QStringLiteral("/workspace ") << QStringLiteral("/workspace /script.sh");
    QTest::newRow("absolute-leading-space-directory") << QStringLiteral("/tmp/script.sh")
        << QStringLiteral(" /workspace") << QStringLiteral("/tmp/script.sh");
    QTest::newRow("dot-segments") << QStringLiteral("sub/../script.sh")
        << QStringLiteral("/workspace") << QStringLiteral("/workspace/script.sh");
    const QString null(1, QChar::Null);
    QTest::newRow("null-target") << QStringLiteral("script") + null
        << QStringLiteral("/workspace") << QString{};
    QTest::newRow("null-directory") << QStringLiteral("script.sh")
        << QStringLiteral("/workspace") + null << QString{};
}

void CommandSafetyTests::remotePathsPreserveConfirmedSpelling()
{
    QFETCH(QString, target);
    QFETCH(QString, directory);
    QFETCH(QString, resolved);
    const ScriptWriteRequest request{1, QByteArrayLiteral("inert"), target, directory};
    QCOMPARE(SshSessionScriptProvider::resolveRemotePath(request), resolved);
}

void CommandSafetyTests::remoteUploadKeepsPublicationEvidenceAfterIdentityChange_data()
{
    QTest::addColumn<bool>("published");
    QTest::addColumn<QString>("errorCode");
    QTest::newRow("published") << true << QString{};
    QTest::newRow("unknown") << false << QStringLiteral("SCRIPT_PUBLICATION_OUTCOME_UNKNOWN");
    QTest::newRow("known-precommit-failure") << false << QStringLiteral("SCRIPT_UPLOAD_CANCELLED");
}

void CommandSafetyTests::remoteUploadKeepsPublicationEvidenceAfterIdentityChange()
{
    QFETCH(bool, published);
    QFETCH(QString, errorCode);
    // 空 Transport 表示身份已失效，只注入结果信号，不连接服务器。
    SshSessionScriptProvider provider(nullptr);
    provider._activeRequest = ScriptWriteRequest{91, QByteArrayLiteral("inert"),
        QStringLiteral("/tmp/never-written.sh"), QStringLiteral("/tmp")};
    provider._resolvedPath = provider._activeRequest->targetPath;
    provider._uploadStarted = true;
    QSignalSpy results(&provider, &ISessionScriptProvider::finished);
    emit provider._sftp.errorOccurred(QStringLiteral("late transport error"));
    QVERIFY(results.isEmpty());
    emit provider._sftp.uploadBytesFinished(91, provider._resolvedPath, published, errorCode);
    QCOMPARE(results.size(), 1);
    const auto result = results.first().first().value<ScriptWriteResult>();
    QCOMPARE(result.success, published);
    QCOMPARE(result.errorCode, errorCode);
    QCOMPARE(result.resolvedTargetPath, QStringLiteral("/tmp/never-written.sh"));
}


QTEST_GUILESS_MAIN(CommandSafetyTests)
#include "CommandSafetyTests.moc"
