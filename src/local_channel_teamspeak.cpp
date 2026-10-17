#include "local_channel.hpp"
#include "teamspeak_bridge/teamspeak_bridge.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QUrl>
#include <QUuid>
#include <QtEndian>
#include <algorithm>

namespace {
QString tsId(const QString& server, const QString& kind, const QString& value) {
    return QString::fromLatin1(QCryptographicHash::hash((server + '/' + kind + '/' + value).toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString tsEndpoint(const QString& address, quint16 port) {
    return (address.contains(':') ? '[' + address + ']' : address) + ':' + QString::number(port);
}
QString tsName(const QString& raw) {
    // TeamSpeak names may exceed our display-name limit. Keep the protocol IDs intact.
    auto points = raw.toUcs4();
    for (auto& point : points) {
        const auto category = QChar::category(point);
        if (category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph) point = ' ';
    }
    const auto name = QString::fromUcs4(points.constData(), std::min<qsizetype>(64, points.size())).trimmed();
    return name.isEmpty() ? QStringLiteral("TeamSpeak") : name;
}
}

LocalChannel::TeamSpeak::~TeamSpeak() { if (handle) ss_ts_close(handle); }

bool LocalChannel::teamSpeakChat() const { const auto* c = chatClient(); return c && c->teamSpeak; }

bool LocalChannel::saveTeamSpeakIdentity(const QJsonObject& identity) {
    const QByteArray context = "SquadSpeak/teamspeak-identity/v1";
    try {
        const auto bytes = QJsonDocument(identity).toJson(QJsonDocument::Compact);
        auto next = security_;
        next.insert("teamSpeakIdentity", QString::fromLatin1(TlsIdentity::seal(bytes,
            identity_->deriveKey(context, "identity"), context).toBase64()));
        return persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, next);
    } catch (const std::exception& e) { return setStatus(QString::fromUtf8(e.what()), false); }
}

bool LocalChannel::addTeamSpeak(const QString& endpoint, const QString& password) {
    if (!ready() || hostOnly_ || service_ || remoteMode()) return false;
    const QUrl url(endpoint.startsWith("ts3://") ? endpoint : "ts3://" + endpoint, QUrl::StrictMode);
    const auto address = canonicalEndpointHost(url.host());
    const auto port = url.port(9987);
    if (!url.isValid() || url.scheme() != "ts3" || address.isEmpty() || port < 1 || port > 65535
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || (!url.path().isEmpty() && url.path() != "/")
        || password.toUtf8().size() > 1024) return setStatus(tr("The channel address or device identity is invalid."), false);
    const auto device = tsId("ts3", "server", tsEndpoint(address, quint16(port)));
    if (teamSpeak_ && teamSpeak_->id == device) return true;
    QString saved;
    for (auto it = savedChannels_.begin(); it != savedChannels_.end(); ++it)
        if (it.value().toObject().value("protocol") == "teamspeak" && it.value().toObject().value("deviceId") == device) { saved = it.key(); break; }
    if (saved.isEmpty()) {
        saved = tsId(device, "channel", "0");
        auto next = savedChannels_;
        next.insert(saved, QJsonObject{{"protocol", "teamspeak"}, {"deviceId", device}, {"tsChannel", "0"},
            {"name", tsName(address)}, {"address", address}, {"port", port}, {"autoJoin", false}, {"lastJoined", 0}});
        if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &next)) return false;
    }
    return joinTeamSpeak(saved, password, !password.isEmpty());
}

bool LocalChannel::teamSpeakCommand(const QJsonObject& command) {
    if (!teamSpeak_ || !teamSpeak_->handle) return false;
    const auto bytes = QJsonDocument(command).toJson(QJsonDocument::Compact);
    return ss_ts_command(teamSpeak_->handle, reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size())) != 0;
}

bool LocalChannel::joinTeamSpeak(const QString& id, const QString& suppliedPassword, bool remember) {
    if (!ready() || hostOnly_ || service_ || remoteMode()) return false;
    const auto entry = savedChannels_.value(id).toObject();
    if (entry.value("protocol") != "teamspeak") return false;
    const auto device = entry.value("deviceId").toString();
    auto c = clients_.value(id);
    if (!c) {
        c = std::make_shared<Client>(); c->id = id; c->teamSpeak = true; c->deviceId = device;
        c->address = entry.value("address").toString(); c->port = quint16(entry.value("port").toInt());
        c->name = entry.value("name").toString(); c->teamSpeakChannel = entry.value("tsChannel").toString();
        c->reconnectWanted = false; clients_.insert(id, c);
    }
    if (teamSpeak_ && teamSpeak_->id == device && !c->passwordRequired && (joinedHostId_ == id || teamSpeak_->requested == id)) {
        selectChat(id); return true;
    }
    QString password = suppliedPassword;
    try {
        if (password.isEmpty() && security_.value("saved").toObject().contains(id)) {
            const auto context = passwordContext(id);
            password = QString::fromUtf8(TlsIdentity::open(QByteArray::fromBase64(security_.value("saved").toObject().value(id).toString().toLatin1()),
                identity_->deriveKey(context, "channel password"), context));
        }
        selectChat(id);
        if (teamSpeak_ && teamSpeak_->id == device && teamSpeak_->connected) {
            if (entry.value("password").toBool() && password.isEmpty()) {
                c->passwordRequired = true; emit stateChanged(); return true;
            }
            if (!teamSpeakCommand({{"action", "join"}, {"channel", c->teamSpeakChannel}, {"password", password}, {"request", "join:" + id}})) return false;
            teamSpeak_->requested = id; c->passwordRequired = false;
            c->password = password; c->rememberPassword = remember;
            return true;
        }
        leaveChannel(true);
        QString serverPassword = suppliedPassword;
        if (serverPassword.isEmpty() && security_.value("saved").toObject().contains(device)) {
            const auto context = passwordContext(device);
            serverPassword = QString::fromUtf8(TlsIdentity::open(QByteArray::fromBase64(security_.value("saved").toObject().value(device).toString().toLatin1()),
                identity_->deriveKey(context, "channel password"), context));
        }
        QJsonObject identity;
        const QByteArray context = "SquadSpeak/teamspeak-identity/v1";
        if (security_.value("teamSpeakIdentity").isString()) {
            identity = QJsonDocument::fromJson(TlsIdentity::open(QByteArray::fromBase64(security_.value("teamSpeakIdentity").toString().toLatin1()),
                identity_->deriveKey(context, "identity"), context)).object();
        } else {
            QByteArray buffer(8192, Qt::Uninitialized);
            const auto size = ss_ts_identity(reinterpret_cast<uint8_t*>(buffer.data()), size_t(buffer.size()));
            if (size <= 0) return setStatus(tr("The channel address or device identity is invalid."), false);
            identity = QJsonDocument::fromJson(buffer.left(size)).object();
            if (!saveTeamSpeakIdentity(identity)) return false;
        }
        if (identity.isEmpty()) return setStatus(tr("The channel address or device identity is invalid."), false);
        auto connection = std::make_shared<TeamSpeak>();
        connection->id = device; connection->address = c->address; connection->port = c->port;
        connection->requested = id;
        QJsonObject options{{"address", tsEndpoint(c->address, c->port)}, {"name", session_.userName()}, {"identity", identity},
            {"password", serverPassword}, {"serverKey", security_.value("teamSpeakPins").toObject().value(device).toString()}, {"inputMuted", session_.muted() || !session_.available() || !session_.inputReady()},
            {"outputMuted", session_.deafened() || !session_.outputReady()}};
        // Initial connection enters the server's default channel. Subsequent moves use channel IDs.
        const auto bytes = QJsonDocument(options).toJson(QJsonDocument::Compact);
        connection->handle = ss_ts_open(reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()));
        if (!connection->handle) return setStatus(tr("Connection failed: %1").arg("TeamSpeak"), false);
        teamSpeak_ = std::move(connection);
        c->passwordRequired = false; c->voice = true; c->access = "connecting";
        c->password = serverPassword; c->rememberPassword = remember;
        joinedHostId_ = id; selectChat(id);
        teamSpeakTimer_.start(10);
        emit stateChanged(); emit hostsChanged();
        return true;
    } catch (const std::exception& e) { return setStatus(QString::fromUtf8(e.what()), false); }
}

void LocalChannel::disconnectTeamSpeak() {
    if (!teamSpeak_) return;
    teamSpeakTimer_.stop();
    teamSpeak_.reset();
    if (const auto active = clients_.value(joinedHostId_); active && active->teamSpeak) joinedHostId_.clear();
    for (const auto& c : clients_) if (c->teamSpeak) {
        c->accepted = false; c->voice = false; c->access = "offline"; c->members.clear(); c->onlineIds.clear();
        c->pending = {}; c->passwordRequired = false;
    }
    emit chatChanged(); emit participantsChanged(); emit hostsChanged(); emit stateChanged();
}

void LocalChannel::pollTeamSpeak() {
    if (!teamSpeak_) return;
    const auto connection = teamSpeak_;
    auto& buffer = connection->buffer;
    // Limit each GUI dispatch. Audio packets cannot indefinitely starve input or paint events.
    for (int i = 0; i < 128 && teamSpeak_ == connection; ++i) {
        const auto size = ss_ts_next(connection->handle, reinterpret_cast<uint8_t*>(buffer.data()), size_t(buffer.size()));
        if (size <= 0) break;
        if (buffer[0] == 1) teamSpeakEvent(QJsonDocument::fromJson(buffer.mid(1, size - 1)).object());
        else if (buffer[0] == 2 && size > 6 && connection->connected && (buffer[5] == 4 || buffer[5] == 5)) {
            const auto clientId = QString::number(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(buffer.constData() + 1)));
            const auto c = clients_.value(joinedHostId_);
            if (!c || !c->teamSpeak || !c->voice || session_.deafened()) continue;
            for (const auto& value : c->members) {
                const auto member = value.toMap();
                if (member.value("tsClient") == clientId && member.value("id").toString() != ownId()) {
                    const auto sequence = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(buffer.constData() + 3));
                    const auto now = controlClock_.elapsed();
                    const auto old = connection->audioSequences.constFind(clientId);
                    int missing = 0;
                    if (old != connection->audioSequences.cend() && now - old->second < 2000) {
                        const auto advance = quint16(sequence - old->first);
                        if (advance == 0 || advance > 32767) break;
                        missing = std::min(10, int(advance) - 1);
                    }
                    connection->audioSequences.insert(clientId, {sequence, now});
                    emit audioReceived(member.value("id").toString(), buffer.mid(6, size - 6), missing); break;
                }
            }
        }
    }
}

void LocalChannel::teamSpeakState(const QJsonObject& state) {
    const auto connection = teamSpeak_;
    const bool first = !connection->connected;
    const auto self = state.value("self").toString();
    const auto active = state.value("channel").toString();
    const auto serverName = tsName(state.value("server").toObject().value("name").toString());
    connection->connected = true; connection->state = state; connection->name = serverName;
    const auto previous = joinedHostId_;
    const auto requested = connection->requested;
    if (first) {
        if (auto initial = clients_.value(requested); initial && initial->rememberPassword) {
            Client credential; credential.id = connection->id;
            if (!savePassword(credential, initial->password, true)) { disconnectTeamSpeak(); return; }
        }
        auto pins = security_.value("teamSpeakPins").toObject();
        const auto uid = state.value("server").toObject().value("uid").toString();
        if (!uid.isEmpty() && !pins.contains(connection->id)) {
            pins.insert(connection->id, uid); auto security = security_; security.insert("teamSpeakPins", pins);
            if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security)) { disconnectTeamSpeak(); return; }
        }
    }
    connection->active = tsId(connection->id, "channel", active);
    joinedHostId_ = connection->active;
    QJsonObject next = savedChannels_;
    QSet<QString> seen;
    const auto channels = state.value("channels").toArray();
    const auto members = state.value("members").toArray();
    QSet<QString> connectedClients;
    for (const auto& member : members) connectedClients.insert(member.toObject().value("id").toString());
    for (auto it = connection->audioSequences.begin(); it != connection->audioSequences.end();)
        if (!connectedClients.contains(it.key())) it = connection->audioSequences.erase(it); else ++it;
    for (const auto& value : channels) {
        const auto info = value.toObject();
        const auto channel = info.value("id").toString();
        const auto id = tsId(connection->id, "channel", channel);
        seen.insert(id);
        auto entry = next.value(id).toObject();
        entry.insert("protocol", "teamspeak"); entry.insert("deviceId", connection->id); entry.insert("tsChannel", channel);
        entry.insert("parent", tsId(connection->id, "channel", info.value("parent").toString()));
        entry.insert("order", info.value("order")); entry.insert("password", info.value("password"));
        entry.insert("name", tsName(info.value("name").toString())); entry.insert("ownerName", serverName);
        entry.insert("address", connection->address); entry.insert("port", connection->port);
        if (!entry.contains("autoJoin")) entry.insert("autoJoin", false);
        if (id == joinedHostId_ && (first || previous != id)) entry.insert("lastJoined", clock_());
        else if (!entry.contains("lastJoined")) entry.insert("lastJoined", 0);
        next.insert(id, entry);
        auto c = clients_.value(id);
        if (!c) { c = std::make_shared<Client>(); c->id = id; clients_.insert(id, c); }
        c->teamSpeak = true; c->deviceId = connection->id; c->teamSpeakChannel = channel;
        c->name = entry.value("name").toString(); c->address = connection->address; c->port = connection->port;
        c->accepted = true; c->voice = id == joinedHostId_; c->access = "accepted"; c->reconnectWanted = false;
        c->messageLifetimeDays = 0; c->presenceKnown = true;
        c->teamSpeakCanSend = state.value("canSend").toBool() && (info.value("codec").toInt() == 4 || info.value("codec").toInt() == 5);
        c->members.clear(); c->onlineIds.clear();
        for (const auto& raw : members) {
            const auto member = raw.toObject();
            const auto tsClient = member.value("id").toString();
            const auto uid = member.value("uid").toString();
            const auto peer = tsClient == self ? ownId() : tsId(connection->id, "member", uid.isEmpty() ? tsClient : uid);
            c->onlineIds.append(peer);
            if (member.value("channel") != channel) continue;
            c->members.append(QVariantMap{{"id", peer}, {"tsClient", tsClient}, {"tsUid", uid}, {"name", tsName(member.value("name").toString())},
                {"avatarId", "mossling"}, {"avatar", "mossling"}, {"available", true}, {"muted", member.value("inputMuted").toBool()},
                {"deafened", member.value("outputMuted").toBool()}, {"sleeping", false}});
        }
        if (c->voice) {
            c->audioBitrate = 0; // TS3 does not report a per-listener receive bitrate.
            c->status = (info.value("codec").toInt() != 4 && info.value("codec").toInt() != 5)
                ? tr("Unsupported audio codec") : !state.value("talkAllowed").toBool(true)
                ? tr("Speaking is not allowed in this channel.") : QString{};
        }
    }
    for (auto it = next.begin(); it != next.end();) {
        const auto entry = it.value().toObject();
        if (entry.value("protocol") == "teamspeak" && entry.value("deviceId") == connection->id && !seen.contains(it.key())) {
            const auto id = it.key(); it = next.erase(it); clients_.remove(id);
        } else ++it;
    }
    if (next != savedChannels_) persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &next);
    if (chatHostId_ == previous || !clients_.contains(chatHostId_)) selectChat(joinedHostId_);
    for (const auto& c : clients_) if (c->teamSpeak && c->deviceId == connection->id && !c->privatePeer.isEmpty()) {
        c->accepted = true; c->access = "accepted"; c->teamSpeakPeer.clear();
        for (const auto& raw : members) {
            const auto m = raw.toObject();
            const auto uid = m.value("uid").toString();
            const auto peer = tsId(connection->id, "member", uid.isEmpty() ? m.value("id").toString() : uid);
            if (peer == c->privatePeer) { c->teamSpeakPeer = m.value("id").toString(); c->name = tsName(m.value("name").toString()); }
        }
        c->privatePeerOnline = !c->teamSpeakPeer.isEmpty();
    }
    if (const auto selected = clients_.value(chatHostId_)) status_ = selected->status;
    if (first) { automaticAttempt_ = false; emit channelEvent("join"); }
    if (connection->requested == joinedHostId_) connection->requested.clear();
    emit hostsChanged(); emit participantsChanged(); emit stateChanged(); emit chatChanged();
    if (first && requested != joinedHostId_ && savedChannels_.contains(requested)) {
        connection->requested.clear(); joinTeamSpeak(requested);
    }
}

void LocalChannel::teamSpeakEvent(const QJsonObject& event) {
    if (!teamSpeak_) return;
    const auto type = event.value("type").toString();
    if (type == "state") { teamSpeakState(event); return; }
    if (type == "identity") { if (event.value("value").isObject()) saveTeamSpeakIdentity(event.value("value").toObject()); return; }
    if (type == "reconnecting") {
        teamSpeak_->connected = false; teamSpeak_->audioSequences.clear();
        for (const auto& c : clients_) if (c->teamSpeak) { c->accepted = false; c->access = "connecting"; c->members.clear(); }
        emit stateChanged(); emit hostsChanged(); emit participantsChanged(); return;
    }
    if (type == "error" || type == "closed") {
        const auto text = event.value("message").toString();
        const auto selected = clients_.value(chatHostId_);
        if (type == "closed") {
            const bool advance = automaticAttempt_;
            disconnectTeamSpeak();
            if (advance) QTimer::singleShot(0, this, &LocalChannel::tryAutoJoin);
        }
        if (selected && event.value("passwordRequired").toBool()) selected->passwordRequired = true;
        setStatus(text, false); emit stateChanged(); return;
    }
    if (type == "result") {
        const auto request = event.value("request").toString();
        const bool okay = event.value("ok").toBool();
        if (request.isEmpty()) { if (!okay) setStatus(event.value("error").toString(), false); return; }
        if (request.startsWith("join:")) {
            const auto c = clients_.value(request.mid(5));
            if (okay && c && c->rememberPassword) savePassword(*c, c->password, true);
            if (!okay) {
                if (c) { c->chatError = event.value("error").toString(); c->passwordRequired = event.value("passwordRequired").toBool(); }
            }
            teamSpeak_->requested.clear(); emit chatChanged(); emit stateChanged(); return;
        }
        for (const auto& c : clients_) {
            if (!c->teamSpeak || c->pending.value("request") != request) continue;
            if (okay) {
                const auto now = clock_();
                QJsonObject record{{"sequence", ++teamSpeak_->sequence}, {"sender", ownId()}, {"name", session_.userName()},
                    {"avatarId", session_.avatar()}, {"request", request}, {"created", now}, {"expires", now + ChatHistory::lifetime},
                    {"text", c->pending.value("text")}};
                c->messages.append(record); c->privateExpires = now + ChatHistory::lifetime;
                while (c->messages.size() > maximumWindowMessages) c->messages.removeFirst();
                emit chatSent(c->pending.value("text").toString(), c->id);
            } else c->chatError = event.value("error").toString();
            c->pending = {}; emit chatChanged(); emit hostsChanged(); break;
        }
        return;
    }
    if (type != "message" || event.value("fromId") == teamSpeak_->state.value("self")) return;
    const auto mode = event.value("mode").toString();
    auto id = tsId(teamSpeak_->id, "channel", event.value("channel").toString());
    auto uid = event.value("fromUid").toString();
    if (uid.isEmpty()) {
        for (const auto& value : teamSpeak_->state.value("members").toArray()) {
            const auto member = value.toObject();
            if (member.value("id") == event.value("fromId")) {
                uid = member.value("uid").toString();
                break;
            }
        }
    }
    const auto peer = tsId(teamSpeak_->id, "member", uid.isEmpty() ? event.value("fromId").toString() : uid);
    if (mode == "private") {
        id = tsId(teamSpeak_->id, "private", peer);
        auto c = clients_.value(id);
        if (!c) {
            c = std::make_shared<Client>(); c->id = id; c->teamSpeak = true; c->deviceId = teamSpeak_->id;
            c->privatePeer = peer; c->name = tsName(event.value("fromName").toString()); c->teamSpeakPeer = event.value("fromId").toString();
            c->accepted = true; c->privatePeerOnline = true; c->address = teamSpeak_->address; c->port = teamSpeak_->port;
            c->access = "accepted"; c->reconnectWanted = false; c->messageLifetimeDays = 0; clients_.insert(id, c);
        }
    } else if (mode == "server") id = joinedHostId_;
    auto c = clients_.value(id);
    if (!c || !c->teamSpeak) return;
    const auto text = event.value("text").toString();
    if (!ChatHistory::validText(text)) return;
    const auto now = clock_();
    c->messages.append(QJsonObject{{"sequence", ++teamSpeak_->sequence}, {"sender", peer}, {"name", tsName(event.value("fromName").toString())},
        {"avatarId", "mossling"}, {"request", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"created", now}, {"expires", now + ChatHistory::lifetime}, {"text", text}});
    c->privateExpires = now + ChatHistory::lifetime;
    while (c->messages.size() > maximumWindowMessages) c->messages.removeFirst();
    if (c->voice) emit chatNotification(c->id, c->messages.last().toObject().toVariantMap());
    emit chatChanged(); emit hostsChanged();
}

bool LocalChannel::sendTeamSpeakChat(Client& c, const QString& text) {
    if (!c.accepted || !teamSpeak_ || !teamSpeak_->connected || !c.pending.isEmpty() || !ChatHistory::validText(text)
        || text.toUtf8().size() > 1024 || (!c.privatePeer.isEmpty() && c.teamSpeakPeer.isEmpty()) || (c.privatePeer.isEmpty() && c.id != joinedHostId_)) return false;
    const auto request = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject command{{"action", "chat"}, {"target", c.privatePeer.isEmpty() ? "channel" : "private"},
        {"client", c.teamSpeakPeer.toInt()}, {"text", text}, {"request", request}};
    if (!teamSpeakCommand(command)) return false;
    c.pending = {{"request", request}, {"text", text}}; c.chatError.clear(); emit chatChanged(); return true;
}

bool LocalChannel::openTeamSpeakPrivate(const QString& host, const QString& peer) {
    const auto source = clients_.value(host);
    if (!source || !source->teamSpeak || !teamSpeak_ || source->deviceId != teamSpeak_->id || !teamSpeak_->connected || peer == ownId()) return false;
    const auto id = tsId(teamSpeak_->id, "private", peer);
    auto c = clients_.value(id);
    if (!c) {
        QVariantMap member;
        for (const auto& other : clients_) if (other->teamSpeak)
            for (const auto& value : other->members) if (value.toMap().value("id") == peer) member = value.toMap();
        if (member.isEmpty()) return false;
        c = std::make_shared<Client>(); c->id = id; c->teamSpeak = true; c->deviceId = teamSpeak_->id;
        c->privatePeer = peer; c->teamSpeakPeer = member.value("tsClient").toString(); c->name = member.value("name").toString();
        c->accepted = true; c->privatePeerOnline = true; c->reconnectWanted = false; c->access = "accepted";
        c->address = teamSpeak_->address; c->port = teamSpeak_->port; c->messageLifetimeDays = 0;
        clients_.insert(id, c);
    }
    selectChat(id); return true;
}
