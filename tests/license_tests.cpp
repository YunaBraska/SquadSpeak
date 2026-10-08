#include "license.hpp"
#include "voice_session.hpp"
#include "tls_identity.hpp"
#include "headless.hpp"
#include "local_channel.hpp"
#include "radio_player.hpp"
#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSslServer>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>
#include <QTimeZone>
#include <memory>

namespace {
constexpr qint64 day = 86400000;
const qint64 start = QDateTime::fromString("2026-10-08T12:00:00Z", Qt::ISODate).toMSecsSinceEpoch();
const QJsonObject product{{"client_id", "fixture-client"}, {"recipient_id", 100},
    {"tier_id", "ST_fixture"}, {"owner", "fixture-owner"}, {"repository", "fixture-app"}};

// A stateful GitHub HTTPS boundary. Tests change its payment ledger and clock,
// then drive the same sign-in/check methods used by QML and the terminal.
class GitHub final : public QSslServer {
public:
    QJsonArray ledger;
    QString contributorFile;
    qint64 viewerId = 200;
    QString login = "fixture-member";
    QString accessToken = "fixture-access-token";
    QStringList paths, failures, pollErrors;
    QList<QJsonObject> requests;
    bool authorized = false, expiringTokens = false;
    QString refreshToken = "fixture-refresh-token";
    int rotations = 0;
    int status = 200, pageSize = 2;
    QString holdPath, unauthorizedCursor;
    bool unauthorizedSent = false;
    QByteArray extraHeaders, bodyOverride;
    QString verificationOverride;
    std::function<void(QJsonObject&)> alter;
    std::function<void()> beforeResponse;
    GitHub() {
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
                    const auto path = QString::fromLatin1(data->split(' ').at(1));
                    const auto input = QJsonDocument::fromJson(data->mid(end + 4)).object();
                    paths << path; requests << input;
                    if (path == holdPath) return;
                    auto response = route(path, input, data->first(end));
                    int responseStatus = status;
                    if (path == "/graphql" && !unauthorizedCursor.isEmpty() && !unauthorizedSent
                        && input.value("variables").toObject().value("after") == unauthorizedCursor) {
                        responseStatus = 401; unauthorizedSent = true;
                    }
                    if (alter && path == "/graphql") alter(response);
                    if (beforeResponse && path == "/graphql") beforeResponse();
                    const auto body = bodyOverride.isEmpty() ? QJsonDocument(response).toJson(QJsonDocument::Compact) : bodyOverride;
                    socket->write("HTTP/1.1 " + QByteArray::number(responseStatus) + " Result\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n" + extraHeaders + "Connection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                };
                connect(socket, &QSslSocket::readyRead, this, read);
                read();
            }
        });
        if (!listen(QHostAddress::LocalHost)) throw std::runtime_error("GitHub fixture could not listen.");
    }
    QUrl oauth() const { return QUrl(QString("https://127.0.0.1:%1").arg(serverPort())); }
    QUrl url() const { return oauth().resolved(QUrl("/graphql")); }
    void payment(QString id, qint64 timestamp, QString action = "NEW_SPONSORSHIP", QString tier = "ST_fixture", qint64 recipient = 100, qint64 payer = 200) {
        ledger.append(QJsonObject{{"id", id}, {"action", action},
            {"timestamp", QDateTime::fromMSecsSinceEpoch(timestamp, QTimeZone::UTC).toString(Qt::ISODateWithMs)},
            {"sponsor", QJsonObject{{"databaseId", payer}}}, {"sponsorable", QJsonObject{{"databaseId", recipient}}},
            {"sponsorsTier", QJsonObject{{"id", tier}, {"isOneTime", true}, {"monthlyPriceInCents", 1200}}}});
    }
private:
    QJsonObject tokenResponse() const {
        QJsonObject result{{"access_token", accessToken}, {"scope", "read:user"}, {"token_type", "bearer"}};
        if (expiringTokens) {
            result.insert("refresh_token", refreshToken); result.insert("expires_in", 28800);
            result.insert("refresh_token_expires_in", 15897600);
        }
        return result;
    }
    QJsonObject route(const QString& path, const QJsonObject& input, const QByteArray& headers) {
        if (path == "/login/device/code") {
            if (input != QJsonObject{{"client_id", "fixture-client"}, {"scope", "read:user"}}) failures << "device request";
            return {{"device_code", "fixture-device-secret"}, {"user_code", "ABCD-EFGH"},
                {"verification_uri", verificationOverride.isEmpty() ? oauth().resolved(QUrl("/login/device")).toString() : verificationOverride}, {"interval", 5}, {"expires_in", 900}};
        }
        if (path == "/login/oauth/access_token") {
            if (input.value("grant_type") == "refresh_token") {
                if (input != QJsonObject{{"client_id", "fixture-client"}, {"refresh_token", refreshToken}, {"grant_type", "refresh_token"}})
                    return {{"error", "bad_refresh_token"}};
                if (status != 200) return {};
                ++rotations;
                accessToken = QString("rotated-access-%1").arg(rotations);
                refreshToken = QString("rotated-refresh-%1").arg(rotations);
                return tokenResponse();
            }
            if (input != QJsonObject{{"client_id", "fixture-client"}, {"device_code", "fixture-device-secret"},
                {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}) failures << "token request";
            if (!pollErrors.isEmpty()) return {{"error", pollErrors.takeFirst()}};
            if (!authorized) return {{"error", "authorization_pending"}};
            return tokenResponse();
        }
        if (path != "/graphql") { failures << "unknown endpoint"; return {}; }
        if (!headers.contains("Authorization: Bearer " + accessToken.toUtf8())) failures << "missing bearer";
        const auto query = input.value("query").toString();
        if (!query.contains("includeAsSponsor:true") || !query.contains("includePrivate:true")
            || !query.contains("actions:[NEW_SPONSORSHIP,REFUND]") || !query.contains("HEAD:CONTRIBUTORS.md")) failures << "query contract";
        const auto variables = input.value("variables").toObject();
        if (variables.value("owner") != "fixture-owner" || variables.value("repository") != "fixture-app") failures << "wrong repository";
        const auto offset = variables.value("after").toString().toInt();
        QJsonArray nodes;
        for (int i = offset; i < ledger.size() && i < offset + pageSize; ++i) nodes.append(ledger[i]);
        return {{"data", QJsonObject{{"viewer", QJsonObject{{"databaseId", viewerId}, {"login", login},
            {"sponsorsActivities", QJsonObject{{"nodes", nodes}, {"pageInfo", QJsonObject{
                {"hasNextPage", offset + pageSize < ledger.size()}, {"endCursor", QString::number(offset + pageSize)}}}}}}},
            {"repository", QJsonObject{{"object", contributorFile.isEmpty() ? QJsonValue(QJsonValue::Null)
                : QJsonValue(QJsonObject{{"text", contributorFile}, {"isTruncated", false}})}}}}}};
    }
};
void advanceAuthorization(License& license, qint64& time) {
    QTRY_VERIFY(license.pending());
    time += 5000;
    QVERIFY(license.refreshIfDue());
    QTRY_VERIFY_WITH_TIMEOUT(!license.busy(), 5000);
}
void checked(License& license) {
    QVERIFY(license.refresh());
    QTRY_VERIFY_WITH_TIMEOUT(!license.busy(), 5000);
}
}

class LicenseTests final : public QObject {
    Q_OBJECT
    QSslConfiguration original_;
private slots:
    void distributionBoundary() {
        QTemporaryDir dir;
        License license(dir.path(), product, QUrl("https://api.github.com/graphql"));
#if SQUADSPEAK_STORE_BUILD
        QVERIFY(!license.configured()); QVERIFY(!license.signIn()); QVERIFY(!license.signOut());
        QVERIFY(!license.refresh()); QVERIFY(!license.active());
        QVERIFY(License::distributionProduct().isEmpty()); QVERIFY(License::purchaseUrl().isEmpty());
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
#else
        QVERIFY(license.configured()); QVERIFY(License::directDistribution());
        QCOMPARE(License::purchaseUrl().host(), "github.com");
#endif
    }
#if !SQUADSPEAK_STORE_BUILD
    void initTestCase() {
        original_ = QSslConfiguration::defaultConfiguration();
        // Isolated loopback fixture only. Production has no TLS bypass. A test
        // below restores certificate verification and proves rejection.
        auto fixtureTls = original_; fixtureTls.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslConfiguration::setDefaultConfiguration(fixtureTls);
    }
    void cleanupTestCase() { QSslConfiguration::setDefaultConfiguration(original_); }

    void multiplePaymentsThroughHeadlessUnlockRenewRevokeAndRestore() {
        GitHub server; QTemporaryDir dir; qint64 time = start;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return time; }, {}, nullptr, server.oauth());
        VoiceSession session;
        LocalChannel host(session, dir.filePath("host.channel"), TlsIdentity::create());
        QVERIFY(host.listen(QHostAddress::LocalHost)); QVERIFY(host.initialize(true));
        connect(&license, &License::changed, &session, [&] { session.setSupporterEnabled(license.active()); });
        RadioPlayer radio(dir.filePath("radio.json"));
        QBuffer input, output; QVERIFY(input.open(QIODevice::ReadWrite)); QVERIFY(output.open(QIODevice::ReadWrite));
        HeadlessController cli(*QCoreApplication::instance(), host, radio, license, input, output, false);
        const auto run = [&](const QJsonObject& request) {
            output.buffer().clear(); output.seek(0); cli.process(request);
            return QJsonDocument::fromJson(output.data().trimmed()).object();
        };
        QVERIFY(!run({{"command", "configure"}, {"values", QJsonObject{{"messageLifetimeDays", 90}}}}).value("ok").toBool());
        run({{"command", "license"}, {"action", "sign-in"}});
        QTRY_VERIFY(!output.data().isEmpty());
        const auto prompt = QJsonDocument::fromJson(output.data().trimmed()).object();
        QVERIFY(prompt.value("ok").toBool());
        QCOMPARE(prompt.value("data").toObject().value("userCode"), "ABCD-EFGH");
        QVERIFY(!output.data().contains("fixture-device-secret"));
        server.authorized = true;
        advanceAuthorization(license, time);
        QVERIFY(!license.active()); QCOMPARE(license.account(), "fixture-member");
        server.payment("expired", start - 500 * day);
        server.payment("wrong-tier", start, "NEW_SPONSORSHIP", "ST_other");
        server.payment("wrong-recipient", start, "NEW_SPONSORSHIP", "ST_fixture", 101);
        server.payment("wrong-payer", start, "NEW_SPONSORSHIP", "ST_fixture", 100, 201);
        server.payment("first-payment", start);
        run({{"command", "license"}, {"action", "refresh"}});
        QTRY_VERIFY(!license.busy()); QVERIFY2(license.active(), qPrintable(license.status()));
        const auto firstExpiry = QDateTime::fromMSecsSinceEpoch(start, QTimeZone::UTC).addYears(1);
        QCOMPARE(license.expiresAt(), firstExpiry);
        for (const auto days : {90, 180, 360})
            QVERIFY(run({{"command", "configure"}, {"values", QJsonObject{{"messageLifetimeDays", days}}}}).value("ok").toBool());
        time += 100 * day;
        checked(license); QCOMPARE(license.expiresAt(), firstExpiry);
        server.payment("second-payment", time);
        checked(license); QVERIFY(license.active());
        QCOMPARE(license.expiresAt(), QDateTime::fromMSecsSinceEpoch(time, QTimeZone::UTC).addYears(1));
        const auto secondExpiry = license.expiresAt();
        checked(license); QCOMPARE(license.expiresAt(), secondExpiry);
        time += day; server.payment("refund", time, "REFUND");
        checked(license); QVERIFY(!license.active()); QVERIFY(!session.supporterEnabled());
        QCOMPARE(host.messageLifetimeDays(), 30);
        QVERIFY(!run({{"command", "configure"}, {"values", QJsonObject{{"messageLifetimeDays", 90}}}}).value("ok").toBool());
        time += day; server.payment("third-payment", time);
        checked(license); QVERIFY(license.active());
        QVERIFY(!host.joined()); QVERIFY(host.hostParticipants().isEmpty());
        const auto status = run({{"command", "license"}, {"action", "status"}});
        QCOMPARE(status.value("data").toObject().value("account"), "fixture-member");
        QVERIFY(!output.data().contains(server.accessToken.toUtf8()));
        QVERIFY(run({{"command", "license"}, {"action", "sign-out"}}).value("ok").toBool());
        QVERIFY(!license.active()); QVERIFY(license.account().isEmpty());
        run({{"command", "license"}, {"action", "sign-in"}});
        QTRY_VERIFY(license.pending());
        QVERIFY(run({{"command", "license"}, {"action", "cancel"}}).value("ok").toBool());
        QVERIFY(!license.pending()); QVERIFY(!license.busy());
        QVERIFY2(server.failures.isEmpty(), qPrintable(server.failures.join('\n')));
    }

    void encryptedSharedReceiptOfflineDeadlineAndDailySchedule() {
        GitHub server; server.authorized = true; server.payment("paid", start);
        QTemporaryDir dir; const auto secret = TlsIdentity::newKey(); qint64 time = start;
        qint64 checkedAt;
        {
            License license(dir.path(), product, server.url(), secret, [&] { return time; }, {}, nullptr, server.oauth());
            QVERIFY(license.signIn()); advanceAuthorization(license, time); QVERIFY(license.active());
            checkedAt = time;
            const auto count = server.paths.size();
            for (int i = 0; i < 20; ++i) QVERIFY(!license.refreshIfDue());
            QCOMPARE(server.paths.size(), count);
            time += day - 1; QVERIFY(!license.refreshIfDue()); QCOMPARE(server.paths.size(), count);
            time += 1; QVERIFY(license.refreshIfDue()); QTRY_VERIFY(!license.busy());
            QCOMPARE(server.paths.size(), count + 1); checkedAt = time;
            QFile file(dir.filePath("supporter.bin")); QVERIFY(file.open(QIODevice::ReadOnly));
            const auto bytes = file.readAll();
            QVERIFY(!bytes.contains(server.accessToken.toUtf8())); QVERIFY(!bytes.contains("fixture-member"));
            QCOMPARE(file.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther), QFileDevice::Permissions{});
            server.status = 503;
        }
        License restored(dir.path(), product, server.url(), secret, [&] { return time; }, {}, nullptr, server.oauth());
        checked(restored); QVERIFY(restored.active()); QVERIFY(!restored.status().isEmpty());
        time = checkedAt + 7 * day - 1; checked(restored); QVERIFY(restored.active());
        time += 1; QVERIFY(!restored.active()); checked(restored); QVERIFY(!restored.active());
        server.status = 200; checked(restored); QVERIFY(restored.active());
        time = restored.expiresAt().toMSecsSinceEpoch(); QVERIFY(!restored.active());
    }

    void malformedAndPartialPagesNeverExtendOfflineAccess_data() {
        QTest::addColumn<QString>("failure");
        for (const auto* value : {"invalid-json", "oversized", "graphql-error", "null-nodes", "null-viewer", "missing-page", "truncated-contributors", "cursor-cycle", "conflicting-event", "future-payment", "wrong-price", "monthly"})
            QTest::newRow(value) << QString(value);
    }
    void malformedAndPartialPagesNeverExtendOfflineAccess() {
        QFETCH(QString, failure);
        GitHub server; server.authorized = true; server.payment("paid", start);
        QTemporaryDir dir; qint64 time = start;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return time; }, {}, nullptr, server.oauth());
        QVERIFY(license.signIn()); advanceAuthorization(license, time); QVERIFY(license.active());
        const auto confirmed = time;
        if (failure == "invalid-json") server.bodyOverride = "broken";
        if (failure == "oversized") server.bodyOverride = QByteArray(1024 * 1024 + 1, ' ');
        server.alter = [failure](QJsonObject& response) {
            auto data = response.value("data").toObject(); auto viewer = data.value("viewer").toObject();
            auto activity = viewer.value("sponsorsActivities").toObject();
            auto nodes = activity.value("nodes").toArray(); auto event = nodes.first().toObject();
            if (failure == "graphql-error") response.insert("errors", QJsonArray{QJsonObject{{"message", "limited"}}});
            if (failure == "null-nodes") activity.insert("nodes", QJsonArray{QJsonValue::Null});
            if (failure == "missing-page") activity.remove("pageInfo");
            if (failure == "cursor-cycle") activity.insert("pageInfo", QJsonObject{{"hasNextPage", true}, {"endCursor", "0"}});
            if (failure == "truncated-contributors") data.insert("repository", QJsonObject{{"object", QJsonObject{{"text", "| 200 | test |"}, {"isTruncated", true}}}});
            if (failure == "conflicting-event") { event.insert("timestamp", "2026-10-08T11:59:00Z"); nodes.append(event); activity.insert("nodes", nodes); }
            if (failure == "future-payment") { event.insert("timestamp", "2099-01-01T00:00:00Z"); activity.insert("nodes", QJsonArray{event}); }
            if (failure == "wrong-price" || failure == "monthly") {
                auto tier = event.value("sponsorsTier").toObject();
                tier.insert(failure == "monthly" ? "isOneTime" : "monthlyPriceInCents", failure == "monthly" ? QJsonValue(false) : QJsonValue(100));
                event.insert("sponsorsTier", tier); activity.insert("nodes", QJsonArray{event});
            }
            viewer.insert("sponsorsActivities", activity); data.insert("viewer", failure == "null-viewer" ? QJsonValue(QJsonValue::Null) : QJsonValue(viewer)); response.insert("data", data);
        };
        time += 6 * day; checked(license); QVERIFY(license.active()); QVERIFY(!license.status().isEmpty());
        time = confirmed + 7 * day; checked(license); QVERIFY(!license.active());
    }

    void pollingSlowDownCancellationExpiryAndDenial() {
        GitHub server; QTemporaryDir dir; qint64 time = start;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return time; }, {}, nullptr, server.oauth());
        QSignalSpy code(&license, &License::authorizationReady);
        QVERIFY(license.signIn()); QTRY_COMPARE(code.count(), 1); QVERIFY(license.pending());
        QCOMPARE(license.verificationUrl(), server.oauth().resolved(QUrl("/login/device")));
        QVERIFY(!license.refresh()); QVERIFY(!license.signIn());
        server.pollErrors << "authorization_pending" << "slow_down";
        QSignalSpy polled(&license, &License::changed);
        time += 5000; QVERIFY(license.refreshIfDue()); QTRY_VERIFY(polled.count() >= 2);
        QTRY_VERIFY(server.pollErrors.size() == 1);
        polled.clear();
        time += 5000; QVERIFY(license.refreshIfDue()); QTRY_VERIFY(polled.count() >= 2);
        QTRY_VERIFY(server.pollErrors.isEmpty());
        time += 5000; QVERIFY(!license.refreshIfDue()); QCOMPARE(server.paths.size(), 3);
        QVERIFY(license.cancelSignIn()); QVERIFY(!license.pending()); QVERIFY(!license.busy());
        QVERIFY(license.userCode().isEmpty()); QVERIFY(!license.cancelSignIn());
        QVERIFY(license.signIn()); QTRY_COMPARE(code.count(), 2);
        time += 900000; license.refreshIfDue(); QVERIFY(!license.pending()); QVERIFY(!license.active());
        QVERIFY(!license.status().isEmpty());
        server.pollErrors << "access_denied";
        QVERIFY(license.signIn()); advanceAuthorization(license, time);
        QVERIFY(!license.active()); QVERIFY(!license.status().isEmpty());
    }

    void tokenRotationPreservesPaymentDeadlineAndIsSharedAcrossProcesses() {
        GitHub server; server.authorized = true; server.expiringTokens = true; server.payment("paid", start);
        QTemporaryDir dir; qint64 time = start; const auto secret = TlsIdentity::newKey();
        License first(dir.path(), product, server.url(), secret, [&] { return time; }, {}, nullptr, server.oauth());
        License second(dir.path(), product, server.url(), secret, [&] { return time; }, {}, nullptr, server.oauth());
        QVERIFY(first.signIn()); advanceAuthorization(first, time); QVERIFY(first.active());
        const auto expiry = first.expiresAt();
        time += day; checked(second); QVERIFY(second.active()); QCOMPARE(server.rotations, 1);
        checked(first); QVERIFY(first.active()); QCOMPARE(server.rotations, 1); QCOMPARE(first.expiresAt(), expiry);
        time += day; server.status = 503; checked(first); QVERIFY(first.active()); QCOMPARE(server.rotations, 1);
        server.status = 200; checked(first); QVERIFY(first.active()); QCOMPARE(server.rotations, 2);
        server.refreshToken = "revoked"; time += day; checked(second); QVERIFY(!second.active());
        checked(first); QVERIFY(!first.active());
        QVERIFY(server.failures.isEmpty());
    }

    void tokenExpiryDuringPaginationRestartsOneConsistentRead() {
        GitHub server; server.authorized = true; server.expiringTokens = true;
        for (int i = 0; i < 5; ++i) server.payment(QString::number(i), start - i * day);
        QTemporaryDir dir; qint64 time = start;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return time; }, {}, nullptr, server.oauth());
        QVERIFY(license.signIn()); advanceAuthorization(license, time); QVERIFY(license.active());
        server.unauthorizedCursor = "2";
        checked(license);
        QVERIFY(server.unauthorizedSent); QCOMPARE(server.rotations, 1);
        QVERIFY2(license.status().isEmpty(), qPrintable(license.status()));
        QCOMPARE(license.expiresAt(), QDateTime::fromMSecsSinceEpoch(start, QTimeZone::UTC).addYears(1));
        QVERIFY(server.failures.isEmpty());
    }

    void accountChangeAndUnauthorizedInvalidateCachedEntitlement() {
        GitHub server; server.authorized = true; server.payment("paid", start);
        QTemporaryDir dir; qint64 time = start;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return time; }, {}, nullptr, server.oauth());
        QVERIFY(license.signIn()); advanceAuthorization(license, time); QVERIFY(license.active());
        server.viewerId = 201; checked(license); QVERIFY(!license.active());
        server.login = "other-member";
        QVERIFY(license.signIn()); advanceAuthorization(license, time); QVERIFY(!license.active());
        QCOMPARE(license.account(), "other-member");
        server.payment("other-payment", time, "NEW_SPONSORSHIP", "ST_fixture", 100, 201);
        checked(license); QVERIFY(license.active());
        server.status = 401; checked(license); QVERIFY(!license.active());
        server.status = 503; checked(license); QVERIFY(!license.active());
    }

    void rateLimitAndClockRollbackCannotRefreshTheGrant() {
        GitHub server; server.authorized = true; server.payment("paid", start);
        QTemporaryDir dir; qint64 time = start;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return time; }, {}, nullptr, server.oauth());
        QVERIFY(license.signIn()); advanceAuthorization(license, time); QVERIFY(license.active());
        const auto confirmed = time;
        server.status = 429; server.extraHeaders = "Retry-After: 120\r\n";
        checked(license); QVERIFY(license.active()); const auto requests = server.paths.size();
        time += 119000; QVERIFY(!license.refresh()); QCOMPARE(server.paths.size(), requests);
        time += 1000; server.status = 200; checked(license); QVERIFY(license.active());
        server.status = 403; server.extraHeaders = "X-RateLimit-Remaining: 0\r\nX-RateLimit-Reset: 9223372036854775807\r\n";
        checked(license);
        time += day - 1; QVERIFY(!license.refresh());
        time += 1; server.status = 200; checked(license); QVERIFY(license.active());
        time = confirmed - 1; QVERIFY(!license.active());
    }

    void moreThanThreeDevicesAndDuplicateEventsRemainIndependent() {
        GitHub server; server.authorized = true; server.payment("paid", start);
        server.ledger.append(server.ledger.first());
        QTemporaryDir dir; qint64 time = start;
        for (int i = 0; i < 4; ++i) {
            License license(dir.filePath(QString::number(i)), product, server.url(), TlsIdentity::newKey(), [&] { return time; }, {}, nullptr, server.oauth());
            QVERIFY(license.signIn()); advanceAuthorization(license, time); QVERIFY(license.active());
            QCOMPARE(license.expiresAt(), QDateTime::fromMSecsSinceEpoch(start, QTimeZone::UTC).addYears(1));
        }
        QCOMPARE(server.paths.count("/login/device/code"), 4);
        QVERIFY(server.failures.isEmpty());
    }

    void contributorUsesStableIdAndExpiresAfterSevenDays() {
        GitHub server; server.authorized = true;
        server.contributorFile = "# Contributors\n\n| GitHub ID | Handle |\n| --- | --- |\n| 200 | fixture-member |\n";
        QTemporaryDir dir; qint64 time = start;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return time; }, {}, nullptr, server.oauth());
        QVERIFY(license.signIn()); advanceAuthorization(license, time); QVERIFY(license.active());
        server.login = "renamed-member"; checked(license); QVERIFY(license.active());
        QCOMPARE(license.account(), "renamed-member");
        server.status = 503; time += 7 * day; checked(license); QVERIFY(!license.active());
        server.status = 200; checked(license); QVERIFY(license.active());
        server.contributorFile = "| 201 | renamed-member |\nText 200 is not a grant.\n";
        checked(license); QVERIFY(!license.active());
    }

    void sharedIdentityFileAndConcurrentOperations() {
        GitHub server; server.authorized = true; server.payment("paid", start);
        QTemporaryDir dir; qint64 time = start;
        const auto identity = dir.filePath("identity.pem");
        const auto generated = TlsIdentity::loadFile(identity, true); QVERIFY(!generated.id().isEmpty());
        License first(dir.path(), product, server.url(), {}, [&] { return time; }, identity, nullptr, server.oauth());
        License second(dir.path(), product, server.url(), {}, [&] { return time; }, {}, nullptr, server.oauth());
        QVERIFY(first.signIn()); QTRY_VERIFY(first.pending());
        QVERIFY(!second.signIn()); QVERIFY(!second.active());
        advanceAuthorization(first, time); QVERIFY(first.active());
        server.status = 503;
        checked(second); QVERIFY2(second.active(), qPrintable(second.status()));
        QVERIFY(second.signOut()); QVERIFY(!second.active());
        checked(first); QVERIFY(!first.active());
        const auto requests = server.paths.size();
        checked(first); QCOMPARE(server.paths.size(), requests);
    }

    void failedRevocationWriteCannotResurrectOldGrant() {
        GitHub server; server.authorized = true; server.payment("paid", start);
        QTemporaryDir dir; qint64 time = start; const auto secret = TlsIdentity::newKey();
        License license(dir.path(), product, server.url(), secret, [&] { return time; }, {}, nullptr, server.oauth());
        QVERIFY(license.signIn()); advanceAuthorization(license, time); QVERIFY(license.active());
        const auto path = dir.filePath("supporter.bin"); QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly)); const auto previous = file.readAll(); file.close();
        server.beforeResponse = [&] { QVERIFY(QFile::remove(path)); QVERIFY(QDir().mkdir(path)); };
        server.payment("refund", time, "REFUND"); checked(license); QVERIFY(!license.active());
        QVERIFY(!license.status().isEmpty()); server.beforeResponse = {};
        QVERIFY(QDir().rmdir(path)); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(previous), previous.size()); file.close();
        server.status = 503; checked(license); QVERIFY(!license.active());
        License restarted(dir.path(), product, server.url(), secret, [&] { return time; }, {}, nullptr, server.oauth());
        checked(restarted); QVERIFY(!restarted.active());
    }

    void corruptCacheWrongKeyAndUntrustedTlsFailClosed() {
        GitHub server; server.authorized = true; server.payment("paid", start);
        QTemporaryDir dir; qint64 time = start; const auto secret = TlsIdentity::newKey();
        {
            License license(dir.path(), product, server.url(), secret, [&] { return time; }, {}, nullptr, server.oauth());
            QVERIFY(license.signIn()); advanceAuthorization(license, time); QVERIFY(license.active());
        }
        const auto requests = server.paths.size();
        License wrongKey(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return time; }, {}, nullptr, server.oauth());
        checked(wrongKey); QVERIFY(!wrongKey.active()); QCOMPARE(server.paths.size(), requests);
        QFile file(dir.filePath("supporter.bin")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("broken"); file.close();
        License broken(dir.path(), product, server.url(), secret, [&] { return time; }, {}, nullptr, server.oauth());
        checked(broken); QVERIFY(!broken.active()); QCOMPARE(server.paths.size(), requests);
        QSslConfiguration::setDefaultConfiguration(original_);
        QTemporaryDir other;
        License tls(other.path(), product, server.url(), secret, [&] { return time; }, {}, nullptr, server.oauth());
        QVERIFY(tls.signIn()); QTRY_VERIFY(!tls.busy()); QVERIFY(!tls.pending()); QVERIFY(!tls.active());
        QCOMPARE(server.paths.size(), requests);
        auto fixtureTls = original_; fixtureTls.setPeerVerifyMode(QSslSocket::VerifyNone); QSslConfiguration::setDefaultConfiguration(fixtureTls);
    }

    void authorizationNeverFollowsRedirectsOrOpensUntrustedVerificationSites() {
        GitHub server, other; QTemporaryDir dir; qint64 time = start;
        License license(dir.path(), product, server.url(), TlsIdentity::newKey(), [&] { return time; }, {}, nullptr, server.oauth());
        server.verificationOverride = "https://example.invalid/login/device";
        QVERIFY(license.signIn()); QTRY_VERIFY(!license.busy());
        QVERIFY(!license.pending()); QVERIFY(license.verificationUrl().isEmpty());
        server.verificationOverride.clear(); server.status = 302;
        server.extraHeaders = "Location: " + other.url().toEncoded() + "\r\n";
        QVERIFY(license.signIn()); QTRY_VERIFY(!license.busy()); QVERIFY(!license.active());
        QVERIFY(other.paths.isEmpty()); QVERIFY(license.verificationUrl().isEmpty());
    }

    void freeStartupDoesNotTouchKeychainOrNetworkAndInvalidConfigurationIsRejected() {
        GitHub server; QTemporaryDir dir;
        License license(dir.path(), product, server.url());
        checked(license); QVERIFY(!license.active()); QVERIFY(server.paths.isEmpty());
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, License(dir.path(), product, QUrl("http://localhost/graphql")));
        auto invalid = product; invalid.insert("recipient_id", -1);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, License(dir.path(), invalid, server.url()));
    }
#endif
};
QTEST_GUILESS_MAIN(LicenseTests)
#include "license_tests.moc"
