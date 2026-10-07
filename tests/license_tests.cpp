#include "license.hpp"
#include "voice_session.hpp"
#include "tls_identity.hpp"
#include "headless.hpp"
#include "local_channel.hpp"
#include "radio_player.hpp"
#include <QBuffer>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSslServer>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>
#include <QUrlQuery>
#include <QElapsedTimer>
#include <QTimeZone>
#include <memory>
#ifdef Q_OS_LINUX
#include <qtkeychain/keychain.h>
#include <QCryptographicHash>
#include <QScopeGuard>
#endif

namespace {
const QJsonObject product{{"store_id", 10}, {"product_id", 20}, {"variant_id", 30}};
const QString key = "example+license-key";
const qint64 start = QDateTime::fromString("2026-10-03T12:00:00Z", Qt::ISODate).toMSecsSinceEpoch();

QJsonObject accepted(const QString& action, QString instance = "device-1") {
    return {{action == "activate" ? "activated" : action == "deactivate" ? "deactivated" : "valid", true},
        {"error", QJsonValue::Null},
        {"license_key", QJsonObject{{"id", 40}, {"key", key}, {"status", "active"}, {"activation_limit", 3},
            {"activation_usage", 1}, {"expires_at", "2027-10-03T12:00:00.000Z"}}},
        {"instance", instance.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonObject{{"id", instance}})},
        {"meta", QJsonObject{{"store_id", 10}, {"product_id", 20}, {"variant_id", 30}, {"order_id", 50}}}};
}

class Provider final : public QSslServer {
public:
    QList<QPair<int, QJsonObject>> replies;
    QList<QString> actions;
    QList<QUrlQuery> forms;
    QString holdAction;
    std::function<void(const QString&)> onAction;
    Provider() {
        auto configuration = TlsIdentity::create().configuration();
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        configuration.setMissingCertificateIsFatal(false);
        configuration.setAllowedNextProtocols({"http/1.1"});
        setSslConfiguration(configuration);
        connect(this, &QSslServer::pendingConnectionAvailable, this, [this] {
            while (auto* socket = qobject_cast<QSslSocket*>(nextPendingConnection())) {
                connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
                auto data = std::make_shared<QByteArray>();
                const auto read = [this, socket, data] {
                    data->append(socket->readAll());
                    const auto end = data->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const auto match = QRegularExpression("content-length: (\\d+)", QRegularExpression::CaseInsensitiveOption)
                        .match(QString::fromLatin1(data->first(end)));
                    if (!match.hasMatch() || data->size() < end + 4 + match.captured(1).toInt()) return;
                    disconnect(socket, &QSslSocket::readyRead, this, nullptr);
                    actions.append(QString::fromLatin1(data->split(' ').at(1)).section('/', -1));
                    forms.append(QUrlQuery(QString::fromUtf8(data->mid(end + 4))));
                    if (onAction) onAction(actions.last());
                    if (actions.last() == holdAction) return;
                    const auto response = replies.isEmpty() ? QPair<int, QJsonObject>{503, {}} : replies.takeFirst();
                    const auto body = QJsonDocument(response.second).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 " + QByteArray::number(response.first) + " Result\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                };
                connect(socket, &QSslSocket::readyRead, this, read);
                read();
            }
        });
        if (!listen(QHostAddress::LocalHost)) throw std::runtime_error("License fixture could not listen.");
    }
    QUrl url() const { return QUrl(QString("https://127.0.0.1:%1/licenses/").arg(serverPort())); }
    void acceptActivation() { replies << qMakePair(200, accepted("validate", {})) << qMakePair(200, accepted("activate")); }
};
}

class LicenseTests final : public QObject {
    Q_OBJECT
    QSslConfiguration original_;
private slots:
    void distributionCannotEnablePurchasesInStoreBuilds() {
        QTemporaryDir dir;
        License license(dir.path(), product, QUrl("https://api.lemonsqueezy.com/v1/licenses/"));
#if SQUADSPEAK_STORE_BUILD
        QVERIFY(!license.configured()); QVERIFY(!license.activate(key)); QVERIFY(!license.refresh());
        QVERIFY(!license.deactivate()); QVERIFY(!license.resetActivation()); QVERIFY(!license.active());
        QVERIFY(License::distributionProduct().isEmpty()); QVERIFY(License::purchaseUrl().isEmpty());
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
#else
        QVERIFY(license.configured()); QVERIFY(License::directDistribution());
#endif
    }
#if !SQUADSPEAK_STORE_BUILD
    void initTestCase() {
        qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1");
        original_ = QSslConfiguration::defaultConfiguration();
        // Only these loopback fixture certificates are self-signed. A separate
        // case below restores normal verification and proves rejection.
        auto fixtureTls = original_; fixtureTls.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslConfiguration::setDefaultConfiguration(fixtureTls);
    }
    void cleanupTestCase() { QSslConfiguration::setDefaultConfiguration(original_); }

    void activationPersistsEncryptedAndDoesNotConsumeAnotherSlot() {
        Provider server; server.acceptActivation();
        QTemporaryDir dir; const auto secret = TlsIdentity::newKey();
        qint64 time = start;
        {
            License license(dir.path(), product, server.url(), secret, [&] { return time; });
            QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy());
            QVERIFY2(license.active(), qPrintable(license.status())); QVERIFY(!license.pending());
            QCOMPARE(license.supportReference(), "Order 50 / License 40");
            QCOMPARE(license.expiresAt().toMSecsSinceEpoch(), start + 365LL * 86400000);
            QCOMPARE(server.actions, QList<QString>({"validate", "activate"}));
            for (const auto& form : server.forms) QCOMPARE(form.queryItemValue("license_key", QUrl::FullyDecoded), key);
            QFile receipt(dir.filePath("license.bin")); QVERIFY(receipt.open(QIODevice::ReadOnly));
            const auto bytes = receipt.readAll(); QVERIFY(!bytes.contains(key.toUtf8())); QVERIFY(!bytes.contains("device-1"));
            receipt.close();
            server.replies << qMakePair(200, accepted("validate"));
            QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
            QCOMPARE(server.actions.last(), "validate");
            QCOMPARE(server.forms.last().queryItemValue("instance_id"), "device-1");
        }
        License restored(dir.path(), product, server.url(), secret, [&] { return time; });
        QVERIFY(restored.refresh()); QTRY_VERIFY(!restored.busy());
        QVERIFY(restored.active()); // Server returned 503, not a revocation.
        QVERIFY(!restored.status().isEmpty());
        time += 365LL * 86400000;
        QVERIFY(!restored.active());
    }

    void existingBinaryReceiptIsNotMistakenForJsonByItsNonce() {
        Provider server; QTemporaryDir dir;
        // AES-256-GCM fixture: key = 32 'A' bytes, nonce = '{' then 0..10,
        // AAD = squadspeak/license/1. An existing receipt has no format prefix.
        const auto bytes = QByteArray::fromBase64(
            "ewABAgMEBQYHCAkKdxLb9mHidNzk7GbTT7yYDT5Wc0m1lYl7jaAtJokRr4b7mUZBcejtkCO2gE8B+LVtSirnpPSy96pLjQ/W"
            "fqL0zRD+GFL8jxDJnM7aONILYS4J44z8VxV4Pgxk45iI6g1WWpVweYonmuRScL3Mq8uXe/EvcHam4LR2b1EjpYcMk9zIwkbj"
            "8by1WxavszH7TyjkACq6MNjafvIU0pgEUdiCXFUor2IikIY92e6IC1V/YvpPGZQJXcrPCGWFwi+jiX6qUI4rLw3UwTb24AVS"
            "GA7iVTCNSXEuYvfBw57UiWM3Y9sO89fy");
        QFile file(dir.filePath("license.bin")); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size()); file.close();
        License license(dir.path(), product, server.url(), QByteArray(32, 'A'), [] { return start; });
        QVERIFY(license.refresh()); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        QCOMPARE(server.actions, QList<QString>{"validate"});
        QCOMPARE(server.forms.last().queryItemValue("instance_id"), "device-1");
    }

    void headlessLicenseCommandsValidateInputAndReplyOnce() {
        Provider server; server.acceptActivation(); QTemporaryDir dir;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [] { return start; });
        VoiceSession session;
        LocalChannel channel(session, dir.filePath("server.channel"), TlsIdentity::create());
        RadioPlayer radio(dir.filePath("radio.json"));
        QBuffer input, output; QVERIFY(input.open(QIODevice::ReadWrite)); QVERIFY(output.open(QIODevice::ReadWrite));
        HeadlessController controller(*QCoreApplication::instance(), channel, radio, license, input, output, false);
        const auto take = [&] {
            const auto line = output.data().trimmed(); output.buffer().clear(); output.seek(0);
            return QJsonDocument::fromJson(line).object(); // More than one reply is invalid JSON.
        };
        for (auto request : {QJsonObject{{"action", 12}}, QJsonObject{{"action", "unknown"}},
                QJsonObject{{"action", "activate"}}, QJsonObject{{"action", "activate"}, {"key", " "}},
                QJsonObject{{"action", "reset"}}}) {
            request.insert("command", "license"); controller.process(request);
            const auto reply = take(); QVERIFY(!reply.isEmpty()); QVERIFY(!reply.value("ok").toBool());
        }
        QVERIFY(server.actions.isEmpty());
        controller.process({{"command", "license"}, {"action", "activate"}, {"key", key}});
        QVERIFY(license.busy());
        controller.process({{"command", "license"}, {"action", "deactivate"}});
        QVERIFY(!take().value("ok").toBool());
        controller.process({{"command", "license"}});
        QVERIFY(take().value("data").toObject().value("busy").toBool());
        QTRY_VERIFY(!license.busy()); QVERIFY(take().value("ok").toBool()); QVERIFY(license.active());
        server.replies << qMakePair(200, accepted("validate")) << qMakePair(200, accepted("deactivate"));
        for (const auto* action : {"refresh", "deactivate", "reset"}) {
            controller.process({{"command", "license"}, {"action", action}, {"confirmed", true}});
            QTRY_VERIFY(!license.busy()); QVERIFY(take().value("ok").toBool());
        }
        QVERIFY(!license.active()); QVERIFY(!license.recoveryNeeded());
        QCOMPARE(server.actions, QList<QString>({"validate", "activate", "validate", "deactivate"}));
    }

#ifndef Q_OS_WIN
    void desktopAndHeadlessShareOneFileBoundActivation() {
        Provider server; server.acceptActivation(); QTemporaryDir dir;
        const auto identity = dir.filePath("server.pem");
        const auto device = TlsIdentity::loadFile(identity, true);
        License headless(dir.path(), product, server.url(), {}, [] { return start; }, identity);
        License desktop(dir.path(), product, server.url(), {}, [] { return start; });
        VoiceSession session;
        LocalChannel channel(session, dir.filePath("server.channel"), device);
        QVERIFY(channel.listen(QHostAddress::LocalHost)); QVERIFY(channel.initialize(true));
        RadioPlayer radio(dir.filePath("radio.json")); QVERIFY(radio.bind(channel));
        connect(&headless, &License::changed, &session, [&] { session.setSupporterEnabled(headless.active()); });
        QBuffer input, output; QVERIFY(input.open(QIODevice::ReadWrite)); QVERIFY(output.open(QIODevice::ReadWrite));
        HeadlessController controller(*QCoreApplication::instance(), channel, radio, headless, input, output, false);
        controller.process({{"command", "license"}, {"action", "activate"}, {"key", key}});
        QVERIFY(!desktop.activate(key)); // The same account lock covers both modes.
        QTRY_VERIFY(!headless.busy());
        const auto result = QJsonDocument::fromJson(output.data().trimmed()).object();
        QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
        QVERIFY(result.value("data").toObject().value("active").toBool());
        QVERIFY(!output.data().contains(key.toUtf8()));
        const auto room = channel.addOwnedChannel("Second room"); QVERIFY(!room.isEmpty());
        QVERIFY(channel.hostParticipants().isEmpty()); QVERIFY(channel.ownChannel(room)->hostParticipants().isEmpty());
        server.replies << qMakePair(200, accepted("validate"));
        QVERIFY(desktop.activate(key)); QTRY_VERIFY(!desktop.busy()); QVERIFY(desktop.active());
        QCOMPARE(server.actions.count("activate"), 1);
        QCOMPARE(server.forms.last().queryItemValue("instance_id"), "device-1");

        const auto otherIdentity = dir.filePath("other-server.pem");
        (void)TlsIdentity::loadFile(otherIdentity, true);
        License otherServer(dir.path(), product, server.url(), {}, [] { return start; }, otherIdentity);
        server.replies << qMakePair(200, accepted("validate"));
        QVERIFY(otherServer.activate(key)); QTRY_VERIFY(!otherServer.busy());
        QVERIFY2(otherServer.active(), qPrintable(otherServer.status()));
        QCOMPARE(server.actions.count("activate"), 1);

        server.replies << qMakePair(200, accepted("deactivate"));
        QVERIFY(desktop.deactivate()); QTRY_VERIFY(!desktop.busy()); QVERIFY(!desktop.active());
        output.buffer().clear(); output.seek(0);
        controller.process({{"command", "license"}, {"action", "refresh"}});
        QTRY_VERIFY(!headless.busy()); QVERIFY(!headless.active());
        QVERIFY(QJsonDocument::fromJson(output.data().trimmed()).object().value("ok").toBool());
        QVERIFY(!channel.ownChannel(room)->hosting()); QVERIFY(channel.hosting());
        QVERIFY(QFileInfo::exists(dir.filePath("server.channel")));
    }

    void fileBoundActivationRejectsMissingChangedOrUnsafeIdentity_data() {
        QTest::addColumn<QString>("failure");
        for (const auto* value : {"missing", "changed", "permissions", "malformed-envelope"})
            QTest::newRow(value) << QString(value);
    }
    void fileBoundActivationRejectsMissingChangedOrUnsafeIdentity() {
        QFETCH(QString, failure);
        Provider server; server.acceptActivation(); QTemporaryDir dir;
        const auto path = dir.filePath("server.pem");
        (void)TlsIdentity::loadFile(path, true);
        License serverPass(dir.path(), product, server.url(), {}, [] { return start; }, path);
        QVERIFY(serverPass.activate(key)); QTRY_VERIFY(!serverPass.busy()); QVERIFY(serverPass.active());
        if (failure == "missing") QVERIFY(QFile::remove(path));
        else if (failure == "changed") {
            QVERIFY(QFile::remove(path)); (void)TlsIdentity::loadFile(path, true);
        } else if (failure == "permissions") QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadOther));
        else {
            QFile receipt(dir.filePath("license.bin")); QVERIFY(receipt.open(QIODevice::WriteOnly));
            receipt.write("{\"identityFile\":\"relative.pem\",\"sealed\":\"invalid!\"}");
        }
        QFile receipt(dir.filePath("license.bin")); QVERIFY(receipt.open(QIODevice::ReadOnly)); const auto before = receipt.readAll(); receipt.close();
        License desktop(dir.path(), product, server.url(), {}, [] { return start; });
        QVERIFY(desktop.activate(key)); QTRY_VERIFY(!desktop.busy()); QVERIFY(!desktop.active());
        QVERIFY(!desktop.status().isEmpty()); QCOMPARE(server.actions.count("activate"), 1);
        QCOMPARE(server.actions.size(), 2);
        QVERIFY(receipt.open(QIODevice::ReadOnly)); QCOMPARE(receipt.readAll(), before);
        QVERIFY(serverPass.refresh()); QTRY_VERIFY(!serverPass.busy()); QVERIFY(!serverPass.active());
        QCOMPARE(server.actions.size(), 2);
    }

    void migrationRequiresExistingReceiptKeyAndSurvivesOfflineRestart() {
        Provider server; server.acceptActivation(); QTemporaryDir dir; const auto secret = TlsIdentity::newKey();
        const auto path = dir.filePath("server.pem"); (void)TlsIdentity::loadFile(path, true);
        License desktop(dir.path(), product, server.url(), secret, [] { return start; });
        QVERIFY(desktop.activate(key)); QTRY_VERIFY(!desktop.busy()); QVERIFY(desktop.active());
        License wrong(dir.path(), product, server.url(), TlsIdentity::newKey(), [] { return start; }, path);
        QVERIFY(wrong.activate(key)); QTRY_VERIFY(!wrong.busy()); QVERIFY(!wrong.active());
        QCOMPARE(server.actions.size(), 2);
        License missing(dir.path(), product, server.url(), secret, [] { return start; }, dir.filePath("missing.pem"));
        bool brieflyGranted = false;
        connect(&missing, &License::changed, this, [&] { brieflyGranted = brieflyGranted || missing.active(); });
        QVERIFY(missing.refresh()); QTRY_VERIFY(!missing.busy()); QVERIFY(!missing.active());
        QVERIFY(!brieflyGranted);
        License headless(dir.path(), product, server.url(), secret, [] { return start; }, path);
        QVERIFY(headless.refresh()); QTRY_VERIFY(!headless.busy()); QVERIFY(headless.active()); // Offline 503.
        License restored(dir.path(), product, server.url(), {}, [] { return start; });
        QVERIFY(restored.refresh()); QTRY_VERIFY(!restored.busy()); QVERIFY(restored.active());
        // The already-running desktop must also discover the changed backend.
        QVERIFY(desktop.refresh()); QTRY_VERIFY(!desktop.busy()); QVERIFY(desktop.active());
        QCOMPARE(server.actions.count("activate"), 1);
        QCOMPARE(server.actions.size(), 5);
    }
#endif

    void rejectionRevokesButTransportFailureDoesNot_data() {
        QTest::addColumn<int>("http"); QTest::addColumn<QJsonObject>("response"); QTest::addColumn<bool>("remainsActive");
        QTest::newRow("disabled") << 400 << QJsonObject{{"valid", false}, {"error", "disabled"}} << false;
        QTest::newRow("missing-device") << 404 << QJsonObject{{"valid", false}, {"error", "unknown instance"}} << false;
        QTest::newRow("rate-limit") << 429 << QJsonObject{{"valid", false}} << true;
        QTest::newRow("server-error") << 503 << QJsonObject{{"valid", false}} << true;
        QTest::newRow("malformed") << 200 << QJsonObject{{"valid", "true"}} << true;
        QTest::newRow("redirect") << 302 << QJsonObject{{"valid", false}} << true;
        QTest::newRow("oversized") << 200 << QJsonObject{{"valid", false}, {"error", QString(70000, 'x')}} << true;
        QTest::newRow("contradictory-http") << 422 << accepted("validate") << true;
    }
    void rejectionRevokesButTransportFailureDoesNot() {
        QFETCH(int, http); QFETCH(QJsonObject, response); QFETCH(bool, remainsActive);
        Provider server; server.acceptActivation(); QTemporaryDir dir; const auto secret = TlsIdentity::newKey();
        {
            License license(dir.path(), product, server.url(), secret, [] { return start; });
            QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
            server.replies << qMakePair(http, response);
            QVERIFY(license.refresh()); QTRY_VERIFY(!license.busy()); QCOMPARE(license.active(), remainsActive);
        }
        License restored(dir.path(), product, server.url(), secret, [] { return start; });
        QVERIFY(restored.refresh()); QTRY_VERIFY(!restored.busy()); QCOMPARE(restored.active(), remainsActive);
    }

    void wrongProductOrMissingExpiryCannotActivate_data() {
        QTest::addColumn<QJsonObject>("response");
        auto response = accepted("validate", {});
        auto meta = response.value("meta").toObject(); meta.insert("variant_id", 99); response.insert("meta", meta);
        QTest::newRow("different-product") << response;
        response = accepted("validate", {});
        auto license = response.value("license_key").toObject(); license.insert("expires_at", QJsonValue::Null);
        response.insert("license_key", license); QTest::newRow("no-expiry") << response;
        license.insert("expires_at", "2020-01-01T00:00:00Z"); response.insert("license_key", license);
        QTest::newRow("expired") << response;
        response = accepted("validate", {}); license = response.value("license_key").toObject();
        license.insert("activation_limit", 10); response.insert("license_key", license);
        QTest::newRow("wrong-slot-count") << response;
    }

    void formValuesPreserveLiteralPercentSignsAndReservedCharacters() {
        const QString value = QString::fromUtf8("key%2B+&=?/#ö");
        Provider server; QTemporaryDir dir;
        for (const auto* action : {"validate", "activate"}) {
            auto response = accepted(action); auto data = response.value("license_key").toObject();
            data.insert("key", value); response.insert("license_key", data);
            server.replies << qMakePair(200, response);
        }
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [] { return start; });
        QVERIFY(license.activate(value)); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        for (const auto& form : server.forms) QCOMPARE(form.queryItemValue("license_key", QUrl::FullyDecoded), value);
    }
    void wrongProductOrMissingExpiryCannotActivate() {
        QFETCH(QJsonObject, response);
        Provider server; server.replies << qMakePair(200, response); QTemporaryDir dir;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [] { return start; });
        QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy()); QVERIFY(!license.active()); QVERIFY(!license.pending());
        QCOMPARE(server.actions, QList<QString>{"validate"});
    }

    void simultaneousProfilesCannotDoubleActivateAndLostRepliesStayPending() {
        Provider server; server.replies << qMakePair(200, accepted("validate", {}));
        QTemporaryDir dir; const auto secret = TlsIdentity::newKey();
        {
            License first(dir.path(), product, server.url(), secret, [] { return start; });
            License second(dir.path(), product, server.url(), secret, [] { return start; });
            QVERIFY(first.activate(key)); QVERIFY(!second.activate(key));
            QTRY_VERIFY(!first.busy()); QVERIFY(first.pending()); QVERIFY(!first.active());
            QCOMPARE(first.supportReference(), "Order 50 / License 40");
            const auto count = server.actions.size();
            QVERIFY(first.activate(key)); QTRY_VERIFY(!first.busy()); QCOMPARE(server.actions.size(), count);
        }
        License restored(dir.path(), product, server.url(), secret, [] { return start; });
        QVERIFY(restored.activate(key)); QTRY_VERIFY(!restored.busy()); QVERIFY(restored.pending());
        QCOMPARE(server.actions.size(), 2);
        QVERIFY(restored.resetActivation()); QTRY_VERIFY(!restored.busy()); QVERIFY(!restored.pending());
        server.acceptActivation(); QVERIFY(restored.activate(key)); QTRY_VERIFY(!restored.busy()); QVERIFY(restored.active());
    }

    void deactivationFreesTheKnownInstanceAndPersists() {
        Provider server; server.acceptActivation(); QTemporaryDir dir; const auto secret = TlsIdentity::newKey();
        {
            License license(dir.path(), product, server.url(), secret, [] { return start; });
            QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
            server.replies << qMakePair(200, accepted("deactivate"));
            QVERIFY(license.deactivate()); QTRY_VERIFY(!license.busy()); QVERIFY(!license.active());
            QCOMPARE(server.actions.last(), "deactivate");
            QCOMPARE(server.forms.last().queryItemValue("instance_id"), "device-1");
        }
        const auto count = server.actions.size();
        License restored(dir.path(), product, server.url(), secret, [] { return start; });
        QVERIFY(restored.refresh()); QTRY_VERIFY(!restored.busy()); QVERIFY(!restored.active());
        QCOMPARE(server.actions.size(), count);
    }

    void manualRecoveryCanReplaceAReleasedDeviceSlotButNeverAnActiveSlot() {
        Provider server; server.acceptActivation(); QTemporaryDir dir;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [] { return start; });
        QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        QVERIFY(!license.recoveryNeeded());
        QVERIFY(license.resetActivation()); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        QCOMPARE(server.actions.size(), 2);
        server.replies << qMakePair(404, QJsonObject{{"valid", false}});
        QVERIFY(license.refresh()); QTRY_VERIFY(!license.busy()); QVERIFY(license.recoveryNeeded());
        QVERIFY(license.resetActivation()); QTRY_VERIFY(!license.busy()); QVERIFY(!license.recoveryNeeded());
        QVERIFY(!license.active()); QCOMPARE(license.supportReference(), "Order 50 / License 40");
        server.acceptActivation(); QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy());
        QVERIFY(license.active()); QCOMPARE(server.actions.last(), "activate");
    }

    void aNewAnnualKeyReplacesThePassOnlyAfterSuccessfulActivation() {
        Provider server; server.acceptActivation(); QTemporaryDir dir;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [] { return start; });
        QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        server.replies << qMakePair(400, QJsonObject{{"valid", false}});
        QVERIFY(license.activate("mistyped-key")); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        for (const auto* action : {"validate", "activate"}) {
            auto response = accepted(action, "device-2"); auto data = response.value("license_key").toObject();
            data.insert("key", "next-year-key"); data.insert("id", 41); data.insert("expires_at", "2028-10-03T12:00:00Z");
            response.insert("license_key", data); server.replies << qMakePair(200, response);
        }
        QVERIFY(license.activate("next-year-key")); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        QCOMPARE(license.supportReference(), "Order 50 / License 41");
        QCOMPARE(license.expiresAt().date(), QDate(2028, 10, 3));
        QCOMPARE(server.forms.last().queryItemValue("license_key"), "next-year-key");
    }

    void anotherProfileReusesTheSameInstanceAndObservesDeactivationOnRefresh() {
        Provider server; server.acceptActivation(); QTemporaryDir dir; const auto secret = TlsIdentity::newKey();
        License first(dir.path(), product, server.url(), secret, [] { return start; });
        License second(dir.path(), product, server.url(), secret, [] { return start; });
        QVERIFY(first.activate(key)); QTRY_VERIFY(!first.busy());
        server.replies << qMakePair(200, accepted("validate"));
        QVERIFY(second.activate(key)); QTRY_VERIFY(!second.busy()); QVERIFY(second.active());
        QCOMPARE(server.actions, QList<QString>({"validate", "activate", "validate"}));
        server.replies << qMakePair(200, accepted("deactivate"));
        QVERIFY(first.deactivate()); QTRY_VERIFY(!first.busy()); QVERIFY(!first.active());
        QVERIFY(second.refresh()); QTRY_VERIFY(!second.busy()); QVERIFY(!second.active());
        QCOMPARE(server.actions.size(), 4);
    }

    void disabledConfigurationAndCorruptStorageNeverGrantAccess() {
        Provider server; QTemporaryDir dir;
        License disabled(dir.path(), {}, server.url());
        QVERIFY(!disabled.configured()); QVERIFY(!disabled.activate(key)); QVERIFY(!disabled.refresh());
        QVERIFY(!disabled.active()); QVERIFY(server.actions.isEmpty());
        QFile file(dir.filePath("license.bin")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"valid\":true}"); file.close();
        License corrupt(dir.path(), product, server.url(), TlsIdentity::newKey(), [] { return start; });
        QVERIFY(corrupt.refresh()); QTRY_VERIFY(!corrupt.busy()); QVERIFY(!corrupt.active());
        QVERIFY(server.actions.isEmpty()); QVERIFY(!corrupt.status().isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, License(dir.path(), product, QUrl("http://localhost/licenses/")));
    }

    void certificateErrorsDoNotSendTheLicenseKey() {
        QSslConfiguration::setDefaultConfiguration(original_);
        Provider server; QTemporaryDir dir;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [] { return start; });
        QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy());
        QVERIFY(!license.active()); QVERIFY(server.actions.isEmpty());
        auto fixtureTls = original_; fixtureTls.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslConfiguration::setDefaultConfiguration(fixtureTls);
    }

    void onlineChecksAreDueEveryFifteenMinutesAndBackOffAfterFailures() {
        Provider server; server.acceptActivation(); QTemporaryDir dir; qint64 time = start;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return time; });
        QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy());
        time += 15 * 60000 - 1; QVERIFY(!license.refreshIfDue()); QCOMPARE(server.actions.size(), 2);
        ++time; server.replies << qMakePair(200, accepted("validate"));
        QVERIFY(license.refreshIfDue()); QTRY_VERIFY(!license.busy()); QCOMPARE(server.actions.size(), 3);
        time += 15 * 60000; QVERIFY(license.refreshIfDue()); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        time += 60000 - 1; QVERIFY(!license.refreshIfDue());
        ++time; QVERIFY(license.refreshIfDue()); QTRY_VERIFY(!license.busy());
        time += 60000; QVERIFY(!license.refreshIfDue());
        time += 60000; server.replies << qMakePair(200, accepted("validate"));
        QVERIFY(license.refreshIfDue()); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        time += 2 * 60000; QVERIFY(!license.refreshIfDue());
    }

    void expiryNotifiesTheInterfaceWithoutAnInternetReply() {
        Provider server; QTemporaryDir dir; QElapsedTimer clock; clock.start();
        const auto expiry = QDateTime::fromMSecsSinceEpoch(start + 1500, QTimeZone::UTC).toString(Qt::ISODateWithMs);
        for (const auto* action : {"validate", "activate"}) {
            auto response = accepted(action); auto data = response.value("license_key").toObject();
            data.insert("expires_at", expiry); response.insert("license_key", data);
            server.replies << qMakePair(200, response);
        }
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return start + clock.elapsed(); });
        QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        bool observedExpiry = false;
        connect(&license, &License::changed, &license, [&] { if (!license.active()) observedExpiry = true; });
        QTRY_VERIFY_WITH_TIMEOUT(observedExpiry, 3000); QCOMPARE(server.actions.size(), 2);
    }

    void destructionDuringActivationLeavesARecoverablePendingReceipt() {
        Provider server; server.holdAction = "activate";
        server.replies << qMakePair(200, accepted("validate", {}));
        QTemporaryDir dir; const auto secret = TlsIdentity::newKey();
        auto license = std::make_unique<License>(dir.path(), product, server.url(), secret, [] { return start; });
        QVERIFY(license->activate(key)); QTRY_COMPARE(server.actions.size(), 2); QVERIFY(license->busy());
        license.reset(); QCoreApplication::processEvents();
        License restored(dir.path(), product, server.url(), secret, [] { return start; });
        QVERIFY(restored.refresh()); QTRY_VERIFY(!restored.busy()); QVERIFY(restored.pending());
        QCOMPARE(restored.supportReference(), "Order 50 / License 40"); QCOMPARE(server.actions.size(), 2);
    }

    void expiryStopsExtrasWhileValidationIsUnavailable_data() {
        QTest::addColumn<bool>("storageBusy");
        QTest::newRow("request-pending") << false;
        QTest::newRow("storage-locked") << true;
    }
    void expiryStopsExtrasWhileValidationIsUnavailable() {
        QFETCH(bool, storageBusy);
        Provider server; QTemporaryDir dir; QElapsedTimer clock; clock.start();
        const auto expiry = QDateTime::fromMSecsSinceEpoch(start + 1500, QTimeZone::UTC).toString(Qt::ISODateWithMs);
        for (const auto* action : {"validate", "activate"}) {
            auto response = accepted(action); auto data = response.value("license_key").toObject();
            data.insert("expires_at", expiry); response.insert("license_key", data);
            server.replies << qMakePair(200, response);
        }
        VoiceSession session(dir.filePath("settings.ini"));
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return start + clock.elapsed(); });
        connect(&license, &License::changed, &session, [&] { session.setSupporterEnabled(license.active()); });
        QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy()); QVERIFY(session.supporterEnabled());
        const auto selected = session.avatars().last(); QVERIFY(session.setAvatar(selected));
        QLockFile otherProfile(dir.filePath("license.lock"));
        if (storageBusy) QVERIFY(otherProfile.tryLock(0));
        server.holdAction = "validate"; QCOMPARE(license.refresh(), !storageBusy);
        if (!storageBusy) QTRY_COMPARE(server.actions.size(), 3);
        QTRY_VERIFY_WITH_TIMEOUT(!session.supporterEnabled(), 3000);
        QCOMPARE(license.busy(), !storageBusy); QVERIFY(!license.active()); QVERIFY(session.avatar() != selected);
        QFile saved(dir.filePath("settings.ini")); QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(saved.readAll()).object().value("avatar").toString(), selected);
    }

    void aFailedRevocationWriteCannotRestoreTheStaleOfflineGrant() {
        Provider server; server.acceptActivation(); QTemporaryDir dir;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [] { return start; });
        QVERIFY(license.activate(key)); QTRY_VERIFY(!license.busy()); QVERIFY(license.active());
        const auto path = dir.filePath("license.bin");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto previous = file.readAll(); file.close();
        server.onAction = [&](const QString& action) {
            if (action != "validate") return;
            QVERIFY(QFile::remove(path)); QVERIFY(QDir().mkdir(path));
        };
        server.replies << qMakePair(400, QJsonObject{{"valid", false}});
        QVERIFY(license.refresh()); QTRY_VERIFY(!license.busy()); QVERIFY(!license.active());
        server.onAction = {};
        QVERIFY(QDir().rmdir(path)); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(previous), previous.size()); file.close();
        QVERIFY(license.refresh()); QTRY_VERIFY(!license.busy()); QVERIFY(!license.active());
    }

    void aFailedWriteCannotOverwriteAnotherProfilesRenewal() {
        Provider server; server.acceptActivation(); QTemporaryDir dir;
        const auto secret = TlsIdentity::newKey();
        License first(dir.path(), product, server.url(), secret, [] { return start; });
        QVERIFY(first.activate(key)); QTRY_VERIFY(!first.busy()); QVERIFY(first.active());
        const auto path = dir.filePath("license.bin");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto previous = file.readAll(); file.close();
        server.onAction = [&](const QString&) { QVERIFY(QFile::remove(path)); QVERIFY(QDir().mkdir(path)); };
        server.replies << qMakePair(400, QJsonObject{{"valid", false}});
        QVERIFY(first.refresh()); QTRY_VERIFY(!first.busy()); QVERIFY(!first.active());
        server.onAction = {};
        QVERIFY(QDir().rmdir(path)); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(previous), previous.size()); file.close();
        License second(dir.path(), product, server.url(), secret, [] { return start; });
        for (const auto* action : {"validate", "activate"}) {
            auto response = accepted(action, "renewed-device"); auto data = response.value("license_key").toObject();
            data.insert("key", "renewed-key"); data.insert("id", 41); response.insert("license_key", data);
            server.replies << qMakePair(200, response);
        }
        QVERIFY(second.activate("renewed-key")); QTRY_VERIFY(!second.busy()); QVERIFY(second.active());
        // A later retry from the first profile must not destroy that renewal.
        QVERIFY(first.refresh()); QTRY_VERIFY(!first.busy()); QVERIFY(first.active());
        QCOMPARE(first.supportReference(), "Order 50 / License 41");
        QCOMPARE(server.forms.last().queryItemValue("license_key"), "renewed-key");
        License restarted(dir.path(), product, server.url(), secret, [] { return start; });
        QVERIFY(restarted.refresh()); QTRY_VERIFY(!restarted.busy()); QVERIFY(restarted.active());
        QCOMPARE(restarted.supportReference(), "Order 50 / License 41");
    }

#ifdef Q_OS_LINUX
    void aFreeInstallationDoesNotCreateAKeychainEntryOrContactTheProvider() {
        Provider server; QTemporaryDir dir;
        License license(dir.path(), product, server.url(), {}, [] { return start; });
        QVERIFY(license.refresh()); QTRY_VERIFY(!license.busy()); QVERIFY(!license.active());
        QVERIFY(server.actions.isEmpty()); QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
        QKeychain::ReadPasswordJob read("SquadSpeak"); read.setAutoDelete(false); read.setInsecureFallback(false);
        read.setKey("license/" + QString::fromLatin1(QCryptographicHash::hash(dir.path().toUtf8(), QCryptographicHash::Sha256).toHex()));
        QSignalSpy finished(&read, &QKeychain::Job::finished); read.start();
        QVERIFY(finished.wait(5000)); QCOMPARE(read.error(), QKeychain::EntryNotFound);
    }

    void keychainEncryptionKeySurvivesRestartWithoutAPlaintextFallback() {
        Provider server; server.acceptActivation(); QTemporaryDir dir;
        const auto slot = "license/" + QString::fromLatin1(QCryptographicHash::hash(dir.path().toUtf8(), QCryptographicHash::Sha256).toHex());
        const auto cleanup = qScopeGuard([&] {
            QKeychain::DeletePasswordJob job("SquadSpeak"); job.setAutoDelete(false); job.setKey(slot); job.setInsecureFallback(false);
            QSignalSpy finished(&job, &QKeychain::Job::finished); job.start();
            QVERIFY(finished.wait(5000)); QCOMPARE(job.error(), QKeychain::NoError);
        });
        {
            License license(dir.path(), product, server.url(), {}, [] { return start; });
            QVERIFY(license.activate(key)); QTRY_VERIFY_WITH_TIMEOUT(!license.busy(), 10000);
            QVERIFY2(license.active(), qPrintable(license.status()));
        }
        License restored(dir.path(), product, server.url(), {}, [] { return start; });
        QVERIFY(restored.refresh()); QTRY_VERIFY_WITH_TIMEOUT(!restored.busy(), 10000);
        QVERIFY2(restored.active(), qPrintable(restored.status()));
    }
#endif
#endif
};

QTEST_GUILESS_MAIN(LicenseTests)
#include "license_tests.moc"
