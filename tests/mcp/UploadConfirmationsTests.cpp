#include "mcp/UploadConfirmations.h"
#include <QTemporaryDir>
#include <QTest>

using namespace NovaTerm::Mcp;
class UploadConfirmationsTests final : public QObject {
    Q_OBJECT
    static UploadConfirmations::Intent intent() {
        return {"access", "1", "connection", "binding-1", "host-key-and-account", "SSH board",
            "session", "epoch", "operation", "C:/build/firmware.bin", "/opt/firmware.bin", "sftp", false};
    }
private slots:
    void expiredTokenCannotUploadAndPendingBudgetIsReclaimed() {
        QTemporaryDir root; qint64 now = 0;
        UploadConfirmations confirmations(root.path(), [&] { return now; });
        auto request = intent();
        const auto token = confirmations.check(request).reply.value("data").toObject().value("confirmationToken").toString();
        for (int i = 1; i < 64; ++i) {
            request.operationId = QString::number(i);
            QCOMPARE(confirmations.check(request).reply.value("data").toObject().value("state").toString(), QString("confirmation_required"));
        }
        request.operationId = "overflow";
        QCOMPARE(confirmations.check(request).reply.value("error").toObject().value("code").toString(), QString("BUSY"));
        now = 300000;
        request = intent(); QVERIFY(!confirmations.check(request, token, true).allowed);
        const auto fresh = confirmations.check(request).reply.value("data").toObject();
        QVERIFY(fresh.value("confirmationToken").toString() != token);
        QCOMPARE(fresh.value("state").toString(), QString("confirmation_required"));
    }
    void memoryUsesPersistentTokenIdentityNotProcessCounter() {
        QTemporaryDir root; auto request = intent(); request.memoryIdentity = "token-A";
        UploadConfirmations first(root.path());
        const auto token = first.check(request).reply.value("data").toObject().value("confirmationToken").toString();
        QVERIFY(first.check(request, token, true).allowed);
        UploadConfirmations restarted(root.path());
        request.identity = "9"; request.operationId = "new-operation";
        QVERIFY(restarted.check(request).allowed);
        request.memoryIdentity = "token-B";
        QVERIFY(!restarted.check(request).allowed);
    }
    void anotherInstanceClearInvalidatesAcceptedAndUnansweredTokens_data() {
        QTest::addColumn<bool>("accepted");
        QTest::newRow("accepted-but-not-started") << true;
        QTest::newRow("awaiting-user") << false;
    }
    void anotherInstanceClearInvalidatesAcceptedAndUnansweredTokens() {
        QFETCH(bool, accepted);
        QTemporaryDir root; UploadConfirmations first(root.path()), second(root.path());
        const auto request = intent();
        const auto token = first.check(request).reply.value("data").toObject().value("confirmationToken").toString();
        if (accepted) QVERIFY(first.check(request, token, true).allowed);
        QVERIFY(second.clear());
        QVERIFY(!first.check(request, token, true).allowed);
        const auto fresh = first.check(request); QVERIFY(!fresh.allowed);
        QVERIFY(fresh.reply.value("data").toObject().value("confirmationToken").toString() != token);
    }
    void onlyExplicitExactConfirmationStartsAndIsRemembered() {
        QTemporaryDir root; UploadConfirmations confirmations(root.path()); auto request = intent();
        const auto first = confirmations.check(request); QVERIFY(!first.allowed);
        const auto prompt = first.reply.value("data").toObject();
        QCOMPARE(prompt.value("state").toString(), QString("confirmation_required"));
        QCOMPARE(prompt.value("localPath").toString(), request.localPath);
        const auto token = prompt.value("confirmationToken").toString(); QVERIFY(!token.isEmpty());
        QCOMPARE(confirmations.check(request).reply.value("data").toObject().value("confirmationToken"), QJsonValue(token));
        QVERIFY(!confirmations.check(request, "forged", true).allowed);
        auto changed = request; changed.remotePath = "/elsewhere/file";
        QVERIFY(!confirmations.check(changed, token, true).allowed);
        QVERIFY(confirmations.check(request, token, true).allowed);
        QVERIFY(confirmations.check(request, token, true).allowed);
        request.operationId = "next-operation";
        QVERIFY(confirmations.check(request).allowed);
        UploadConfirmations restarted(root.path());
        QVERIFY(restarted.check(request).allowed);
        request.overwrite = true; QVERIFY(!restarted.check(request).allowed);
    }
    void declineAndChangedIdentityNeverStart() {
        QTemporaryDir root; UploadConfirmations confirmations(root.path()); auto request = intent();
        const auto prompt = confirmations.check(request).reply.value("data").toObject();
        const auto token = prompt.value("confirmationToken").toString();
        const auto declined = confirmations.check(request, token, false); QVERIFY(!declined.allowed);
        QCOMPARE(declined.reply.value("data").toObject().value("state").toString(), QString("declined"));
        QVERIFY(!confirmations.check(request, token, true).allowed);
        request.operationId = "other";
        const auto next = confirmations.check(request).reply.value("data").toObject().value("confirmationToken").toString();
        request.identity = "2";
        QVERIFY(!confirmations.check(request, next, true).allowed);
    }
    void disconnectAndClearInvalidatePendingAndMemory() {
        QTemporaryDir root; UploadConfirmations confirmations(root.path()); auto request = intent();
        auto token = confirmations.check(request).reply.value("data").toObject().value("confirmationToken").toString();
        confirmations.disconnect(request.connection); QVERIFY(!confirmations.check(request, token, true).allowed);
        token = confirmations.check(request).reply.value("data").toObject().value("confirmationToken").toString();
        QVERIFY(confirmations.check(request, token, true).allowed);
        QVERIFY(confirmations.clear()); QVERIFY(!confirmations.check(request).allowed);
        UploadConfirmations restarted(root.path()); QVERIFY(!restarted.check(request).allowed);
    }
};
QTEST_GUILESS_MAIN(UploadConfirmationsTests)
#include "UploadConfirmationsTests.moc"
