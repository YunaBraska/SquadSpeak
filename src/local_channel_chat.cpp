#include "local_channel.hpp"
#include "chat_content.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslSocket>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace {
const QSet<QString> knownChatKinds{
    QStringLiteral("history"), QStringLiteral("send"), QStringLiteral("message"), QStringLiteral("time"),
    QStringLiteral("error"), QStringLiteral("imageStart"), QStringLiteral("imageChunk"),
    QStringLiteral("imageGet"), QStringLiteral("imageOffset"), QStringLiteral("imageData"), QStringLiteral("imageError")};
}

QVariantList LocalChannel::messages() const {
    const auto* c = chatClient();
    QVariantList result;
    if (!c || !c->accepted) return result;
    const auto now = c->chatClock.isValid() ? c->serverTime + c->chatClock.elapsed() : c->serverTime;
    const auto bot = chatBot();
    for (const auto value : c->messages) {
        auto record = value.toObject().toVariantMap();
        if (record.value("expires").toLongLong() <= now) continue;
        if (record.contains("event") && record.value("sender") == bot.value("id")) record.insert("name", bot.value("name"));
        result.append(record);
    }
    return result;
}

void LocalChannel::sendChatKey(QSslSocket* socket) {
    if (!history_ || !peers_.value(socket).joined || peers_.value(socket).controller) return;
    writeMessage(socket, {{"type", "chatKey"}, {"epoch", hostChatEpoch_},
        {"key", QString::fromLatin1(hostChatKey_.toBase64())}, {"lifetimeDays", peers_.value(socket).capabilities.contains("extended-retention") ? messageLifetimeDays() : std::min(messageLifetimeDays(), 30)}, {"now", std::max(history_->time(), clock_())}});
}

void LocalChannel::rotateChatKey() {
    if (!hosting_ || !history_) return;
    try {
        hostChatKey_ = TlsIdentity::newKey();
        hostChatEpoch_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (auto* socket : peers_.keys()) sendChatKey(socket);
    } catch (const std::exception& error) {
        stopHost(); setStatus(tr(error.what()), false);
    }
}

bool LocalChannel::writeChat(QSslSocket* socket, const QJsonObject& payload) {
    const auto context = "SquadSpeak/chat/v1/" + channelId().toUtf8() + '/' + hostChatEpoch_.toUtf8() + "/host";
    auto compatible = payload;
    if (compatible.value("kind") == "message" && !peers_.value(socket).capabilities.contains("extended-retention")) {
        const auto record = ChatHistory::forReader(compatible.value("record").toObject(), false);
        if (record.value("expires").toInteger() <= compatible.value("now").toInteger()) return true;
        compatible.insert("record", record);
    }
    const auto sealed = TlsIdentity::seal(QJsonDocument(compatible).toJson(QJsonDocument::Compact), hostChatKey_, context);
    return writeMessage(socket, {{"type", "chat"}, {"epoch", hostChatEpoch_}, {"data", QString::fromLatin1(sealed.toBase64())}});
}

bool LocalChannel::sendChatCommand(const QJsonObject& payload) {
    auto* c = chatClient(); return c && sendChatCommand(*c, payload);
}

bool LocalChannel::sendChatCommand(Client& c, const QJsonObject& payload) {
    if (!(c.accepted && c.chatKey.size() == 32)) return false;
    const auto context = "SquadSpeak/chat/v1/" + c.id.toUtf8() + '/' + c.chatEpoch.toUtf8() + '/' + ownId().toUtf8();
    const auto sealed = TlsIdentity::seal(QJsonDocument(payload).toJson(QJsonDocument::Compact), c.chatKey, context);
    return writeMessage(c.socket, {{"type", "chat"}, {"epoch", c.chatEpoch}, {"data", QString::fromLatin1(sealed.toBase64())}});
}

QJsonObject LocalChannel::hostHistory(const QJsonObject& query) {
    const auto cursorValue = query.value("cursor");
    const auto cursor = cursorValue.isUndefined() ? 0 : cursorValue.toInteger(-1);
    const auto directionValue = query.value("direction");
    const auto direction = directionValue.isUndefined() ? QStringLiteral("latest") : directionValue.toString();
    if (cursor < 0 || cursor > 9007199254740991LL || (!cursorValue.isUndefined()
        && (!cursorValue.isDouble() || cursorValue.toDouble(-1) != double(cursor)))
        || (direction != "latest" && direction != "older" && direction != "newer"))
        throw std::invalid_argument("History requires an integer cursor from 0 to 9007199254740991 and direction latest, older or newer.");
    if (!hosting_ || !history_) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "The local channel is not ready."));
    history_->expire(clock_());
    return history_->page(cursor, direction, !query.contains("extended") || query.value("extended").toBool());
}

bool LocalChannel::hostChat(QSslSocket* socket, const QJsonObject& message) {
    if (!history_) { socket->abort(); return false; }
    try {
        // A membership change can race an already encrypted submission. Send
        // the new key through authenticated TLS; retry keeps the request ID.
        if (message.value("epoch") != hostChatEpoch_) { sendChatKey(socket); return true; }
        const auto bytes = QByteArray::fromBase64Encoding(message.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!bytes) { socket->abort(); return false; }
        const auto sender = peers_.value(socket).id;
        const auto context = "SquadSpeak/chat/v1/" + channelId().toUtf8() + '/' + hostChatEpoch_.toUtf8() + '/' + sender.toUtf8();
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(TlsIdentity::open(bytes.decoded, hostChatKey_, context), &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) { socket->abort(); return false; }
        const auto payload = document.object();
        const auto kind = payload.value("kind").toString();
        if (!validMessageType(kind)) { socket->abort(); return false; }
        if (!knownChatKinds.contains(kind)) return true;
        if (payload.value("kind") == "history") {
            const auto request = payload.value("request").toString();
            if (QUuid(request).isNull() || !payload.contains("cursor") || !payload.contains("direction")) {
                socket->abort(); return false;
            }
            QJsonObject page;
            try {
                auto request = payload;
                request.insert("extended", peers_.value(socket).capabilities.contains("extended-retention"));
                page = hostHistory(request);
            }
            catch (const std::invalid_argument&) { socket->abort(); return false; }
            page.insert("request", request);
            return writeChat(socket, page);
        }
        history_->expire(clock_());
        const auto now = history_->time();
        if (payload.value("kind") == "imageGet" || payload.value("kind") == "imageChunk") return hostImage(socket, payload, now);
        if (payload.value("kind") != "send" && payload.value("kind") != "imageStart") { socket->abort(); return false; }
        const auto issued = payload.value("issued").toInteger(-1);
        if (issued < 0 || issued > now + 30000 || now - issued >= ChatHistory::lifetime
            || payload.value("issued").toDouble(-1) != double(issued))
            return writeChat(socket, {{"kind", "error"}, {"now", now}, {"error", tr("The message is too old. Check the draft again.")}});
        for (auto it = chatWindows_.begin(); it != chatWindows_.end();)
            if (now - it->first >= 10000) it = chatWindows_.erase(it); else ++it;
        auto& window = chatWindows_[sender];
        if (!window.second) window.first = now;
        if (++window.second > 10)
            return writeChat(socket, {{"kind", "error"}, {"now", now}, {"error", tr("Too many messages. Please wait briefly.")}});
        if (payload.value("kind") == "imageStart") return hostImage(socket, payload, now);
        const auto record = history_->append(sender, peers_.value(socket).name,
            payload.value("request").toString(), payload.value("text").toString(), now, {}, peers_.value(socket).avatar, qint64(messageLifetimeDays()) * ChatHistory::lifetime);
        // Acknowledgement and broadcast follow the successful atomic save.
        publishChat(record);
        return true;
    } catch (const std::exception& error) {
        // Keep drafts on the client; disk/key errors never become successful receipts.
        try { writeChat(socket, {{"kind", "error"}, {"now", std::max(history_->time(), clock_())}, {"error", QString::fromUtf8(error.what())}}); }
        catch (const std::exception&) { socket->abort(); }
        return false;
    }
}

bool LocalChannel::clientChat(Client& c, const QJsonObject& message) {
    try {
        QJsonObject payload;
        const bool keyMessage = message.value("type") == "chatKey";
        if (keyMessage) {
            const auto key = QByteArray::fromBase64Encoding(message.value("key").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            const auto epoch = message.value("epoch").toString();
            if (!key || key.decoded.size() != 32 || QUuid(epoch).isNull()) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat key."));
            c.chatKey = key.decoded; c.chatEpoch = epoch; payload = message;
        } else {
            if (message.value("epoch") != c.chatEpoch) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Unexpected chat key change."));
            const auto bytes = QByteArray::fromBase64Encoding(message.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!bytes) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat data."));
            const auto context = "SquadSpeak/chat/v1/" + c.id.toUtf8() + '/' + c.chatEpoch.toUtf8() + "/host";
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(TlsIdentity::open(bytes.decoded, c.chatKey, context), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat response."));
            payload = document.object();
            const auto kind = payload.value("kind").toString();
            if (!validMessageType(kind)) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat response."));
            if (!knownChatKinds.contains(kind)) return true;
        }
        const auto now = payload.value("now").toInteger(-1);
        if (now < 0 || now > 9007199254740991LL - ChatHistory::lifetime || payload.value("now").toDouble(-1) != double(now))
            throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat timestamp."));
        c.serverTime = std::max(now, c.chatClock.isValid() ? c.serverTime + c.chatClock.elapsed() : now);
        c.chatClock.restart();
        if (keyMessage) {
            const auto days = message.value("lifetimeDays").toInt(1);
            if (!ChatHistory::validLifetime(qint64(days) * ChatHistory::lifetime)
                || (message.contains("lifetimeDays") && message.value("lifetimeDays").toDouble() != days))
                throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid channel message lifetime."));
            c.messageLifetimeDays = days;
            emit hostsChanged();
            c.chatError.clear();
            if (c.id == chatHostId_) {
                if (!c.historyRequest.isEmpty()) {
                    const auto direction = c.historyDirection;
                    c.historyRequest.clear(); requestHistory(c, direction);
                } else if (c.messages.isEmpty()) requestHistory(c, "latest");
            }
            emit chatChanged();
            if (c.id == chatHostId_) {
                if (!downloadingImage_.isEmpty()) imageQueue_.prepend(downloadingImage_);
                downloadingImage_.clear(); downloadedImage_.clear(); downloadImage();
            }
            retryChat(c); return true;
        }
        const auto kind = payload.value("kind").toString();
        if (kind == "error") {
            c.historyRequest.clear();
            c.pending = {}; c.pendingImage.clear(); c.chatError = payload.value("error").toString().left(512); emit chatChanged(); return true;
        }
        if (kind == "imageOffset") {
            const auto offset = payload.value("offset").toInteger(-1);
            if (c.pending.isEmpty() || payload.value("request") != c.pending.value("request")) return true;
            if (offset < 0 || offset >= c.pendingImage.size()) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid image progress."));
            return sendChatCommand(c, {{"kind", "imageChunk"}, {"request", c.pending.value("request")}, {"offset", offset},
                {"data", QString::fromLatin1(c.pendingImage.mid(offset, 16384).toBase64())}});
        }
        if (kind == "imageData") return c.id != chatHostId_ || receiveImage(payload);
        if (kind == "imageError") {
            if (c.id != chatHostId_) return true;
            for (auto* socket : peers_.keys()) {
                const auto request = peers_.value(socket).remoteImageRequest;
                if (request.isEmpty() || request.value("hash").toString() != downloadingImage_) continue;
                peers_[socket].remoteImageRequest = {};
                writeMessage(socket, {{"type", "remoteImage"}, {"hostId", request.value("hostId")},
                    {"payload", QJsonObject{{"kind", "imageError"}, {"hash", downloadingImage_}}}});
            }
            downloadingImage_.clear(); downloadedImage_.clear(); c.chatError = payload.value("error").toString().left(512);
            emit chatChanged(); downloadImage(); return true;
        }
        if (kind == "time") { emit chatChanged(); return true; }
        if (kind == "history") {
            if (c.historyRequest.isEmpty() || payload.value("request").toString() != c.historyRequest) return true;
            const auto records = payload.value("records").toArray();
            if (!payload.value("records").isArray() || records.size() > ChatHistory::maximumPageMessages
                || QJsonDocument(payload).toJson(QJsonDocument::Compact).size() > ChatHistory::maximumPageBytes
                || !payload.value("older").isBool() || !payload.value("newer").isBool()
                || payload.value("direction").toString() != c.historyDirection)
                throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid history page."));
            const auto direction = c.historyDirection;
            const auto first = c.historyCursor;
            const auto last = c.historyCursor;
            qint64 previous = 0;
            for (const auto value : records) {
                const auto record = value.toObject();
                const auto sequence = record.value("sequence").toInteger();
                if (!ChatHistory::validMessage(record) || record.value("created").toInteger() > now || sequence <= previous
                    || (direction == "older" && sequence >= first) || (direction == "newer" && sequence <= last))
                    throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid history order."));
                previous = sequence;
            }
            c.historyRequest.clear();
            c.lastChatSequence = std::max(c.lastChatSequence, previous);
            if (direction == "latest") { c.messages = {}; c.hasNewer = false; }
            QJsonArray retained;
            for (const auto value : records) if (value.toObject().value("expires").toInteger() > c.serverTime) retained.append(value);
            if (direction == "older") {
                for (qsizetype i = retained.size(); i > 0; --i) c.messages.prepend(retained.at(i - 1));
                c.hasOlder = payload.value("older").toBool();
            } else {
                for (const auto value : retained) c.messages.append(value);
                c.hasNewer = payload.value("newer").toBool();
                if (direction == "latest") c.hasOlder = payload.value("older").toBool();
            }
            trimHistory(c, direction == "older");
            emit chatChanged(); downloadImage(); return true;
        }
        if (kind != "message") throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Unknown chat response."));
        const auto record = payload.value("record").toObject();
        if (!ChatHistory::validMessage(record) || record.value("created").toInteger() > now)
            throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid message from host."));
        const auto sequence = record.value("sequence").toInteger();
        const bool fresh = sequence > c.lastChatSequence;
        c.lastChatSequence = std::max(c.lastChatSequence, sequence);
        bool known = false;
        for (qsizetype i = 0; i < c.messages.size(); ++i) {
            auto previous = c.messages.at(i).toObject();
            if (previous.value("sequence") != record.value("sequence")) continue;
            auto content = record;
            for (const auto* key : {"name", "avatar", "avatarId"}) { previous.remove(key); content.remove(key); }
            if (previous != content) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Conflicting chat order."));
            c.messages[i] = record;
            known = true; break;
        }
        if (c.id == chatHostId_ && !c.historyRequest.isEmpty() && c.historyDirection == "older" && !known) c.hasNewer = true;
        if (c.id == chatHostId_ && !c.hasNewer && !known && record.value("expires").toInteger() > c.serverTime) {
            const auto position = std::lower_bound(c.messages.constBegin(), c.messages.constEnd(), sequence,
                [](const QJsonValue& value, qint64 order) { return value.toObject().value("sequence").toInteger() < order; });
            c.messages.insert(std::distance(c.messages.constBegin(), position), record);
            trimHistory(c, false);
        }
        if (record.value("sender") == ownId() && record.value("request") == c.pending.value("request")) {
            const auto text = c.pending.value("text").toString(); c.pending = {}; c.pendingImage.clear(); c.chatError.clear(); emit chatSent(text, c.id);
        }
        const auto event = record.value("event").toObject();
        if (fresh && !hostOnly_ && !remoteMode() && c.accepted && c.voice && c.id == joinedHostId_
            && record.value("sender") != ownId() && record.value("expires").toInteger() > c.serverTime
            && (event.isEmpty() || event.value("kind") == "announcement"))
            emit chatNotification(c.id, record.toVariantMap());
        emit chatChanged();
        return true;
    } catch (const std::exception& error) {
        c.chatError = QString::fromUtf8(error.what());
        if (c.socket) c.socket->abort();
        emit chatChanged(); return false;
    }
}

bool LocalChannel::requestHistory(Client& c, const QString& direction) {
    if (!(c.accepted && c.chatKey.size() == 32) || !c.historyRequest.isEmpty()) return false;
    c.historyDirection = direction;
    c.historyRequest = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto cursor = c.messages.isEmpty() || direction == "latest" ? 0
        : c.messages.at(direction == "older" ? 0 : c.messages.size() - 1).toObject().value("sequence").toInteger();
    c.historyCursor = cursor;
    if (!sendChatCommand(c, {{"kind", "history"}, {"request", c.historyRequest}, {"direction", direction}, {"cursor", cursor}})) {
        c.historyRequest.clear(); return false;
    }
    emit chatChanged(); return true;
}

bool LocalChannel::loadOlderMessages() {
    if (remoteMode()) return remoteAction("older");
    auto* c = chatClient(); return c && c->hasOlder && requestHistory(*c, "older");
}

void LocalChannel::retryChat(Client& c) {
    if (c.pending.isEmpty() || !(c.accepted && c.chatKey.size() == 32)) return;
    if (c.serverTime + c.chatClock.elapsed() >= c.pendingUntil) {
        c.pending = {}; c.pendingImage.clear(); c.chatError = tr("Delivery was not confirmed. Check the draft again."); emit chatChanged(); return;
    }
    sendChatCommand(c, c.pending);
}

bool LocalChannel::sendChat(const QString& text, const QByteArray& image) {
    auto* current = chatClient();
    if (!current) return false;
    auto& c = *current;
    if (remoteMode() || !(c.accepted && c.chatKey.size() == 32) || !c.pending.isEmpty() || (!ChatHistory::validText(text) && !(text.isEmpty() && !image.isEmpty()))
        || image.size() > ChatContent::maximumSourceBytes) {
        c.chatError = tr("Chat is not ready, delivery is in progress, or the text is empty or too long (maximum 16 KiB).");
        emit chatChanged(); return false;
    }
    if (!image.isEmpty() && std::any_of(clients_.begin(), clients_.end(), [](const auto& client) { return !client->pendingImage.isEmpty(); })) {
        c.chatError = tr("An image is already being sent. Wait for its receipt before sending another."); emit chatChanged(); return false;
    }
    const auto issued = c.serverTime + c.chatClock.elapsed();
    c.pendingImage = image;
    c.pending = {{"kind", image.isEmpty() ? "send" : "imageStart"}, {"request", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"text", text}, {"issued", issued}, {"size", image.size()}, {"hash", QString::fromLatin1(QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex())}};
    c.pendingUntil = issued + ChatHistory::lifetime;
    c.chatError.clear();
    try {
        if (!image.isEmpty()) {
            const auto reference = "attachment:" + c.pending.value("hash").toString();
            if (!text.contains(reference)) c.pending.insert("text", text + (text.isEmpty() ? "" : "\n\n") + "![Bild](" + reference + ")");
        }
        if (!ChatHistory::validText(c.pending.value("text").toString())) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Message with image reference is too long."));
        if (!sendChatCommand(c, c.pending)) { c.pending = {}; c.pendingImage.clear(); c.chatError = tr("Message could not be sent."); emit chatChanged(); return false; }
    } catch (const std::exception& error) {
        c.pending = {}; c.pendingImage.clear(); c.chatError = QString::fromUtf8(error.what()); emit chatChanged(); return false;
    }
    emit chatChanged(); return true;
}

void LocalChannel::publishChat(const QJsonObject& record) {
    for (auto* socket : peers_.keys())
        if (peers_.value(socket).joined && !peers_.value(socket).controller)
            writeChat(socket, {{"kind", "message"}, {"record", record}, {"now", history_->time()}});
}

void LocalChannel::publishMembership(const QString& kind, const QString& member, const QString& name) {
    const auto text = name + (kind == "joined" ? " joined." : kind == "kicked" ? " was kicked."
        : kind == "banned" ? " was banned from the channel." : " left.");
    publishSystem(text, {{"kind", kind}, {"member", member}, {"name", name}});
}

bool LocalChannel::sendSystemMessage(const QString& text) {
    if (!hosting() || !ChatHistory::validText(text)) return setStatus(tr("The local channel or message is not ready."), false);
    return publishSystem(text, {{"kind", "announcement"}});
}

bool LocalChannel::publishSystem(const QString& text, const QJsonObject& event) {
    if (!history_) return false;
    try {
        publishChat(history_->append(musicId(), botName_, QUuid::createUuid().toString(QUuid::WithoutBraces),
            text, clock_(), {}, "system", messageLifetimeDays() * ChatHistory::lifetime, event));
        botLastActive_ = clock_();
        if (botSleeping_) { botSleeping_ = false; broadcastRoster(); }
        return true;
    } catch (const std::exception& error) {
        return setStatus(tr("Channel event could not be saved: %1").arg(tr(error.what())), false);
    }
}

bool LocalChannel::hostImage(QSslSocket* socket, const QJsonObject& payload, qint64 now) {
    const auto kind = payload.value("kind").toString();
    if (kind == "imageGet") {
        try {
            const auto hash = payload.value("hash").toString();
            const auto png = history_->image(hash, now);
            const auto offset = payload.value("offset").toInteger(-1);
            if (offset < 0 || offset >= png.size() || payload.value("offset").toDouble(-1) != double(offset))
                throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid image section."));
            return writeChat(socket, {{"kind", "imageData"}, {"now", now}, {"hash", hash}, {"size", png.size()},
                {"offset", offset}, {"data", QString::fromLatin1(png.mid(offset, 16384).toBase64())}});
        } catch (const std::exception& error) {
            return writeChat(socket, {{"kind", "imageError"}, {"now", now}, {"error", QString::fromUtf8(error.what())}});
        }
    }
    auto& peer = peers_[socket];
    if (kind == "imageStart") {
        const auto size = payload.value("size").toInteger(-1);
        const auto request = payload.value("request").toString();
        if (size < 1 || size > ChatContent::maximumSourceBytes || payload.value("size").toDouble(-1) != double(size)
            || !validId(payload.value("hash").toString()) || QUuid(request).isNull()
            || QUuid(request).toString(QUuid::WithoutBraces) != request
            || (!payload.value("text").toString().isEmpty() && !ChatHistory::validText(payload.value("text").toString()))) {
            socket->abort(); return false;
        }
        if (peer.imageProcessing) return true;
        int active = 0;
        const auto* root = service_ ? service_ : this;
        const auto count = [&active](const LocalChannel& channel) {
            for (const auto& other : channel.peers_)
                if (other.imageProcessing || !other.imageRequest.isEmpty()) ++active;
        };
        count(*root); for (const auto& child : root->owned_) count(*child);
        if (active >= 4 && peer.imageRequest.isEmpty())
            return writeChat(socket, {{"kind", "error"}, {"now", now}, {"error", tr("Four images are currently being transferred. Please wait briefly.")}});
        peer.imageRequest = payload; peer.imageUpload.clear();
        QTimer::singleShot(120000, socket, [this, socket, request] {
            if (!peers_.contains(socket) || peers_.value(socket).imageRequest.value("request") != request) return;
            peers_[socket].imageRequest = {}; peers_[socket].imageUpload.clear();
            try { writeChat(socket, {{"kind", "error"}, {"now", history_ ? history_->time() : clock_()}, {"error", tr("Image transfer took too long.")}}); }
            catch (const std::exception&) { socket->abort(); }
        });
        return writeChat(socket, {{"kind", "imageOffset"}, {"now", now}, {"request", request}, {"offset", 0}});
    }
    const auto request = peer.imageRequest;
    const auto bytes = QByteArray::fromBase64Encoding(payload.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (request.isEmpty() || peer.imageProcessing || payload.value("request") != request.value("request") || !bytes || bytes.decoded.isEmpty()
        || bytes.decoded.size() > 16384 || payload.value("offset").toInteger(-1) != peer.imageUpload.size()
        || peer.imageUpload.size() + bytes.decoded.size() > request.value("size").toInteger()) { socket->abort(); return false; }
    peer.imageUpload.append(bytes.decoded);
    if (peer.imageUpload.size() < request.value("size").toInteger())
        return writeChat(socket, {{"kind", "imageOffset"}, {"now", now}, {"request", request.value("request")}, {"offset", peer.imageUpload.size()}});
    if (QString::fromLatin1(QCryptographicHash::hash(peer.imageUpload, QCryptographicHash::Sha256).toHex()) != request.value("hash")) {
        socket->abort(); return false;
    }
    peer.imageProcessing = true;
    return ChatContent::prepare(peer.imageUpload, socket, [this, socket, request](QByteArray png, QString error) {
        if (!peers_.contains(socket)) return;
        peers_[socket].imageProcessing = false;
        if (!history_ || !peers_.value(socket).joined
            || peers_.value(socket).imageRequest.value("request") != request.value("request")) return;
        const auto sender = peers_.value(socket).id, name = peers_.value(socket).name;
        peers_[socket].imageUpload.clear(); peers_[socket].imageRequest = {};
        try {
            if (png.isEmpty()) throw std::runtime_error(error.toStdString());
            auto text = request.value("text").toString();
            const auto storedHash = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
            text.replace("attachment:" + request.value("hash").toString(), "attachment:" + storedHash);
            publishChat(history_->append(sender, name, request.value("request").toString(), text, clock_(), png, peers_.value(socket).avatar, qint64(messageLifetimeDays()) * ChatHistory::lifetime));
        } catch (const std::exception& failure) {
            try { writeChat(socket, {{"kind", "error"}, {"now", history_->time()}, {"error", QString::fromUtf8(failure.what())}}); }
            catch (const std::exception&) { socket->abort(); }
        }
    });
}

bool LocalChannel::requestImage(const QString& hash) {
    if (!validId(hash) || (remoteMode() ? !remoteAllowed_ || !remoteView_.value("chatReady").toBool() : !chatReady())) return false;
    if (!visibleImage(hash)) return false;
    if (imageCache_.contains(hash) || downloadingImage_ == hash || imageQueue_.contains(hash)) return true;
    if (imageQueue_.size() >= 64) return false;
    imageQueue_.append(hash); downloadImage(); return true;
}

void LocalChannel::downloadImage() {
    if ((remoteMode() ? !remoteAllowed_ || !remoteView_.value("chatReady").toBool() : !chatReady()) || !downloadingImage_.isEmpty()) return;
    while (!imageQueue_.isEmpty()) {
        const auto hash = imageQueue_.takeFirst();
        if (!visibleImage(hash) || imageCache_.contains(hash)) continue;
        downloadingImage_ = hash; downloadedImage_.clear();
        sendImageRequest(hash, 0); return;
    }
}

QString LocalChannel::imageSource(const QString& hash) const {
    const auto* png = imageCache_.object(hash);
    if (!png) return {};
    return visibleImage(hash) ? "data:image/png;base64," + QString::fromLatin1(png->toBase64()) : QString{};
}

bool LocalChannel::receiveImage(const QJsonObject& payload) {
    if (payload.value("hash") != downloadingImage_ || downloadingImage_.isEmpty()) return true;
    const auto bytes = QByteArray::fromBase64Encoding(payload.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    const auto size = payload.value("size").toInteger(-1);
    if (!bytes || bytes.decoded.isEmpty() || bytes.decoded.size() > 16384 || size < 1 || size > ChatContent::maximumImageBytes
        || payload.value("offset").toInteger(-1) != downloadedImage_.size() || downloadedImage_.size() + bytes.decoded.size() > size)
        throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid image block."));
    downloadedImage_.append(bytes.decoded);
    if (downloadedImage_.size() < size)
        return sendImageRequest(downloadingImage_, downloadedImage_.size());
    const auto hash = downloadingImage_;
    if (QString::fromLatin1(QCryptographicHash::hash(downloadedImage_, QCryptographicHash::Sha256).toHex()) != hash)
        throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Image verification failed."));
    const auto dimensions = ChatContent::sanitizedImageSize(downloadedImage_);
    bool matches = false;
    for (const auto& record : (remoteMode() ? remoteView_.value("messages").toList() : messages())) {
        const auto attachment = record.toMap().value("image").toMap();
        if (attachment.value("hash").toString() == hash)
            matches = attachment.value("size").toLongLong() == size
                && QSize(attachment.value("width").toInt(), attachment.value("height").toInt()) == dimensions;
    }
    if (!matches) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Image does not match its host receipt."));
    imageCache_.insert(hash, new QByteArray(std::move(downloadedImage_)), int(size));
    downloadingImage_.clear(); downloadedImage_.clear();
    ++imageRevision_; emit imagesChanged(); downloadImage(); return true;
}

bool LocalChannel::sendImageRequest(const QString& hash, qint64 offset) {
    if (remoteMode()) return writeMessage(controller_, {{"type", "remoteImageGet"}, {"hash", hash}, {"offset", offset},
        {"hostId", remoteView_.value("chatHostId").toString()}});
    return sendChatCommand({{"kind", "imageGet"}, {"hash", hash}, {"offset", offset}});
}

void LocalChannel::sendRemoteImage(QSslSocket* socket) {
    if (!peers_.contains(socket)) return;
    const auto request = peers_.value(socket).remoteImageRequest;
    if (request.isEmpty()) return;
    const auto id = peers_.value(socket).id;
    if (!peers_.value(socket).joined || !control_.value("controllers").toObject().value(id).toObject().value("remote").toBool()) {
        peers_[socket].remoteImageRequest = {}; return;
    }
    const auto hash = request.value("hash").toString();
    const auto offset = request.value("offset").toInteger();
    QJsonObject payload{{"kind", "imageError"}, {"hash", hash}};
    if (!remoteMode() && chatReady() && request.value("hostId").toString() == chatHostId_ && requestImage(hash)) {
        const auto* png = imageCache_.object(hash);
        if (!png) return;
        if (offset < png->size()) payload = {{"kind", "imageData"}, {"hash", hash}, {"size", png->size()},
            {"offset", offset}, {"data", QString::fromLatin1(png->mid(offset, remoteChunkSize).toBase64())}};
    }
    peers_[socket].remoteImageRequest = {};
    writeMessage(socket, {{"type", "remoteImage"}, {"hostId", request.value("hostId")}, {"payload", payload}});
}

bool LocalChannel::loadNewerMessages() {
    if (remoteMode()) return remoteAction("newer");
    auto* c = chatClient(); return c && c->hasNewer && requestHistory(*c, "newer");
}

bool LocalChannel::refreshChat() {
    if (remoteMode()) return remoteAction("latest");
    auto* c = chatClient(); return c && requestHistory(*c, "latest");
}

void LocalChannel::trimHistory(Client& c, bool older) {
    while (c.messages.size() > maximumWindowMessages
        || QJsonDocument(c.messages).toJson(QJsonDocument::Compact).size() > maximumWindowBytes) {
        if (older) { c.messages.removeLast(); c.hasNewer = true; }
        else { c.messages.removeFirst(); c.hasOlder = true; }
    }
}

bool LocalChannel::chatReady() const { const auto* c = chatClient(); return c && c->accepted && c->chatKey.size() == 32; }

bool LocalChannel::chatPending() const { const auto* c = chatClient(); return c && !c->pending.isEmpty(); }

bool LocalChannel::historyLoading() const { const auto* c = chatClient(); return c && !c->historyRequest.isEmpty(); }

bool LocalChannel::hasOlderMessages() const { const auto* c = chatClient(); return c && c->hasOlder; }

bool LocalChannel::hasNewerMessages() const { const auto* c = chatClient(); return c && c->hasNewer; }

QString LocalChannel::chatError() const {
    const auto* c = chatClient();
    const auto text = c ? c->chatError : chatError_;
    return text.contains(QChar::Null) ? text : tr(text.toUtf8().constData());
}

int LocalChannel::chatLifetimeDays() const {
    const auto* c = chatClient();
    return c && c->accepted ? c->messageLifetimeDays : 0;
}

bool LocalChannel::visibleImage(const QString& hash) const {
    const auto visible = remoteMode() ? remoteView_.value("messages").toList() : messages();
    return std::any_of(visible.begin(), visible.end(), [&hash](const auto& value) {
        return value.toMap().value("image").toMap().value("hash").toString() == hash;
    });
}
