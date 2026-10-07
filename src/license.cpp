#include "license.hpp"
#include "tls_identity.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSysInfo>
#include <QTimeZone>
#include <qtkeychain/keychain.h>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace {
constexpr qint64 checkInterval = 15 * 60 * 1000;
constexpr qsizetype maxResponse = 64 * 1024;
const QByteArray receiptContext("squadspeak/license/1");

bool positiveId(const QJsonValue& value) {
    return value.isDouble() && value.toInteger() > 0 && double(value.toInteger()) == value.toDouble();
}

bool validKey(const QString& key) {
    return !key.isEmpty() && key.size() <= 256
        && std::none_of(key.cbegin(), key.cend(), [](QChar c) { return c.isSpace() || !c.isPrint(); });
}
}

QJsonObject License::distributionProduct() {
    if (!directDistribution() || SQUADSPEAK_LICENSE_STORE_ID == 0) return {};
    return {{"store_id", SQUADSPEAK_LICENSE_STORE_ID}, {"product_id", SQUADSPEAK_LICENSE_PRODUCT_ID},
            {"variant_id", SQUADSPEAK_LICENSE_VARIANT_ID}};
}

bool License::directDistribution() { return !SQUADSPEAK_STORE_BUILD; }
QUrl License::purchaseUrl() {
    return directDistribution() && !distributionProduct().isEmpty() ? QUrl(QStringLiteral(SQUADSPEAK_PURCHASE_URL)) : QUrl{};
}

License::License(QString directory, QJsonObject product, QUrl endpoint, QByteArray storageKey,
                 std::function<qint64()> clock, QString identityFile, QObject* parent)
    : QObject(parent), directory_(QFileInfo(directory).absoluteFilePath()), product_(std::move(product)),
      endpoint_(std::move(endpoint)), storageKey_(std::move(storageKey)),
      identityFile_(std::move(identityFile)), suppliedStorageKey_(storageKey_), clock_(std::move(clock)),
      lock_(directory_ + "/license.lock"), network_(this) {
    if (!product_.isEmpty()) {
        for (const auto* field : {"store_id", "product_id", "variant_id"})
            if (!positiveId(product_.value(field))) throw std::invalid_argument("Invalid license product configuration.");
        if (endpoint_.scheme() != "https" || endpoint_.host().isEmpty() || !endpoint_.userInfo().isEmpty()
            || endpoint_.hasQuery() || endpoint_.hasFragment() || !endpoint_.path().endsWith('/'))
            throw std::invalid_argument("The license endpoint must be an HTTPS directory URL.");
    }
    if (!storageKey_.isEmpty() && storageKey_.size() != 32)
        throw std::invalid_argument("Invalid license storage key.");
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

bool License::active() const {
    const auto receipt = record_.value("receipt").toObject();
    return configured() && storageHealthy_ && receipt.value("valid").toBool()
        && receipt.value("expires").toInteger() > now();
}

bool License::recoveryNeeded() const {
    return pending() || (!active() && !record_.value("receipt").toObject().value("instance").toString().isEmpty());
}

QDateTime License::expiresAt() const {
    const auto expiry = record_.value("receipt").toObject().value("expires").toInteger();
    return expiry > 0 ? QDateTime::fromMSecsSinceEpoch(expiry, QTimeZone::UTC) : QDateTime{};
}

QString License::supportReference() const {
    const auto entry = record_.value(pending() ? "pending" : "receipt").toObject();
    if (!positiveId(entry.value("order_id")) || !positiveId(entry.value("license_id"))) return {};
    return QString("Order %1 / License %2").arg(entry.value("order_id").toInteger()).arg(entry.value("license_id").toInteger());
}

bool License::refresh() { return begin(Action::Check); }
bool License::refreshIfDue() {
    if (!configured()) return false;
    emit changed();
    if (busy_) { schedule(); return false; }
    if (now() >= nextCheck_) return refresh();
    schedule(); return false;
}
bool License::deactivate() { return begin(Action::Deactivate); }
bool License::resetActivation() { return begin(Action::Reset); }
bool License::activate(const QString& input) {
    const auto key = input.trimmed();
    if (!validKey(key)) { status_ = tr("Enter a valid license key."); emit changed(); return false; }
    return begin(Action::Activate, key);
}

bool License::begin(Action action, QString key) {
    if (!configured() || busy_) return false;
    if (!QDir().mkpath(directory_) || !lock_.tryLock(0)) {
        status_ = tr("License storage is busy or unavailable.");
        emit changed();
        nextCheck_ = now() + 60000; schedule();
        return false;
    }
    busy_ = true; status_.clear(); emit changed(); schedule();
    if (action != Action::Activate && record_.isEmpty() && !QFileInfo::exists(directory_ + "/license.bin")) {
        finish({}); return true;
    }
    QByteArray sealed;
    receiptIdentityFile_.clear();
    try {
        QFile file(directory_ + "/license.bin");
        if (file.exists()) {
            if (!file.open(QIODevice::ReadOnly) || file.size() > maxResponse)
                throw std::runtime_error("Invalid receipt.");
            sealed = file.read(maxResponse + 1);
            if (file.error() != QFileDevice::NoError || sealed.isEmpty() || sealed.size() > maxResponse)
                throw std::runtime_error("Invalid receipt.");
            const auto document = QJsonDocument::fromJson(sealed);
            if (document.isObject()) {
                const auto envelope = document.object();
                const auto path = envelope.value("identityFile").toString();
                const auto payload = QByteArray::fromBase64Encoding(envelope.value("sealed").toString().toLatin1(),
                    QByteArray::AbortOnBase64DecodingErrors);
                if (path.isEmpty() || !QDir::isAbsolutePath(path) || !payload || payload.decoded.isEmpty())
                    throw std::runtime_error("Invalid receipt.");
                // Another server profile may have its own host key. The first
                // activation's account binding remains authoritative.
                receiptIdentityFile_ = path;
                sealed = payload.decoded;
            }
        } else receiptIdentityFile_ = identityFile_;
        if (!receiptIdentityFile_.isEmpty()) {
            const auto identity = TlsIdentity::loadFile(receiptIdentityFile_, false);
            receiptIdentityFile_ = QFileInfo(receiptIdentityFile_).canonicalFilePath();
            storageKey_ = identity.deriveKey(directory_.toUtf8(), receiptContext);
            read(action, key, sealed); return true;
        }
    } catch (const std::exception&) {
        storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return true;
    }
    if (!suppliedStorageKey_.isEmpty()) { storageKey_ = suppliedStorageKey_; read(action, key, sealed); return true; }
    storageKey_.clear();
    auto* job = new QKeychain::ReadPasswordJob("SquadSpeak", this);
    const auto slot = "license/" + QString::fromLatin1(QCryptographicHash::hash(directory_.toUtf8(), QCryptographicHash::Sha256).toHex());
    job->setKey(slot); job->setInsecureFallback(false);
    connect(job, &QKeychain::Job::finished, this, [this, job, slot, action, key, sealed] {
        if (job->error() == QKeychain::NoError && job->binaryData().size() == 32) {
            storageKey_ = job->binaryData(); read(action, key, sealed); return;
        }
        if (job->error() != QKeychain::EntryNotFound || QFileInfo::exists(directory_ + "/license.bin")) {
            storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return;
        }
        QByteArray generated;
        try { generated = TlsIdentity::newKey(); }
        catch (const std::exception&) {
            storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return;
        }
        auto* write = new QKeychain::WritePasswordJob("SquadSpeak", this);
        write->setKey(slot); write->setInsecureFallback(false); write->setBinaryData(generated);
        connect(write, &QKeychain::Job::finished, this, [this, write, generated, action, key, sealed] {
            if (write->error() != QKeychain::NoError) {
                storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return;
            }
            storageKey_ = generated; read(action, key, sealed);
        });
        write->start();
    });
    job->start();
    return true;
}

void License::read(Action action, const QString& key, const QByteArray& sealed) {
    QJsonObject loaded;
    if (!sealed.isEmpty()) {
        try {
            const auto document = QJsonDocument::fromJson(TlsIdentity::open(sealed, storageKey_, receiptContext));
            if (!document.isObject()) throw std::runtime_error("Invalid receipt.");
            loaded = document.object();
            if (loaded.value("product").toObject() != product_) throw std::runtime_error("Different product.");
        } catch (const std::exception&) {
            storageHealthy_ = false; finish(tr("License storage is unavailable."), true); return;
        }
    }
    if (unsaved_) {
        // Retry only against the file this operation read. Another profile may
        // have renewed the pass or resolved an activation since the failure.
        // Reloading the unchanged old grant must still preserve our revocation.
        if (loaded == persisted_) {
            if (!save(record_)) return;
            loaded = record_;
        } else unsaved_ = false;
    }
    record_ = loaded; persisted_ = loaded; storageHealthy_ = true;
    if (!identityFile_.isEmpty() && receiptIdentityFile_.isEmpty()) {
        // Migrate only after decrypting the existing grant. An inaccessible
        // keychain must never turn into another provider activation.
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
    const auto current = record_.value("receipt").toObject();
    if (action == Action::Reset) {
        auto next = record_; next.remove("pending");
        if (!active()) {
            auto previous = current; previous.insert("valid", false);
            previous.remove("key"); previous.remove("instance");
            next.insert("receipt", previous);
        }
        if (save(next)) finish({});
        return;
    }
    if (action == Action::Activate) {
        if (pending()) { finish(tr("An activation is unresolved. Contact support before trying again.")); return; }
        const bool known = current.value("key").toString() == key && !current.value("instance").toString().isEmpty();
        validate(key, known ? current.value("instance").toString() : QString{}, !known);
        return;
    }
    if (current.value("instance").toString().isEmpty()) { finish({}); return; }
    if (action == Action::Check) {
        validate(current.value("key").toString(), current.value("instance").toString(), false);
        return;
    }
    request("deactivate", current.value("key").toString(), current.value("instance").toString(),
        [this](QJsonObject response, bool received) {
            if (!received || !response.value("deactivated").isBool()) {
                finish(tr("The license could not be checked. Your confirmed expiry is unchanged."), true); return;
            }
            if (!response.value("deactivated").toBool()) { finish(tr("Deactivation was declined. Contact support.")); return; }
            auto next = record_;
            auto current = next.value("receipt").toObject();
            current.insert("valid", false); current.remove("key"); current.remove("instance");
            next.insert("receipt", current);
            if (save(next)) finish({});
        });
}

void License::request(const QString& action, const QString& key, const QString& instance,
                      std::function<void(QJsonObject, bool)> completion) {
    QNetworkRequest request(endpoint_.resolved(QUrl(action)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    QByteArray form = "license_key=" + QUrl::toPercentEncoding(key);
    if (!instance.isEmpty()) form += "&instance_id=" + QUrl::toPercentEncoding(instance);
    if (action == "activate") form += "&instance_name=" + QUrl::toPercentEncoding(QSysInfo::machineHostName().left(80));
    auto* reply = network_.post(request, form);
    reply->setReadBufferSize(maxResponse + 1);
    auto* timeout = new QTimer(reply); timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort); timeout->start(15000);
    auto body = std::make_shared<QByteArray>();
    connect(reply, &QIODevice::readyRead, reply, [reply, body] {
        body->append(reply->readAll());
        if (body->size() > maxResponse) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [reply, body, completion = std::move(completion)] {
        if (reply->isOpen()) body->append(reply->readAll());
        const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool received = body->size() <= maxResponse && (code == 200 || code == 400 || code == 404 || code == 422)
            && (reply->error() == QNetworkReply::NoError || reply->error() >= QNetworkReply::ContentAccessDenied);
        const auto document = received ? QJsonDocument::fromJson(*body) : QJsonDocument{};
        const auto object = document.object();
        const bool contradictory = code != 200 && (object.value("valid").toBool()
            || object.value("activated").toBool() || object.value("deactivated").toBool());
        reply->deleteLater();
        completion(object, received && document.isObject() && !contradictory);
    });
}

QJsonObject License::receipt(const QJsonObject& response, const QString& key, const QString& instance, bool activated) const {
    const auto metadata = response.value("meta").toObject();
    for (auto it = product_.begin(); it != product_.end(); ++it)
        if (metadata.value(it.key()) != it.value()) return {};
    const auto license = response.value("license_key").toObject();
    const auto date = license.value("expires_at").toString();
    const auto expiry = QDateTime::fromString(date, Qt::ISODateWithMs);
    const auto id = response.value("instance").toObject().value("id").toString();
    if (!positiveId(license.value("id")) || !positiveId(metadata.value("order_id"))
        || license.value("key").toString() != key || license.value("activation_limit").toInt() != 3
        || !expiry.isValid() || !date.endsWith('Z') || expiry.toMSecsSinceEpoch() <= now()
        || (activated && (id.isEmpty() || id.size() > 128 || (!instance.isEmpty() && id != instance)))) return {};
    const auto state = license.value("status").toString();
    if (state != "active" && !(state == "inactive" && !activated)) return {};
    return {{"key", key}, {"instance", id}, {"valid", activated}, {"expires", expiry.toMSecsSinceEpoch()},
            {"checked", now()}, {"license_id", license.value("id")}, {"order_id", metadata.value("order_id")}};
}

void License::validate(const QString& key, const QString& instance, bool activation) {
    request("validate", key, instance, [this, key, instance, activation](QJsonObject response, bool received) {
        if (!received || !response.value("valid").isBool()) {
            finish(tr("The license could not be checked. Your confirmed expiry is unchanged."), true); return;
        }
        if (!response.value("valid").toBool()) {
            if (!activation) {
                auto next = record_; auto current = next.value("receipt").toObject();
                current.insert("valid", false); next.insert("receipt", current);
                if (!save(next)) return;
            }
            finish(tr("The license or device activation is no longer valid.")); return;
        }
        const auto confirmed = receipt(response, key, instance, !activation);
        if (confirmed.isEmpty()) { finish(tr("The license response does not match this annual pass."), true); return; }
        if (!activation) {
            auto next = record_; next.insert("receipt", confirmed);
            if (save(next)) finish({});
            return;
        }
        auto next = record_; next.insert("pending", confirmed);
        // Persist before the non-idempotent request. A lost response or crash
        // must not silently consume another activation after restarting.
        if (!save(next)) {
            // The request was not sent. This pending slot is not ambiguous.
            record_.remove("pending"); emit changed(); return;
        }
        request("activate", key, {}, [this, key](QJsonObject response, bool received) {
            if (!received || !response.value("activated").isBool()) {
                finish(tr("An activation is unresolved. Contact support before trying again.")); return;
            }
            auto next = record_;
            if (!response.value("activated").toBool()) {
                next.remove("pending");
                if (save(next)) finish(tr("Activation was declined. Check your key and available device slots."));
                return;
            }
            const auto confirmed = receipt(response, key, {}, true);
            if (confirmed.isEmpty()) { finish(tr("An activation is unresolved. Contact support before trying again.")); return; }
            next.remove("pending"); next.insert("receipt", confirmed);
            if (save(next)) finish({});
        });
    });
}

bool License::save(const QJsonObject& value) {
    auto next = value; next.insert("product", product_);
    QSaveFile file(directory_ + "/license.bin"); file.setDirectWriteFallback(false);
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
    failures_ = transient ? std::min(failures_ + 1, 7) : 0;
    nextCheck_ = now() + (transient ? std::min<qint64>(60000LL << (failures_ - 1), 3600000) : checkInterval);
    emit changed(); schedule();
}

void License::schedule() {
    timer_.stop();
    if (!configured()) return;
    const auto current = record_.value("receipt").toObject();
    if (busy_ && !active()) return;
    auto due = busy_ ? current.value("expires").toInteger() : nextCheck_;
    if (active()) due = std::min(due, current.value("expires").toInteger());
    timer_.start(int(std::clamp<qint64>(due - now(), 1, checkInterval)));
}
