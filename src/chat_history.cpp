#include "chat_history.hpp"
#include "chat_content.hpp"
#include "voice_session.hpp"

#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <QSet>
#include <QDirIterator>
#include <QMessageAuthenticationCode>
#include <QScopeGuard>
#include <QJsonDocument>
#include <QSaveFile>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace {
constexpr int maximumLegacyStore = 40 * 1024 * 1024;
const QString recordColumns = "m.sequence,m.created,m.expires,m.sender,m.request,m.image,m.body,p.body";
const QString recordTables = "messages m LEFT JOIN profiles p ON p.sender=m.sender";
constexpr qint64 maximumTime = 9007199254740991LL - ChatHistory::maximumLifetime;
void require(bool okay, const char* text = QT_TRANSLATE_NOOP("LocalChannel", "Chat encryption failed.")) {
    if (!okay) throw std::runtime_error(text);
}
bool identifier(const QString& value) { return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces) == value; }
bool deviceId(const QString& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](QChar c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
qint64 timestamp(qint64 value) {
    require(value >= 0 && value <= maximumTime, QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat timestamp.")); return value;
}
}

bool ChatHistory::validText(const QString& text) {
    return !text.trimmed().isEmpty() && text.toUtf8().size() <= maximumTextBytes
        && std::none_of(text.begin(), text.end(), [](QChar c) {
            return c.unicode() < 32 && c != '\n' && c != '\r' && c != '\t';
        }) && QString::fromUtf8(text.toUtf8()) == text;
}

bool ChatHistory::validMessage(const QJsonObject& message) {
    const auto created = message.value("created").toInteger(-1);
    const auto sequence = message.value("sequence").toInteger(-1);
    bool name = false;
    try { name = VoiceSession::validatedName(message.value("name").toString()) == message.value("name").toString(); }
    catch (const std::invalid_argument&) { return false; }
    if (message.contains("event")) {
        const auto event = message.value("event").toObject();
        const auto kind = event.value("kind").toString();
        static const QRegularExpression eventName(QStringLiteral("\\A[A-Za-z][A-Za-z0-9._-]{0,63}\\z"));
        if (message.value("avatarId") != "system" || message.contains("image") || !eventName.match(kind).hasMatch()) return false;
        if (kind == "joined" || kind == "left" || kind == "kicked" || kind == "banned") {
            if (!deviceId(event.value("member").toString())) return false;
            try {
                if (VoiceSession::validatedName(event.value("name").toString()) != event.value("name").toString()) return false;
            } catch (const std::invalid_argument&) { return false; }
        }
    }
    const auto image = message.value("image").toObject();
    const bool hasImage = deviceId(image.value("hash").toString()) && image.value("size").toInteger() > 0
        && image.value("size").toInteger() <= 8 * 1024 * 1024 && image.value("width").toInt() > 0
        && image.value("width").toInt() <= 8192 && image.value("height").toInt() > 0 && image.value("height").toInt() <= 8192
        && qint64(image.value("width").toInt()) * image.value("height").toInt() <= 24 * 1024 * 1024;
    return name && (!message.contains("avatar") || VoiceSession::validAvatar(message.value("avatar").toString()))
        && (!message.contains("avatarId") || VoiceSession::validAvatar(message.value("avatarId").toString()))
        && identifier(message.value("request").toString()) && deviceId(message.value("sender").toString())
        && (!message.contains("image") || hasImage)
        && (validText(message.value("text").toString()) || (hasImage && message.value("text").toString().isEmpty()))
        && created >= 0 && created <= maximumTime
        && message.value("created").toDouble(-1) == double(created)
        && message.value("expires").toDouble(-1) == double(message.value("expires").toInteger(-1))
        && message.value("expires").toInteger(-1) > created
        && validLifetime(message.value("expires").toInteger(-1) - created)
        && sequence > 0 && sequence <= maximumTime && message.value("sequence").toDouble(-1) == double(sequence);
}

QJsonObject ChatHistory::forReader(QJsonObject message, bool extended) {
    if (!extended) message.insert("expires", std::min(message.value("expires").toInteger(), message.value("created").toInteger() + 30 * lifetime));
    return message;
}

ChatHistory::ChatHistory(QString file, const TlsIdentity& identity, qint64 now)
    : file_(std::move(file)), context_("SquadSpeak/chat-store/v1/" + identity.id().toUtf8()), time_(timestamp(now)) {
    key_ = identity.deriveKey(context_, "history encryption");
    indexKey_ = identity.deriveKey(context_, "history index");
    const auto connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    database_ = QSqlDatabase::addDatabase("QSQLITE", connection);
    try {
        const auto path = file_ + ".sqlite";
        QFile disk(path);
        if (!disk.exists()) {
            require(disk.open(QIODevice::WriteOnly | QIODevice::NewOnly), QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be saved."));
            require(disk.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            disk.close();
        }
        database_.setDatabaseName(path);
        database_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
        require(database_.open(), QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be read."));
        query("PRAGMA journal_mode=DELETE");
        query("PRAGMA synchronous=FULL");
        query("PRAGMA cache_size=-2048");
        query("PRAGMA secure_delete=ON");
        query("PRAGMA auto_vacuum=INCREMENTAL");
        query("BEGIN IMMEDIATE");
        const auto rollback = qScopeGuard([this] { database_.rollback(); });
        query("CREATE TABLE IF NOT EXISTS metadata (id INTEGER PRIMARY KEY CHECK(id=1),body BLOB NOT NULL)");
        query("CREATE TABLE IF NOT EXISTS messages (sequence INTEGER PRIMARY KEY,created INTEGER NOT NULL,expires INTEGER NOT NULL,sender BLOB NOT NULL,request BLOB UNIQUE NOT NULL,image TEXT NOT NULL,body BLOB NOT NULL)");
        query("CREATE INDEX IF NOT EXISTS expiry ON messages(expires)");
        query("CREATE INDEX IF NOT EXISTS image ON messages(image)");
        query("CREATE INDEX IF NOT EXISTS sender ON messages(sender)");
        query("CREATE TABLE IF NOT EXISTS profiles (sender BLOB PRIMARY KEY,body BLOB NOT NULL)");
        bool initialized;
        { auto meta = query("SELECT 1 FROM metadata"); initialized = meta.next(); }
        if (initialized) readClock();
        else {
            QFile legacy(file_);
            if (legacy.exists()) {
                require(legacy.open(QIODevice::ReadOnly) && legacy.size() <= maximumLegacyStore + 28,
                    QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be read."));
                const auto document = QJsonDocument::fromJson(TlsIdentity::open(legacy.readAll(), key_, context_));
                const auto object = document.object();
                require(object.value("version").toInt() == 1 && object.value("messages").isArray(), QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat history."));
                time_ = std::max(time_, timestamp(object.value("time").toInteger(-1)));
                sequence_ = object.value("sequence").toInteger(-1);
                require(sequence_ >= 0 && sequence_ <= maximumTime, QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat sequence."));
                qint64 last = 0;
                for (const auto value : object.value("messages").toArray()) {
                    const auto message = value.toObject();
                    require(validMessage(message) && message.value("sequence").toInteger() > last
                        && message.value("sequence").toInteger() <= sequence_ && message.value("created").toInteger() <= time_,
                        QT_TRANSLATE_NOOP("LocalChannel", "Invalid message in chat history."));
                    last = message.value("sequence").toInteger();
                    if (message.value("expires").toInteger() > time_) insert(message);
                }
            }
            saveClock(time_, sequence_);
        }
        require(database_.commit(), QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be saved atomically."));
        // Only remove the legacy store after the complete encrypted migration commits.
        if (QFile::exists(file_)) require(QFile::remove(file_), QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be saved."));
        expire(time_);
        pruneImages();
    } catch (...) {
        database_.close(); database_ = {}; QSqlDatabase::removeDatabase(connection);
        throw;
    }
}

ChatHistory::~ChatHistory() {
    key_.fill(0); indexKey_.fill(0);
    const auto connection = database_.connectionName();
    database_.close(); database_ = {}; QSqlDatabase::removeDatabase(connection);
}

QSqlQuery ChatHistory::query(const QString& sql, const QVariantList& values) const {
    QSqlQuery result(database_);
    result.setForwardOnly(true);
    require(result.prepare(sql), QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat history."));
    for (const auto& value : values) result.addBindValue(value);
    require(result.exec(), QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be saved."));
    return result;
}

QByteArray ChatHistory::index(const QString& value) const {
    return QMessageAuthenticationCode::hash(value.toUtf8(), indexKey_, QCryptographicHash::Sha256);
}

void ChatHistory::saveClock(qint64 now, qint64 sequence) {
    const auto data = QJsonDocument(QJsonObject{{"version", 2}, {"time", now}, {"sequence", sequence}}).toJson(QJsonDocument::Compact);
    query("INSERT OR REPLACE INTO metadata VALUES(1,?)", {TlsIdentity::seal(data, key_, context_ + "/metadata")});
}

void ChatHistory::readClock() {
    auto meta = query("SELECT body FROM metadata WHERE id=1");
    require(meta.next(), QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat history."));
    const auto value = QJsonDocument::fromJson(TlsIdentity::open(meta.value(0).toByteArray(), key_, context_ + "/metadata")).object();
    require(value.value("version").toInt() == 2, QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat history."));
    time_ = std::max(time_, timestamp(value.value("time").toInteger(-1)));
    sequence_ = value.value("sequence").toInteger(-1);
    require(sequence_ >= 0 && sequence_ <= maximumTime, QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat sequence."));
}

void ChatHistory::insert(const QJsonObject& message) {
    const auto sequence = message.value("sequence").toInteger();
    const auto sender = message.value("sender").toString();
    query("INSERT INTO messages VALUES(?,?,?,?,?,?,?)", {sequence, message.value("created").toInteger(), message.value("expires").toInteger(),
        index(sender), index(sender + '/' + message.value("request").toString()), message.value("image").toObject().value("hash").toString(QLatin1String("")),
        TlsIdentity::seal(QJsonDocument(message).toJson(QJsonDocument::Compact), key_, context_ + "/record/" + QByteArray::number(sequence))});
}

QJsonObject ChatHistory::record(const QSqlQuery& row) const {
    auto message = QJsonDocument::fromJson(TlsIdentity::open(row.value(6).toByteArray(), key_, context_ + "/record/" + QByteArray::number(row.value(0).toLongLong()))).object();
    const auto sender = message.value("sender").toString();
    require(validMessage(message) && message.value("sequence").toInteger() == row.value(0).toLongLong()
        && message.value("created").toInteger() == row.value(1).toLongLong() && message.value("expires").toInteger() == row.value(2).toLongLong()
        && index(sender) == row.value(3).toByteArray() && index(sender + '/' + message.value("request").toString()) == row.value(4).toByteArray()
        && message.value("image").toObject().value("hash").toString() == row.value(5).toString(),
        QT_TRANSLATE_NOOP("LocalChannel", "Invalid message in chat history."));
    if (!row.value(7).isNull()) {
        const auto profile = QJsonDocument::fromJson(TlsIdentity::open(row.value(7).toByteArray(), key_, context_ + "/profile/" + row.value(3).toByteArray().toHex())).object();
        const auto name = profile.value("name").toString(), avatar = profile.value("avatar").toString();
        require(VoiceSession::validatedName(name) == name && VoiceSession::validAvatar(avatar), QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat profile."));
        message.insert("name", name); message.insert("avatar", VoiceSession::avatarFallback(avatar)); message.insert("avatarId", avatar);
    }
    return message;
}

QJsonObject ChatHistory::page(qint64 cursor, const QString& direction, bool extended) const {
    require(cursor >= 0 && cursor <= 9007199254740991LL && (direction == "latest" || direction == "older" || direction == "newer"),
        QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat sequence."));
    const QString active = "m.expires>? AND (? OR m.created>?)";
    const QVariantList bounds{time_, extended, time_ - 30 * lifetime};
    auto values = bounds;
    QString where = active;
    if (direction != "latest") { where += direction == "newer" ? " AND m.sequence>?" : " AND m.sequence<?"; values.append(cursor); }
    QJsonArray records;
    qsizetype bytes = 512;
    {
        values.append(maximumPageMessages);
        auto rows = query("SELECT " + recordColumns + " FROM " + recordTables + " WHERE " + where
            + " ORDER BY m.sequence " + (direction == "newer" ? "ASC" : "DESC") + " LIMIT ?", values);
        while (rows.next()) {
            const auto message = forReader(record(rows), extended);
            bytes += QJsonDocument(message).toJson(QJsonDocument::Compact).size() + 1;
            if (bytes > maximumPageBytes) break;
            if (direction == "newer") records.append(message); else records.prepend(message);
        }
    }
    const auto edge = [&](bool older) {
        const auto sequence = records.isEmpty() ? cursor : (older ? records.first() : records.last()).toObject().value("sequence").toInteger();
        auto args = bounds; args.append(sequence);
        auto result = query("SELECT 1 FROM messages m WHERE " + active + (older ? " AND m.sequence<?" : " AND m.sequence>?") + " LIMIT 1", args);
        return result.next();
    };
    return {{"kind", "history"}, {"direction", direction}, {"records", records}, {"older", edge(true)}, {"newer", edge(false)}, {"now", time_}};
}

bool ChatHistory::expire(qint64 now) {
    now = std::max(time_, timestamp(now));
    // Bound each maintenance pass. Queries immediately hide every expired row,
    // while later passes reclaim remaining ciphertext without a long UI stall.
    query("BEGIN IMMEDIATE");
    const auto rollback = qScopeGuard([this] { database_.rollback(); });
    readClock(); now = std::max(time_, now);
    QStringList hashes;
    QList<qint64> expired;
    {
        auto rows = query("SELECT " + recordColumns + " FROM " + recordTables + " WHERE m.expires<=? ORDER BY m.expires LIMIT 256", {now});
        while (rows.next()) {
            const auto message = record(rows);
            expired.append(message.value("sequence").toInteger());
            const auto hash = message.value("image").toObject().value("hash").toString();
            if (!hash.isEmpty()) hashes.append(hash);
        }
    }
    if (!expired.isEmpty()) {
        for (const auto sequence : expired) query("DELETE FROM messages WHERE sequence=?", {sequence});
        query("DELETE FROM profiles WHERE NOT EXISTS(SELECT 1 FROM messages WHERE messages.sender=profiles.sender)");
        saveClock(now, sequence_);
    }
    auto pages = query("PRAGMA freelist_count");
    const int reclaim = pages.next() ? std::min(64, pages.value(0).toInt()) : 0;
    pages.finish();
    // QSQLITE does not iterate zero-column PRAGMAs. Reclaim one page per
    // statement, within this transaction, and bound the work per maintenance pass.
    for (int i = 0; i < reclaim; ++i) query("PRAGMA incremental_vacuum(1)");
    require(database_.commit(), QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be saved atomically."));
    time_ = now;
    for (const auto& hash : hashes) pruneImage(hash);
    return !expired.isEmpty();
}

bool ChatHistory::updateProfile(const QString& sender, const QString& name, const QString& avatar) {
    require(deviceId(sender) && VoiceSession::validatedName(name) == name && VoiceSession::validAvatar(avatar), QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat profile."));
    auto existing = query("SELECT 1 FROM messages WHERE sender=? LIMIT 1", {index(sender)});
    if (!existing.next()) return false;
    existing.finish();
    const auto body = QJsonDocument(QJsonObject{{"name", name}, {"avatar", avatar}}).toJson(QJsonDocument::Compact);
    query("INSERT OR REPLACE INTO profiles VALUES(?,?)", {index(sender), TlsIdentity::seal(body, key_, context_ + "/profile/" + index(sender).toHex())});
    return true;
}

QJsonObject ChatHistory::append(const QString& sender, const QString& name, const QString& request, const QString& text, qint64 now, const QByteArray& image, const QString& avatar, qint64 duration, const QJsonObject& event) {
    require(validLifetime(duration), QT_TRANSLATE_NOOP("LocalChannel", "Invalid message lifetime."));
    expire(now);
    query("BEGIN IMMEDIATE");
    const auto rollback = qScopeGuard([this] { database_.rollback(); });
    readClock(); now = time_;
    QJsonObject message{{"sender", sender}, {"name", name}, {"request", request}, {"text", text},
        {"created", now}, {"expires", now + duration}, {"sequence", sequence_ + 1}, {"avatar", VoiceSession::avatarFallback(avatar)}, {"avatarId", avatar}};
    if (!event.isEmpty()) message.insert("event", event);
    QString hash;
    if (!image.isEmpty()) {
        const auto size = ChatContent::sanitizedImageSize(image);
        hash = QString::fromLatin1(QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex());
        message.insert("image", QJsonObject{{"hash", hash}, {"size", image.size()}, {"width", size.width()}, {"height", size.height()}});
    }
    require(validMessage(message), QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat message (maximum 16 KiB of text)."));
    QString retiredImage;
    {
        auto existing = query("SELECT " + recordColumns + " FROM " + recordTables + " WHERE m.request=?", {index(sender + '/' + request)});
        if (existing.next()) {
            const auto previous = record(existing);
            if (previous.value("expires").toInteger() > now) {
                require(previous.value("text") == text && previous.value("image") == message.value("image") && previous.value("event") == message.value("event"),
                    QT_TRANSLATE_NOOP("LocalChannel", "Message identifier was reused with different content."));
                return previous;
            }
            retiredImage = previous.value("image").toObject().value("hash").toString();
        }
    }
    // An expired retry may reuse its ID even when maintenance has more batches.
    query("DELETE FROM messages WHERE request=? AND expires<=?", {index(sender + '/' + request), now});
    bool newImage = false;
    const auto imagePath = file_ + ".images/" + hash + ".enc";
    const auto discard = qScopeGuard([&] { if (newImage) QFile::remove(imagePath); });
    if (!image.isEmpty() && !QFile::exists(imagePath)) {
        require(QDir().mkpath(file_ + ".images"), QT_TRANSLATE_NOOP("LocalChannel", "Image storage could not be created."));
        const auto encrypted = TlsIdentity::seal(image, key_, context_ + "/image/" + hash.toLatin1());
        QSaveFile output(imagePath);
        require(output.open(QIODevice::WriteOnly) && output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            && output.write(encrypted) == encrypted.size() && output.commit(), QT_TRANSLATE_NOOP("LocalChannel", "Image could not be saved."));
        newImage = true;
    }
    insert(message); saveClock(now, sequence_ + 1);
    require(database_.commit(), QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be saved atomically."));
    newImage = false; ++sequence_;
    if (!retiredImage.isEmpty()) pruneImage(retiredImage);
    return message;
}

QByteArray ChatHistory::image(const QString& hash, qint64 now) {
    expire(now);
    require(deviceId(hash), QT_TRANSLATE_NOOP("LocalChannel", "Invalid image identifier."));
    auto active = query("SELECT " + recordColumns + " FROM " + recordTables + " WHERE m.image=? AND m.expires>? LIMIT 1", {hash, time_});
    require(active.next(), QT_TRANSLATE_NOOP("LocalChannel", "Image has expired or is not present in the channel."));
    record(active); // Authenticate the reference before returning a cached image.
    if (const auto* cached = images_.object(hash)) return *cached;
    QFile input(file_ + ".images/" + hash + ".enc");
    require(input.open(QIODevice::ReadOnly) && input.size() <= 8 * 1024 * 1024 + 28, QT_TRANSLATE_NOOP("LocalChannel", "Image could not be read."));
    const auto plain = TlsIdentity::open(input.readAll(), key_, context_ + "/image/" + hash.toLatin1());
    require(QString::fromLatin1(QCryptographicHash::hash(plain, QCryptographicHash::Sha256).toHex()) == hash, QT_TRANSLATE_NOOP("LocalChannel", "Image content does not match the message."));
    images_.insert(hash, new QByteArray(plain), int(plain.size()));
    return plain;
}

void ChatHistory::pruneImage(const QString& hash) {
    // Serialize the reference check and unlink with append's image creation.
    // The retired message is already committed; this transaction only locks.
    query("BEGIN IMMEDIATE");
    const auto unlock = qScopeGuard([this] { database_.rollback(); });
    auto active = query("SELECT 1 FROM messages WHERE image=? LIMIT 1", {hash});
    if (active.next()) return;
    images_.remove(hash);
    const auto path = file_ + ".images/" + hash + ".enc";
    if (QFile::exists(path)) require(QFile::remove(path), QT_TRANSLATE_NOOP("LocalChannel", "Expired image could not be deleted."));
}

void ChatHistory::pruneImages() {
    QDirIterator files(file_ + ".images", {"*.enc"}, QDir::Files);
    while (files.hasNext()) {
        files.next();
        const auto hash = files.fileName().chopped(4);
        if (!deviceId(hash)) continue;
        pruneImage(hash);
    }
}
