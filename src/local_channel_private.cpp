#include "local_channel.hpp"
#include "chat_content.hpp"

#include <QCborValue>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace {
QString conversationId(const QString& host, QString a, QString b) {
    if (b < a) std::swap(a, b);
    return QString::fromLatin1(QCryptographicHash::hash(("SquadSpeak/private/v1/" + host + '/' + a + '/' + b).toUtf8(), QCryptographicHash::Sha256).toHex());
}
QByteArray context(const QString& host, const QString& conversation, const QString& request, const QString& sender) {
    return ("SquadSpeak/private/v1/" + host + '/' + conversation + '/' + request + '/' + sender).toUtf8();
}
QByteArray decoded(const QJsonValue& value) {
    const auto bytes = QByteArray::fromBase64Encoding(value.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!bytes) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid chat data."));
    return bytes.decoded;
}
}

bool LocalChannel::validPrivateState(const QJsonObject& security) {
    const auto days = security.value("privateLifetimeDays").toInteger(1);
    if (days < 1 || days > ChatHistory::maximumLifetime / ChatHistory::lifetime
        || !ChatHistory::validLifetime(days * ChatHistory::lifetime)
        || (security.contains("privateLifetimeDays") && security.value("privateLifetimeDays").toDouble() != double(days))) return false;
    for (const auto* key : {"privatePeers", "privateStates"})
        if (security.contains(key) && !security.value(key).isObject()) return false;
    const auto records = security.value("privatePeers").toObject();
    for (auto it = records.begin(); it != records.end(); ++it) {
        const auto entry = it.value().toObject();
        const auto certificate = QSslCertificate(QByteArray::fromBase64(entry.value("certificate").toString().toLatin1()), QSsl::Der);
        if (!validId(it.key()) || TlsIdentity::peerId(certificate) != it.key() || !displayName(entry.value("name"))
            || !VoiceSession::validAvatar(entry.value("avatar").toString()) || !entry.value("contacts").isObject()) return false;
        const auto contacts = entry.value("contacts").toObject();
        for (auto contact = contacts.begin(); contact != contacts.end(); ++contact)
            if (contact.key() == it.key() || !records.contains(contact.key()) || contact.value() != true
                || records.value(contact.key()).toObject().value("contacts").toObject().value(it.key()) != true) return false;
    }
    const auto states = security.value("privateStates").toObject();
    for (auto it = states.begin(); it != states.end(); ++it) {
        const auto entry = it.value().toObject();
        const auto expires = entry.value("expires").toInteger(0);
        if (!validId(it.key()) || expires < 0 || expires > 9007199254740991LL
            || (entry.contains("expires") && entry.value("expires").toDouble(-1) != double(expires))
            || (entry.contains("blocks") && !entry.value("blocks").isObject())) return false;
        const auto blocks = entry.value("blocks").toObject();
        if (blocks.size() > 2) return false;
        for (auto block = blocks.begin(); block != blocks.end(); ++block)
            if (!records.contains(block.key()) || block.value() != true) return false;
    }
    return true;
}

void LocalChannel::maintainPrivate() {
    // Reopen at most one expired, evicted conversation per maintenance pass.
    auto states = security_.value("privateStates").toObject();
    for (auto it = states.begin(); it != states.end(); ++it) {
        auto state = it.value().toObject();
        if (state.value("expires").toInteger() <= 0 || state.value("expires").toInteger() > clock_()) continue;
        const auto id = it.key();
        privateHistory(id);
        state.insert("expires", 0); states.insert(id, state);
        auto next = security_; next.insert("privateStates", states);
        if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, next)) return;
        publishPrivate(id);
        break;
    }
    for (const auto& id : privateHistories_.keys()) {
        auto* history = privateHistories_.object(id);
        if (history->expire(clock_())) publishPrivate(id, {{"kind", "time"}, {"now", history->time()}});
    }
}

LocalChannel::Client* LocalChannel::privateTransport(const QString& device) const {
    Client* result = nullptr;
    for (const auto& c : clients_) if (c->privatePeer.isEmpty() && c->deviceId == device && c->accepted
        && c->socket && c->capabilities.contains("private-chat") && (!result || c->id < result->id)) result = c.get();
    return result;
}

bool LocalChannel::privateChat() const { const auto* c = chatClient(); return c && !c->privatePeer.isEmpty(); }
bool LocalChannel::privateBlocked() const { const auto* c = chatClient(); return c && (c->blockedByMe || c->blockedByPeer); }
bool LocalChannel::privateBlockedByMe() const { const auto* c = chatClient(); return c && c->blockedByMe; }
int LocalChannel::privateLifetimeDays() const {
    if (service_) return service_->privateLifetimeDays();
    const auto days = security_.value("privateLifetimeDays").toInt(1);
    return session_.supporterEnabled() ? days : std::min(days, 30);
}
bool LocalChannel::setPrivateLifetimeDays(int days) {
    if (service_) return service_->setPrivateLifetimeDays(days);
    return configureHost({{"privateLifetimeDays", days}});
}

QVariantList LocalChannel::privateChats() const {
    QVariantList result;
    for (const auto& c : clients_) {
        if (c->privatePeer.isEmpty()) continue;
        const auto* link = c->teamSpeak && c->accepted ? c.get() : privateTransport(c->deviceId);
        QString owner;
        for (const auto& saved : savedChannels_) if (saved.toObject().value("deviceId").toString() == c->deviceId) {
            owner = saved.toObject().value("ownerName").toString(); if (!owner.isEmpty()) break;
        }
        if (c->deviceId == ownId()) owner = hostDisplayName();
        result.append(QVariantMap{{"id", c->id}, {"name", c->name}, {"private", true}, {"peerId", c->privatePeer},
            {"deviceId", c->deviceId}, {"ownerName", owner}, {"address", c->address}, {"port", c->port},
            {"protocol", c->teamSpeak ? "teamspeak" : "squadspeak"}, {"online", link != nullptr}, {"peerOnline", link && c->privatePeerOnline}, {"access", link ? "accepted" : "offline"}, {"autoJoin", false},
            {"members", link ? c->members : QVariantList{}}, {"expires", c->privateExpires},
            {"hasMessages", c->privateExpires > (c->teamSpeak ? clock_() : c->chatClock.isValid() ? c->serverTime + c->chatClock.elapsed() : c->serverTime)},
            {"unread", c->unreadExpires > (c->teamSpeak ? clock_() : c->chatClock.isValid() ? c->serverTime + c->chatClock.elapsed() : c->serverTime)},
            {"blockedByMe", c->blockedByMe}, {"blocked", c->blockedByMe || c->blockedByPeer}});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.toMap().value("id").toString() < b.toMap().value("id").toString(); });
    return result;
}

bool LocalChannel::canPrivateChat(const QString& host, const QString& peer) const {
    if (remoteMode()) {
        const auto entries = remoteView_.value("privateChats").toList();
        const auto channels = remoteView_.value("channels").toList();
        QString device;
        for (const auto value : channels) if (value.toMap().value("id") == host) device = value.toMap().value("deviceId").toString();
        for (const auto value : entries) {
            const auto entry = value.toMap();
            if (entry.value("peerId") == peer && entry.value("deviceId") == device && entry.value("online").toBool()) return true;
        }
        return false;
    }
    const auto c = clients_.value(host);
    if (!c || !validId(peer) || peer == ownId()) return false;
    if (c->teamSpeak) {
        if (!teamSpeak_ || !teamSpeak_->connected || c->deviceId != teamSpeak_->id) return false;
        for (const auto& other : clients_) if (other->teamSpeak && other->accepted) {
            if (other->privatePeer == peer) return true;
            for (const auto& member : other->members) if (member.toMap().value("id") == peer) return true;
        }
        return false;
    }
    const auto privateClient = clients_.value(conversationId(c->deviceId, ownId(), peer));
    return privateClient && !privateClient->privateCertificate.isNull() && privateTransport(c->deviceId);
}
bool LocalChannel::openPrivateChat(const QString& host, const QString& peer) {
    if (hostOnly_) return false;
    if (remoteMode()) return remoteAction("openPrivate", {{"hostId", host}, {"peerId", peer}});
    if (!canPrivateChat(host, peer)) return false;
    if (clients_.value(host)->teamSpeak) return openTeamSpeakPrivate(host, peer);
    const auto c = clients_.value(conversationId(clients_.value(host)->deviceId, ownId(), peer));
    c->accepted = true; selectChat(c->id); return true;
}
bool LocalChannel::setPrivateBlocked(const QString& id, bool blocked) {
    if (remoteMode()) return remoteAction("blockPrivate", {{"conversation", id}, {"value", blocked}});
    const auto c = clients_.value(id);
    return c && !c->privatePeer.isEmpty() && sendChatCommand(*c, {{"kind", "block"}, {"value", blocked}});
}

void LocalChannel::rememberPrivatePeer(QSslSocket* socket, bool joining) {
    const auto peer = peers_.value(socket);
    if (!peer.joined || peer.controller || !peer.capabilities.contains("private-chat")) return;
    auto* root = service_ ? service_ : this;
    auto records = root->security_.value("privatePeers").toObject();
    auto record = records.value(peer.id).toObject();
    record.insert("name", peer.name); record.insert("avatar", peer.avatar);
    record.insert("certificate", QString::fromLatin1(socket->peerCertificate().toDer().toBase64()));
    auto contacts = record.value("contacts").toObject();
    for (auto* otherSocket : peers_.keys()) {
        const auto other = peers_.value(otherSocket);
        if (other.id == peer.id || !other.joined || other.controller || !other.capabilities.contains("private-chat")) continue;
        contacts.insert(other.id, true);
        auto otherRecord = records.value(other.id).toObject();
        auto reciprocal = otherRecord.value("contacts").toObject(); reciprocal.insert(peer.id, true);
        otherRecord.insert("contacts", reciprocal); records.insert(other.id, otherRecord);
    }
    record.insert("contacts", contacts); records.insert(peer.id, record);
    if (records != root->security_.value("privatePeers").toObject()) {
        auto next = root->security_; next.insert("privatePeers", records);
        if (!root->persist(root->approved_, root->attempts_, root->requestsAllowed_, root->endpointPins_, root->control_, next)) return;
    } else if (!joining) return;
    root->publishPrivateContacts(peer.id);
}

void LocalChannel::publishPrivateContacts(const QString& peer) {
    if (service_) { service_->publishPrivateContacts(peer); return; }
    const auto records = security_.value("privatePeers").toObject();
    QList<LocalChannel*> channels{this}; for (const auto& child : owned_) channels.append(child.get());
    for (auto* channel : channels) for (auto* socket : channel->peers_.keys()) {
        const auto recipient = channel->peers_.value(socket);
        if (!recipient.joined || recipient.controller || !recipient.capabilities.contains("private-chat")) continue;
        const auto contacts = records.value(recipient.id).toObject().value("contacts").toObject();
        const auto changed = peer.isEmpty() || recipient.id == peer ? contacts.keys()
            : contacts.contains(peer) ? QStringList{peer} : QStringList{};
        QJsonArray page;
        for (qsizetype index = 0; index < changed.size(); ++index) {
            page.append(privateContact(recipient.id, changed[index]));
            if (page.size() < 16 && index + 1 < changed.size()) continue;
            if (!channel->writeMessage(socket, {{"type", "privateChat"}, {"payload", QJsonObject{
                    {"kind", "contacts"}, {"contacts", page}, {"now", clock_()}}}})) break;
            page = {};
        }
    }
}

QJsonObject LocalChannel::privateContact(const QString& viewer, const QString& other) const {
    const auto records = security_.value("privatePeers").toObject();
    if (viewer == other || !records.value(viewer).toObject().value("contacts").toObject().contains(other)) return {};
    const auto record = records.value(other).toObject();
    const auto id = conversationId(ownId(), viewer, other);
    const auto state = security_.value("privateStates").toObject().value(id).toObject();
    const auto blocks = state.value("blocks").toObject();
    bool online = false;
    const auto connected = [&other](const LocalChannel& channel) {
        for (const auto& peer : channel.peers_) if (peer.id == other && peer.joined && !peer.controller) return true;
        return false;
    };
    online = connected(*this);
    for (const auto& child : owned_) online = online || connected(*child);
    return {{"id", id}, {"peer", other}, {"name", record.value("name")}, {"avatar", record.value("avatar")},
        {"certificate", record.value("certificate")}, {"online", online}, {"mine", blocks.value(viewer).toBool()}, {"theirs", blocks.value(other).toBool()},
        {"expires", state.value("expires").toInteger()}, {"lifetimeDays", privateLifetimeDays()}};
}

ChatHistory* LocalChannel::privateHistory(const QString& id) {
    if (auto* existing = privateHistories_.object(id)) return existing;
    if (!validId(id) || !QDir().mkpath(storageFile_ + ".private")) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be saved."));
    auto* history = new ChatHistory(storageFile_ + ".private/" + id, *identity_, clock_());
    privateHistories_.insert(id, history); return history;
}

void LocalChannel::publishPrivate(const QString& id, const QJsonObject& payload) {
    if (service_) { service_->publishPrivate(id, payload); return; }
    QList<LocalChannel*> channels{this}; for (const auto& child : owned_) channels.append(child.get());
    const auto records = security_.value("privatePeers").toObject();
    const auto hostId = ownId();
    for (auto* channel : channels) for (auto* socket : channel->peers_.keys()) {
        const auto& peer = channel->peers_.value(socket);
        if (!peer.joined || peer.controller || !peer.capabilities.contains("private-chat")) continue;
        const auto contacts = records.value(peer.id).toObject().value("contacts").toObject();
        for (auto it = contacts.begin(); it != contacts.end(); ++it) {
            if (conversationId(hostId, peer.id, it.key()) != id) continue;
            const auto contact = privateContact(peer.id, it.key());
            channel->writeMessage(socket, {{"type", "privateChat"}, {"payload", QJsonObject{{"kind", "contacts"}, {"contacts", QJsonArray{contact}}, {"now", clock_()}}}});
            if (!payload.isEmpty()) channel->writeChat(socket, payload, id);
            break;
        }
    }
}

void LocalChannel::hostPrivate(QSslSocket* socket, const QJsonObject& payload) {
    auto* root = service_ ? service_ : this;
    const auto sender = peers_.value(socket).id;
    const auto kind = payload.value("kind").toString();
    if (!validMessageType(kind)) { socket->abort(); return; }
    static const QSet<QString> kinds{"list", "block", "history", "imageGet", "imageChunk", "imageStart", "send"};
    if (!kinds.contains(kind)) return;
    const auto records = root->security_.value("privatePeers").toObject();
    const auto known = records.value(sender).toObject().value("contacts").toObject();
    if (kind == "list") {
        const auto after = payload.value("after").toString();
        if (!after.isEmpty() && !validId(after)) { socket->abort(); return; }
        QJsonArray contacts; QString next;
        for (auto it = known.begin(); it != known.end(); ++it) {
            if (it.key() <= after) continue;
            if (contacts.size() == 16) { next = contacts.last().toObject().value("peer").toString(); break; }
            contacts.append(root->privateContact(sender, it.key()));
        }
        writeMessage(socket, {{"type", "privateChat"}, {"payload", QJsonObject{{"kind", "contacts"}, {"contacts", contacts}, {"next", next}, {"now", clock_()}}}}); return;
    }
    const auto recipient = payload.value("peer").toString();
    const auto id = conversationId(root->ownId(), sender, recipient);
    if (!validId(recipient) || !known.contains(recipient) || payload.value("conversation") != id) { socket->abort(); return; }
    try {
        auto states = root->security_.value("privateStates").toObject();
        auto state = states.value(id).toObject();
        auto blocks = state.value("blocks").toObject();
        if (kind == "block") {
            if (!payload.value("value").isBool()) { socket->abort(); return; }
            if (payload.value("value").toBool()) blocks.insert(sender, true); else blocks.remove(sender);
            state.insert("blocks", blocks); states.insert(id, state);
            auto next = root->security_; next.insert("privateStates", states);
            if (!root->persist(root->approved_, root->attempts_, root->requestsAllowed_, root->endpointPins_, root->control_, next))
                throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be saved."));
            root->publishPrivate(id); return;
        }
        auto* history = root->privateHistory(id); history->expire(clock_());
        if (kind == "history") {
            for (const auto& id : {sender, recipient}) {
                const auto profile = records.value(id).toObject();
                history->updateProfile(id, profile.value("name").toString(), profile.value("avatar").toString());
            }
            const auto cursor = payload.value("cursor").toInteger(-1);
            if (QUuid(payload.value("request").toString()).isNull() || payload.value("cursor").toDouble(-1) != double(cursor)) { socket->abort(); return; }
            auto page = history->page(cursor, payload.value("direction").toString());
            page.insert("request", payload.value("request")); writeChat(socket, page, id); return;
        }
        if (kind == "imageGet") { hostImage(socket, payload, history->time(), history, id); return; }
        if (!blocks.isEmpty()) {
            if (peers_[socket].imageRequest.value("conversation") == id) {
                peers_[socket].imageRequest = {}; peers_[socket].imageUpload.clear();
            }
            writeChat(socket, {{"kind", "error"}, {"now", history->time()}, {"error", tr("Blocked")}}, id); return;
        }
        if (kind == "imageChunk") { hostImage(socket, payload, history->time(), history, id); return; }
        if (kind != "send" && kind != "imageStart") { socket->abort(); return; }
        const auto issued = payload.value("issued").toInteger(-1);
        const auto envelope = decoded(payload.value("sealed"));
        if (issued < 0 || issued > clock_() + 30000 || clock_() - issued >= ChatHistory::lifetime || envelope.isEmpty()
            || envelope.size() > 28 * 1024 || QUuid(payload.value("request").toString()).isNull()
            || payload.value("issued").toDouble(-1) != double(issued)) { socket->abort(); return; }
        auto& window = root->chatWindows_[sender];
        if (clock_() - window.first >= 10000) window = {clock_(), 0};
        if (++window.second > 10) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Too many messages. Please wait briefly."));
        if (kind == "imageStart") { hostImage(socket, payload, history->time(), history, id); return; }
        const auto receipt = history->append(sender, peers_.value(socket).name, payload.value("request").toString(), {}, clock_(), {}, peers_.value(socket).avatar,
            qint64(root->privateLifetimeDays()) * ChatHistory::lifetime, {}, envelope);
        root->publishPrivateReceipt(id, receipt, history->time());
    } catch (const std::exception& error) {
        writeChat(socket, {{"kind", "error"}, {"now", clock_()}, {"error", QString::fromUtf8(error.what())}}, id);
    }
}

void LocalChannel::publishPrivateReceipt(const QString& id, const QJsonObject& receipt, qint64 now) {
    auto states = security_.value("privateStates").toObject();
    auto state = states.value(id).toObject();
    state.insert("expires", std::max(state.value("expires").toInteger(), receipt.value("expires").toInteger()));
    states.insert(id, state); auto next = security_; next.insert("privateStates", states);
    if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, next))
        throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Chat history could not be saved."));
    publishPrivate(id, {{"kind", "message"}, {"record", receipt}, {"now", now}});
}

bool LocalChannel::clientPrivate(Client& transport, const QJsonObject& message) {
    try {
        const auto payload = message.value("payload").toObject();
        if (payload.value("kind") == "contacts") {
            const auto values = payload.value("contacts").toArray();
            const auto now = payload.value("now").toInteger(-1);
            if (!payload.value("contacts").isArray() || values.size() > 16 || now < 0 || now > 9007199254740991LL - ChatHistory::maximumLifetime
                || payload.value("now").toDouble(-1) != double(now)) return false;
            for (const auto value : values) {
                const auto entry = value.toObject(); const auto peer = entry.value("peer").toString(), id = entry.value("id").toString();
                const QSslCertificate certificate(decoded(entry.value("certificate")), QSsl::Der);
                const auto days = entry.value("lifetimeDays").toInt();
                if (!validId(peer) || peer == ownId() || id != conversationId(transport.deviceId, ownId(), peer)
                    || TlsIdentity::peerId(certificate) != peer || !displayName(entry.value("name")) || !VoiceSession::validAvatar(entry.value("avatar").toString())
                    || !entry.value("online").isBool() || !entry.value("mine").isBool() || !entry.value("theirs").isBool() || entry.value("expires").toInteger(-1) < 0 || entry.value("expires").toInteger() > now + ChatHistory::maximumLifetime
                    || entry.value("expires").toDouble(-1) != double(entry.value("expires").toInteger())
                    || entry.value("lifetimeDays").toDouble(-1) != days || !ChatHistory::validLifetime(qint64(days) * ChatHistory::lifetime)) return false;
                auto c = clients_.value(id);
                if (!c) { c = std::make_shared<Client>(); c->id = id; c->deviceId = transport.deviceId; c->privatePeer = peer; c->reconnectWanted = false; clients_.insert(id, c); }
                if (c->privatePeer != peer || c->deviceId != transport.deviceId) return false;
                const bool reconnecting = !c->accepted;
                c->privatePeerOnline = entry.value("online").toBool();
                c->presenceKnown = true; c->onlineIds = {ownId()};
                if (c->privatePeerOnline) c->onlineIds.append(peer);
                c->privateCertificate = certificate; c->name = entry.value("name").toString();
                c->blockedByMe = entry.value("mine").toBool(); c->blockedByPeer = entry.value("theirs").toBool();
                c->privateExpires = entry.value("expires").toInteger(); c->messageLifetimeDays = days;
                c->address = transport.address; c->port = transport.port; c->accepted = true;
                c->serverTime = std::max(now, c->chatClock.isValid() ? c->serverTime + c->chatClock.elapsed() : now); c->chatClock.restart();
                c->members = {QVariantMap{{"id", peer}, {"name", c->name}, {"avatarId", entry.value("avatar").toString()},
                    {"avatar", VoiceSession::avatarFallback(entry.value("avatar").toString())}, {"muted", true}, {"deafened", false}, {"available", c->privatePeerOnline}}};
                if (c->id == chatHostId_) {
                    c->chatError = c->blockedByMe || c->blockedByPeer ? tr("Blocked") : QString{};
                    if (reconnecting && c->historyRequest.isEmpty()) requestHistory(*c, "latest");
                    if (reconnecting) retryChat(*c);
                }
            }
            const auto next = payload.value("next").toString();
            if (!next.isEmpty()) {
                if (!validId(next) || values.isEmpty() || next != values.last().toObject().value("peer").toString()) return false;
                writeMessage(transport.socket, {{"type", "privateChat"}, {"payload", QJsonObject{{"kind", "list"}, {"after", next}}}});
            }
            emit hostsChanged(); emit chatChanged(); return true;
        }
        const auto c = clients_.value(message.value("conversation").toString());
        if (!c || c->privatePeer.isEmpty() || c->deviceId != transport.deviceId) return false;
        // Invalid peer content cannot tear down the shared voice connection.
        applyChat(*c, payload);
        return true;
    } catch (const std::exception& error) {
        transport.chatError = QString::fromUtf8(error.what()); emit chatChanged(); return false;
    }
}

bool LocalChannel::sendPrivate(Client& c, const QString& text, const QByteArray& image) {
    if (hostOnly_ || !privateTransport(c.deviceId) || !c.pending.isEmpty() || c.blockedByMe || c.blockedByPeer
        || (!ChatHistory::validText(text) && !(text.isEmpty() && !image.isEmpty())) || image.size() > ChatContent::maximumSourceBytes) return false;
    if (!image.isEmpty() && std::any_of(clients_.begin(), clients_.end(), [](const auto& other) {
        return !other->pendingImage.isEmpty() || other->pending.value("kind") == "preparing";
    })) return false;
    c.chatError.clear();
    const auto request = QUuid::createUuid().toString(QUuid::WithoutBraces);
    c.pending = {{"request", request}, {"kind", "preparing"}}; c.pendingText = text;
    const auto finish = [this, id = c.id, request, text, sourceHash = QString::fromLatin1(QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex())](QByteArray png, QString error) {
        const auto c = clients_.value(id);
        if (!c || c->pending.value("request") != request) return;
        try {
            if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
            if (c->blockedByMe || c->blockedByPeer || !privateTransport(c->deviceId)) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "The local channel is not ready."));
            auto messageText = text; QJsonObject content;
            const auto binding = context(c->deviceId, id, request, ownId());
            if (!png.isEmpty()) {
                const auto dimensions = ChatContent::sanitizedImageSize(png);
                const auto hash = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
                messageText.replace("attachment:" + sourceHash, "attachment:" + hash);
                messageText = ChatContent::embedImage(messageText, png);
                const auto key = TlsIdentity::newKey(); c->pendingImage = TlsIdentity::seal(png, key, binding + "/image");
                content.insert("image", QJsonObject{{"hash", hash}, {"size", png.size()}, {"width", dimensions.width()}, {"height", dimensions.height()},
                    {"key", QString::fromLatin1(key.toBase64())}, {"context", QString::fromLatin1(binding.toBase64())},
                    {"wireHash", QString::fromLatin1(QCryptographicHash::hash(c->pendingImage, QCryptographicHash::Sha256).toHex())}, {"wireSize", c->pendingImage.size()}});
            }
            if (!ChatHistory::validText(messageText)) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Message with image reference is too long."));
            content.insert("text", messageText);
            const auto envelope = identity_->sealFor(QCborValue::fromJsonValue(content).toCbor(), c->privateCertificate, binding);
            c->pending = {{"kind", png.isEmpty() ? "send" : "imageStart"}, {"request", request}, {"text", ""},
                {"issued", c->serverTime + c->chatClock.elapsed()}, {"sealed", QString::fromLatin1(envelope.toBase64())},
                {"size", c->pendingImage.size()}, {"hash", content.value("image").toObject().value("wireHash")}};
            c->pendingUntil = c->pending.value("issued").toInteger() + ChatHistory::lifetime;
            if (!sendChatCommand(*c, c->pending)) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Message could not be sent."));
        } catch (const std::exception& failure) {
            c->pending = {}; c->pendingImage.clear(); c->chatError = QString::fromUtf8(failure.what());
        }
        emit chatChanged();
    };
    if (image.isEmpty()) finish({}, {});
    else if (!ChatContent::prepare(image, this, finish)) { c.pending = {}; return false; }
    emit chatChanged(); return !c.pending.isEmpty();
}

QJsonObject LocalChannel::openPrivateRecord(Client& c, QJsonObject record) {
    if (!ChatHistory::validMessage(record) || !record.contains("sealed")) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid message from host."));
    const auto sender = record.value("sender").toString();
    if (sender != ownId() && sender != c.privatePeer) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid message from host."));
    const auto binding = context(c.deviceId, c.id, record.value("request").toString(), sender);
    const auto certificate = sender == ownId() ? identity_->configuration().localCertificate() : c.privateCertificate;
    const auto plain = identity_->openFrom(decoded(record.value("sealed")), certificate, binding);
    QCborParserError error;
    const auto content = QCborValue::fromCbor(plain, &error).toJsonValue().toObject();
    if (error.error != QCborError::NoError || !ChatHistory::validText(content.value("text").toString())) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid message from host."));
    const auto attachment = content.value("image").toObject();
    if (!attachment.isEmpty()) {
        if (attachment.value("wireHash") != record.value("image").toObject().value("hash")
            || attachment.value("wireSize") != record.value("image").toObject().value("size")
            || attachment.value("wireSize").toInteger() != attachment.value("size").toInteger() + 28
            || decoded(attachment.value("key")).size() != 32 || decoded(attachment.value("context")) != binding) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Image verification failed."));
        auto publicImage = attachment;
        for (const auto* field : {"key", "context", "wireHash", "wireSize"}) publicImage.remove(field);
        record.insert("image", publicImage);
    } else if (record.contains("image")) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Image verification failed."));
    record.remove("sealed"); record.insert("text", content.value("text"));
    if (!ChatHistory::validMessage(record)) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Invalid message from host."));
    if (!attachment.isEmpty() && c.id == chatHostId_) c.privateImages.insert(attachment.value("hash").toString(), attachment);
    return record;
}
