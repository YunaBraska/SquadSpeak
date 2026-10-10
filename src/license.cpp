#include "license.hpp"
#include "tls_identity.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>
#include <qtkeychain/keychain.h>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace {
constexpr qint64 day = 86400000;
constexpr qint64 checkInterval = day;
constexpr qsizetype maxResponse = 1024 * 1024;
const QByteArray receiptContext("yuna/supporter/1");

bool positiveId(const QJsonValue& value) {
    return value.isDouble() && value.toInteger() > 0 && double(value.toInteger()) == value.toDouble();
}
bool https(const QUrl& url) {
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty()
        && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}
bool token(const QString& value) {
    return !value.isEmpty() && value.size() <= 4096
        && std::all_of(value.cbegin(), value.cend(), [](QChar c) { return c.unicode() > 32 && c.unicode() < 127; });
}
qint64 utcTimestamp(const QJsonValue& value) {
    const auto text = value.toString();
    const auto date = QDateTime::fromString(text, Qt::ISODateWithMs);
    return date.isValid() && text.endsWith('Z') ? date.toMSecsSinceEpoch() : 0;
}
}

QJsonObject License::distributionProduct() {
    if (!directDistribution() || QStringLiteral(SQUADSPEAK_GITHUB_CLIENT_ID).isEmpty()) return {};
    return {{"client_id", QStringLiteral(SQUADSPEAK_GITHUB_CLIENT_ID)}, {"recipient_id", 13748223},
        {"tier_id", "ST_kwDOANHH_84ACiYG"}, {"owner", "YunaBraska"}, {"repository", "SquadSpeak"}};
}
QString License::storageDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/YunaSupporter";
}
bool License::directDistribution() { return !SQUADSPEAK_STORE_BUILD; }
QUrl License::purchaseUrl() {
    return directDistribution() ? QUrl("https://github.com/sponsors/YunaBraska/sponsorships?tier_id=665094") : QUrl{};
}
QJsonObject License::policy() const {
    auto result = product_;
    result.remove("client_id");
    return result;
}
License::License(QString directory, QJsonObject product, QUrl endpoint, QByteArray storageKey,
                 std::function<qint64()> clock, QString identityFile, QObject* parent, QUrl oauthEndpoint)
    : QObject(parent), directory_(QFileInfo(directory).absoluteFilePath()), product_(std::move(product)),
      endpoint_(std::move(endpoint)), oauthEndpoint_(std::move(oauthEndpoint)), storageKey_(std::move(storageKey)),
      identityFile_(std::move(identityFile)), suppliedStorageKey_(storageKey_), clock_(std::move(clock)),
      lock_(directory_ + "/supporter.lock"), network_(this) {
    if (!product_.isEmpty()) {
        if (!positiveId(product_.value("recipient_id"))) throw std::invalid_argument("Invalid supporter recipient.");
        for (const auto* field : {"client_id", "tier_id", "owner", "repository"})
            if (!token(product_.value(field).toString())) throw std::invalid_argument("Invalid supporter configuration.");
        if (!https(endpoint_) || !https(oauthEndpoint_) || !oauthEndpoint_.path().isEmpty())
            throw std::invalid_argument("Supporter endpoints must use HTTPS.");
    }
    if (!storageKey_.isEmpty() && storageKey_.size() != 32) throw std::invalid_argument("Invalid supporter storage key.");
    lock_.setStaleLockTime(0);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &License::refreshIfDue);
}
License::~License() {
    timer_.stop();
    for (auto* reply : network_.findChildren<QNetworkReply*>()) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
    }
}
qint64 License::now() const { return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch(); }
qint64 License::usableUntil() const {
    return std::min(record_.value("expires").toInteger(), record_.value("checked").toInteger() + 7 * day);
}
bool License::active() const {
    const auto checked = record_.value("checked").toInteger();
    return configured() && storageHealthy_ && record_.value("valid").toBool()
        && checked > 0 && now() >= checked && now() < usableUntil();
}
QDateTime License::expiresAt() const {
    const auto expiry = record_.value("expires").toInteger();
    return expiry > 0 ? QDateTime::fromMSecsSinceEpoch(expiry, QTimeZone::UTC) : QDateTime{};
}
bool License::signIn() { return begin(Action::SignIn); }
bool License::signOut() { return begin(Action::SignOut); }
bool License::refresh() { return begin(Action::Check); }
bool License::cancelSignIn() {
    if (!pending()) return false;
    ++generation_;
    for (auto* reply : network_.findChildren<QNetworkReply*>()) reply->abort();
    finish({}); return true;
}
bool License::refreshIfDue() {
    if (!configured()) return false;
    emit changed();
    if (pending()) {
        if (now() >= authorizationExpires_) { cancelSignIn(); status_ = tr("Sign-in expired. Try again."); emit changed(); return false; }
        if (now() >= nextPoll_ && network_.findChildren<QNetworkReply*>().isEmpty()) { pollAuthorization(); return true; }
    } else if (!busy_ && now() >= nextCheck_) return refresh();
    schedule(); return false;
}

bool License::begin(Action action) {
    if (!configured() || busy_ || (action == Action::Check && now() < retryAfter_)) return false;
    if (!QDir().mkpath(directory_) || !lock_.tryLock(0)) {
        status_ = tr("License storage is busy or unavailable.");
        emit changed();
        nextCheck_ = now() + 60000; schedule();
        return false;
    }
    events_.clear(); cursors_.clear(); viewer_ = {}; contributor_ = false; rotated_ = false;
    busy_ = true; status_.clear(); emit changed(); schedule();
    if (action != Action::SignIn && record_.isEmpty() && !QFileInfo::exists(directory_ + "/supporter.bin")) {
        finish({}); return true;
    }
    QByteArray sealed;
    receiptIdentityFile_.clear();
    try {
        QFile file(directory_ + "/supporter.bin");
        if (file.exists()) {
            if (!file.open(QIODevice::ReadOnly) || file.size() > maxResponse)
                throw std::runtime_error("Invalid supporter record.");
            sealed = file.read(maxResponse + 1);
            if (file.error() != QFileDevice::NoError || sealed.isEmpty() || sealed.size() > maxResponse)
                throw std::runtime_error("Invalid supporter record.");
            const auto document = QJsonDocument::fromJson(sealed);
            if (document.isObject()) {
                const auto envelope = document.object();
                const auto path = envelope.value("identityFile").toString();
                const auto payload = QByteArray::fromBase64Encoding(envelope.value("sealed").toString().toLatin1(),
                    QByteArray::AbortOnBase64DecodingErrors);
                if (path.isEmpty() || !QDir::isAbsolutePath(path) || !payload || payload.decoded.isEmpty())
                    throw std::runtime_error("Invalid supporter record.");
                // Another server profile may have its own host key. The first
                // sign-in's account binding remains authoritative.
                receiptIdentityFile_ = path;
                sealed = payload.decoded;
            }
        } else receiptIdentityFile_ = identityFile_;
        if (!receiptIdentityFile_.isEmpty()) {
            const auto identity = TlsIdentity::loadFile(receiptIdentityFile_, false);
            receiptIdentityFile_ = QFileInfo(receiptIdentityFile_).canonicalFilePath();
            storageKey_ = identity.deriveKey(directory_.toUtf8(), receiptContext);
            read(action, sealed); return true;
        }
    } catch (const std::exception&) {
        storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return true;
    }
    if (!suppliedStorageKey_.isEmpty()) { storageKey_ = suppliedStorageKey_; read(action, sealed); return true; }
    storageKey_.clear();
    auto* job = new QKeychain::ReadPasswordJob("YunaSupporter", this);
    const auto slot = "supporter/" + QString::fromLatin1(QCryptographicHash::hash(directory_.toUtf8(), QCryptographicHash::Sha256).toHex());
    job->setKey(slot); job->setInsecureFallback(false);
    connect(job, &QKeychain::Job::finished, this, [this, job, slot, action, sealed] {
        if (job->error() == QKeychain::NoError && job->binaryData().size() == 32) {
            storageKey_ = job->binaryData(); read(action, sealed); return;
        }
        if (job->error() != QKeychain::EntryNotFound || QFileInfo::exists(directory_ + "/supporter.bin")) {
            storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return;
        }
        QByteArray generated;
        try { generated = TlsIdentity::newKey(); }
        catch (const std::exception&) {
            storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return;
        }
        auto* write = new QKeychain::WritePasswordJob("YunaSupporter", this);
        write->setKey(slot); write->setInsecureFallback(false); write->setBinaryData(generated);
        connect(write, &QKeychain::Job::finished, this, [this, write, generated, action, sealed] {
            if (write->error() != QKeychain::NoError) {
                storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return;
            }
            storageKey_ = generated; read(action, sealed);
        });
        write->start();
    });
    job->start();
    return true;
}

void License::read(Action action, const QByteArray& sealed) {
    QJsonObject loaded;
    if (!sealed.isEmpty()) {
        try {
            const auto document = QJsonDocument::fromJson(TlsIdentity::open(sealed, storageKey_, receiptContext));
            if (!document.isObject()) throw std::runtime_error("Invalid supporter record.");
            loaded = document.object();
            if (loaded.value("policy").toObject() != policy()) throw std::runtime_error("Different supporter policy.");
        } catch (const std::exception&) {
            storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return;
        }
    }
    if (unsaved_) {
        // Retry only against the file this operation read. Another profile may
        // have renewed or signed out since the failure.
        // Reloading the unchanged old grant must still preserve our revocation.
        if (loaded == persisted_) {
            if (!save(record_)) return;
            loaded = record_;
        } else unsaved_ = false;
    }
    record_ = loaded; persisted_ = loaded; storageHealthy_ = true;
    if (!identityFile_.isEmpty() && receiptIdentityFile_.isEmpty()) {
        // Migrate only after decrypting the existing grant. An inaccessible
        // keychain must never silently rebind the account.
        try {
            const auto identity = TlsIdentity::loadFile(identityFile_, false);
            storageKey_ = identity.deriveKey(directory_.toUtf8(), receiptContext);
            receiptIdentityFile_ = QFileInfo(identityFile_).canonicalFilePath();
        } catch (const std::exception&) {
            storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return;
        }
        if (!save(loaded)) return;
    }
    emit changed(); schedule();
    if (action == Action::SignOut) {
        if (save({})) finish({});
    } else if (action == Action::SignIn) {
        if (save({})) startAuthorization();
    } else if (record_.value("token").toString().isEmpty()) finish({});
    else if (record_.value("token_expires").toInteger() > 0
             && record_.value("token_expires").toInteger() <= now() + 60000) refreshToken();
    else checkPage({}, account().isEmpty());
}

void License::request(const QUrl& url, const QJsonObject& payload, bool authenticated,
                      std::function<void(QJsonObject, int)> completion) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "YunaSupporter");
    if (authenticated) request.setRawHeader("Authorization", "Bearer " + record_.value("token").toString().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    const auto generation = generation_;
    auto* reply = network_.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    reply->setReadBufferSize(maxResponse + 1);
    auto* timeout = new QTimer(reply); timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort); timeout->start(15000);
    auto body = std::make_shared<QByteArray>();
    connect(reply, &QIODevice::readyRead, reply, [reply, body] {
        body->append(reply->readAll());
        if (body->size() > maxResponse) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, body, generation, completion = std::move(completion)] {
        if (reply->isOpen()) body->append(reply->readAll());
        const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 429 || code == 403) {
            const auto header = reply->rawHeader("Retry-After");
            bool numeric = false;
            const auto seconds = header.toLongLong(&numeric);
            qint64 retry = numeric && seconds > 0 ? now() + std::min<qint64>(seconds, 86400) * 1000
                : QDateTime::fromString(QString::fromLatin1(header), Qt::RFC2822Date).toMSecsSinceEpoch();
            if (reply->rawHeader("X-RateLimit-Remaining") == "0")
                retry = std::max(retry, std::clamp<qint64>(reply->rawHeader("X-RateLimit-Reset").toLongLong(), 0, (now() + day) / 1000) * 1000);
            retryAfter_ = std::clamp(retry, now() + 60000, now() + day);
        }
        const bool received = body->size() <= maxResponse && (code == 200 || code == 401)
            && (reply->error() == QNetworkReply::NoError || reply->error() == QNetworkReply::AuthenticationRequiredError);
        const auto document = received ? QJsonDocument::fromJson(*body) : QJsonDocument{};
        reply->deleteLater();
        if (generation == generation_) completion(document.object(), received && document.isObject() ? code : 0);
    });
}
void License::startAuthorization() {
    request(oauthEndpoint_.resolved(QUrl("/login/device/code")),
        {{"client_id", product_.value("client_id")}, {"scope", "read:user"}}, false,
        [this](const QJsonObject& response, int code) {
            const auto verification = QUrl(response.value("verification_uri").toString());
            const auto seconds = response.value("expires_in").toInteger();
            const auto interval = response.value("interval").toInteger(5);
            const auto device = response.value("device_code").toString();
            const auto user = response.value("user_code").toString();
            if (code != 200 || !token(device) || !token(user) || user.size() > 32
                || verification != oauthEndpoint_.resolved(QUrl("/login/device"))
                || seconds < 1 || seconds > 3600 || interval < 1 || interval > 60) {
                finish(tr("GitHub sign-in is unavailable. Try again."), true); return;
            }
            deviceCode_ = device; userCode_ = user; verificationUrl_ = verification;
            pollInterval_ = int(interval * 1000); nextPoll_ = now() + pollInterval_;
            authorizationExpires_ = now() + seconds * 1000;
            emit changed(); emit authorizationReady(); schedule();
        });
}
void License::pollAuthorization() {
    nextPoll_ = now() + pollInterval_;
    request(oauthEndpoint_.resolved(QUrl("/login/oauth/access_token")),
        {{"client_id", product_.value("client_id")}, {"device_code", deviceCode_},
         {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}, false,
        [this](const QJsonObject& response, int code) {
            if (now() >= authorizationExpires_) { finish(tr("Sign-in expired. Try again.")); return; }
            const auto error = response.value("error").toString();
            if (code == 200 && (error == "authorization_pending" || error == "slow_down")) {
                if (error == "slow_down") pollInterval_ = int(std::min<qint64>(3600000,
                    std::max<qint64>(pollInterval_ + 5000, std::clamp<qint64>(response.value("interval").toInteger(), 0, 3600) * 1000)));
                nextPoll_ = now() + pollInterval_; schedule(); emit changed(); return;
            }
            if (code == 0) {
                pollInterval_ = std::min(60000, pollInterval_ * 2);
                nextPoll_ = now() + pollInterval_; schedule(); return;
            }
            if (code != 200 || !error.isEmpty() || !acceptToken(response)) {
                if (busy_) finish(tr("GitHub sign-in was not completed. Try again."));
                return;
            }
            deviceCode_.clear(); userCode_.clear(); verificationUrl_.clear();
            checkPage({}, true);
        });
}
bool License::acceptToken(const QJsonObject& response) {
    const auto access = response.value("access_token").toString();
    const auto scopes = response.value("scope").toString().split(QRegularExpression("[, ]+"), Qt::SkipEmptyParts);
    if (!token(access) || response.value("token_type").toString().toLower() != "bearer" || !scopes.contains("read:user")) return false;
    auto next = record_;
    next.insert("token", access);
    next.remove("refresh_token"); next.remove("token_expires"); next.remove("refresh_expires");
    if (response.contains("expires_in") || response.contains("refresh_token")) {
        const auto refresh = response.value("refresh_token").toString();
        const auto lifetime = response.value("expires_in").toInteger();
        const auto refreshLifetime = response.value("refresh_token_expires_in").toInteger();
        if (!token(refresh) || lifetime < 1 || lifetime > 86400 || refreshLifetime < lifetime || refreshLifetime > 366 * 86400LL) return false;
        next.insert("refresh_token", refresh); next.insert("token_expires", now() + lifetime * 1000);
        next.insert("refresh_expires", now() + refreshLifetime * 1000);
    }
    return save(next);
}
void License::refreshToken() {
    const auto refresh = record_.value("refresh_token").toString();
    if (refresh.isEmpty() || record_.value("refresh_expires").toInteger() <= now()) {
        auto next = record_; next.remove("token"); next.remove("refresh_token"); next.insert("valid", false);
        if (save(next)) finish(tr("Sign in with GitHub again."));
        return;
    }
    rotated_ = true;
    request(oauthEndpoint_.resolved(QUrl("/login/oauth/access_token")),
        {{"client_id", product_.value("client_id")}, {"refresh_token", refresh}, {"grant_type", "refresh_token"}}, false,
        [this](const QJsonObject& response, int code) {
            if (code == 200 && response.value("error") == "bad_refresh_token") {
                auto next = record_; next.remove("token"); next.remove("refresh_token"); next.insert("valid", false);
                if (save(next)) finish(tr("Sign in with GitHub again."));
            } else if (code != 200 || response.contains("error") || !acceptToken(response)) {
                if (busy_) failCheck();
            } else {
                events_.clear(); cursors_.clear(); viewer_ = {}; contributor_ = false;
                checkPage({}, account().isEmpty());
            }
        });
}
void License::failCheck() { finish(tr("GitHub could not be checked. Offline access lasts at most seven days."), true); }
void License::checkPage(const QString& cursor, bool accountOnly) {
    static const QString query = QStringLiteral(R"(query($after:String,$owner:String!,$repository:String!) {
      viewer { databaseId login sponsorsActivities(first:100,after:$after,period:ALL,includeAsSponsor:true,
        includePrivate:true,actions:[NEW_SPONSORSHIP,REFUND],orderBy:{field:TIMESTAMP,direction:DESC}) {
          nodes { id action timestamp sponsor { ... on User { databaseId } ... on Organization { databaseId } }
            sponsorable { ... on User { databaseId } ... on Organization { databaseId } }
            sponsorsTier { id isOneTime monthlyPriceInCents } }
          pageInfo { hasNextPage endCursor }
      } }
      repository(owner:$owner,name:$repository) { object(expression:"HEAD:CONTRIBUTORS.md") { ... on Blob { text isTruncated } } }
    })");
    request(endpoint_, {{"query", accountOnly ? QStringLiteral("query { viewer { databaseId login } }") : query}, {"variables", QJsonObject{{"after", cursor.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(cursor)},
        {"owner", product_.value("owner")}, {"repository", product_.value("repository")}}}}, true,
        [this, cursor, accountOnly](const QJsonObject& response, int code) {
            if (code == 401 && !rotated_ && record_.contains("refresh_token")) { refreshToken(); return; }
            if (code == 401) {
                auto next = record_; next.remove("token"); next.insert("valid", false);
                if (save(next)) finish(tr("Sign in with GitHub again."));
                return;
            }
            if (code != 200 || (response.contains("errors") && (!response.value("errors").isArray() || !response.value("errors").toArray().isEmpty()))) { failCheck(); return; }
            const auto data = response.value("data").toObject();
            const auto viewer = data.value("viewer").toObject();
            const auto id = viewer.value("databaseId");
            const auto login = viewer.value("login").toString();
            if (accountOnly) {
                if (!positiveId(id) || login.isEmpty()) { failCheck(); return; }
                auto next = record_;
                next.insert("account_id", id); next.insert("login", login);
                if (save(next)) checkPage();
                return;
            }
            if (!positiveId(id) || login.isEmpty() || !data.value("repository").isObject()) { failCheck(); return; }
            if ((!viewer_.isEmpty() && viewer_.value("id") != id)
                || (record_.contains("account_id") && record_.value("account_id") != id)) {
                auto next = record_; next.remove("token"); next.insert("valid", false);
                if (save(next)) finish(tr("Sign in with GitHub again."));
                return;
            }
            viewer_ = {{"id", id}, {"login", login}};
            const auto repository = data.value("repository").toObject();
            if (!repository.contains("object")) { failCheck(); return; }
            if (!repository.value("object").isNull()) {
                const auto blob = repository.value("object").toObject();
                if (!blob.value("text").isString() || !blob.value("isTruncated").isBool() || blob.value("isTruncated").toBool()) { failCheck(); return; }
                // A table row's first cell is the immutable numeric GitHub ID.
                // Display names and prose never grant access.
                const QRegularExpression row(QString("^\\|[ \t]*%1[ \t]*\\|").arg(id.toInteger()), QRegularExpression::MultilineOption);
                const bool contributor = row.match(blob.value("text").toString()).hasMatch();
                if (!cursor.isEmpty() && contributor_ != contributor) { failCheck(); return; }
                contributor_ = contributor;
            } else if (!cursor.isEmpty() && contributor_) { failCheck(); return; }
            if (!contributor_) {
                const auto activities = viewer.value("sponsorsActivities").toObject();
                const auto page = activities.value("pageInfo").toObject();
                if (!activities.value("nodes").isArray() || activities.value("nodes").toArray().size() > 100
                    || !page.value("hasNextPage").isBool()) { failCheck(); return; }
                for (const auto& value : activities.value("nodes").toArray()) {
                    if (!value.isObject()) { failCheck(); return; }
                    const auto event = value.toObject();
                    const auto payer = event.value("sponsor").toObject().value("databaseId");
                    const auto recipient = event.value("sponsorable").toObject().value("databaseId");
                    if (!positiveId(payer) || !positiveId(recipient)) { failCheck(); return; }
                    if (payer != id || recipient != product_.value("recipient_id")) continue;
                    const auto tier = event.value("sponsorsTier").toObject();
                    if (!tier.contains("id")) { failCheck(); return; }
                    if (tier.value("id") != product_.value("tier_id")) continue;
                    const auto eventId = event.value("id").toString();
                    const auto action = event.value("action").toString();
                    const auto timestamp = utcTimestamp(event.value("timestamp"));
                    if (!token(eventId) || (action != "NEW_SPONSORSHIP" && action != "REFUND")
                        || !tier.value("isOneTime").toBool() || tier.value("monthlyPriceInCents").toInt() != 1200
                        || timestamp <= 0 || timestamp > now() + 60000
                        || (events_.contains(eventId) && events_.value(eventId) != event)) { failCheck(); return; }
                    events_.insert(eventId, event);
                }
                if (page.value("hasNextPage").toBool()) {
                    const auto next = page.value("endCursor").toString();
                    if (next.isEmpty() || cursors_.contains(next) || cursors_.size() >= 99) { failCheck(); return; }
                    cursors_.insert(next); checkPage(next); return;
                }
            }
            qint64 paid = 0, refunded = 0;
            for (const auto& event : std::as_const(events_)) {
                auto& latest = event.value("action") == "REFUND" ? refunded : paid;
                latest = std::max(latest, utcTimestamp(event.value("timestamp")));
            }
            const auto expires = paid > refunded
                ? QDateTime::fromMSecsSinceEpoch(paid, QTimeZone::UTC).addYears(1).toMSecsSinceEpoch() : 0;
            auto next = record_;
            next.insert("account_id", id); next.insert("login", login);
            next.insert("expires", contributor_ ? std::max(expires, now() + 7 * day) : expires);
            next.insert("checked", now()); next.insert("valid", contributor_ || expires > now());
            if (save(next)) finish({});
        });
}

bool License::save(const QJsonObject& value) {
    auto next = value; next.insert("policy", policy());
    QSaveFile file(directory_ + "/supporter.bin"); file.setDirectWriteFallback(false);
    try {
        auto data = TlsIdentity::seal(QJsonDocument(next).toJson(QJsonDocument::Compact), storageKey_, receiptContext);
        if (!receiptIdentityFile_.isEmpty())
            data = QJsonDocument(QJsonObject{{"identityFile", receiptIdentityFile_},
                {"sealed", QString::fromLatin1(data.toBase64())}}).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
            || file.write(data) != data.size() || !file.commit()) throw std::runtime_error("License save failed.");
    } catch (const std::exception&) {
        record_ = next; unsaved_ = true;
        storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return false;
    }
    record_ = next; persisted_ = next; unsaved_ = false; storageHealthy_ = true; emit changed(); return true;
}

void License::finish(QString status, bool transient) {
    status_ = std::move(status); busy_ = false; lock_.unlock();
    deviceCode_.clear(); userCode_.clear(); verificationUrl_.clear();
    events_.clear(); cursors_.clear(); viewer_ = {};
    failures_ = transient ? std::min(failures_ + 1, 7) : 0;
    nextCheck_ = std::max(retryAfter_, now() + (transient ? std::min<qint64>(60000LL << (failures_ - 1), 3600000) : checkInterval));
    emit changed(); schedule();
}
void License::schedule() {
    timer_.stop();
    if (!configured()) return;
    qint64 due = nextCheck_;
    if (pending()) due = std::min(nextPoll_, authorizationExpires_);
    else if (busy_) {
        if (!active()) return;
        due = usableUntil();
    }
    if (active()) due = std::min(due, usableUntil());
    timer_.start(int(std::clamp<qint64>(due - now(), 1, checkInterval)));
}
