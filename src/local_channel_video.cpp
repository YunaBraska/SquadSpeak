#include "local_channel.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QtEndian>
#include <algorithm>

namespace {
Q_LOGGING_CATEGORY(videoLog, "squadspeak.media", QtWarningMsg)
constexpr qsizetype maximumPacket = 2 * 1024 * 1024;
constexpr qsizetype chunkSize = 16384;
constexpr qsizetype datagramHeader = 20, datagramChunk = chunkSize - datagramHeader;
constexpr qsizetype maximumMetadata = 32768;
constexpr qsizetype maximumEnvelope = maximumPacket + maximumMetadata + 4;
constexpr int framePeriods[]{34, 34, 67, 125};
bool validFormat(const QJsonObject& format) {
    const auto width = format.value("width").toInt();
    const auto height = format.value("height").toInt();
    const auto codec = format.value("codec").toString();
    return (codec == "h264" || codec == "mpeg4") && width >= 2 && width <= 3840
        && height >= 2 && height <= 2160 && width % 2 == 0 && height % 2 == 0
        && format.value("width").toDouble() == width && format.value("height").toDouble() == height
        && format.value("extra").isString() && format.value("extra").toString().size() <= 16384;
}
}

QSet<QString> LocalChannel::mediaParticipants() const {
    if (service_) return service_->mediaParticipants();
    QSet<QString> ids;
    const auto collect = [&ids](const LocalChannel& channel) {
        for (const auto& peer : channel.peers_) if (peer.joined && !peer.controller && !peer.observer) ids.insert(peer.id);
        for (const auto& viewer : channel.viewers_) ids.insert(viewer.id);
    };
    collect(*this); for (const auto& child : owned_) collect(*child);
    return ids;
}

bool LocalChannel::screenBusy(const Client* receiver, const LocalChannel* sender) const {
    const auto* root = service_ ? service_ : this;
    const auto occupied = [receiver, sender](const LocalChannel& channel) {
        return (&channel != sender && channel.screenSharing_) ||
            std::any_of(channel.clients_.cbegin(), channel.clients_.cend(), [receiver](const auto& client) {
                return client.get() != receiver && client->screenWanted;
            });
    };
    return occupied(*root) || std::any_of(root->owned_.cbegin(), root->owned_.cend(),
        [&occupied](const auto& channel) { return occupied(*channel); });
}

bool LocalChannel::setScreenSharing(bool active) {
    if (active && (!hosting() || hostOnly_)) return setStatus(tr("Channel unavailable"), false);
    if (screenSharing_ == active) return true;
    if (active && screenBusy(nullptr, this))
        return setStatus(QCoreApplication::translate("ScreenShare", "Close a video view before opening another stream."), false);
    screenSharing_ = active; slowScreenEncodes_ = 0;
    if (!active) { screenAudio_ = false; closeViewers(); }
    broadcastRoster(); emit screenChanged(); return true;
}

bool LocalChannel::setScreenAudio(bool enabled) {
    if (enabled && !screenSharing_) return false;
    if (screenAudio_ == enabled) return true;
    screenAudio_ = enabled;
    musicRelay_.reset();
    broadcastRoster(); emit screenChanged(); return true;
}

bool LocalChannel::receivesScreenAudio(const Client& client) const {
    return client.accepted && client.screenAudioAvailable &&
        ((client.voice && client.id == joinedHostId_) ||
         (client.screenWanted && client.screenAudioWanted && client.screenSocket && client.screenSocket->isEncrypted()));
}

bool LocalChannel::receivesScreenAudio(const Peer& peer) const {
    // The publisher already hears the original system output locally.
    return screenAudio_ && peer.id != ownId() && peer.capabilities.contains("screen-audio") &&
        (!peer.observer || std::any_of(viewers_.cbegin(), viewers_.cend(), [&peer](const auto& viewer) {
            return viewer.id == peer.id && viewer.audio;
        }));
}

void LocalChannel::closeViewers(const QString& id) {
    for (auto* socket : viewers_.keys()) if (id.isEmpty() || viewers_.value(socket).id == id) socket->abort();
}

void LocalChannel::acceptViewer(QSslSocket* socket, int minimumTier, bool audioAllowed, bool audio, bool udp) {
    if (minimumTier < 0 || minimumTier > 3) { socket->abort(); return; }
    const auto id = peers_.value(socket).id;
    const auto admitted = std::any_of(peers_.cbegin(), peers_.cend(), [&id](const auto& peer) {
        return peer.id == id && peer.joined && !peer.controller;
    });
    auto occupied = mediaParticipants(); occupied.remove(id);
    if (!screenSharing_ || !admitted || occupied.size() >= 64) {
        writeMessage(socket, {{"type", "screenQuality"}, {"tier", -1}, {"result", admitted && screenSharing_ ? "full" : "unavailable"}});
        finishPeer(socket); return;
    }
    closeViewers(id);
    peers_.remove(socket);
    socket->disconnect(this);
    Viewer viewer; viewer.id = id; viewer.minimumTier = minimumTier; viewer.tier = std::max(1, minimumTier);
    viewer.audioAllowed = audioAllowed; viewer.audio = audioAllowed && audio;
    viewers_.insert(socket, std::move(viewer));
    connect(socket, &QSslSocket::readyRead, this, [this, socket] { readViewer(socket); });
    connect(socket, &QSslSocket::bytesWritten, this, [this, socket] { pumpScreen(socket); });
    connect(socket, &QSslSocket::encryptedBytesWritten, this, [this, socket] { pumpScreen(socket); });
    connect(socket, &QSslSocket::disconnected, this, [this, socket] {
        viewers_.remove(socket); socket->deleteLater(); emit screenChanged();
    });
    if (udp) {
        const auto media = std::make_shared<squad::MediaTransport>(true, port(), socket->peerAddress(), 0, squad::MediaTransport::Medium::Data);
        viewers_[socket].media = media;
        connect(media.get(), &squad::MediaTransport::signaling, this, [this, socket](QJsonObject message) {
            if (!viewers_.contains(socket)) return;
            message.insert("type", "screenMedia"); writeMessage(socket, message);
        });
        connect(media.get(), &squad::MediaTransport::writable, this, [this, socket] { pumpScreen(socket); });
        connect(media.get(), &squad::MediaTransport::datagramReceived, this, [this, socket](const QByteArray& bytes) {
            if (!bytes.startsWith("SQVA")) return;
            if (bytes.size() != 16) { socket->abort(); return; }
            finishScreen(socket, qint64(qFromBigEndian<quint64>(bytes.constData() + 4)), int(qFromBigEndian<quint32>(bytes.constData() + 12)));
        });
    }
    writeMessage(socket, {{"type", "screenQuality"}, {"tier", viewers_.value(socket).tier}, {"udp", udp}});
    emit screenChanged();
}

QSet<int> LocalChannel::screenTiers() const {
    QSet<int> result;
    if (screenSharing_) for (const auto& viewer : viewers_) if (viewer.tier < 4 && !viewer.waiting) result.insert(viewer.tier);
    return result;
}

bool LocalChannel::screenNeedsKeyFrame(int tier) const {
    for (const auto& viewer : viewers_) if (viewer.tier == tier && !viewer.waiting && viewer.keyFrameNeeded) return true;
    return false;
}

bool LocalChannel::reportScreenEncodeTime(int milliseconds) {
    if (milliseconds < 0 || milliseconds > 60000 || !screenSharing_) return false;
    int budget = 125;
    for (const auto& viewer : viewers_) if (viewer.tier < 4) budget = std::min(budget, framePeriods[viewer.tier]);
    if (milliseconds <= budget) { slowScreenEncodes_ = 0; return true; }
    if (milliseconds >= 1000) slowScreenEncodes_ = 2;
    if (++slowScreenEncodes_ < 3) return true;
    slowScreenEncodes_ = 0;
    for (auto* socket : viewers_.keys()) {
        auto& v = viewers_[socket];
        if (v.tier == 4 || v.waiting) continue;
        ++v.tier; v.keyFrameNeeded = true; v.goodSince = -1; v.badSamples = 0;
        if (v.tier == 4) v.resumeAt = controlClock_.elapsed() + 3000;
        writeMessage(socket, {{"type", "screenQuality"}, {"tier", v.tier}});
    }
    emit screenChanged(); return true;
}

bool LocalChannel::sendScreenFrame(int tier, const QJsonObject& format, const QByteArray& packet, bool keyFrame) {
    if (!screenSharing_ || tier < 0 || tier > 3 || packet.isEmpty() || packet.size() > maximumPacket || !validFormat(format)) return false;
    const auto metadata = QJsonDocument(format).toJson(QJsonDocument::Compact);
    if (metadata.size() > maximumMetadata) return false;
    QByteArray envelope;
    const auto serial = ++screenSequence_;
    for (auto* socket : viewers_.keys()) {
        auto& viewer = viewers_[socket];
        if (viewer.tier != tier) continue;
        if (viewer.waiting) { viewer.keyFrameNeeded = true; continue; }
        if (viewer.keyFrameNeeded && !keyFrame) continue;
        viewer.keyFrameNeeded = false; viewer.waiting = true;
        viewer.packet = packet; viewer.format = format; viewer.offset = 0;
        viewer.datagram = viewer.media && viewer.media->udpActive();
        if (viewer.datagram) {
            if (envelope.isEmpty()) {
                envelope = QByteArray(4, '\0');
                qToBigEndian<quint32>(quint32(metadata.size()), envelope.data());
                envelope += metadata; envelope += packet;
            }
            viewer.packet = envelope;
        }
        viewer.serial = serial; viewer.sentTier = tier; viewer.sent = controlClock_.elapsed();
        if (!viewer.datagram && !writeMessage(socket, {{"type", "screenFrame"}, {"serial", serial}, {"size", packet.size()}, {"format", format}})) continue;
        pumpScreen(socket);
    }
    return true;
}

void LocalChannel::pumpScreen(QSslSocket* socket) {
    // Only a small socket window is queued. Each receiver holds at most one
    // implicitly shared encoded frame, never an accumulating frame backlog.
    auto it = viewers_.find(socket);
    if (it == viewers_.end() || !it->waiting) return;
    if (it->datagram) {
        const auto media = it->media;
        while (it->offset < it->packet.size()) {
            auto bytes = QByteArray(datagramHeader, '\0'); bytes.replace(0, 4, "SQVF");
            qToBigEndian<quint64>(quint64(it->serial), bytes.data() + 4);
            qToBigEndian<quint32>(quint32(it->packet.size()), bytes.data() + 12);
            qToBigEndian<quint32>(quint32(it->offset), bytes.data() + 16);
            const auto size = std::min(datagramChunk, it->packet.size() - it->offset);
            bytes.append(it->packet.constData() + it->offset, size);
            if (!media->sendDatagram(bytes)) return;
            it->offset += size;
        }
        return;
    }
    while (it->offset < it->packet.size() && socket->bytesToWrite() + socket->encryptedBytesToWrite() < 32768) {
        const auto offset = it->offset;
        const auto chunk = it->packet.mid(offset, chunkSize);
        it->offset += chunk.size();
        if (!writeMessage(socket, {{"type", "screenChunk"}, {"serial", it->serial}, {"offset", offset},
            {"data", QString::fromLatin1(chunk.toBase64())}})) return;
        it = viewers_.find(socket);
        if (it == viewers_.end()) return;
    }
}

void LocalChannel::readViewer(QSslSocket* socket) {
    if (!viewers_.contains(socket)) return;
    auto buffer = std::move(viewers_[socket].buffer);
    readMessages(socket, buffer, [this, socket](const QJsonObject& message) { viewerMessage(socket, message); });
    if (viewers_.contains(socket)) viewers_[socket].buffer = std::move(buffer);
}

void LocalChannel::viewerMessage(QSslSocket* socket, const QJsonObject& message) {
    auto it = viewers_.find(socket);
    if (it == viewers_.end()) return;
    const auto type = message.value("type").toString();
    if (unknownMessageType(type)) return;
    if (type == "screenMedia") {
        const auto media = it->media;
        if (!media || !media->receive(message)) socket->abort();
        return;
    }
    if (type == "screenLimit") {
        const int minimum = message.value("minimumTier").toInt(-1);
        if (minimum < 0 || minimum > 3 || message.value("minimumTier").toDouble(-1) != minimum) { socket->abort(); return; }
        if (message.contains("audio") && !message.value("audio").isBool()) { socket->abort(); return; }
        it->audio = it->audioAllowed && message.value("audio").toBool();
        const int previous = it->minimumTier;
        it->minimumTier = minimum;
        if (it->tier < minimum || (minimum < previous && it->tier < 4)) {
            it->tier = minimum < previous ? std::max(1, minimum) : minimum;
            it->keyFrameNeeded = true; it->goodSince = -1; it->badSamples = 0;
            writeMessage(socket, {{"type", "screenQuality"}, {"tier", it->tier}}); emit screenChanged();
        }
        return;
    }
    const auto serial = message.value("serial").toInteger(-1);
    const auto decode = message.value("decodeMs").toInt(-1);
    if (type != "screenAck" || message.value("serial").toDouble(-1) != double(serial)
        || message.value("decodeMs").toDouble(-1) != decode) { socket->abort(); return; }
    finishScreen(socket, serial, decode);
}

void LocalChannel::finishScreen(QSslSocket* socket, qint64 serial, int decode, bool lost) {
    auto it = viewers_.find(socket);
    if (it == viewers_.end()) return;
    if (decode < 0 || decode > 60000) { socket->abort(); return; }
    if (it->media && serial > 0 && (serial < it->serial || (!it->waiting && serial == it->serial))) return;
    if (!it->waiting || serial != it->serial || (!lost && it->offset != it->packet.size())) { socket->abort(); return; }
    if (lost) it->keyFrameNeeded = true;
    const auto elapsed = controlClock_.elapsed();
    const auto duration = elapsed - it->sent;
    const int previous = it->tier;
    const auto* device = service_ ? service_ : this;
    const auto delayedVoice = [&it](const LocalChannel& channel) {
        return std::any_of(channel.peers_.cbegin(), channel.peers_.cend(), [&it](const auto& peer) {
            return peer.id == it->id && peer.joined && !peer.observer && peer.audioTier > 0;
        });
    };
    const bool voicePressure = delayedVoice(*device) || std::any_of(device->owned_.cbegin(), device->owned_.cend(),
        [&delayedVoice](const auto& channel) { return delayedVoice(*channel); });
    const bool bad = duration > framePeriods[it->sentTier] * 2 || decode > framePeriods[it->sentTier] || voicePressure;
    if (bad) {
        it->goodSince = -1;
        if (++it->badSamples >= 3) {
            it->badSamples = 0; it->tier = std::min(4, it->tier + 1);
            if (it->tier == 4) it->resumeAt = elapsed + 3000;
        }
    } else {
        it->badSamples = 0;
        if (it->goodSince < 0) it->goodSince = elapsed;
        if (elapsed - it->goodSince >= 10000 && it->tier > it->minimumTier) { --it->tier; it->goodSince = -1; }
    }
    it->tier = std::max(it->tier, it->minimumTier);
    if (bad || previous != it->tier || it->goodSince == elapsed)
        qCDebug(videoLog) << "screen quality tier" << it->sentTier << "duration_ms" << duration
                         << "decode_ms" << decode << "voice_pressure" << voicePressure
                         << "bad_samples" << it->badSamples << "good_since_ms" << it->goodSince
                         << "next_tier" << it->tier << "udp" << it->datagram;
    it->waiting = false; it->packet.clear(); it->format = {};
    if (previous != it->tier) {
        it->keyFrameNeeded = true;
        writeMessage(socket, {{"type", "screenQuality"}, {"tier", it->tier}}); emit screenChanged();
    }
}

QVariantMap LocalChannel::screenView() const {
    return screenInfo(chatHostId_);
}
QVariantMap LocalChannel::screenInfo(const QString& hostId) const {
    const auto c = clients_.value(hostId);
    return c ? QVariantMap{{"hostId", c->id}, {"available", c->accepted && c->screenAvailable},
        {"watching", c->screenWanted}, {"tier", c->screenTier}, {"status", c->screenStatus},
        {"width", c->screenFormat.value("width").toInt()}, {"height", c->screenFormat.value("height").toInt()}} : QVariantMap{};
}

bool LocalChannel::watchScreen(const QString& id, bool enabled, int minimumTier, bool audio) {
    const auto c = clients_.value(id);
    if (!c || minimumTier < 0 || minimumTier > 3 || (enabled && (!c->accepted || !c->screenAvailable || remoteMode() || hostOnly_))) return false;
    if (enabled && screenBusy(c.get(), nullptr)) return false;
    c->screenWanted = enabled;
    if (enabled && c->screenSocket && (c->screenMinimumTier != minimumTier || c->screenAudioWanted != audio))
        writeMessage(c->screenSocket, {{"type", "screenLimit"}, {"minimumTier", minimumTier}, {"audio", audio}});
    c->screenAudioWanted = enabled && audio;
    c->screenMinimumTier = minimumTier;
    if (enabled && !c->screenSocket) connectScreen(*c);
    else if (!enabled) closeScreen(*c);
    emit screenChanged(); emit participantsChanged(); return true;
}

void LocalChannel::closeScreen(Client& c) {
    if (auto* socket = c.screenSocket.data()) {
        c.screenSocket = nullptr; socket->disconnect(this); socket->abort(); socket->deleteLater();
    }
    c.screenMedia.reset(); c.screenChunks.clear(); c.screenDatagram = false; c.screenLastSerial = 0;
    c.screenPacket.clear(); c.screenBuffer.clear(); c.screenFormat = {};
    c.screenSize = 0; c.screenSerial = 0; c.screenTier = -1;
    emit screenChanged(); emit participantsChanged();
}

void LocalChannel::connectScreen(Client& c) {
    if (!identity_ || !c.accepted || !c.screenAvailable || c.screenSocket) return;
    auto connection = clients_.value(c.id);
    auto* socket = new QSslSocket(this); c.screenSocket = socket;
    socket->setReadBufferSize(131072);
    socket->setSslConfiguration(identity_->configuration());
    connect(socket, &QSslSocket::sslErrors, this, [socket, connection](const QList<QSslError>& errors) {
        if (TlsIdentity::peerId(socket->peerCertificate()) != connection->deviceId) { socket->abort(); return; }
        if (acceptableCertificateErrors(errors)) socket->ignoreSslErrors(errors);
    });
    connect(socket, &QSslSocket::encrypted, this, [this, socket, connection] {
        if (TlsIdentity::peerId(socket->peerCertificate()) != connection->deviceId) { socket->abort(); return; }
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        emit participantsChanged();
        writeMessage(socket, {{"type", "screenHello"}, {"version", 1}, {"targetChannel", connection->id}, {"minimumTier", connection->screenMinimumTier}, {"audio", connection->screenAudioWanted}, {"capabilities", QJsonArray{QStringLiteral("screen-share"), QStringLiteral("screen-audio"), QStringLiteral("udp-screen")}}});
    });
    connect(socket, &QSslSocket::readyRead, this, [this, socket, connection] {
        auto buffer = std::move(connection->screenBuffer);
        readMessages(socket, buffer, [this, connection](const QJsonObject& message) { receiveScreen(*connection, message); });
        if (connection->screenSocket == socket) connection->screenBuffer = std::move(buffer);
    });
    connect(socket, &QSslSocket::disconnected, this, [this, connection] { closeScreen(*connection); });
    connect(socket, &QSslSocket::errorOccurred, this, [this, socket, connection](QAbstractSocket::SocketError) {
        if (socket->state() == QAbstractSocket::UnconnectedState && connection->screenSocket == socket) closeScreen(*connection);
    });
    // Follow the authenticated control connection's resolved address. Its normal
    // DNS reconnect logic will discover address changes before resubscribing.
    socket->connectToHostEncrypted(c.socket->peerAddress().toString(), c.port);
    QTimer::singleShot(5000, socket, [socket] { if (!socket->isEncrypted()) socket->abort(); });
}

void LocalChannel::receiveScreen(Client& c, const QJsonObject& message) {
    const auto type = message.value("type").toString();
    // TLS has already pinned this host. Quality negotiation is not admission.
    if (unknownMessageType(type)) return;
    if (type == "screenMedia") {
        const auto media = c.screenMedia;
        if (!media || !media->receive(message)) c.screenSocket->abort();
        return;
    }
    if (type == "screenQuality") {
        const int tier = message.value("tier").toInt(-2);
        if (tier < -1 || tier > 4 || message.value("tier").toDouble(-2) != tier) { c.screenSocket->abort(); return; }
        if (message.contains("udp") && !message.value("udp").isBool()) { c.screenSocket->abort(); return; }
        if (tier >= 0 && message.value("udp").toBool() && !c.screenMedia && c.capabilities.contains("udp-screen")) {
            const auto media = std::make_shared<squad::MediaTransport>(false, 0, c.screenSocket->peerAddress(), c.port, squad::MediaTransport::Medium::Data);
            c.screenMedia = media;
            const auto id = c.id; const auto socket = c.screenSocket;
            connect(media.get(), &squad::MediaTransport::signaling, this, [this, id, socket](QJsonObject message) {
                const auto connection = clients_.value(id);
                if (!connection || !socket || connection->screenSocket != socket) return;
                message.insert("type", "screenMedia"); writeMessage(socket, message);
            });
            connect(media.get(), &squad::MediaTransport::datagramReceived, this, [this, id, socket](const QByteArray& bytes) {
                const auto connection = clients_.value(id);
                if (connection && socket && connection->screenSocket == socket) receiveScreenDatagram(*connection, bytes);
            });
        }
        c.screenTier = tier; c.screenStatus = message.value("result").toString();
        if (tier == -1) { c.screenWanted = false; c.screenSocket->disconnectFromHost(); }
        emit screenChanged(); return;
    }
    if (type == "screenFrame") {
        const auto serial = message.value("serial").toInteger(-1);
        const auto size = message.value("size").toInteger(-1);
        const auto format = message.value("format").toObject();
        if ((c.screenSerial && !c.screenDatagram) || serial <= c.screenLastSerial || serial < 1 || serial > 9007199254740991LL || message.value("serial").toDouble(-1) != double(serial)
            || size < 1 || size > maximumPacket || message.value("size").toDouble(-1) != double(size) || !validFormat(format)) {
            c.screenSocket->abort(); return;
        }
        c.screenDatagram = false; c.screenChunks.clear(); c.screenLastSerial = serial;
        c.screenSerial = serial; c.screenSize = size; c.screenFormat = format; c.screenPacket.clear(); return;
    }
    if (type == "screenChunk") {
        const auto data = message.value("data").toString().toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(data, QByteArray::AbortOnBase64DecodingErrors);
        if (!c.screenSerial || message.value("serial").toDouble(-1) != double(c.screenSerial)
            || message.value("offset").toDouble(-1) != double(c.screenPacket.size()) || !decoded
            || decoded.decoded.isEmpty() || decoded.decoded.size() > chunkSize || c.screenPacket.size() + decoded.decoded.size() > c.screenSize) {
            c.screenSocket->abort(); return;
        }
        c.screenPacket.append(decoded.decoded);
        if (c.screenPacket.size() == c.screenSize) {
            const auto packet = std::move(c.screenPacket); c.screenSize = 0;
            emit screenFrameReceived(c.id, c.screenSerial, c.screenFormat, packet);
        }
        return;
    }
    c.screenSocket->abort();
}

void LocalChannel::receiveScreenDatagram(Client& c, const QByteArray& bytes) {
    if (!bytes.startsWith("SQVF")) return;
    if (bytes.size() <= datagramHeader || bytes.size() > chunkSize) { c.screenSocket->abort(); return; }
    const auto serial = qFromBigEndian<quint64>(bytes.constData() + 4);
    const auto total = qFromBigEndian<quint32>(bytes.constData() + 12);
    const auto offset = qFromBigEndian<quint32>(bytes.constData() + 16);
    const auto size = bytes.size() - datagramHeader;
    if (!serial || serial > 9007199254740991ULL || total < 6 || total > maximumEnvelope
        || offset >= total || offset % datagramChunk || size != std::min<qsizetype>(datagramChunk, total - offset)) {
        c.screenSocket->abort(); return;
    }
    if (serial < quint64(c.screenLastSerial) || (serial == quint64(c.screenLastSerial) && (!c.screenDatagram || !c.screenSize))) return;
    if (serial > quint64(c.screenLastSerial)) {
        c.screenLastSerial = c.screenSerial = qint64(serial); c.screenSize = total; c.screenDatagram = true;
        c.screenPacket = QByteArray(total, '\0'); c.screenChunks = QBitArray((total + datagramChunk - 1) / datagramChunk);
    }
    if (total != c.screenSize) { c.screenSocket->abort(); return; }
    const auto index = offset / datagramChunk;
    if (c.screenChunks.testBit(index)) return;
    std::copy(bytes.begin() + datagramHeader, bytes.end(), c.screenPacket.begin() + offset);
    c.screenChunks.setBit(index);
    if (c.screenChunks.count(true) != c.screenChunks.size()) return;
    const auto metadataSize = qFromBigEndian<quint32>(c.screenPacket.constData());
    if (!metadataSize || metadataSize > maximumMetadata || metadataSize + 4 >= total || total - metadataSize - 4 > maximumPacket) { c.screenSocket->abort(); return; }
    QJsonParseError error;
    const auto metadata = QJsonDocument::fromJson(c.screenPacket.mid(4, metadataSize), &error);
    if (error.error != QJsonParseError::NoError || !metadata.isObject() || !validFormat(metadata.object())) { c.screenSocket->abort(); return; }
    c.screenFormat = metadata.object(); c.screenSize = 0; c.screenChunks.clear();
    const auto packet = c.screenPacket.mid(metadataSize + 4); c.screenPacket.clear();
    emit screenFrameReceived(c.id, c.screenSerial, c.screenFormat, packet);
}

bool LocalChannel::acknowledgeScreen(const QString& id, qint64 serial, int decodeMilliseconds) {
    const auto c = clients_.value(id);
    if (!c || !c->screenSocket || !c->screenSerial || serial != c->screenSerial || c->screenSize != 0 || !c->screenPacket.isEmpty()
        || decodeMilliseconds < 0 || decodeMilliseconds > 60000) return false;
    c->screenSerial = 0; c->screenSize = 0;
    emit screenChanged();
    if (c->screenDatagram && c->screenMedia) {
        QByteArray ack(16, '\0'); ack.replace(0, 4, "SQVA");
        qToBigEndian<quint64>(quint64(serial), ack.data() + 4);
        qToBigEndian<quint32>(quint32(decodeMilliseconds), ack.data() + 12);
        if (c->screenMedia->sendDatagram(ack)) return true;
    }
    return writeMessage(c->screenSocket, {{"type", "screenAck"}, {"serial", serial}, {"decodeMs", decodeMilliseconds}});
}
