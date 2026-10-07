#include "local_channel.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslSocket>
#include <QUuid>
#include <algorithm>
#include <cmath>

bool LocalChannel::setControlStatus(QString text, bool result) {
    controlStatus_ = std::move(text); emit controlChanged(); return result;
}

bool LocalChannel::persistControl(const QJsonObject& control) {
    return persist(approved_, attempts_, requestsAllowed_, endpointPins_, control, security_);
}

void LocalChannel::updateControlIntent() {
    if (service_) return;
    if (hostOnly_) { controlTimer_.stop(); return; }
    const auto entries = control_.value("controllers").toObject();
    const bool held = std::any_of(entries.begin(), entries.end(), [](const auto& entry) { return entry.toObject().value("held").toBool(); });
    session_.setRemotePttHeld(held);
    const auto target = control_.value("target").toObject();
    session_.setPttLocal(!remoteMode());
    if (remoteMode() && !target.isEmpty() && !target.value("blocked").toBool() && !controlStopped_) {
        if (!controlTimer_.isActive()) controlTimer_.start();
    } else controlTimer_.stop();
    emit controlChanged();
}

QVariantList LocalChannel::controlRequests() const {
    QVariantList result;
    QSet<QString> seen;
    for (const auto& peer : peers_) {
        if (!peer.pending || !peer.controller || seen.contains(peer.id)) continue;
        seen.insert(peer.id); result.append(QVariantMap{{"id", peer.id}, {"name", peer.name}});
    }
    return result;
}

QVariantList LocalChannel::controllers() const {
    if (service_) return service_->controllers();
    QVariantList result;
    const auto entries = control_.value("controllers").toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        auto entry = it.value().toObject(); entry.insert("id", it.key()); result.append(entry.toVariantMap());
    }
    return result;
}

bool LocalChannel::acceptControlState(QSslSocket* socket, const QJsonObject& message) {
    const auto id = peers_.value(socket).id;
    auto entries = control_.value("controllers").toObject();
    auto previous = entries.value(id).toObject();
    const auto revision = message.value("revision").toInteger(-1);
    if (previous.isEmpty() || !message.value("held").isBool() || revision < 0 || revision > 9007199254740991LL
        || message.value("revision").toDouble(-1) != double(revision)) { socket->abort(); return false; }
    const bool held = message.value("held").toBool();
    const auto previousRevision = previous.value("revision").toInteger();
    if (revision < previousRevision || (revision == previousRevision && previous.value("held").toBool() != held)) {
        writeMessage(socket, {{"type", "control"}, {"result", "stale"}}); finishPeer(socket); return false;
    }
    if (revision == previousRevision) return true;
    previous.insert("revision", revision); previous.insert("held", held);
    previous.insert("name", peers_.value(socket).name);
    entries.insert(id, previous);
    auto next = control_; next.insert("controllers", entries);
    if (!persistControl(next)) { socket->abort(); return false; }
    return true;
}

bool LocalChannel::decideControl(const QString& id, bool allow) {
    if (service_) return service_->decideControl(id, allow);
    if (!validId(id)) return setControlStatus(tr("Invalid device identity."), false);
    auto entries = control_.value("controllers").toObject();
    QSslSocket* selected = nullptr;
    if (allow) {
        for (auto* socket : peers_.keys()) {
            const auto& peer = peers_[socket];
            if (peer.id == id && peer.controller && peer.pending
                && (!selected || peer.revision > peers_.value(selected).revision)) selected = socket;
        }
        if (!selected) return setControlStatus(tr("No open pairing request exists for this device."), false);
        const auto& peer = peers_[selected];
        entries.insert(id, QJsonObject{{"name", peer.name}, {"held", peer.held}, {"revision", peer.revision},
            {"remote", entries.value(id).toObject().value("remote").toBool()}});
    } else entries.remove(id);
    auto next = control_; next.insert("controllers", entries);
    if (!persistControl(next)) return false;
    if (allow) acceptPeer(selected);
    else {
        for (auto* socket : peers_.keys()) {
            if (peers_.value(socket).id != id) continue;
            if (peers_.value(socket).controller) {
                writeMessage(socket, {{"type", "control"}, {"result", "revoked"}});
                finishPeer(socket);
            } else if (peers_.value(socket).joined && peers_.value(socket).remoteOffers) writeMessage(socket, {{"type", "remoteOffer"}, {"allowed", false}});
        }
    }
    emit requestsChanged();
    publishRemoteAvailability();
    return setControlStatus(allow ? tr("Controller paired. Local mute takes precedence.") : tr("Pairing removed."));
}

bool LocalChannel::setRemotePermission(const QString& id, bool allow) {
    if (service_) return service_->setRemotePermission(id, allow);
    if (!validId(id) || id == ownId()) return setControlStatus(tr("Invalid device identity."), false);
    if (!allow) return decideControl(id, false);
    auto entries = control_.value("controllers").toObject();
    if (!entries.contains(id)) {
        auto members = hostClients();
        for (const auto& child : owned_) members.append(child->hostClients());
        const auto known = std::find_if(members.begin(), members.end(), [&](const auto& value) {
            return value.toMap().value("id").toString() == id;
        });
        if (known == members.end()) return setControlStatus(tr("No known member exists for this device."), false);
        entries.insert(id, QJsonObject{{"name", known->toMap().value("name").toString()}, {"held", false}, {"revision", 0}, {"remote", false}});
    }
    auto entry = entries.value(id).toObject();
    entry.insert("remote", true); entries.insert(id, entry);
    auto next = control_; next.insert("controllers", entries);
    if (!persistControl(next)) return false;
    for (auto* socket : peers_.keys()) {
        const auto peer = peers_.value(socket);
        if (peer.id != id || !peer.joined) continue;
        if (peer.controller) {
            const bool supported = !peer.capabilitiesAdvertised || peer.capabilities.contains("remote-control");
            writeMessage(socket, {{"type", "control"}, {"result", "accepted"}, {"remoteAllowed", supported},
                {"capabilities", capabilityAdvertisement()}});
            sendRemoteView(socket);
        } else if (peer.remoteOffers) writeMessage(socket, {{"type", "remoteOffer"}, {"allowed", true}, {"available", !hostOnly_ && !remoteMode()}});
    }
    publishRemoteAvailability();
    return setControlStatus(tr("Remote control allowed for this device."));
}

QVariantList LocalChannel::remoteOffers() const {
    QVariantList result;
    QSet<QString> devices;
    for (const auto& value : savedChannels()) {
        const auto entry = value.toMap(); const auto id = entry.value("id").toString();
        const auto device = entry.value("deviceId", id).toString();
        if (entry.value("remoteControl").toBool() && device != ownId() && !devices.contains(device)
            && !unavailableControlTargets_.contains(device)) { result.append(value); devices.insert(device); }
    }
    return result;
}

void LocalChannel::publishRemoteAvailability() {
    const auto* device = service_ ? service_ : this;
    const bool available = !device->hostOnly_ && !device->remoteMode();
    if (!service_) for (const auto& child : owned_) child->publishRemoteAvailability();
    for (auto* socket : peers_.keys()) {
        const auto peer = peers_.value(socket);
        if (!peer.joined) continue;
        if (peer.controller && !available) {
            peers_[socket].joined = false;
            writeMessage(socket, {{"type", "control"}, {"result", "unavailable"}});
            finishPeer(socket);
        } else if (!peer.controller && peer.remoteOffers) {
            writeMessage(socket, {{"type", "remoteOffer"}, {"available", available},
                {"allowed", device->control_.value("controllers").toObject().value(peer.id).toObject().value("remote").toBool()}});
        }
    }
}

bool LocalChannel::receiveRemoteOffer(const QString& id, bool allowed, bool available) {
    auto channels = savedChannels_;
    const auto entry = channels.value(id).toObject();
    if (entry.isEmpty()) return false;
    const auto device = entry.value("deviceId").toString(id);
    if (device == ownId()) return true;
    bool changed = false;
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        auto item = it.value().toObject();
        if (item.value("deviceId").toString(it.key()) != device || item.value("remoteControl").toBool() == allowed) continue;
        item.insert("remoteControl", allowed); it.value() = item; changed = true;
    }
    if (changed && !persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &channels)) return false;
    if (!allowed && changed && control_.value("target").toObject().value("id").toString() == device) {
        auto next = control_; next.insert("target", QJsonObject{}); next.insert("remoteMode", false);
        if (!persistControl(next)) return false;
        closeControl(); controlStopped_ = false;
        setControlStatus(tr("Remote control permission was removed by the host."));
    }
    if (available) {
        unavailableControlTargets_.remove(device);
    } else {
        unavailableControlTargets_.insert(device);
    }
    emit remoteChanged(); emit hostsChanged(); return true;
}

bool LocalChannel::chooseRemoteOffer(const QString& id) {
    const auto entry = savedChannels_.value(id).toObject();
    const auto device = entry.value("deviceId").toString(id);
    if (!entry.value("remoteControl").toBool() || device == ownId() || unavailableControlTargets_.contains(device))
        return setControlStatus(tr("This host has not allowed remote control."), false);
    return startControl(device, entry.value("address").toString(), quint16(entry.value("port").toInt()), entry.value("name").toString(), false);
}

bool LocalChannel::setRemoteMode(bool enabled) {
    if (hostOnly_) return false;
    if (enabled == remoteMode()) return true;
    const auto target = control_.value("target").toObject();
    if (enabled && target.isEmpty()) return setControlStatus(tr("Pair a target device first."), false);
    if (session_.pttInputHeld() || (!enabled && !target.value("revoked").toBool()
        && (!controlConnected_ || controlAckRevision_ < session_.pttInputRevision())))
        return setControlStatus(tr("Release first and wait for confirmation from the target."), false);
    auto next = control_; next.insert("remoteMode", enabled);
    if (!persistControl(next)) return false;
    if (enabled) {
        closeClients();
        controlStopped_ = false;
        updateControlIntent();
        connectControl();
    } else closeControl();
    emit remoteChanged();
    return setControlStatus(enabled ? tr("Connecting to the control target.") : tr("Using this device."));
}

bool LocalChannel::remoteAction(const QString& action, const QVariantMap& data) {
    if (!remoteMode() || !remoteAllowed_ || !controlConnected_ || action.isEmpty() || action.size() > 32) return false;
    auto payload = QJsonObject::fromVariantMap(data);
    if (QJsonDocument(payload).toJson(QJsonDocument::Compact).size() > maximumFrame - 512) return false;
    if (action != "join" && action != "leave" && action != "mute" && action != "deafen" && action != "chat" && action != "older" && action != "newer" && action != "latest" && action != "openChat") return false;
    if (action == "chat" && (!payload.value("text").isString() || !ChatHistory::validText(payload.value("text").toString()))) return false;
    if (action == "chat" || action == "older" || action == "newer" || action == "latest")
        payload.insert("hostId", remoteView_.value("chatHostId").toString());
    return writeMessage(controller_, {{"type", "remoteAction"}, {"action", action}, {"data", payload}});
}

bool LocalChannel::clearControlTarget() {
    const auto target = control_.value("target").toObject();
    if (target.isEmpty()) return true;
    if (remoteMode() && (session_.pttInputHeld() || (!target.value("revoked").toBool()
        && (!controlConnected_ || controlAckRevision_ < session_.pttInputRevision()))))
        return setControlStatus(tr("Release first and wait for confirmation from the target. Alternatively remove pairing at the target and reconnect."), false);
    auto next = control_; next.insert("target", QJsonObject{}); next.insert("remoteMode", false);
    if (!persistControl(next)) return false;
    closeControl(); controlStopped_ = false;
    return setControlStatus(tr("The push-to-talk key controls this device again."));
}

bool LocalChannel::startControl(const QString& id, const QString& address, quint16 port, const QString& name, bool pairing) {
    if (hostOnly_) return false;
    // Resolve the authenticated identity before distinguishing a retry from a
    // target change. A pending release must not be abandoned on another target.
    const auto target = control_.value("target").toObject();
    if (session_.pttInputHeld()) return setControlStatus(tr("Release the held transmit state first."), false);
    if (remoteMode() && !target.isEmpty() && target.value("id") != id && !target.value("revoked").toBool()
        && (!controlConnected_ || controlAckRevision_ < session_.pttInputRevision()))
        return setControlStatus(tr("Before changing targets, the previous target must confirm release. Alternatively remove pairing there and reconnect."), false);
    const auto canonicalAddress = canonicalEndpointHost(address);
    if (canonicalAddress.isEmpty()) return setControlStatus(tr("The control target address is invalid."), false);
    auto next = control_; next.insert("remoteMode", true);
    next.insert("target", QJsonObject{{"id", id}, {"address", canonicalAddress}, {"port", port},
        {"name", name}, {"pairing", pairing}, {"blocked", false}, {"revoked", false}});
    if (!persistControl(next)) return setControlStatus(tr("Control target could not be saved."), false);
    closeClients();
    closeControl();
    controlStopped_ = false;
    updateControlIntent();
    connectControl();
    return true;
}

void LocalChannel::closeControl() {
    if (controller_) {
        auto* socket = controller_.data(); controller_ = nullptr;
        socket->disconnect(this); socket->abort(); socket->deleteLater();
    }
    controlConnected_ = false; controlPending_ = false; controlAckRevision_ = -1;
    controlBuffer_.clear();
    remoteAllowed_ = false; remoteView_.clear(); remoteLevels_.clear(); emit remoteLevelsChanged();
    imageQueue_.clear(); downloadingImage_.clear(); downloadedImage_.clear(); imageCache_.clear();
    ++imageRevision_; emit imagesChanged(); remoteSnapshotBuffer_.clear();
    remoteSnapshotId_.clear(); remoteSnapshotExpected_ = -1;
    emit remoteChanged();
    emit controlChanged();
}

void LocalChannel::blockControl(QString reason, bool revoked) {
    controlStopped_ = true;
    auto next = control_;
    auto target = next.value("target").toObject(); target.insert("blocked", true); target.insert("revoked", revoked); next.insert("target", target);
    if (!persistControl(next)) reason += tr(" The connection stop could not be saved.");
    controlTimer_.stop(); closeControl(); setControlStatus(std::move(reason), false);
}

void LocalChannel::connectControl() {
    if (service_) return;
    const auto target = control_.value("target").toObject();
    if (!remoteMode() || !ready() || controller_ || target.isEmpty() || target.value("blocked").toBool() || controlStopped_) return;
    const auto address = target.value("address").toString();
    const bool preferIpv4 = QHostAddress(address).isNull();
    auto* socket = new QSslSocket(this);
    controller_ = socket; controlBuffer_.clear();
    controlReply_ = controlClock_.elapsed();
    socket->setReadBufferSize(2 * maximumFrame);
    socket->setSslConfiguration(identity_->configuration());
    const auto expected = target.value("id").toString();
    connect(socket, &QSslSocket::sslErrors, this, [this, socket, expected](const QList<QSslError>& errors) {
        socket->setProperty("squadTlsSeen", true);
        if (TlsIdentity::peerId(socket->peerCertificate()) != expected) {
            blockControl(tr("The control target has a different device identity. Remote control stopped."));
        } else if (acceptableCertificateErrors(errors)) socket->ignoreSslErrors(errors);
    });
    connect(socket, &QSslSocket::encrypted, this, [this, socket, expected] {
        socket->setProperty("squadTlsSeen", true);
        if (TlsIdentity::peerId(socket->peerCertificate()) != expected) {
            blockControl(tr("The control target's device identity does not match.")); return;
        }
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        writeMessage(socket, {{"type", "controlHello"}, {"version", 1}, {"name", session_.userName()},
            {"held", session_.pttInputHeld()}, {"revision", session_.pttInputRevision()},
            {"pairing", control_.value("target").toObject().value("pairing")}, {"capabilities", capabilityAdvertisement()}});
    });
    connect(socket, &QSslSocket::readyRead, this, [this, socket] {
        auto buffer = std::move(controlBuffer_);
        readMessages(socket, buffer, [this](const QJsonObject& message) { controlMessage(message); });
        if (controller_ == socket) controlBuffer_ = std::move(buffer);
    });
    connect(socket, &QSslSocket::errorOccurred, this, [this, socket, address, port = quint16(target.value("port").toInt()), ipv4Attempt = preferIpv4](QAbstractSocket::SocketError error) mutable {
        if (controller_ != socket) return;
        if (ipv4Attempt && addressConnectionFailure(error, socket)) {
            ipv4Attempt = false;
            socket->setProperty("squadIpv6Retry", true);
            QTimer::singleShot(0, socket, [this, socket, address, port] {
                if (controller_ != socket) return;
                socket->setProperty("squadIpv6Retry", false);
                connectEndpoint(socket, address, port, QAbstractSocket::IPv6Protocol);
            });
            return;
        }
        const auto reason = socket->errorString(); closeControl();
        setControlStatus(tr("Remote control interrupted: %1. Reconnecting.").arg(reason), false);
    });
    connect(socket, &QSslSocket::disconnected, this, [this, socket] {
        if (controller_ != socket) return;
        if (socket->property("squadIpv6Retry").toBool()) return;
        closeControl(); setControlStatus(tr("Control target is not connected. The last key state is retained."));
    });
    setControlStatus(tr("Connecting to the control target."));
    connectEndpoint(socket, address, quint16(target.value("port").toInt()),
        preferIpv4 ? QAbstractSocket::IPv4Protocol : QAbstractSocket::AnyIPProtocol);
}

void LocalChannel::sendControlState() {
    if (!controlConnected_) return;
    writeMessage(controller_, {{"type", "pttState"}, {"held", session_.pttInputHeld()}, {"revision", session_.pttInputRevision()}});
}

void LocalChannel::sendRemoteView(QSslSocket* socket) {
    const auto controllers = control_.value("controllers").toObject();
    QByteArray encoded;
    for (auto* peerSocket : peers_.keys()) {
        if (socket && peerSocket != socket) continue;
        if (!peers_.value(peerSocket).controller || !peers_.value(peerSocket).joined
            || !controllers.value(peers_.value(peerSocket).id).toObject().value("remote").toBool()
            || (peers_.value(peerSocket).capabilitiesAdvertised && !peers_.value(peerSocket).capabilities.contains("remote-control"))) continue;
        if (peers_.value(peerSocket).remoteSnapshotWaiting) { peers_[peerSocket].remoteSnapshotDirty = true; continue; }
        if (encoded.isEmpty()) {
            const QVariantMap view{{"channels", savedChannels()}, {"participants", participants()},
                {"messages", messages()}, {"chatBot", chatBot()}, {"chatLifetimeDays", chatLifetimeDays()}, {"chatHostId", chatHostId_}, {"chatMembers", chatMembers()}, {"joinedHostId", joinedHostId_}, {"joined", joined()},
                {"chatReady", chatReady()}, {"chatPending", chatPending()},
                {"chatOnlineIds", chatOnlineIds()}, {"chatPresenceKnown", chatPresenceKnown()}, {"receiveAudioBitrate", receiveAudioBitrate()},
                {"historyLoading", historyLoading()}, {"hasOlderMessages", hasOlderMessages()}, {"hasNewerMessages", hasNewerMessages()}, {"muted", session_.muted()},
                {"deafened", session_.deafened()}, {"name", session_.userName()}, {"avatar", VoiceSession::avatarFallback(session_.avatar())}, {"avatarId", session_.avatar()},
                {"available", session_.available()}, {"pushToTalk", session_.pushToTalk()}, {"ownId", ownId()}};
            encoded = QJsonDocument(QJsonObject::fromVariantMap(view)).toJson(QJsonDocument::Compact);
        }
        auto snapshot = encoded;
        if (!peers_.value(peerSocket).capabilities.contains("extended-retention")) {
            auto view = QJsonDocument::fromJson(snapshot).object();
            QJsonArray records;
            const auto* client = chatClient();
            const auto now = client && client->chatClock.isValid() ? client->serverTime + client->chatClock.elapsed() : clock_();
            for (const auto value : view.value("messages").toArray()) {
                const auto record = ChatHistory::forReader(value.toObject(), false);
                if (record.value("expires").toInteger() > now) records.append(record);
            }
            view.insert("messages", records);
            view.insert("chatLifetimeDays", std::min(view.value("chatLifetimeDays").toInt(), 30));
            snapshot = QJsonDocument(view).toJson(QJsonDocument::Compact);
        }
        if (snapshot.size() > maximumRemoteView) { peerSocket->abort(); continue; }
        auto& peer = peers_[peerSocket];
        peer.remoteSnapshot = snapshot; peer.remoteSnapshotId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        peer.remoteSnapshotOffset = 0; peer.remoteSnapshotWaiting = true; peer.remoteSnapshotDirty = false;
        const auto snapshotId = peer.remoteSnapshotId;
        if (writeMessage(peerSocket, {{"type", "remoteViewStart"}, {"id", snapshotId}, {"size", snapshot.size()}}))
            sendRemoteViewChunk(peerSocket);
    }
}

void LocalChannel::sendRemoteViewChunk(QSslSocket* socket) {
    const auto peer = peers_.constFind(socket);
    if (peer == peers_.cend() || !peer->remoteSnapshotWaiting) return;
    const auto chunk = peer->remoteSnapshot.mid(peer->remoteSnapshotOffset, remoteChunkSize);
    writeMessage(socket, {{"type", "remoteViewChunk"}, {"id", peer->remoteSnapshotId},
        {"offset", peer->remoteSnapshotOffset}, {"data", QString::fromLatin1(chunk.toBase64())}});
}

bool LocalChannel::applyRemoteView(const QByteArray& bytes) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    const auto view = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || !view.value("channels").isArray() || !view.value("participants").isArray() || !view.value("messages").isArray()
        || !validId(view.value("ownId").toString()) || !displayName(view.value("name")) || !VoiceSession::validAvatar(view.value("avatar").toString()) || !validPresenceDetails(view)) return false;
    for (const auto* key : {"joined", "chatReady", "chatPending", "historyLoading", "hasOlderMessages", "hasNewerMessages", "muted", "deafened", "available", "pushToTalk"})
        if (!view.value(key).isBool()) return false;
    if (view.contains("chatLifetimeDays")) {
        const auto days = view.value("chatLifetimeDays").toInteger(-1);
        if (view.value("chatLifetimeDays").toDouble(-1) != double(days)
            || (days != 0 && days != 1 && days != 7 && days != 30)) return false;
    }
    const auto host = view.value("joinedHostId").toString();
    if (!view.value("joinedHostId").isString() || (!host.isEmpty() && !validId(host)) || (view.value("joined").toBool() && host.isEmpty())) return false;
    QSet<QString> ids;
    for (const auto value : view.value("channels").toArray()) {
        const auto entry = value.toObject(); const auto id = entry.value("id").toString();
        if (!validId(id) || ids.contains(id) || !displayName(entry.value("name")) || !entry.value("online").isBool()
            || !entry.value("autoJoin").isBool() || !validEndpointHost(entry.value("address").toString())
            || entry.value("port").toInt() < 1 || entry.value("port").toInt() > 65535
            || !validMembers(entry.value("members")) || (!entry.value("online").toBool() && !entry.value("members").toArray().isEmpty())
            || !entry.value("access").isString() || entry.value("access").toString().size() > 32) return false;
        ids.insert(id);
    }
    if (!validMembers(view.value("participants")) || !validMembers(view.value("chatMembers"))) return false;
    if (view.contains("chatBot") && (!view.value("chatBot").isObject()
        || (!view.value("chatBot").toObject().isEmpty()
            && (!validMembers(QJsonArray{view.value("chatBot")}) || !view.value("chatBot").toObject().value("music").toBool())))) return false;
    if (view.contains("chatPresenceKnown")) {
        if (!view.value("chatPresenceKnown").isBool() || !view.value("chatOnlineIds").isArray()) return false;
        QSet<QString> online;
        for (const auto item : view.value("chatOnlineIds").toArray()) {
            const auto id = item.toString();
            if (!validId(id) || online.contains(id)) return false;
            online.insert(id);
        }
        if ((!view.value("chatReady").toBool() || !view.value("chatPresenceKnown").toBool()) && !online.isEmpty()) return false;
    }
    const auto chat = view.value("chatHostId").toString();
    if (!view.value("chatHostId").isString() || (!chat.isEmpty() && !validId(chat))
        || (view.value("chatReady").toBool() && chat.isEmpty())) return false;
    const auto messages = view.value("messages").toArray();
    if ((!view.value("chatReady").toBool() && (!messages.isEmpty() || !view.value("chatMembers").toArray().isEmpty()))
        || messages.size() > maximumWindowMessages || QJsonDocument(messages).toJson(QJsonDocument::Compact).size() > maximumWindowBytes) return false;
    qint64 sequence = 0;
    for (const auto value : messages) {
        const auto entry = value.toObject();
        if (!ChatHistory::validMessage(entry) || entry.value("sequence").toInteger() <= sequence) return false;
        sequence = entry.value("sequence").toInteger();
    }
    if (remoteView_.value("chatHostId").toString() != view.value("chatHostId").toString()) {
        imageQueue_.clear(); downloadingImage_.clear(); downloadedImage_.clear(); imageCache_.clear();
        ++imageRevision_; emit imagesChanged();
    }
    if (remoteView_.value("joinedHostId").toString() != host) { remoteLevels_.clear(); emit remoteLevelsChanged(); }
    remoteView_ = view.toVariantMap(); emit remoteChanged(); downloadImage(); return true;
}

bool LocalChannel::handleRemoteAction(QSslSocket* socket, const QJsonObject& message) {
    const auto action = message.value("action").toString();
    const auto data = message.value("data").toObject();
    if (action.isEmpty() || action.size() > 32 || QJsonDocument(data).toJson(QJsonDocument::Compact).size() > maximumFrame - 512) { socket->abort(); return false; }
    bool okay = false;
    if ((action == "chat" || action == "older" || action == "newer" || action == "latest")
        && data.value("hostId").toString() != chatHostId_) {
        writeMessage(socket, {{"type", "remoteActionResult"}, {"action", action}, {"ok", false}, {"error", tr("The viewed channel changed. Try again.")}});
        return false;
    }
    if (remoteMode() || !message.value("data").isObject()) {
        writeMessage(socket, {{"type", "remoteActionResult"}, {"action", action}, {"ok", false}, {"error", tr("The target is in remote mode or the action is invalid.")}});
        return false;
    }
    if (action == "join") {
        const auto id = data.value("hostId").toString();
        okay = validId(id) && (savedChannels_.contains(id) || ownChannel(id)) && joinSaved(id);
    } else if (action == "openChat") {
        const auto id = data.value("hostId").toString();
        okay = validId(id) && (savedChannels_.contains(id) || ownChannel(id)) && openSavedChat(id);
    } else if (action == "older") {
        okay = loadOlderMessages();
    } else if (action == "newer") {
        okay = loadNewerMessages();
    } else if (action == "latest") {
        okay = refreshChat();
    } else if (action == "leave") {
        okay = leave();
    } else if (action == "mute" && data.value("value").isBool()) {
        okay = session_.setMuted(data.value("value").toBool());
        if (okay) sendPresence();
    } else if (action == "deafen" && data.value("value").isBool()) {
        okay = session_.setDeafened(data.value("value").toBool());
        if (okay) sendPresence();
    } else if (action == "chat" && data.value("text").isString()) {
        const auto text = data.value("text").toString();
        okay = !remoteChatSocket_ && ChatHistory::validText(text) && sendChat(text);
        if (okay) { remoteChatSocket_ = socket; remotePendingChat_ = text; remotePendingHost_ = chatHostId_; }
    }
    writeMessage(socket, {{"type", "remoteActionResult"}, {"action", action}, {"ok", okay}, {"pending", action == "chat" && okay},
        {"error", okay ? QString{} : (chatError_.isEmpty() ? tr("Remote action rejected.") : chatError_)}});
    return okay;
}

void LocalChannel::controlMessage(const QJsonObject& message) {
    controlReply_ = controlClock_.elapsed();
    const auto type = message.value("type").toString();
    if (controlConnected_ && unknownMessageType(type)) return;
    if (type == "remoteImage") {
        if (!remoteAllowed_ || !controlConnected_) { blockControl(tr("Remote images are not authorized.")); return; }
        if (message.value("hostId").toString() != remoteView_.value("chatHostId").toString()) return;
        const auto payload = message.value("payload").toObject();
        if (payload.value("kind") == "imageError") {
            if (payload.value("hash").toString() == downloadingImage_) {
                downloadingImage_.clear(); downloadedImage_.clear(); downloadImage();
                setControlStatus(tr("This chat image is no longer available."), false);
            }
            return;
        }
        try { if (payload.value("kind") == "imageData" && receiveImage(payload)) return; }
        catch (const std::exception& error) { blockControl(tr(error.what())); return; }
        blockControl(tr("Invalid remote image.")); return;
    }
    if (type == "remoteLevels") {
        if (!remoteAllowed_ || !controlConnected_ || !message.value("levels").isObject()) { blockControl(tr("Invalid speaking activity.")); return; }
        if (message.value("hostId").toString() != remoteView_.value("joinedHostId").toString()) return;
        QVariantMap levels;
        const auto values = message.value("levels").toObject();
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!validId(it.key()) || !it.value().isDouble() || it.value().toDouble() < 0 || it.value().toDouble() > 1) {
                blockControl(tr("Invalid speaking activity.")); return;
            }
            levels.insert(it.key(), it.value().toDouble());
        }
        remoteLevels_ = levels; emit remoteLevelsChanged(); return;
    }
    if (type.startsWith("remoteView")) {
        if (!remoteMode() || !remoteAllowed_ || !controlConnected_) { blockControl(tr("Remote view is not authorized.")); return; }
        if (type == "remoteViewStart") {
            const auto id = message.value("id").toString(); const auto size = message.value("size").toInteger(-1);
            if (!remoteSnapshotId_.isEmpty() || QUuid(id).isNull() || size < 2 || size > maximumRemoteView
                || message.value("size").toDouble(-1) != double(size)) { blockControl(tr("Invalid remote view.")); return; }
            remoteSnapshotId_ = id; remoteSnapshotExpected_ = size; remoteSnapshotBuffer_.clear(); return;
        }
        if (type == "remoteViewChunk") {
            const auto id = message.value("id").toString(); const auto offset = message.value("offset").toInteger(-1);
            const auto bytes = QByteArray::fromBase64Encoding(message.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (id != remoteSnapshotId_ || id.isEmpty() || offset != remoteSnapshotBuffer_.size() || message.value("offset").toDouble(-1) != double(offset)
                || !bytes || bytes.decoded.isEmpty() || bytes.decoded.size() > remoteChunkSize || remoteSnapshotExpected_ < 0
                || remoteSnapshotBuffer_.size() + bytes.decoded.size() > remoteSnapshotExpected_) { blockControl(tr("Invalid remote view chunk.")); return; }
            remoteSnapshotBuffer_.append(bytes.decoded);
            writeMessage(controller_, {{"type", "remoteViewAck"}, {"id", id}, {"offset", offset}}); return;
        }
        if (type == "remoteViewEnd") {
            if (message.value("id").toString() != remoteSnapshotId_ || remoteSnapshotId_.isEmpty()
                || remoteSnapshotBuffer_.size() != remoteSnapshotExpected_ || !applyRemoteView(remoteSnapshotBuffer_)) {
                blockControl(tr("Invalid or incomplete remote view.")); return;
            }
            remoteSnapshotBuffer_.clear(); remoteSnapshotId_.clear(); remoteSnapshotExpected_ = -1; return;
        }
        blockControl(tr("Invalid remote view message.")); return;
    }
    if (type == "remoteActionResult") {
        const auto action = message.value("action").toString();
        if (!remoteAllowed_ || !controlConnected_ || !message.value("ok").isBool()) { blockControl(tr("Invalid remote action result.")); return; }
        if (!message.value("ok").toBool()) {
            const auto error = message.value("error").toString().left(512);
            setControlStatus(error.isEmpty() ? tr("Remote action rejected.") : error, false);
            if (action == "chat") emit remoteChatFailed(controlStatus_);
        } else if (action == "chat" && message.value("delivered").toBool() && message.value("text").isString())
            emit remoteChatSent(message.value("text").toString());
        return;
    }
    if (type == "controlWait" && controlPending_) return;
    if (type == "controlAck" && controlConnected_) {
        const auto revision = message.value("revision").toInteger(-1);
        if (revision < 0 || revision > session_.pttInputRevision() || message.value("revision").toDouble(-1) != double(revision)) {
            blockControl(tr("Invalid confirmation of push-to-talk state.")); return;
        }
        controlAckRevision_ = std::max(controlAckRevision_, revision);
        return;
    }
    if (type != "control") { blockControl(tr("Invalid response from the control target.")); return; }
    const auto result = message.value("result").toString();
    if (result == "accepted") {
        QSet<QString> capabilities;
        if (!parseCapabilities(message, &capabilities)) { blockControl(tr("Invalid response from the control target.")); return; }
        unavailableControlTargets_.remove(control_.value("target").toObject().value("id").toString());
        auto next = control_;
        auto target = next.value("target").toObject(); target.insert("pairing", false); next.insert("target", target);
        if (!persistControl(next)) { controlStopped_ = true; controlTimer_.stop(); closeControl(); setControlStatus(tr("Pairing could not be saved."), false); return; }
        controlConnected_ = true; controlPending_ = false;
        remoteAllowed_ = message.value("remoteAllowed").toBool()
            && (!message.contains("capabilities") || capabilities.contains("remote-control"));
        if (!remoteAllowed_) {
            remoteView_.clear(); remoteSnapshotBuffer_.clear(); remoteSnapshotId_.clear(); remoteSnapshotExpected_ = -1;
            remoteLevels_.clear(); emit remoteLevelsChanged();
            imageQueue_.clear(); downloadingImage_.clear(); downloadedImage_.clear(); imageCache_.clear();
            ++imageRevision_; emit imagesChanged();
        }
        emit remoteChanged();
        setControlStatus(remoteAllowed_ ? tr("Remote control connected. Mute at the target takes precedence.")
            : tr("Push-to-talk connected. Enable additional controls on the target device."));
        sendControlState();
    } else if (result == "unavailable") {
        unavailableControlTargets_.insert(control_.value("target").toObject().value("id").toString());
        closeControl();
        emit hostsChanged();
        setControlStatus(tr("Remote control is temporarily unavailable at the target."));
    } else if (result == "pending") {
        controlPending_ = true;
        setControlStatus(tr("Confirm remote push-to-talk pairing on the target device."));
    } else if (result == "rejected") {
        blockControl(tr("Pairing rejected. A new request is possible in %1 seconds at the earliest.").arg((message.value("retryMs").toInteger() + 999) / 1000), true);
    } else if (result == "stale") {
        blockControl(tr("The saved key state is stale. Confirm pairing again on the target."));
    } else if (result == "replaced") {
        blockControl(tr("A newer connection from this controller is active."));
    } else if (result == "revoked") {
        const auto id = control_.value("target").toObject().value("id").toString();
        QString channel;
        for (auto it = savedChannels_.begin(); it != savedChannels_.end(); ++it) {
            const auto entry = it.value().toObject();
            if (entry.value("deviceId").toString(it.key()) == id && entry.value("remoteControl").toBool()) { channel = it.key(); break; }
        }
        if (!channel.isEmpty()) {
            if (!receiveRemoteOffer(channel, false)) blockControl(tr("Remote permission could not be saved."), true);
        } else blockControl(tr("Pairing was removed or rejected at the target."), true);
    } else blockControl(tr("Unknown response from the control target."));
}

bool LocalChannel::publishLevels(const QVariantMap& levels) {
    if (!joined()) return false;
    QJsonObject active;
    for (const auto& value : audioSources()) {
        const auto id = value.toMap().value("id").toString();
        const auto level = levels.value(id, 0).toDouble();
        if (!std::isfinite(level) || level < 0 || level > 1) return false;
        if (level > 0) active.insert(id, level);
    }
    for (auto* socket : peers_.keys()) {
        const auto peer = peers_.value(socket);
        if (peer.controller && peer.joined && control_.value("controllers").toObject().value(peer.id).toObject().value("remote").toBool()
            && (!peer.capabilitiesAdvertised || peer.capabilities.contains("remote-control")))
            writeMessage(socket, {{"type", "remoteLevels"}, {"hostId", joinedHostId_}, {"levels", active}});
    }
    return true;
}
