#include "local_channel.hpp"
#include "identity_fixture.hpp"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QJsonDocument>

namespace {
QByteArray exportedKey(char value = '\1') {
    return value == '\1' ? firstIdentityExport : secondIdentityExport;
}
bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
}

class IdentityImportTests final : public QObject {
    Q_OBJECT
private slots:
    void importRequiresConfirmationAndSurvivesRestartEncrypted() {
        QTemporaryDir directory;
        const auto path = directory.filePath("export.ini");
        const auto store = directory.filePath("channel");
        const auto data = exportedKey();
        QVERIFY(writeFile(path, data));
        const auto localIdentity = TlsIdentity::create();
        QString importedUid;
        {
            VoiceSession session(directory.filePath("session"));
            LocalChannel channel(session, store, localIdentity);
            const auto before = readFile(store);
            const auto result = channel.prepareTeamSpeakIdentityImport(QUrl::fromLocalFile(path));
            QCOMPARE(result.keys(), QStringList{"uid"});
            importedUid = result.value("uid").toString();
            QCOMPARE(importedUid, firstIdentityUid);
            QVERIFY(channel.teamSpeakIdentityId().isEmpty());
            QCOMPARE(readFile(store), before);
            QVERIFY(channel.confirmTeamSpeakIdentityImport());
            QCOMPARE(channel.teamSpeakIdentityId(), importedUid);
            QVERIFY(!channel.confirmTeamSpeakIdentityImport());
            QCOMPARE(channel.teamSpeakIdentityId(), importedUid);
        }
        const auto stored = readFile(store);
        QVERIFY(!stored.contains(QByteArray(32, '\1').toBase64()));
        QVERIFY(!stored.contains("17V"));
        const auto encrypted = QJsonDocument::fromJson(stored).object().value("security").toObject().value("teamSpeakIdentity").toString();
        QVERIFY(!encrypted.isEmpty());
        const QByteArray context = "SquadSpeak/teamspeak-identity/v1";
        const auto plain = TlsIdentity::open(QByteArray::fromBase64(encrypted.toLatin1()), localIdentity.deriveKey(context, "identity"), context);
        const auto decoded = QJsonDocument::fromJson(plain).object();
        QCOMPARE(decoded.value("key").toString().toLatin1(), QByteArray(32, '\1').toBase64());
        QCOMPARE(decoded.value("counter").toInteger(), 17);
        QCOMPARE(decoded.value("max_counter").toInteger(), 17);
        QVERIFY_EXCEPTION_THROWN(TlsIdentity::open(QByteArray::fromBase64(encrypted.toLatin1()), TlsIdentity::create().deriveKey(context, "identity"), context), std::exception);
        QCOMPARE(readFile(path), data);
        VoiceSession session(directory.filePath("session"));
        LocalChannel restored(session, store, localIdentity);
        QCOMPARE(restored.teamSpeakIdentityId(), importedUid);
    }

    void cancelAndInvalidSelectionDiscardThePendingIdentity() {
        QTemporaryDir directory;
        const auto path = directory.filePath("export.ini");
        QVERIFY(writeFile(path, exportedKey()));
        VoiceSession session(directory.filePath("session"));
        LocalChannel channel(session, directory.filePath("channel"), TlsIdentity::create());
        const auto url = QUrl::fromLocalFile(path);
        QVERIFY(channel.prepareTeamSpeakIdentityImport(url).contains("uid"));
        channel.cancelTeamSpeakIdentityImport();
        QVERIFY(!channel.confirmTeamSpeakIdentityImport());
        QVERIFY(channel.teamSpeakIdentityId().isEmpty());
        QVERIFY(channel.prepareTeamSpeakIdentityImport(url).contains("uid"));
        QVERIFY(channel.prepareTeamSpeakIdentityImport(QUrl("https://example.test/identity.ini")).contains("error"));
        QVERIFY(!channel.confirmTeamSpeakIdentityImport());
        QVERIFY(channel.teamSpeakIdentityId().isEmpty());
    }

    void confirmationUsesExactlyThePreviewedIdentity() {
        QTemporaryDir directory;
        const auto path = directory.filePath("export.ini");
        QVERIFY(writeFile(path, exportedKey()));
        VoiceSession session(directory.filePath("session"));
        LocalChannel channel(session, directory.filePath("channel"), TlsIdentity::create());
        const auto uid = channel.prepareTeamSpeakIdentityImport(QUrl::fromLocalFile(path)).value("uid").toString();
        QVERIFY(!uid.isEmpty());
        QVERIFY(writeFile(path, exportedKey('\2')));
        QVERIFY(channel.confirmTeamSpeakIdentityImport());
        QCOMPARE(channel.teamSpeakIdentityId(), uid);
        const auto replacement = channel.prepareTeamSpeakIdentityImport(QUrl::fromLocalFile(path)).value("uid").toString();
        QCOMPARE(replacement, secondIdentityUid);
        QVERIFY(channel.confirmTeamSpeakIdentityImport());
        QCOMPARE(channel.teamSpeakIdentityId(), replacement);
    }

    void invalidFilePreservesTheExistingIdentity_data() {
        QTest::addColumn<QByteArray>("data");
        QTest::newRow("empty") << QByteArray{};
        QTest::newRow("only-public-id") << QByteArray("[Identity]\nidentity=\"lks7QL5OVMKo4pZ79cEOI5r5oEA=\"\n");
        QTest::newRow("duplicate-key") << exportedKey() + "identity=\"17Vnotakey\"\n";
        QTest::newRow("two-identities") << exportedKey() + exportedKey('\2');
        QTest::newRow("missing-key") << QByteArray("[Identity]\nnickname=Example\n");
        QTest::newRow("wrong-section") << QByteArray("[Settings]\nidentity=\"17Vbad\"\n");
        QTest::newRow("oversize") << QByteArray(16385, 'a');
        QTest::newRow("invalid-utf8") << QByteArray("[Identity]\nidentity=\"") + char(0xff) + "\"";
        QTest::newRow("negative-counter") << QByteArray("[Identity]\nidentity=\"-1V") + QByteArray(32, '\1').toBase64() + '"';
        QTest::newRow("overflow-counter") << QByteArray("[Identity]\nidentity=\"18446744073709551616V") + QByteArray(32, '\1').toBase64() + '"';
    }

    void invalidFilePreservesTheExistingIdentity() {
        QFETCH(QByteArray, data);
        QTemporaryDir directory;
        const auto path = directory.filePath("export.ini");
        const auto store = directory.filePath("channel");
        QVERIFY(writeFile(path, exportedKey()));
        VoiceSession session(directory.filePath("session"));
        LocalChannel channel(session, store, TlsIdentity::create());
        const auto uid = channel.prepareTeamSpeakIdentityImport(QUrl::fromLocalFile(path)).value("uid").toString();
        QVERIFY(channel.confirmTeamSpeakIdentityImport());
        const auto before = readFile(store);
        QVERIFY(writeFile(path, data));
        const auto result = channel.prepareTeamSpeakIdentityImport(QUrl::fromLocalFile(path));
        QCOMPARE(result.keys(), QStringList{"error"});
        QVERIFY(!channel.confirmTeamSpeakIdentityImport());
        QCOMPARE(channel.teamSpeakIdentityId(), uid);
        QCOMPARE(readFile(store), before);
    }

    void failedSavePreservesTheIdentityAndAllowsRetry() {
        QTemporaryDir directory;
        const auto path = directory.filePath("export.ini");
        const auto store = directory.filePath("channel");
        QVERIFY(writeFile(path, exportedKey()));
        VoiceSession session(directory.filePath("session"));
        LocalChannel channel(session, store, TlsIdentity::create());
        const auto first = channel.prepareTeamSpeakIdentityImport(QUrl::fromLocalFile(path)).value("uid").toString();
        QVERIFY(channel.confirmTeamSpeakIdentityImport());
        const auto before = readFile(store);
        QVERIFY(writeFile(path, exportedKey('\2')));
        const auto second = channel.prepareTeamSpeakIdentityImport(QUrl::fromLocalFile(path)).value("uid").toString();
        QVERIFY(QFile::rename(store, store + ".backup"));
        QVERIFY(QDir().mkdir(store));
        QVERIFY(!channel.confirmTeamSpeakIdentityImport());
        QCOMPARE(channel.teamSpeakIdentityId(), first);
        QCOMPARE(readFile(store + ".backup"), before);
        QVERIFY(QDir().rmdir(store));
        QVERIFY(QFile::rename(store + ".backup", store));
        QVERIFY(channel.confirmTeamSpeakIdentityImport());
        QCOMPARE(channel.teamSpeakIdentityId(), second);
    }

    void missingAndRemoteFilesAreRejected() {
        QTemporaryDir directory;
        VoiceSession session(directory.filePath("session"));
        LocalChannel channel(session, directory.filePath("channel"), TlsIdentity::create());
        for (const auto& url : {QUrl{}, QUrl::fromLocalFile(directory.filePath("missing.ini")),
             QUrl::fromLocalFile(directory.path()), QUrl("file://host/export.ini"), QUrl("https://example.test/export.ini")}) {
            QVERIFY(channel.prepareTeamSpeakIdentityImport(url).contains("error"));
            QVERIFY(!channel.confirmTeamSpeakIdentityImport());
        }
    }
};
QTEST_GUILESS_MAIN(IdentityImportTests)
#include "identity_import_tests.moc"
