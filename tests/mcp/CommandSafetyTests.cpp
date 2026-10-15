/** @file CommandSafetyTests.cpp @brief 命令风险与脚本精确路径的无外部副作用回归。 */
#include "mcp/CommandRiskPolicy.h"
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
    void literalListArguments_data();
    void literalListArguments();
    void localWritePreservesConfirmedPath_data();
    void localWritePreservesConfirmedPath();
    void localRelativeTargetDoesNotBecomeAbsolute();
    void localRejectsNullPaths();
    void remotePathsPreserveConfirmedSpelling_data();
    void remotePathsPreserveConfirmedSpelling();
    void remoteUploadKeepsPublicationEvidenceAfterIdentityChange_data();
    void remoteUploadKeepsPublicationEvidenceAfterIdentityChange();
    void repeatedRiskPrefixesStayBounded_data();
    void repeatedRiskPrefixesStayBounded();
    void forbiddenSuffixesRemainDenied_data();
    void forbiddenSuffixesRemainDenied();
    void repeatedPrefixesDoNotHideForbiddenFamilies_data();
    void repeatedPrefixesDoNotHideForbiddenFamilies();
};

void CommandSafetyTests::literalListArguments_data()
{
    QTest::addColumn<QString>("command");
    QTest::addColumn<bool>("allowed");
    QTest::newRow("bare") << QStringLiteral("ls") << true;
    QTest::newRow("flags") << QStringLiteral("ls -lahtrS1") << true;
    QTest::newRow("literal-relative") << QStringLiteral("ls -la ./cache-1/log.txt") << true;
    QTest::newRow("literal-absolute") << QStringLiteral("ls /var/log") << true;
    QTest::newRow("literal-hidden") << QStringLiteral("ls ../.cache") << true;
    QTest::newRow("zsh-file-substitution") << QStringLiteral("ls =(id)") << false;
    QTest::newRow("zsh-nested-substitution") << QStringLiteral("ls -la =(echo${IFS}x)") << false;
    QTest::newRow("posix-substitution") << QStringLiteral("ls $(id)") << false;
    QTest::newRow("backticks") << QStringLiteral("ls `id`") << false;
    QTest::newRow("process-substitution") << QStringLiteral("ls <(id)") << false;
    QTest::newRow("variable") << QStringLiteral("ls $HOME") << false;
    QTest::newRow("tilde") << QStringLiteral("ls ~root") << false;
    QTest::newRow("glob") << QStringLiteral("ls *.txt") << false;
    QTest::newRow("braces") << QStringLiteral("ls {one,two}") << false;
    QTest::newRow("single-quotes") << QStringLiteral("ls 'name'") << false;
    QTest::newRow("double-quotes") << QStringLiteral("ls \"name\"") << false;
    QTest::newRow("backslash") << QStringLiteral("ls a\\b") << false;
    QTest::newRow("arbitrary-option") << QStringLiteral("ls --format=long") << false;
    QTest::newRow("unrecognized-option") << QStringLiteral("ls -R") << false;
    QTest::newRow("unicode-space") << QStringLiteral("ls\u00a0file") << false;
    QTest::newRow("unicode-line-separator") << QStringLiteral("ls\u2028file") << false;
    QTest::newRow("newline") << QStringLiteral("ls\nid") << false;
    QTest::newRow("trailing-newline") << QStringLiteral("ls\n") << false;
    QTest::newRow("unicode-leading-space") << QStringLiteral("\u00a0ls") << false;
    QTest::newRow("unicode-fixed-template") << QStringLiteral("cat\u00a0/proc/version") << false;
}

void CommandSafetyTests::literalListArguments()
{
    QFETCH(QString, command);
    QFETCH(bool, allowed);
    const auto risk = CommandRiskPolicy{}.classify(command);
    QCOMPARE(risk.decision == RiskDecision::Allow, allowed);
}

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

void CommandSafetyTests::repeatedRiskPrefixesStayBounded_data()
{
    QTest::addColumn<QByteArray>("prefix");
    QTest::addColumn<bool>("longWhitespace");
    QTest::newRow("dd") << QByteArrayLiteral("dd ") << false;
    QTest::newRow("chmod") << QByteArrayLiteral("chmod ") << false;
    QTest::newRow("mixed") << QByteArrayLiteral("dd chmod ") << false;
    QTest::newRow("dd-long-whitespace") << QByteArrayLiteral("dd ") << true;
    QTest::newRow("chmod-long-whitespace") << QByteArrayLiteral("chmod ") << true;
}

void CommandSafetyTests::repeatedRiskPrefixesStayBounded()
{
    QFETCH(QByteArray, prefix);
    QFETCH(bool, longWhitespace);
    // Classify bytes only. No shell, ScriptProvider, or command execution is involved.
    const QByteArray content = longWhitespace
        ? prefix + QByteArray(2 * 1024 * 1024 - prefix.size(), ' ')
        : prefix.repeated((2 * 1024 * 1024) / prefix.size());
    QElapsedTimer timer;
    timer.start();
    const auto risk = CommandRiskPolicy{}.classifyScript(content,
        QStringLiteral("/tmp/inert.sh"), QStringLiteral("/tmp"),
        QStringLiteral("sh ./inert.sh"));
    const auto elapsed = timer.elapsed();
    qInfo() << prefix << content.size() << "bytes classified in" << elapsed << "ms";
    QCOMPARE(risk.decision, RiskDecision::Confirm);
    // A coarse watchdog catches multi-second/quadratic GUI stalls; this is not a
    // throughput benchmark and leaves ample room for Debug/sanitized/loaded CI.
    QVERIFY2(elapsed < 5000, "2 MiB risk classification exceeded the GUI-stall watchdog");
}

void CommandSafetyTests::forbiddenSuffixesRemainDenied_data()
{
    QTest::addColumn<QByteArray>("content");
    QTest::addColumn<bool>("denied");
    QTest::newRow("dd-device") << QByteArrayLiteral("dd if=x of=/dev/example") << true;
    QTest::newRow("chmod-setuid") << QByteArrayLiteral("chmod 755 u+s") << true;
    QTest::newRow("chmod-setgid") << QByteArrayLiteral("chmod g+s example") << true;
    QTest::newRow("mixed-case") << QByteArrayLiteral("DD if=x OF=/DEV/example") << true;
    QTest::newRow("dd-newline-space") << QByteArrayLiteral("dd \n  of=/dev/example") << true;
    QTest::newRow("chmod-newline-space") << QByteArrayLiteral("chmod\n  u+s example") << true;
    QTest::newRow("dd-other-line") << QByteArrayLiteral("dd safe\necho of=/dev/example") << false;
    QTest::newRow("chmod-other-line") << QByteArrayLiteral("chmod safe\necho u+s") << false;
    QTest::newRow("not-word-boundary") << QByteArrayLiteral("dd proof=/dev/example") << false;
    QTest::newRow("safe-script") << QByteArrayLiteral("#!/bin/sh\nprintf safe\n") << false;
}

void CommandSafetyTests::forbiddenSuffixesRemainDenied()
{
    QFETCH(QByteArray, content);
    QFETCH(bool, denied);
    const auto risk = CommandRiskPolicy{}.classifyScript(content,
        QStringLiteral("/tmp/inert.sh"), QStringLiteral("/tmp"),
        QStringLiteral("sh ./inert.sh"));
    QCOMPARE(risk.decision == RiskDecision::Deny, denied);
}

void CommandSafetyTests::repeatedPrefixesDoNotHideForbiddenFamilies_data()
{
    QTest::addColumn<QByteArray>("prefix");
    QTest::newRow("dd") << QByteArrayLiteral("dd ");
    QTest::newRow("chmod") << QByteArrayLiteral("chmod ");
}

void CommandSafetyTests::repeatedPrefixesDoNotHideForbiddenFamilies()
{
    QFETCH(QByteArray, prefix);
    QByteArray content = prefix.repeated((2 * 1024 * 1024 - 128) / prefix.size());
    content += " sudo forbidden";
    const auto risk = CommandRiskPolicy{}.classifyScript(content,
        QStringLiteral("/tmp/inert.sh"), QStringLiteral("/tmp"),
        QStringLiteral("sh ./inert.sh"));
    QCOMPARE(risk.decision, RiskDecision::Deny);
}

QTEST_GUILESS_MAIN(CommandSafetyTests)
#include "CommandSafetyTests.moc"
