#include "local_channel.hpp"
#include "external_voice.hpp"
#include "chat_content.hpp"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QUrl>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <opus.h>

namespace {
QString tsError(const QString& message) {
    return message == "PermissionsClientInsufficient"
        ? QCoreApplication::translate("LocalChannel", "Insufficient permissions.") : message;
}
QString externalId(const QString& server, const QString& kind, const QString& value) {
    return QString::fromLatin1(QCryptographicHash::hash((server + '/' + kind + '/' + value).toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString externalEndpoint(const QString& address, quint16 port) {
    return (address.contains(':') ? '[' + address + ']' : address) + ':' + QString::number(port);
}
QString externalName(const QString& raw) {
    // External names may exceed our display-name limit. Keep the protocol IDs intact.
    auto points = raw.toUcs4();
    for (auto& point : points) {
        const auto category = QChar::category(point);
        if (category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph) point = ' ';
    }
    const auto name = QString::fromUcs4(points.constData(), std::min<qsizetype>(64, points.size())).trimmed();
    return name.isEmpty() ? QStringLiteral("Unknown") : name;
}
}

LocalChannel::ExternalVoice::~ExternalVoice() { if (handle) ss_ts_close(handle); }

bool LocalChannel::externalChat() const { const auto* c = chatClient(); return c && c->external(); }

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
    return addExternal("teamspeak", endpoint, password);
}

bool LocalChannel::addMumble(const QString& endpoint, const QString& password) {
    return addExternal("mumble", endpoint, password);
}

bool LocalChannel::addExternal(const QString& providerId, const QString& endpoint, const QString& password) {
    if (!ready() || hostOnly_ || service_ || remoteMode()) return false;
    const auto* provider = externalVoiceProvider(providerId);
    if (!provider) return false;
    const auto scheme = QString::fromLatin1(provider->scheme);
    const auto prefix = scheme + "://";
    const QUrl url(endpoint.startsWith(prefix) ? endpoint : prefix + endpoint, QUrl::StrictMode);
    const auto address = canonicalEndpointHost(url.host());
    const auto port = url.port(provider->port);
    if (!url.isValid() || url.scheme() != scheme || address.isEmpty() || port < 1 || port > 65535
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || (!url.path().isEmpty() && url.path() != "/")
        || password.toUtf8().size() > 1024) return setStatus(tr("The channel address or device identity is invalid."), false);
    const auto device = externalId(scheme, "server", externalEndpoint(address, quint16(port)));
    if (external_ && external_->id == device) return true;
    QString saved;
    for (auto it = savedChannels_.begin(); it != savedChannels_.end(); ++it)
        if (it.value().toObject().value("protocol") == providerId && it.value().toObject().value("deviceId") == device) { saved = it.key(); break; }
    if (saved.isEmpty()) {
        saved = externalId(device, "channel", "0");
        auto next = savedChannels_;
        next.insert(saved, QJsonObject{{"protocol", providerId}, {"deviceId", device}, {"providerChannel", "0"},
            {"name", externalName(address)}, {"address", address}, {"port", port}, {"autoJoin", false}, {"lastJoined", 0}});
        if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &next)) return false;
    }
    return joinExternal(saved, password, !password.isEmpty());
}

void LocalChannel::probeExternal(const QString& address, quint16 mumblePort, quint16 teamSpeakPort) {
    auto* socket = new QSslSocket(this);
    direct_ = socket;
    socket->setReadBufferSize(65542);
    // Discovery sends no identity, password or application data. The selected
    // adapter performs authentication and verifies the saved server key.
    socket->setPeerVerifyMode(QSslSocket::VerifyNone);
    const auto complete = [this, socket, address, mumblePort, teamSpeakPort](bool mumble) {
        if (direct_ != socket) return;
        finishDirect();
        if (mumble) addMumble(externalEndpoint(address, mumblePort));
        else addTeamSpeak(externalEndpoint(address, teamSpeakPort));
    };
    connect(socket, &QSslSocket::readyRead, this, [socket, complete] {
        if (socket->bytesAvailable() < 6) return;
        complete(mumbleGreeting(socket->peek(6)));
    });
    connect(socket, &QSslSocket::errorOccurred, this, [complete] { complete(false); });
    connect(socket, &QSslSocket::disconnected, this, [complete] { complete(false); });
    QTimer::singleShot(1500, socket, [complete] { complete(false); });
    connectEndpoint(socket, address, mumblePort, QAbstractSocket::AnyIPProtocol);
    emit stateChanged();
}

bool LocalChannel::externalCommand(const QJsonObject& command) {
    if (!external_ || !external_->handle) return false;
    if (command.value("action") == "join") sendAudio({});
    const auto bytes = QJsonDocument(command).toJson(QJsonDocument::Compact);
    return ss_ts_command(external_->handle, reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size())) != 0;
}

bool LocalChannel::joinExternal(const QString& id, const QString& suppliedPassword, bool remember) {
    if (!ready() || hostOnly_ || service_ || remoteMode()) return false;
    const auto entry = savedChannels_.value(id).toObject();
    const auto* provider = externalVoiceProvider(entry.value("protocol").toString());
    if (!provider) return false;
    const auto device = entry.value("deviceId").toString();
    auto c = clients_.value(id);
    if (!c) {
        c = std::make_shared<Client>(); c->id = id; c->provider = entry.value("protocol").toString(); c->deviceId = device;
        c->address = entry.value("address").toString(); c->port = quint16(entry.value("port").toInt());
        c->name = entry.value("name").toString(); c->providerChannel = entry.value("providerChannel").toString();
        c->reconnectWanted = false; clients_.insert(id, c);
    }
    if (external_ && external_->id == device && !c->passwordRequired && (joinedHostId_ == id || (external_->movePending && external_->requested == id))) {
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
        if (external_ && external_->id == device && external_->connected) {
            if (entry.value("password").toBool() && password.isEmpty()) {
                c->passwordRequired = true; emit stateChanged(); return true;
            }
            if (!externalCommand({{"action", "join"}, {"channel", c->providerChannel}, {"password", password}, {"request", "join:" + id}})) return false;
            external_->requested = id; external_->movePending = true; c->passwordRequired = false; c->chatError.clear();
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
        if (entry.value("protocol") == "teamspeak") {
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
        }
        auto connection = std::make_shared<ExternalVoice>();
        connection->id = device; connection->address = c->address; connection->port = c->port;
        connection->requested = id; connection->provider = QString::fromLatin1(provider->id);
        QJsonObject options{{"address", externalEndpoint(c->address, c->port)}, {"name", session_.userName()}, {"identity", identity},
            {"password", serverPassword}, {"serverKey", security_.value(QLatin1StringView(provider->pins)).toObject().value(device).toString()}, {"inputMuted", session_.muted() || !session_.available() || !session_.inputReady()},
            {"outputMuted", session_.deafened() || !session_.outputReady()}};
        if (connection->provider == "mumble") options.insert("identityPem", QString::fromLatin1(identity_->pem()));
        // Initial connection enters the server's default channel. Subsequent moves use channel IDs.
        const auto bytes = QJsonDocument(options).toJson(QJsonDocument::Compact);
        connection->handle = provider->open(reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size()));
        if (!connection->handle) return setStatus(tr("Connection failed: %1").arg(QLatin1StringView(provider->name)), false);
        external_ = std::move(connection);
        c->passwordRequired = false; c->voice = true; c->access = "connecting";
        c->password = serverPassword; c->rememberPassword = remember;
        joinedHostId_ = id; selectChat(id);
        externalTimer_.start(10);
        emit stateChanged(); emit hostsChanged();
        return true;
    } catch (const std::exception& e) { return setStatus(QString::fromUtf8(e.what()), false); }
}

void LocalChannel::disconnectExternal() {
    if (!external_) return;
    externalTimer_.stop();
    external_.reset();
    if (const auto active = clients_.value(joinedHostId_); active && active->external()) joinedHostId_.clear();
    for (const auto& c : clients_) if (c->external()) {
        c->accepted = false; c->voice = false; c->access = "offline"; c->members.clear(); c->onlineIds.clear();
        c->pending = {}; c->passwordRequired = false;
    }
    emit chatChanged(); emit participantsChanged(); emit hostsChanged(); emit stateChanged();
}

void LocalChannel::pollExternal() {
    if (!external_) return;
    const auto connection = external_;
    auto& buffer = connection->buffer;
    if (session_.deafened()) connection->audioSequences.clear();
    const bool mumble = connection->provider == "mumble";
    const int quantum = mumble ? 480 : 960;
    const auto flush = [&](const QString& clientId, bool full = false) {
        while (external_ == connection && connection->connected && !session_.deafened()) {
            auto it = connection->audioSequences.find(clientId);
            if (it == connection->audioSequences.end() || it->pending.isEmpty()) return;
            auto& stream = *it;
            const auto packet = stream.pending.cbegin();
            const auto missing = stream.delivered < 0 || stream.ended ? 0
                : int(std::clamp<qint64>(packet.key() - stream.delivered - stream.lastTicks, 0, 5760 / quantum));
            // Wait only for a gap. TS3 may deliver a whole batch out of order.
            if (missing && !full && controlClock_.elapsed() - packet->received < 40) return;
            const auto peer = stream.peer;
            const auto payload = packet->payload;
            const auto last = packet->last;
            stream.lastTicks = packet->ticks;
            stream.delivered = packet.key();
            stream.pending.remove(packet.key());
            full = false;
            // A completed talk burst cannot lose frames during its following silence.
            stream.ended = last;
            if (!payload.isEmpty() && (mumble || payload.size() > 1)) emit audioReceived(peer, payload, missing, quantum, last);
            else if (mumble && last) emit audioReceived(peer, {}, 0, quantum, true);
        }
    };
    // Limit each GUI dispatch. Audio packets cannot indefinitely starve input or paint events.
    for (int i = 0; i < 128 && external_ == connection; ++i) {
        const auto size = ss_ts_next(connection->handle, reinterpret_cast<uint8_t*>(buffer.data()), size_t(buffer.size()));
        if (size <= 0) break;
        if (buffer[0] == 1) externalEvent(QJsonDocument::fromJson(buffer.mid(1, size - 1)).object());
        else if (connection->connected && ((mumble && buffer[0] == 3 && size >= 14)
            || (!mumble && buffer[0] == 2 && size >= 6 && (buffer[5] == 4 || buffer[5] == 5)))) {
            const auto* bytes = reinterpret_cast<const uchar*>(buffer.constData());
            const auto clientId = QString::number(mumble ? qFromBigEndian<quint32>(bytes + 1) : qFromBigEndian<quint16>(bytes + 1));
            const auto payload = buffer.mid(mumble ? 14 : 6, size - (mumble ? 14 : 6));
            const bool last = mumble ? (quint8(buffer[13]) & 1) != 0 : payload.size() <= 1;
            const int samples = payload.isEmpty() ? 0 : opus_packet_get_nb_samples(
                reinterpret_cast<const unsigned char*>(payload.constData()), int(payload.size()), 48000);
            if (mumble && ((!payload.isEmpty() && (samples < 480 || samples > 5760 || samples % 480 != 0))
                || (payload.isEmpty() && !last))) continue;
            const auto c = clients_.value(joinedHostId_);
            if (!c || !c->external() || !c->voice || session_.deafened()) continue;
            for (const auto& value : c->members) {
                const auto member = value.toMap();
                if (member.value("providerClient") == clientId && member.value("id").toString() != ownId()
                    && !member.value("muted").toBool()) {
                    const quint64 sequence = mumble ? qFromBigEndian<quint64>(bytes + 5) : qFromBigEndian<quint16>(bytes + 3);
                    if (sequence > quint64(std::numeric_limits<qint64>::max() - 65536)) break;
                    const auto now = controlClock_.elapsed();
                    auto& stream = connection->audioSequences[clientId];
                    if (now - stream.received >= 2000) stream = {};
                    const auto serial = mumble ? qint64(sequence) : stream.highest < 0 ? qint64(sequence) + 65536
                        : stream.highest + qint16(sequence - quint16(stream.highest));
                    // An empty Mumble end marker consumes no audio ticks. The
                    // next burst may therefore start at exactly that sequence.
                    const bool followsMarker = mumble && stream.ended && stream.lastTicks == 0
                        && !payload.isEmpty() && serial == stream.delivered;
                    const auto pending = stream.pending.constFind(serial);
                    // If both arrive before the next playback dispatch, prefer
                    // real audio over the zero-duration marker at that tick.
                    const bool replacesMarker = mumble && !payload.isEmpty() && pending != stream.pending.cend()
                        && pending->payload.isEmpty() && pending->last;
                    if ((serial <= stream.delivered && !followsMarker)
                        || (pending != stream.pending.cend() && !replacesMarker)) break;
                    stream.peer = member.value("id").toString();
                    stream.highest = std::max(stream.highest, serial);
                    stream.received = now;
                    stream.pending.insert(serial, {payload, now, mumble ? samples / 480 : 1, last});
                    if (stream.pending.size() > 6) flush(clientId, true);
                    break;
                }
            }
        }
    }
    for (const auto& clientId : connection->audioSequences.keys()) flush(clientId);
}

void LocalChannel::externalState(const QJsonObject& state) {
    const auto connection = external_;
    const bool first = !connection->connected;
    const auto self = state.value("self").toString();
    const auto active = state.value("channel").toString();
    if (active.isEmpty()) return;
    const auto serverName = externalName(state.value("server").toObject().value("name").toString());
    connection->connected = true; connection->state = state; connection->name = serverName;
    const auto previous = joinedHostId_;
    const auto requested = connection->requested;
    if (first) {
        if (auto initial = clients_.value(requested); initial && initial->rememberPassword) {
            Client credential; credential.id = connection->id;
            if (!savePassword(credential, initial->password, true)) { disconnectExternal(); return; }
        }
        const auto pinKey = QLatin1StringView(externalVoiceProvider(connection->provider)->pins);
        auto pins = security_.value(pinKey).toObject();
        const auto uid = state.value("server").toObject().value("uid").toString();
        if (!uid.isEmpty() && !pins.contains(connection->id)) {
            pins.insert(connection->id, uid); auto security = security_; security.insert(pinKey, pins);
            if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security)) { disconnectExternal(); return; }
        }
    }
    connection->active = externalId(connection->id, "channel", active);
    if (previous != connection->active) connection->audioSequences.clear();
    joinedHostId_ = connection->active;
    QJsonObject next = savedChannels_;
    QSet<QString> seen;
    const auto channels = state.value("channels").toArray();
    auto members = state.value("members").toArray();
    QHash<QString, QJsonObject> profiles;
    for (qsizetype index = 0; index < members.size(); ++index) {
        auto member = members[index].toObject();
        const auto client = member.value("id").toString();
        const auto uid = member.value("uid").toString();
        const auto peer = client == self ? ownId() : externalId(connection->id, "member", uid.isEmpty() ? client : uid);
        const auto metadata = member.value("metadata").toString();
        const auto profile = metadata.size() <= 256 ? QJsonDocument::fromJson(metadata.toUtf8()).object() : QJsonObject{};
        const bool squadSpeak = client == self || metadata == "SquadSpeak" || profile.value("application") == "SquadSpeak";
        const auto advertised = profile.value("avatar").toString();
        const auto avatar = client == self ? session_.avatar()
            : squadSpeak && VoiceSession::validAvatar(advertised) ? advertised : VoiceSession::avatarForIdentity(uid.isEmpty() ? peer : uid, false);
        member.insert("squadSpeak", squadSpeak); member.insert("avatarId", avatar);
        members[index] = member;
        profiles.insert(peer, QJsonObject{{"name", externalName(member.value("name").toString())}, {"avatarId", avatar}, {"provider", squadSpeak ? QString{} : connection->provider}});
    }
    connection->state.insert("members", members);
    QSet<QString> connectedClients;
    for (const auto& value : members) {
        const auto member = value.toObject();
        if (member.value("channel") == active && !member.value("inputMuted").toBool())
            connectedClients.insert(member.value("id").toString());
    }
    for (auto it = connection->audioSequences.begin(); it != connection->audioSequences.end();)
        if (!connectedClients.contains(it.key())) it = connection->audioSequences.erase(it); else ++it;
    for (const auto& value : channels) {
        const auto info = value.toObject();
        const auto channel = info.value("id").toString();
        const auto id = externalId(connection->id, "channel", channel);
        seen.insert(id);
        auto entry = next.value(id).toObject();
        entry.insert("protocol", connection->provider); entry.insert("deviceId", connection->id); entry.insert("providerChannel", channel);
        const auto parent = info.value("parent").toString();
        entry.insert("parent", parent.isEmpty() || parent == channel ? QString{} : externalId(connection->id, "channel", parent));
        entry.insert("order", info.value("order")); entry.insert("password", info.value("password"));
        entry.insert("name", externalName(info.value("name").toString())); entry.insert("ownerName", serverName);
        entry.insert("address", connection->address); entry.insert("port", connection->port);
        if (!entry.contains("autoJoin")) entry.insert("autoJoin", false);
        if (id == joinedHostId_ && (first || previous != id)) entry.insert("lastJoined", clock_());
        else if (!entry.contains("lastJoined")) entry.insert("lastJoined", 0);
        next.insert(id, entry);
        auto c = clients_.value(id);
        if (!c) { c = std::make_shared<Client>(); c->id = id; clients_.insert(id, c); }
        c->provider = connection->provider; c->deviceId = connection->id; c->providerChannel = channel;
        c->name = entry.value("name").toString(); c->address = connection->address; c->port = connection->port;
        c->accepted = true; c->voice = id == joinedHostId_; c->access = "accepted"; c->reconnectWanted = false;
        c->messageLifetimeDays = 0; c->presenceKnown = true;
        c->externalCanSend = state.value("canSend").toBool() && (info.value("codec").toInt() == 4 || info.value("codec").toInt() == 5);
        c->members.clear(); c->onlineIds.clear();
        for (const auto& raw : members) {
            const auto member = raw.toObject();
            const auto providerClient = member.value("id").toString();
            const auto uid = member.value("uid").toString();
            const auto peer = providerClient == self ? ownId() : externalId(connection->id, "member", uid.isEmpty() ? providerClient : uid);
            c->onlineIds.append(peer);
            if (member.value("channel") != channel) continue;
            const auto avatar = member.value("avatarId").toString();
            c->members.append(QVariantMap{{"id", peer}, {"providerClient", providerClient}, {"providerUid", uid}, {"name", externalName(member.value("name").toString())},
                {"avatarId", avatar}, {"avatar", VoiceSession::avatarFallback(avatar)}, {"provider", providerClient != self && !member.value("squadSpeak").toBool() ? connection->provider : QString{}}, {"available", true}, {"muted", member.value("inputMuted").toBool()},
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
        if (state.value("directoryComplete").toBool() && entry.value("protocol") == connection->provider
            && entry.value("deviceId") == connection->id && !seen.contains(it.key())) {
            const auto id = it.key(); it = next.erase(it); clients_.remove(id);
        } else ++it;
    }
    if (next != savedChannels_) persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &next);
    if (chatHostId_ == previous || !clients_.contains(chatHostId_)) selectChat(joinedHostId_);
    for (const auto& c : clients_) if (c->external() && c->deviceId == connection->id && !c->privatePeer.isEmpty()) {
        c->accepted = true; c->access = "accepted"; c->providerPeer.clear();
        for (const auto& raw : members) {
            const auto m = raw.toObject();
            const auto uid = m.value("uid").toString();
            const auto peer = externalId(connection->id, "member", uid.isEmpty() ? m.value("id").toString() : uid);
            if (peer == c->privatePeer) { c->providerPeer = m.value("id").toString(); c->name = externalName(m.value("name").toString()); }
        }
        c->privatePeerOnline = !c->providerPeer.isEmpty();
    }
    for (const auto& c : clients_) if (c->external() && c->deviceId == connection->id) {
        for (qsizetype index = 0; index < c->messages.size(); ++index) {
            auto record = c->messages[index].toObject();
            const auto profile = profiles.constFind(record.value("sender").toString());
            if (profile == profiles.cend()) continue;
            for (auto field = profile->begin(); field != profile->end(); ++field) record.insert(field.key(), field.value());
            c->messages[index] = record;
        }
    }
    if (const auto selected = clients_.value(chatHostId_)) status_ = selected->status;
    if (first) { automaticAttempt_ = false; sendPresence(); emit channelEvent("join"); }
    if (connection->requested == joinedHostId_) { connection->requested.clear(); connection->movePending = false; }
    emit hostsChanged(); emit participantsChanged(); emit stateChanged(); emit chatChanged();
    // The server sends its channel directory incrementally after connecting.
    if (!connection->movePending && requested != joinedHostId_ && seen.contains(requested)) {
        connection->requested.clear(); joinExternal(requested);
    }
}

void LocalChannel::externalEvent(const QJsonObject& event) {
    if (!external_) return;
    const auto type = event.value("type").toString();
    if (type == "state") { externalState(event); return; }
    if (type == "identity") { if (event.value("value").isObject()) saveTeamSpeakIdentity(event.value("value").toObject()); return; }
    if (type == "reconnecting") {
        external_->connected = false; external_->movePending = false; external_->sendingAudio = false;
        external_->audioSequences.clear(); external_->lastPresence = {};
        for (const auto& c : clients_) if (c->external()) { c->accepted = false; c->access = "connecting"; c->members.clear(); }
        emit stateChanged(); emit hostsChanged(); emit participantsChanged(); return;
    }
    if (type == "error" || type == "closed") {
        const auto text = tsError(event.value("message").toString());
        const auto selected = clients_.value(chatHostId_);
        if (type == "closed") {
            const bool advance = automaticAttempt_;
            disconnectExternal();
            if (advance) QTimer::singleShot(0, this, &LocalChannel::tryAutoJoin);
        }
        if (selected && event.value("passwordRequired").toBool()) selected->passwordRequired = true;
        setStatus(text, false); emit stateChanged(); return;
    }
    if (type == "result") {
        const auto request = event.value("request").toString();
        const bool okay = event.value("ok").toBool();
        if (request.isEmpty()) { if (!okay) setStatus(tsError(event.value("error").toString()), false); return; }
        if (request.startsWith("join:")) {
            const auto c = clients_.value(request.mid(5));
            if (okay && c && c->rememberPassword) savePassword(*c, c->password, true);
            if (!okay) {
                if (c) { c->chatError = tsError(event.value("error").toString()); c->passwordRequired = event.value("passwordRequired").toBool(); }
            }
            if (external_->requested == request.mid(5)) { external_->requested.clear(); external_->movePending = false; }
            emit chatChanged(); emit stateChanged(); return;
        }
        for (const auto& c : clients_) {
            if (!c->external() || c->pending.value("request") != request) continue;
            if (okay) {
                const auto now = clock_();
                QJsonObject record{{"sequence", ++external_->sequence}, {"sender", ownId()}, {"name", session_.userName()},
                    {"avatarId", session_.avatar()}, {"provider", ""}, {"request", request}, {"created", now}, {"expires", now + ChatHistory::lifetime},
                    {"text", c->pending.value("text")}};
                c->messages.append(record); c->privateExpires = now + ChatHistory::lifetime;
                c->lastChatSequence = std::max(c->lastChatSequence, record.value("sequence").toInteger());
                while (c->messages.size() > maximumWindowMessages) c->messages.removeFirst();
                emit chatSent(c->pending.value("text").toString(), c->id);
            } else c->chatError = tsError(event.value("error").toString());
            c->pending = {}; emit chatChanged(); emit hostsChanged(); break;
        }
        return;
    }
    if (type != "message" || event.value("fromId") == external_->state.value("self")) return;
    const auto mode = event.value("mode").toString();
    if (mode != "channel" && mode != "private" && mode != "server") return;
    auto id = externalId(external_->id, "channel", event.value("channel").toString());
    auto uid = event.value("fromUid").toString();
    bool squadSpeak = false;
    QString avatar;
    for (const auto& value : external_->state.value("members").toArray()) {
        const auto member = value.toObject();
        if (member.value("id") == event.value("fromId")) {
            if (uid.isEmpty()) uid = member.value("uid").toString();
            squadSpeak = member.value("squadSpeak").toBool();
            avatar = member.value("avatarId").toString();
            break;
        }
    }
    const auto peer = externalId(external_->id, "member", uid.isEmpty() ? event.value("fromId").toString() : uid);
    if (mode == "private") {
        id = externalId(external_->id, "private", peer);
        auto c = clients_.value(id);
        if (!c) {
            c = std::make_shared<Client>(); c->id = id; c->provider = external_->provider; c->deviceId = external_->id;
            c->privatePeer = peer; c->name = externalName(event.value("fromName").toString()); c->providerPeer = event.value("fromId").toString();
            c->accepted = true; c->privatePeerOnline = true; c->address = external_->address; c->port = external_->port;
            c->access = "accepted"; c->reconnectWanted = false; c->messageLifetimeDays = 0; clients_.insert(id, c);
        }
    } else if (mode == "server") id = joinedHostId_;
    auto c = clients_.value(id);
    if (!c || !c->external()) return;
    const auto text = external_->provider == "mumble"
        ? ChatContent::importedHtmlText(event.value("text").toString()) : event.value("text").toString();
    if (!ChatHistory::validText(text)) return;
    const auto now = clock_();
    const bool unreadBefore = c->unreadExpires > now;
    const auto sequence = ++external_->sequence;
    c->messages.append(QJsonObject{{"sequence", sequence}, {"sender", peer}, {"name", externalName(event.value("fromName").toString())},
        {"avatarId", avatar.isEmpty() ? VoiceSession::avatarForIdentity(uid.isEmpty() ? peer : uid, false) : avatar}, {"provider", squadSpeak ? QString{} : external_->provider}, {"request", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"created", now}, {"expires", now + ChatHistory::lifetime}, {"text", text}});
    c->privateExpires = now + ChatHistory::lifetime;
    c->lastChatSequence = std::max(c->lastChatSequence, sequence);
    c->unreadExpires = now + ChatHistory::lifetime;
    while (c->messages.size() > maximumWindowMessages) c->messages.removeFirst();
    if (c->voice) emit chatNotification(c->id, c->messages.last().toObject().toVariantMap());
    emit chatChanged();
    if (!unreadBefore) emit hostsChanged();
}

bool LocalChannel::sendExternalChat(Client& c, const QString& text) {
    if (!c.accepted || !external_ || !external_->connected || !c.pending.isEmpty() || !ChatHistory::validText(text)
        || text.toUtf8().size() > 1024 || (!c.privatePeer.isEmpty() && c.providerPeer.isEmpty()) || (c.privatePeer.isEmpty() && c.id != joinedHostId_)) return false;
    const auto request = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto outgoing = c.provider == "mumble" ? QString(text).toHtmlEscaped().replace('\n', "<br/>") : text;
    QJsonObject command{{"action", "chat"}, {"target", c.privatePeer.isEmpty() ? "channel" : "private"},
        {"client", c.providerPeer.toLongLong()}, {"channel", c.providerChannel}, {"text", outgoing}, {"request", request}};
    if (!externalCommand(command)) return false;
    c.pending = {{"request", request}, {"text", text}}; c.chatError.clear(); emit chatChanged(); return true;
}

bool LocalChannel::openExternalPrivate(const QString& host, const QString& peer) {
    const auto source = clients_.value(host);
    if (!source || !source->external() || !external_ || source->deviceId != external_->id || !external_->connected || peer == ownId()) return false;
    const auto id = externalId(external_->id, "private", peer);
    auto c = clients_.value(id);
    if (!c) {
        QVariantMap member;
        for (const auto& other : clients_) if (other->external())
            for (const auto& value : other->members) if (value.toMap().value("id") == peer) member = value.toMap();
        if (member.isEmpty()) return false;
        c = std::make_shared<Client>(); c->id = id; c->provider = external_->provider; c->deviceId = external_->id;
        c->privatePeer = peer; c->providerPeer = member.value("providerClient").toString(); c->name = member.value("name").toString();
        c->accepted = true; c->privatePeerOnline = true; c->reconnectWanted = false; c->access = "accepted";
        c->address = external_->address; c->port = external_->port; c->messageLifetimeDays = 0;
        clients_.insert(id, c);
    }
    selectChat(id); return true;
}
