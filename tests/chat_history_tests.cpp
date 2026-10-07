#include "chat_history.hpp"
#include "chat_content.hpp"
#include <QBuffer>
#include <QImage>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <future>

class ChatHistoryTests final : public QObject {
    Q_OBJECT
    static QString request() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    static QJsonArray records(const ChatHistory& history) { return history.page().value("records").toArray(); }
    static QJsonObject append(ChatHistory& history, const TlsIdentity& identity, qint64 now, int days = 1, const QString& id = {}) {
        return history.append(identity.id(), "Alice", id.isEmpty() ? request() : id, "private message", now, {}, "mossling", days * ChatHistory::lifetime);
    }
private slots:
    void expiredRetryReclaimsItsImageBeyondTheMaintenanceBatch_data() {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"removed", "reused", "replaced", "shared"}) QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void expiredRetryReclaimsItsImageBeyondTheMaintenanceBatch() {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        const auto identity = TlsIdentity::create();
        const auto path = directory.filePath("chat"), id = request();
        const qint64 now = 1700000000000;
        ChatHistory history(path, identity, now);
        for (int i = 0; i < 256; ++i) append(history, identity, now);
        QImage pixels(8, 8, QImage::Format_RGB32); pixels.fill(Qt::blue);
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(pixels.save(&buffer, "PNG"));
        const auto image = ChatContent::sanitizeImage(bytes);
        const auto original = history.append(identity.id(), "Alice", id, "old image", now, image);
        const auto hash = original.value("image").toObject().value("hash").toString();
        QCOMPARE(history.image(hash, now), image);
        if (mode == "shared") history.append(identity.id(), "Alice", request(), "keep image", now, image, "mossling", 30 * ChatHistory::lifetime);
        QCOMPARE(QDir(path + ".images").entryList({"*.enc"}, QDir::Files).size(), 1);
        QByteArray replacement;
        if (mode == "reused") replacement = image;
        else if (mode == "replaced") {
            pixels.fill(Qt::red); buffer.close(); bytes.clear(); QVERIFY(buffer.open(QIODevice::WriteOnly));
            QVERIFY(pixels.save(&buffer, "PNG")); replacement = ChatContent::sanitizeImage(bytes);
        }
        const auto receipt = history.append(identity.id(), "Alice", id, "new message", now + ChatHistory::lifetime, replacement);
        QCOMPARE(QDir(path + ".images").entryList({"*.enc"}, QDir::Files).size(), mode == "removed" ? 0 : 1);
        QCOMPARE(records(history).size(), mode == "shared" ? 2 : 1);
        QCOMPARE(records(history).last().toObject(), receipt);
        if (mode == "removed" || mode == "replaced") QVERIFY_THROWS_EXCEPTION(std::runtime_error, history.image(hash, now + ChatHistory::lifetime));
        else QCOMPARE(history.image(hash, now + ChatHistory::lifetime), image);
        if (!replacement.isEmpty()) QCOMPARE(history.image(receipt.value("image").toObject().value("hash").toString(), now + ChatHistory::lifetime), replacement);
    }
    void retainsMoreThanTwoThousandUnexpiredMessages() {
        QTemporaryDir directory;
        const auto identity = TlsIdentity::create();
        const qint64 now = 1700000000000;
        ChatHistory history(directory.filePath("chat"), identity, now);
        for (int i = 1; i <= 2100; ++i) {
            const auto receipt = append(history, identity, now);
            QCOMPARE(receipt.value("sequence").toInteger(), i);
        }
        qint64 cursor = 0, count = 0;
        do {
            const auto page = history.page(cursor, "newer");
            const auto messages = page.value("records").toArray();
            QVERIFY(messages.size() <= 40);
            QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size() <= 40 * 1024);
            count += messages.size();
            cursor = messages.last().toObject().value("sequence").toInteger();
            if (!page.value("newer").toBool()) break;
        } while (count < 2200);
        QCOMPARE(count, 2100);
        QCOMPARE(records(history).size(), 40);
        const auto fullSize = QFileInfo(directory.filePath("chat.sqlite")).size();
        history.expire(now + ChatHistory::lifetime);
        QVERIFY(records(history).isEmpty()); // Includes rows awaiting the next bounded deletion batch.
        ChatHistory restarted(directory.filePath("chat"), identity, now);
        QVERIFY(records(restarted).isEmpty());
        for (int i = 0; i < 30; ++i) restarted.expire(now + ChatHistory::lifetime);
        QVERIFY(QFileInfo(directory.filePath("chat.sqlite")).size() < fullSize / 2);
    }
    void migratesEncryptedLegacyAndKeepsReceiptAcrossRestarts() {
        QTemporaryDir directory;
        const auto identity = TlsIdentity::create();
        const qint64 now = 1700000000000;
        const auto path = directory.filePath("chat"), id = request();
        const QJsonObject original{{"sequence", 17}, {"created", now}, {"expires", now + ChatHistory::lifetime},
            {"sender", identity.id()}, {"name", "Alice"}, {"request", id}, {"text", "private message"}, {"avatar", "mossling"}, {"avatarId", "mossling"}};
        const auto context = "SquadSpeak/chat-store/v1/" + identity.id().toUtf8();
        QFile legacy(path); QVERIFY(legacy.open(QIODevice::WriteOnly));
        const auto sealed = TlsIdentity::seal(QJsonDocument(QJsonObject{{"version", 1}, {"sequence", 17}, {"time", now},
            {"messages", QJsonArray{original}}}).toJson(), identity.deriveKey(context, "history encryption"), context);
        QCOMPARE(legacy.write(sealed), sealed.size()); legacy.close();
        {
            ChatHistory migrated(path, identity, now);
            QVERIFY(!QFile::exists(path));
            QCOMPARE(records(migrated), QJsonArray{original});
            QCOMPARE(append(migrated, identity, now + 100, 1, id), original);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, migrated.append(identity.id(), "Alice", id, "conflict", now));
        }
        ChatHistory restarted(path, identity, now + 500);
        QCOMPARE(records(restarted), QJsonArray{original});
        QCOMPARE(append(restarted, identity, now).value("sequence").toInteger(), 18);
        QFile disk(path + ".sqlite"); QVERIFY(disk.open(QIODevice::ReadOnly));
        const auto bytes = disk.readAll();
        for (const auto& secret : {QByteArray("private message"), QByteArray("Alice"), identity.id().toUtf8(), id.toUtf8()}) QVERIFY(!bytes.contains(secret));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ChatHistory(path, TlsIdentity::create(), now));
    }
    void writeFailureDoesNotAcknowledgeOrEvictAndRetrySucceeds() {
        QTemporaryDir directory;
        const auto identity = TlsIdentity::create();
        const qint64 now = 1700000000000;
        const auto path = directory.filePath("chat");
        ChatHistory history(path, identity, now);
        const auto first = append(history, identity, now);
        const auto id = request();
        // A journal path that cannot be written exercises an actual filesystem
        // failure without filling the user's disk or mocking the database.
        QVERIFY(QDir().mkpath(path + ".sqlite-journal"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, append(history, identity, now + 100, 1, id));
        QVERIFY(QDir().rmdir(path + ".sqlite-journal"));
        QCOMPARE(records(history), QJsonArray{first});
        const auto second = append(history, identity, now + 200, 1, id);
        QCOMPARE(second.value("sequence").toInteger(), 2);
        QCOMPARE(append(history, identity, now + 300, 1, id), second);
        ChatHistory restarted(path, identity, now);
        QCOMPARE(records(restarted).size(), 2);
    }
    void allDurationsUseUtcAndLegacyViewsNeverChangeStoredExpiry() {
        QTemporaryDir directory;
        const auto identity = TlsIdentity::create();
        // A UTC instant around a daylight-saving transition; duration is elapsed
        // time, independent of the machine's timezone or a 23/25-hour local day.
        const qint64 now = 1743296400000;
        ChatHistory history(directory.filePath("chat"), identity, now);
        for (const auto days : {1, 7, 30, 90, 180, 360}) {
            const auto value = append(history, identity, now, days);
            QCOMPARE(value.value("expires").toInteger(), now + days * ChatHistory::lifetime);
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, append(history, identity, now, 2));
        QCOMPARE(history.page(0, "latest", false).value("records").toArray().last().toObject().value("expires").toInteger(), now + 30 * ChatHistory::lifetime);
        QCOMPARE(records(history).last().toObject().value("expires").toInteger(), now + 360 * ChatHistory::lifetime);
        history.expire(now + 30 * ChatHistory::lifetime);
        QVERIFY(history.page(0, "latest", false).value("records").toArray().isEmpty());
        QCOMPARE(records(history).size(), 3);
        ChatHistory restarted(directory.filePath("chat"), identity, now);
        QCOMPARE(records(restarted).size(), 3);
        restarted.expire(now + 360 * ChatHistory::lifetime);
        QVERIFY(records(restarted).isEmpty());
    }
    void profileChangesAreSharedAcrossPagesWithoutChangingContentOrExpiry() {
        QTemporaryDir directory;
        const auto identity = TlsIdentity::create();
        const qint64 now = 1700000000000;
        ChatHistory history(directory.filePath("chat"), identity, now);
        for (int i = 0; i < 100; ++i) append(history, identity, now, 90);
        QVERIFY(history.updateProfile(identity.id(), "Renamed", "courier"));
        for (const auto direction : {QStringLiteral("latest"), QStringLiteral("newer")})
            for (const auto entry : history.page(0, direction).value("records").toArray()) {
                const auto value = entry.toObject();
                QCOMPARE(value.value("name").toString(), QString("Renamed"));
                QCOMPARE(value.value("avatarId").toString(), QString("courier"));
                QCOMPARE(value.value("text").toString(), QString("private message"));
                QCOMPARE(value.value("expires").toInteger(), now + 90 * ChatHistory::lifetime);
            }
        QVERIFY(!history.updateProfile(TlsIdentity::create().id(), "Unknown", "courier"));
    }
    void alteredCiphertextAndLookupFieldsAreRejected() {
        QTemporaryDir directory;
        const auto identity = TlsIdentity::create();
        const auto path = directory.filePath("chat");
        const qint64 now = 1700000000000;
        ChatHistory history(path, identity, now);
        append(history, identity, now);
        const auto connection = request();
        {
            auto disk = QSqlDatabase::addDatabase("QSQLITE", connection);
            disk.setDatabaseName(path + ".sqlite"); QVERIFY(disk.open());
            QSqlQuery query(disk);
            QVERIFY(query.exec("UPDATE messages SET expires=expires+1"));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, history.page());
            QVERIFY(query.exec("UPDATE messages SET expires=expires-1,body=zeroblob(length(body))"));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, history.page());
            disk.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void separateWritersSerializeReceiptsAndRetries() {
        QTemporaryDir directory;
        const auto identity = TlsIdentity::create();
        const auto path = directory.filePath("chat"), id = request();
        const qint64 now = 1700000000000;
        { ChatHistory initial(path, identity, now); }
        const auto write = [&] {
            ChatHistory history(path, identity, now);
            return append(history, identity, now, 1, id);
        };
        auto first = std::async(std::launch::async, write);
        auto second = std::async(std::launch::async, write);
        QCOMPARE(first.get(), second.get());
        ChatHistory restored(path, identity, now);
        QCOMPARE(records(restored).size(), 1);
    }
    void concurrentExpiryPreservesAnImageReusedByANewReceipt() {
        QImage pixels(8, 8, QImage::Format_RGB32); pixels.fill(Qt::blue);
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(pixels.save(&buffer, "PNG"));
        const auto image = ChatContent::sanitizeImage(bytes);
        const auto identity = TlsIdentity::create();
        const qint64 now = 1700000000000;
        for (int i = 0; i < 16; ++i) {
            QTemporaryDir directory;
            const auto path = directory.filePath("chat");
            { ChatHistory initial(path, identity, now); initial.append(identity.id(), "Alice", request(), "old", now, image); }
            auto expire = std::async(std::launch::async, [&] {
                ChatHistory history(path, identity, now);
                return history.expire(now + ChatHistory::lifetime);
            });
            auto renew = std::async(std::launch::async, [&] {
                ChatHistory history(path, identity, now);
                return history.append(identity.id(), "Alice", request(), "new", now + ChatHistory::lifetime, image);
            });
            expire.get();
            const auto receipt = renew.get();
            ChatHistory restored(path, identity, now + ChatHistory::lifetime);
            QCOMPARE(records(restored), QJsonArray{receipt});
            QCOMPARE(restored.image(receipt.value("image").toObject().value("hash").toString(), now + ChatHistory::lifetime), image);
        }
    }
};
QTEST_GUILESS_MAIN(ChatHistoryTests)
#include "chat_history_tests.moc"
