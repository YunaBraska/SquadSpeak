#include "local_channel.hpp"
#include "chat_content.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QSaveFile>
#include <QtEndian>
#include <QUuid>
#include <QPromise>
#include <QFutureWatcher>
#include <openssl/crypto.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <array>
#include <opus.h>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QLoggingCategory>
#include <cerrno>

namespace {
Q_LOGGING_CATEGORY(discoveryLog, "squadspeak.discovery", QtWarningMsg)
Q_LOGGING_CATEGORY(mediaLog, "squadspeak.media", QtWarningMsg)
constexpr quint16 discoveryPort = 48762;
const QHostAddress discoveryGroup(QStringLiteral("239.255.85.73"));
constexpr qsizetype maximumFrame = 65536;
constexpr qsizetype maximumRemoteView = 2 * 1024 * 1024;
constexpr int maximumWindowMessages = 160;
constexpr qsizetype maximumWindowBytes = 512 * 1024;
constexpr qsizetype remoteChunkSize = 16384;
constexpr int maximumPendingPeers = 32;
constexpr int maximumMediaParticipants = 64;
constexpr std::array<int, 4> audioBitrates{32, 20, 12, 8};
const QSet<QString> protocolCapabilities{
    QStringLiteral("udp-audio"), QStringLiteral("udp-screen"), QStringLiteral("adaptive-audio"), QStringLiteral("screen-share"), QStringLiteral("screen-audio"),
    QStringLiteral("remote-control"), QStringLiteral("extended-retention")};
QJsonArray capabilityAdvertisement() {
    QJsonArray result;
    for (const auto& capability : protocolCapabilities) result.append(capability);
    return result;
}
bool parseCapabilities(const QJsonObject& message, QSet<QString>* negotiated = nullptr) {
    if (!message.contains("capabilities")) {
        if (negotiated) negotiated->clear();
        return true;
    }
    const auto value = message.value("capabilities");
    if (!value.isArray() || value.toArray().size() > 32) return false;
    static const QRegularExpression name(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    QSet<QString> offered;
    for (const auto& item : value.toArray()) {
        if (!item.isString() || item.toString().size() < 1 || item.toString().size() > 64
            || !name.match(item.toString()).hasMatch())
            return false;
        offered.insert(item.toString());
    }
    if (negotiated) *negotiated = offered & protocolCapabilities;
    return true;
}
bool validMessageType(const QString& type) {
    static const QRegularExpression name(QStringLiteral("^[A-Za-z][A-Za-z0-9._-]*$"));
    return type.size() >= 1 && type.size() <= 64 && name.match(type).hasMatch();
}
const QSet<QString> knownMessageTypes{
    QStringLiteral("media"), QStringLiteral("audio"), QStringLiteral("audioAck"), QStringLiteral("audioProbe"), QStringLiteral("audioQuality"),
    QStringLiteral("chat"), QStringLiteral("chatKey"), QStringLiteral("control"), QStringLiteral("controlAck"),
    QStringLiteral("controlHello"), QStringLiteral("controlPing"), QStringLiteral("controlWait"), QStringLiteral("discover"),
    QStringLiteral("hello"), QStringLiteral("identify"), QStringLiteral("join"), QStringLiteral("password"),
    QStringLiteral("ping"), QStringLiteral("pong"), QStringLiteral("presence"), QStringLiteral("pttState"),
    QStringLiteral("remoteAction"), QStringLiteral("remoteActionResult"), QStringLiteral("remoteImage"),
    QStringLiteral("remoteImageGet"), QStringLiteral("remoteLevels"), QStringLiteral("remoteOffer"),
    QStringLiteral("remoteViewAck"), QStringLiteral("remoteViewChunk"), QStringLiteral("remoteViewEnd"),
    QStringLiteral("remoteViewStart"), QStringLiteral("roster"), QStringLiteral("screenChunk"),
    QStringLiteral("screenMedia"), QStringLiteral("screenFrame"), QStringLiteral("screenHello"), QStringLiteral("screenLimit"), QStringLiteral("screenAck"),
    QStringLiteral("screenQuality"), QStringLiteral("screenAudio"), QStringLiteral("voiceLeave")};
const QSet<QString> knownChatKinds{
    QStringLiteral("history"), QStringLiteral("send"), QStringLiteral("message"), QStringLiteral("time"),
    QStringLiteral("error"), QStringLiteral("imageStart"), QStringLiteral("imageChunk"),
    QStringLiteral("imageGet"), QStringLiteral("imageOffset"), QStringLiteral("imageData"), QStringLiteral("imageError")};
qint64 queuedBytes(const QSslSocket* socket) {
    return socket->bytesToWrite() + socket->encryptedBytesToWrite();
}

bool addressConnectionFailure(QAbstractSocket::SocketError error, const QSslSocket* socket) {
    // SecureTransport may emit connected before reporting a refused connection.
    // Transport fallback stops once TLS has supplied a certificate or an error.
    return !socket->property("squadTlsSeen").toBool()
        && (error == QAbstractSocket::HostNotFoundError
            || error == QAbstractSocket::ConnectionRefusedError
            || error == QAbstractSocket::NetworkError);
}
void connectEndpoint(QSslSocket* socket, const QString& host, quint16 port, QAbstractSocket::NetworkLayerProtocol protocol) {
    // RFC 6761 localhost names stay on loopback even when the system resolver
    // filters IPv6 because no external IPv6 interface is configured.
    auto relative = host;
    if (relative.endsWith('.')) relative.chop(1);
    const bool loopback = relative == "localhost" || relative.endsWith(".localhost");
    const auto address = loopback
        ? (protocol == QAbstractSocket::IPv6Protocol ? QStringLiteral("::1") : QStringLiteral("127.0.0.1")) : host;
    socket->connectToHostEncrypted(address, port, host, QIODevice::ReadWrite, protocol);
}
bool displayName(const QJsonValue& value) {
    if (!value.isString()) return false;
    try { return VoiceSession::validatedName(value.toString()) == value.toString(); }
    catch (const std::invalid_argument&) { return false; }
}
QVariantList humanMembers(QVariantList members) {
    members.removeIf([](const auto& value) { return value.toMap().value("music").toBool(); });
    return members;
}
bool validPresenceDetails(const QJsonObject& message) {
    return (!message.contains("deafened") || message.value("deafened").isBool())
        && (!message.contains("avatar") || VoiceSession::validAvatar(message.value("avatar").toString()))
        && (!message.contains("avatarId") || VoiceSession::validAvatar(message.value("avatarId").toString()));
}
bool targetAddress(const QHostAddress& address) {
    return !address.isNull() && !address.isMulticast() && address != QHostAddress::Any
        && address != QHostAddress::AnyIPv4 && address != QHostAddress::AnyIPv6 && address != QHostAddress::Broadcast;
}
QString canonicalEndpointHost(const QString& value) {
    const auto text = value.trimmed();
    if (text.isEmpty() || text != value || text.size() > 253) return {};
    const QHostAddress literal(text);
    if (!literal.isNull()) return targetAddress(literal) ? literal.toString() : QString{};
    auto host = text;
    if (host.endsWith('.')) host.chop(1);
    if (host.isEmpty() || host.size() > 253) return {};
    static const QRegularExpression hostname(
        R"(^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$)");
    if (!hostname.match(host).hasMatch()) return {};
    const auto numeric = std::all_of(host.begin(), host.end(), [](QChar c) { return c == '.' || c.isDigit(); });
    return numeric ? QString{} : text.toLower();
}
bool validEndpointHost(const QString& value) {
    return !canonicalEndpointHost(value).isEmpty();
}
QString endpointKey(const QString& host, quint16 port) {
    const auto canonical = canonicalEndpointHost(host);
    const QHostAddress literal(canonical);
    return !literal.isNull()
        ? "[" + literal.toString() + "]:" + QString::number(port)
        : canonical + ":" + QString::number(port);
}
QList<QNetworkInterface> discoveryInterfaces() {
    const auto interfaces = QNetworkInterface::allInterfaces();
    const auto usable = [](const auto& interface) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp)
            || !interface.flags().testFlag(QNetworkInterface::IsRunning)) return false;
        for (const auto& entry : interface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) return true;
        return false;
    };
    QList<QNetworkInterface> result, loopback;
    for (const auto& interface : interfaces) {
        if (!usable(interface)) continue;
        if (interface.flags().testFlag(QNetworkInterface::IsLoopBack)) loopback.append(interface);
        else if (interface.flags().testFlag(QNetworkInterface::CanMulticast)) result.append(interface);
    }
    return result.isEmpty() ? loopback : result;
}
}

struct LocalChannel::Relay final {
    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder{nullptr, opus_decoder_destroy};
    std::array<std::shared_ptr<OpusEncoder>, 4> encoders;
    Relay() {
        int error;
        decoder.reset(opus_decoder_create(48000, 1, &error));
        if (!decoder) throw std::runtime_error(opus_strerror(error));
    }
    std::array<QByteArray, 4> packets(const QByteArray& source, const std::array<bool, 4>& needed, int missing) {
        std::array<QByteArray, 4> result; result[0] = source;
        std::array<float, 960> samples;
        for (int gap = 0; gap < missing; ++gap) {
            const int restored = opus_decode_float(decoder.get(), nullptr, 0, samples.data(), 960, 0);
            if (restored < 0) throw std::runtime_error(opus_strerror(restored));
        }
        const auto size = opus_decode_float(decoder.get(), reinterpret_cast<const unsigned char*>(source.constData()), int(source.size()), samples.data(), 960, 0);
        if (size != 960) throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Voice packets must contain 20 ms of Opus audio."));
        for (size_t tier = 1; tier < needed.size(); ++tier) {
            if (!needed[tier]) { encoders[tier].reset(); continue; }
            auto& encoder = encoders[tier];
            if (!encoder) {
                int error;
                encoder = {opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error), opus_encoder_destroy};
                if (!encoder || opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(audioBitrates[tier] * 1000)) != OPUS_OK
                    || opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(0)) != OPUS_OK
                    || opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(5)) != OPUS_OK)
                    throw std::runtime_error(QT_TRANSLATE_NOOP("LocalChannel", "Voice relay codec could not be configured."));
            }
            auto& packet = result[tier]; packet.resize(audioBitrates[tier] * 1000 / 8 / 50);
            const auto bytes = opus_encode_float(encoder.get(), samples.data(), 960,
                reinterpret_cast<unsigned char*>(packet.data()), int(packet.size()));
            if (bytes < 0) throw std::runtime_error(opus_strerror(bytes));
            packet.resize(bytes);
        }
        return result;
    }
};

bool LocalChannel::unknownMessageType(const QString& type) { return !knownMessageTypes.contains(type); }

LocalChannel::LocalChannel(VoiceSession& session, QString storageFile,
                           std::optional<TlsIdentity> identity, std::function<qint64()> clock, QObject* parent)
    : LocalChannel(session, std::move(storageFile), std::move(identity), std::move(clock), parent, nullptr, {}) {}

LocalChannel::LocalChannel(VoiceSession& session, QString storageFile, std::optional<TlsIdentity> identity,
                           std::function<qint64()> clock, QObject* parent, LocalChannel* service, QString channelId)
    : QObject(parent), service_(service), channelId_(std::move(channelId)), session_(session), storageFile_(std::move(storageFile)),
      identity_(std::move(identity)), clock_(std::move(clock)) {
    bool migrateName = !service_ && !session_.legacyChannelName().isEmpty();
    if (migrateName) channelName_ = session_.legacyChannelName();
    QFile file(storageFile_);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
            throw std::runtime_error(tr("Channel permissions could not be read.").toStdString());
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        const auto object = document.object();
        if (error.error != QJsonParseError::NoError || !document.isObject()
            || object.value("version").toInt() != 1 || !object.value("approved").isArray()
            || !object.value("attempts").isObject() || !object.value("requestsAllowed").isBool())
            throw std::runtime_error(tr("Saved channel permissions are invalid.").toStdString());
        for (const auto value : object.value("approved").toArray()) {
            if (!value.isString() || !validId(value.toString())) throw std::runtime_error(tr("Invalid device permission.").toStdString());
            approved_.insert(value.toString());
        }
        const auto attempts = object.value("attempts").toObject();
        for (auto it = attempts.begin(); it != attempts.end(); ++it) {
            const auto value = it.value().toObject();
            const auto count = value.value("count").toInt(-1);
            const auto next = value.value("next").toInteger(-1);
            const auto identityKey = it.key().startsWith("ptt:") ? it.key().mid(4)
                : it.key().startsWith("pw:") ? it.key().mid(3) : it.key();
            if (!validId(identityKey) || count < 1 || count > 6 || next < 0)
                throw std::runtime_error(tr("Invalid request delay.").toStdString());
            attempts_.insert(it.key(), {count, next});
        }
        requestsAllowed_ = object.value("requestsAllowed").toBool();
        if (object.contains("ownedChannels")) {
            if (service_ || !object.value("ownedChannels").isArray() || object.value("ownedChannels").toArray().size() > 9)
                throw std::runtime_error(tr("Invalid owned channel list.").toStdString());
            for (const auto& value : object.value("ownedChannels").toArray()) {
                const auto id = value.toString();
                if (!validId(id) || ownedIds_.contains(id) || id == ownId())
                    throw std::runtime_error(tr("Invalid owned channel identity.").toStdString());
                ownedIds_.append(id);
            }
        }
        if (object.contains("channelName")) {
            channelName_ = VoiceSession::validatedName(object.value("channelName").toString());
            migrateName = false;
        }
        configuredPort_ = object.value("servicePort").toInt(defaultPort);
        if (object.contains("botName")) botName_ = VoiceSession::validatedName(object.value("botName").toString());
        messageLifetimeDays_ = object.value("messageLifetimeDays").toInt(1);
        if (configuredPort_ < 1 || configuredPort_ > 65535
            || !ChatHistory::validLifetime(qint64(messageLifetimeDays_) * ChatHistory::lifetime)
            || (object.contains("servicePort") && object.value("servicePort").toDouble() != configuredPort_)
            || (object.contains("messageLifetimeDays") && object.value("messageLifetimeDays").toDouble() != messageLifetimeDays_))
            throw std::runtime_error(tr("Invalid host port or message lifetime.").toStdString());
        if (object.contains("channels") && !object.value("channels").isObject())
            throw std::runtime_error(tr("Invalid saved channel list.").toStdString());
        savedChannels_ = object.value("channels").toObject();
        for (auto it = savedChannels_.begin(); it != savedChannels_.end(); ++it) {
            const auto entry = it.value().toObject();
            const auto port = entry.value("port").toInt();
            const auto joined = entry.value("lastJoined").toInteger(-1);
            if (!validId(it.key()) || (entry.contains("deviceId") && !validId(entry.value("deviceId").toString()))
                || !displayName(entry.value("name"))
                || !validEndpointHost(entry.value("address").toString())
                || port < 1 || port > 65535 || !entry.value("autoJoin").isBool()
                || (entry.contains("remoteControl") && !entry.value("remoteControl").isBool())
                || joined < 0 || joined > 9007199254740991LL || entry.value("lastJoined").toDouble(-1) != double(joined))
                throw std::runtime_error(tr("Invalid saved channel.").toStdString());
        }
        if (object.contains("endpointPins") && !object.value("endpointPins").isObject())
            throw std::runtime_error(tr("Saved address bindings are invalid.").toStdString());
        const auto pins = object.value("endpointPins").toObject();
        for (auto it = pins.begin(); it != pins.end(); ++it) {
            if (!it.value().isString() || !validId(it.value().toString()) || it.key().size() > 300)
                throw std::runtime_error(tr("Saved address binding is invalid.").toStdString());
            endpointPins_.insert(it.key(), it.value().toString());
        }
        if (object.contains("security")) {
            const auto security = object.value("security").toObject();
            const auto verifier = QByteArray::fromBase64Encoding(security.value("verifier").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!security.value("verifier").isString() || !security.value("saved").isObject()
                || (!security.value("verifier").toString().isEmpty() && (!verifier || verifier.decoded.size() != 64)))
                throw std::runtime_error(tr("Saved password data is invalid.").toStdString());
            const auto saved = security.value("saved").toObject();
            for (auto it = saved.begin(); it != saved.end(); ++it) {
                const auto value = QByteArray::fromBase64Encoding(it.value().toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
                if (!validId(it.key()) || !it.value().isString() || !value || value.decoded.size() < 29 || value.decoded.size() > 1052)
                    throw std::runtime_error(tr("Saved channel password is invalid.").toStdString());
            }
            security_ = security;
            if (security.contains("blocked") && !security.value("blocked").isObject())
                throw std::runtime_error(tr("Invalid stored channel blocks.").toStdString());
            const auto blocked = security.value("blocked").toObject();
            for (auto it = blocked.begin(); it != blocked.end(); ++it)
                if (!validId(it.key()) || !displayName(it.value()))
                    throw std::runtime_error(tr("Invalid blocked device.").toStdString());
        }
        if (object.contains("control")) {
            const auto control = object.value("control").toObject();
            if (!object.value("control").isObject() || !control.value("controllers").isObject() || !control.value("target").isObject()
                || (control.contains("remoteMode") && !control.value("remoteMode").isBool()))
                throw std::runtime_error(tr("Saved remote control is invalid.").toStdString());
            const auto controllers = control.value("controllers").toObject();
            for (auto it = controllers.begin(); it != controllers.end(); ++it) {
                const auto entry = it.value().toObject();
                const auto revision = entry.value("revision").toInteger(-1);
                if (!validId(it.key()) || !displayName(entry.value("name")) || !entry.value("held").isBool()
                    || (entry.contains("remote") && !entry.value("remote").isBool())
                    || revision < 0 || revision > 9007199254740991LL || entry.value("revision").toDouble(-1) != double(revision))
                    throw std::runtime_error(tr("Saved controller device is invalid.").toStdString());
            }
            const auto target = control.value("target").toObject();
            if (!target.isEmpty() && (!validId(target.value("id").toString()) || !displayName(target.value("name"))
                || !validEndpointHost(target.value("address").toString()) || target.value("port").toInt() < 1
                || target.value("port").toInt() > 65535 || !target.value("pairing").isBool() || !target.value("blocked").isBool()
                || (target.contains("revoked") && !target.value("revoked").isBool())))
                throw std::runtime_error(tr("Saved control target is invalid.").toStdString());
            control_ = control;
            if (!control_.contains("remoteMode") && !control_.value("target").toObject().isEmpty()) control_.insert("remoteMode", true);
        }
    }
    passwordWorkers_.setMaxThreadCount(2);
    connect(&server_, &QSslServer::pendingConnectionAvailable, this, &LocalChannel::acceptConnections);
    connect(&server_, &QSslServer::errorOccurred, this, [this](QSslSocket* socket, QAbstractSocket::SocketError) {
        setStatus(tr("Encrypted join failed: %1").arg(socket->errorString()), false);
    });
    connect(&server_, &QSslServer::sslErrors, this, [](QSslSocket* socket, const QList<QSslError>& errors) {
        if (!TlsIdentity::peerId(socket->peerCertificate()).isEmpty() && acceptableCertificateErrors(errors))
            socket->ignoreSslErrors(errors);
    });
    connect(&discovery_, &QUdpSocket::readyRead, this, &LocalChannel::receiveDiscovery);
    heartbeat_.setInterval(1500);
    connect(&heartbeat_, &QTimer::timeout, this, &LocalChannel::announce);
    maintenanceTimer_.setInterval(1000);
    connect(&maintenanceTimer_, &QTimer::timeout, this, [this] {
        const auto elapsed = controlClock_.elapsed();
        if (passwordRequired() && passwordRetrySeconds() > 0) emit stateChanged();
        for (auto* socket : peers_.keys()) {
            auto& peer = peers_[socket];
            if (elapsed - peer.lastActivity > (peer.closing ? 5000 : 8000)) { socket->abort(); continue; }
            if (peer.joined && !peer.controller && (!peer.observer || receivesScreenAudio(peer)) && peer.adaptiveAudio && (!peer.media || !peer.media->udpActive())) {
                if ((peer.audioProbeSent >= 0 && elapsed - peer.audioProbeSent > 350) || queuedBytes(socket) > 16384)
                    sampleAudioLink(socket, -1);
                if (!peers_.contains(socket)) continue;
                if (peer.audioProbeSent < 0) {
                    peer.audioProbeSent = elapsed;
                    writeMessage(socket, {{"type", "audioProbe"}, {"serial", ++peer.audioProbe}});
                }
            }
            if (peer.joined && !peer.controller && !peer.sleeping && clock_() - peer.lastSpoke >= 3600000) {
                peer.sleeping = true; broadcastRoster();
            }
        }
        for (auto* socket : viewers_.keys()) {
            if (viewers_.value(socket).waiting && viewers_.value(socket).datagram && elapsed - viewers_.value(socket).sent >= 1000) {
                finishScreen(socket, viewers_.value(socket).serial, 60000, true); continue;
            }
            if (elapsed - viewers_.value(socket).sent > 8000 && viewers_.value(socket).waiting) socket->abort();
            else if (viewers_.value(socket).tier == 4 && elapsed >= viewers_.value(socket).resumeAt) {
                auto& v = viewers_[socket]; v.tier = 3; v.keyFrameNeeded = true; v.badSamples = 0;
                writeMessage(socket, {{"type", "screenQuality"}, {"tier", 3}}); emit screenChanged();
            }
        }
        for (const auto& connection : clients_) {
            auto& c = *connection;
            if (!c.socket) {
                if (c.reconnectWanted && elapsed >= c.reconnectAt && !remoteMode()) connectClient(c);
                continue;
            }
            if (elapsed - c.reply > 8000) {
                if (c.voice && automaticAttempt_) advanceAutoJoin(false);
                else { closeClient(c); setClientStatus(c, tr("Host is not responding. Reconnecting."), false); }
            } else if (c.socket->isEncrypted() && elapsed - c.probe >= 2000) {
                c.probe = elapsed; writeMessage(c.socket, {{"type", "ping"}});
            }
            if (c.accepted && c.screenAvailable && c.screenWanted && !c.screenSocket) connectScreen(c);
            if (c.passwordAutoRetry && c.passwordRequired && c.passwordRetryAt <= elapsed)
                submitPassword(c, c.password, c.rememberPassword);
        }
        if (hosting_ && history_) {
            if (!botSleeping_ && clock_() - botLastActive_ >= 3600000) { botSleeping_ = true; broadcastRoster(); }
            try {
                if (history_->expire(clock_())) {
                    for (auto* socket : peers_.keys())
                        if (peers_.value(socket).joined && !peers_.value(socket).controller)
                            writeChat(socket, {{"kind", "time"}, {"now", history_->time()}});
                }
            } catch (const std::exception& error) {
                chatError_ = QString::fromUtf8(error.what()); emit chatChanged();
            }
        }
        for (const auto& connection : clients_) {
            auto& c = *connection;
            if (!c.chatClock.isValid()) continue;
            const auto now = c.serverTime + c.chatClock.elapsed();
            QJsonArray retained;
            for (const auto value : c.messages) if (value.toObject().value("expires").toInteger() > now) retained.append(value);
            if (retained.size() != c.messages.size()) { c.messages = retained; emit chatChanged(); }
            if (!c.pending.isEmpty() && now >= c.pendingUntil) {
                c.pending = {}; c.pendingImage.clear(); c.chatError = tr("Delivery was not confirmed. The 24-hour period has expired."); emit chatChanged();
            }
        }
        QSet<QString> activeImages;
        const auto records = remoteMode() ? remoteView_.value("messages").toList() : messages();
        for (const auto& value : records) activeImages.insert(value.toMap().value("image").toMap().value("hash").toString());
        bool removedImage = false;
        for (const auto& hash : imageCache_.keys()) if (!activeImages.contains(hash)) { imageCache_.remove(hash); removedImage = true; }
        if (removedImage) { ++imageRevision_; emit imagesChanged(); }
    });
    maintenanceTimer_.start();
    controlClock_.start();
    controlTimer_.setInterval(2000);
    connect(&controlTimer_, &QTimer::timeout, this, [this] {
        if (!controller_) { connectControl(); return; }
        if (controlClock_.elapsed() - controlReply_ > 6000) {
            closeControl(); setControlStatus(tr("Remote control interrupted. The last key state is retained."));
        } else if (controlConnected_) sendControlState();
        else if (controlPending_) writeMessage(controller_, {{"type", "controlPing"}});
    });
    connect(&session_, &VoiceSession::presenceChanged, this, &LocalChannel::sendPresence);
    connect(&session_, &VoiceSession::preferencesChanged, this, [this] {
        if (hosting()) { announce(); broadcastRoster(); }
        if (controlConnected_) sendControlState();
    });
    connect(this, &LocalChannel::chatSent, this, [this](const QString& text, const QString& hostId) {
        if (!remoteChatSocket_ || remotePendingChat_ != text || remotePendingHost_ != hostId) return;
        writeMessage(remoteChatSocket_, {{"type", "remoteActionResult"}, {"action", "chat"},
            {"ok", true}, {"delivered", true}, {"text", text}});
        remoteChatSocket_ = nullptr; remotePendingChat_.clear(); remotePendingHost_.clear();
    });
    connect(this, &LocalChannel::chatChanged, this, [this] {
        if (!remoteChatSocket_) return;
        const auto pending = clients_.value(remotePendingHost_);
        if (pending && !pending->pending.isEmpty()) return;
        writeMessage(remoteChatSocket_, {{"type", "remoteActionResult"}, {"action", "chat"}, {"ok", false}, {"error", chatError().isEmpty() ? tr("Chat canceled because the channel changed.") : chatError()}});
        remoteChatSocket_ = nullptr; remotePendingChat_.clear(); remotePendingHost_.clear();
    });
    const auto changedView = [this] {
        if (remoteViewPending_) return;
        remoteViewPending_ = true;
        QTimer::singleShot(0, this, [this] { remoteViewPending_ = false; sendRemoteView(); });
    };
    connect(this, &LocalChannel::stateChanged, this, changedView);
    connect(this, &LocalChannel::hostsChanged, this, changedView);
    connect(this, &LocalChannel::participantsChanged, this, changedView);
    connect(this, &LocalChannel::chatChanged, this, changedView);
    const auto imageReady = [this] { for (auto* socket : peers_.keys()) sendRemoteImage(socket); };
    connect(this, &LocalChannel::imagesChanged, this, imageReady);
    connect(this, &LocalChannel::chatChanged, this, imageReady);
    connect(&session_, &VoiceSession::presenceChanged, this, changedView);
    connect(&session_, &VoiceSession::preferencesChanged, this, [this, previousSupporter = session_.supporterEnabled()]() mutable {
        if (!session_.supporterEnabled()) setScreenSharing(false);
        if (previousSupporter != session_.supporterEnabled()) {
            previousSupporter = session_.supporterEnabled();
            for (auto* socket : peers_.keys()) sendChatKey(socket);
            emit stateChanged();
        }
        if (!service_ && ready() && hosting()) syncOwnedChannels();
    });
    if (service_) { controlTimer_.stop(); autoJoinTimer_.stop(); }
    updateControlIntent();
    const auto target = control_.value("target").toObject();
    if (remoteMode() && !target.isEmpty()) controlStatus_ = target.value("blocked").toBool()
        ? tr("Remote control stopped. Request pairing again or return to your own device.")
        : tr("Reconnecting to the control target. The last key state is retained.");
    QTimer::singleShot(0, this, &LocalChannel::connectControl);
    autoJoinTimer_.setInterval(15000);
    connect(&autoJoinTimer_, &QTimer::timeout, this, [this] {
        if (!joinedHostId_.isEmpty()) return;
        triedAutoJoin_.clear(); tryAutoJoin();
    });
    if (!service_) autoJoinTimer_.start();
    QTimer::singleShot(0, this, &LocalChannel::tryAutoJoin);
    if (ready()) status_ = tr("Ready for local channels.");
    if (migrateName && !persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_))
        throw std::runtime_error(status_.toStdString());
}

LocalChannel::~LocalChannel() {
    // Destruction emits signals. Observers must not retrieve a child whose
    // final shared reference is already being released.
    auto children = std::exchange(owned_, {});
    children.clear();
    clearDiscoverySearch();
    controlTimer_.stop();
    closeControl();
    finishDirect();
    closeClients();
    stopHost();
    for (auto* socket : peers_.keys()) socket->abort();
    server_.close();
}

LocalChannel::Client* LocalChannel::chatClient() const {
    return clients_.value(chatHostId_).get();
}

bool LocalChannel::joined() const {
    const auto c = clients_.value(joinedHostId_);
    return c && c->voice && c->accepted;
}

int LocalChannel::receiveAudioBitrate() const {
    const auto c = clients_.value(joinedHostId_);
    return c && c->voice && c->accepted ? c->audioBitrate : 0;
}

QVariantList LocalChannel::participants() const {
    return humanMembers(audioSources());
}

QVariantList LocalChannel::audioSources() const {
    const auto c = clients_.value(joinedHostId_);
    auto sources = c && c->accepted ? c->members : QVariantList{};
    for (const auto& client : clients_) if (client != c && receivesScreenAudio(*client))
        for (const auto& member : client->members) if (member.toMap().value("music").toBool()) sources.append(member);
    return sources;
}

QVariantList LocalChannel::chatMembers() const {
    const auto* c = chatClient();
    return c && c->accepted ? humanMembers(c->members) : QVariantList{};
}

QVariantMap LocalChannel::chatBot() const {
    const auto* c = chatClient();
    if (c && c->accepted) for (const auto& member : c->members)
        if (member.toMap().value("music").toBool()) return member.toMap();
    return {};
}

QStringList LocalChannel::chatOnlineIds() const {
    const auto* c = chatClient(); return c && c->accepted ? c->onlineIds : QStringList{};
}
bool LocalChannel::chatPresenceKnown() const {
    const auto* c = chatClient(); return c && c->accepted && c->presenceKnown;
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
bool LocalChannel::passwordRequired() const { const auto* c = chatClient(); return c && c->passwordRequired; }
bool LocalChannel::passwordSaved() const { return security_.value("saved").toObject().contains(chatHostId_); }

bool LocalChannel::submitPassword(const QString& password, bool remember) {
    auto* c = chatClient(); return c && submitPassword(*c, password, remember);
}

bool LocalChannel::sendChatCommand(const QJsonObject& payload) {
    auto* c = chatClient(); return c && sendChatCommand(*c, payload);
}

bool LocalChannel::setClientStatus(Client& c, QString text, bool result) {
    c.status = std::move(text);
    if (c.id == chatHostId_ || (chatHostId_.isEmpty() && c.id == joinedHostId_)) status_ = c.status;
    emit stateChanged(); emit hostsChanged(); return result;
}

void LocalChannel::selectChat(const QString& id) {
    if (chatHostId_ == id) return;
    if (auto* previous = chatClient()) {
        previous->messages = {}; previous->historyRequest.clear(); previous->hasOlder = false; previous->hasNewer = false;
    }
    chatHostId_ = id;
    imageQueue_.clear(); downloadingImage_.clear(); downloadedImage_.clear(); imageCache_.clear();
    ++imageRevision_; emit imagesChanged();
    if (auto* c = chatClient()) {
        status_ = c->status;
        if (c->accepted && c->chatKey.size() == 32) requestHistory(*c, "latest");
    }
    emit chatChanged(); emit screenChanged(); emit participantsChanged(); emit stateChanged();
}

bool LocalChannel::openConnection(const QString& id, const QString& address, int port, bool voice, bool automatic) {
    if (hostOnly_ || service_) return setStatus(tr("Headless mode has no personal channel participation."), false);
    if (remoteMode()) return setStatus(tr("Leave remote mode before opening a channel."), false);
    const auto canonicalAddress = canonicalEndpointHost(address);
    if (!ready() || !validId(id) || canonicalAddress.isEmpty() || port < 1 || port > 65535)
        return setStatus(tr("The channel address or device identity is invalid."), false);
    if (!voice) {
        if (auto c = clients_.value(id); c && c->socket && (c->accepted || c->access == "pending" || c->passwordRequired)) {
            selectChat(id); return true;
        }
    } else {
        leaveChannel(!automatic);
        automaticAttempt_ = automatic;
        joinedHostId_ = id;
    }
    auto c = clients_.value(id);
    if (c) { c->reconnectWanted = false; closeClient(*c); }
    else {
        c = std::make_shared<Client>(); c->id = id; clients_.insert(id, c);
    }
    c->deviceId = ownChannel(id) ? ownId() : savedChannels_.value(id).toObject().value("deviceId").toString();
    if (c->deviceId.isEmpty()) c->deviceId = hosts_.value(id).deviceId;
    if (c->deviceId.isEmpty()) c->deviceId = id;
    c->name = hosts_.contains(id) ? hosts_.value(id).name
        : savedChannels_.value(id).toObject().value("name").toString(c->name);
    c->address = canonicalAddress; c->port = quint16(port); c->voice = voice;
    c->access = "connecting"; c->passwordRequired = false; c->passwordRetryAt = 0;
    c->password.clear(); c->rememberPassword = security_.value("saved").toObject().contains(id);
    try {
        if (c->rememberPassword) {
            const auto context = passwordContext(id);
            c->password = QString::fromUtf8(TlsIdentity::open(
                QByteArray::fromBase64(security_.value("saved").toObject().value(id).toString().toLatin1()),
                identity_->deriveKey(context, "channel password"), context));
        }
    } catch (const std::exception& error) { c->access = "error"; return setClientStatus(*c, tr(error.what()), false); }
    selectChat(id);
    c->reconnectWanted = true;
    connectClient(*c);
    return true;
}

bool LocalChannel::openChat(const QString& id, const QString& address, int port) {
    return openConnection(id, address, port, false, false);
}

bool LocalChannel::openSavedChat(const QString& id) {
    if (auto c = clients_.value(id)) return openChat(id, c->address, c->port);
    const auto entry = savedChannels_.value(id).toObject();
    const auto host = hosts_.constFind(id);
    if (entry.isEmpty() && host == hosts_.cend()) return false;
    const auto [address, port] = channelEndpoint(id);
    return openChat(id, address, port);
}

QPair<QString, int> LocalChannel::channelEndpoint(const QString& id) const {
    const auto entry = savedChannels_.value(id).toObject();
    const auto address = entry.value("address").toString();
    const auto discovered = hosts_.constFind(id);
    if ((!address.isEmpty() && QHostAddress(address).isNull()) || discovered == hosts_.cend())
        return {address, entry.value("port").toInt()};
    return {discovered->address, discovered->port};
}

bool LocalChannel::closeChat(const QString& id) {
    auto c = clients_.value(id);
    if (!c) return false;
    if (id == joinedHostId_) leave();
    c->reconnectWanted = false; closeClient(*c);
    if (chatHostId_ == id) selectChat({});
    clients_.remove(id); emit hostsChanged(); return true;
}

void LocalChannel::closeClients() {
    leaveChannel(false);
    for (const auto& c : clients_) { c->reconnectWanted = false; closeClient(*c); }
    selectChat({}); clients_.clear();
}

QVariantList LocalChannel::ownedChannels() const {
    if (service_) return service_->ownedChannels();
    QVariantList result{{QVariantMap{{"id", channelId()}, {"name", channelName_}, {"hosting", hosting()}}}};
    for (const auto& id : ownedIds_) if (const auto child = owned_.value(id))
        result.append(QVariantMap{{"id", id}, {"name", child->channelName()}, {"hosting", child->hosting()}});
    return result;
}

LocalChannel* LocalChannel::ownChannel(const QString& id) const {
    if (service_) return service_->ownChannel(id);
    return id == channelId() ? const_cast<LocalChannel*>(this) : owned_.value(id).get();
}

bool LocalChannel::syncOwnedChannels() {
    if (service_ || !ready()) return false;
    for (const auto& id : ownedIds_) {
        if (id == ownId()) return setStatus(tr("Invalid owned channel identity."), false);
        auto child = owned_.value(id);
        if (!child) {
            const auto directory = storageFile_ + ".hosts/" + id;
            if (!QFile::exists(directory) && QFile::exists(directory + ".deleted"))
                QDir().rename(directory + ".deleted", directory);
            if (!QFile::exists(directory + "/channel.json")) return setStatus(tr("An owned channel's settings are missing."), false);
            try { child.reset(new LocalChannel(session_, directory + "/channel.json", identity_, clock_, this, this, id)); }
            catch (const std::exception& error) { return setStatus(tr(error.what()), false); }
            owned_.insert(id, child);
            connect(child.get(), &LocalChannel::stateChanged, this, [this] { emit ownedChannelsChanged(); emit hostsChanged(); });
            connect(child.get(), &LocalChannel::requestsChanged, this, &LocalChannel::requestsChanged);
            connect(child.get(), &LocalChannel::screenChanged, this, &LocalChannel::screenChanged);
        }
        child->hostOnly_ = hostOnly_;
        if (hosting() && session_.supporterEnabled()) {
            if (!child->listen(server_.serverAddress(), port())) return setStatus(child->status(), false);
        } else if (child->hosting()) child->stopHost();
    }
    const QDir directory(storageFile_ + ".hosts");
    for (const auto& name : directory.entryList({"*.deleted"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto id = name.chopped(8);
        if (validId(id) && !ownedIds_.contains(id) && !QDir(directory.filePath(name)).removeRecursively())
            return setStatus(tr("Removed channel data could not be deleted."), false);
    }
    emit ownedChannelsChanged(); emit hostsChanged(); return true;
}

QString LocalChannel::addOwnedChannel(const QString& name) {
    if (service_ || !ready() || !hosting() || !session_.supporterEnabled() || ownedIds_.size() >= 9) {
        setStatus(tr("An active Supporter pass allows up to ten own channels."), false); return {};
    }
    QString validated;
    try { validated = VoiceSession::validatedName(name); }
    catch (const std::invalid_argument& error) { setStatus(tr(error.what()), false); return {}; }
    const auto id = QString::fromLatin1(TlsIdentity::newKey().toHex());
    const auto directory = storageFile_ + ".hosts/" + id;
    if (!QDir().mkpath(directory)) { setStatus(tr("Channel storage could not be created."), false); return {}; }
    {
        LocalChannel child(session_, directory + "/channel.json", identity_, clock_, nullptr, this, id);
        if (!child.setChannelName(validated)) { setStatus(child.status(), false); QDir(directory).removeRecursively(); return {}; }
    }
    ownedIds_.append(id);
    if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_)) {
        ownedIds_.removeAll(id); QDir(directory).removeRecursively(); return {};
    }
    if (!syncOwnedChannels()) return {};
    announce(); return id;
}

bool LocalChannel::removeOwnedChannel(const QString& id) {
    if (service_ || !ownedIds_.contains(id)) return false;
    const auto child = owned_.value(id);
    if (!child) return false;
    const auto directory = QFileInfo(child->storageFile_).absolutePath();
    // Windows cannot rename a directory while SQLite holds a file inside it.
    // Keep the connection's chat key and membership if removal is rolled back.
    const bool hadHistory = bool(child->history_);
    child->history_.reset();
    const auto reopen = [&] {
        if (!hadHistory) return true;
        try { child->history_ = std::make_unique<ChatHistory>(child->storageFile_ + ".chat", *child->identity_, clock_()); return true; }
        catch (const std::exception& error) { child->stopHost(); return setStatus(tr(error.what()), false); }
    };
    if (!QDir().rename(directory, directory + ".deleted")) {
        if (!reopen()) return false;
        return setStatus(tr("Channel data could not be prepared for removal."), false);
    }
    const auto previous = ownedIds_; ownedIds_.removeAll(id);
    auto saved = savedChannels_; saved.remove(id);
    if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &saved)) {
        ownedIds_ = previous;
        if (QDir().rename(directory + ".deleted", directory)) reopen();
        else child->stopHost(); // Startup recovers the retained deletion directory.
        return false;
    }
    closeChat(id); child->stopHost(); owned_.remove(id);
    // The channel is offline before deleting its private directory. A leftover
    // deletion directory is retried at startup, without restoring access.
    const bool removed = QDir(directory + ".deleted").removeRecursively();
    emit ownedChannelsChanged(); announce();
    return removed || setStatus(tr("Removed channel data could not be deleted."), false);
}

QJsonArray LocalChannel::channelDirectory() const {
    QJsonArray result;
    if (hosting()) result.append(QJsonObject{{"id", channelId()}, {"name", channelName_}});
    for (const auto& id : ownedIds_) if (const auto child = owned_.value(id); child && child->hosting())
        result.append(QJsonObject{{"id", id}, {"name", child->channelName()}});
    return result;
}

bool LocalChannel::receiveDirectory(const QString& device, const QJsonObject& message, const QString& address,
                                    quint16 port, bool direct, bool scanned) {
    const auto directory = message.contains("channels") ? message.value("channels").toArray()
        : QJsonArray{QJsonObject{{"id", device}, {"name", message.value("channel")}}};
    if (!validId(device) || directory.isEmpty() || directory.size() > 10
        || (message.contains("channels") && !message.value("channels").isArray())) return false;
    QSet<QString> ids;
    for (const auto& entry : directory) {
        const auto value = entry.toObject(); const auto id = value.value("id").toString();
        if (!validId(id) || !displayName(value.value("name")) || ids.contains(id)) return false;
        if ((id == ownId() || ownedIds_.contains(id)) && device != ownId()) return false;
        const auto saved = savedChannels_.value(id).toObject();
        const auto pinned = saved.value("deviceId").toString(id);
        if (!saved.isEmpty() && pinned != device) return false;
        ids.insert(id);
    }
    for (auto it = hosts_.begin(); it != hosts_.end();) {
        if (it->deviceId == device && !ids.contains(it.key())) it = hosts_.erase(it); else ++it;
    }
    for (const auto& entry : directory) {
        const auto value = entry.toObject(); const auto id = value.value("id").toString();
        if (hosts_.size() >= 1024 && !hosts_.contains(id)) break;
        hosts_.insert(id, {value.value("name").toString(), address, port, clock_(), direct || hosts_.value(id).direct, scanned, device});
    }
    emit hostsChanged(); return true;
}

bool LocalChannel::validId(const QString& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](auto c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool LocalChannel::validMembers(const QJsonValue& value) {
    if (!value.isArray()) return false;
    QSet<QString> ids;
    for (const auto item : value.toArray()) {
        const auto member = item.toObject(); const auto id = member.value("id").toString();
        if (!validId(id) || ids.contains(id) || !displayName(member.value("name"))
            || !member.value("available").isBool() || !member.value("muted").isBool()
            || (member.contains("music") && !member.value("music").isBool())
            || !validPresenceDetails(member) || (member.contains("sleeping") && !member.value("sleeping").isBool())) return false;
        ids.insert(id);
    }
    return true;
}

bool LocalChannel::localAddress(const QHostAddress& address) {
    if (address.isLoopback() || address.isLinkLocal()) return true;
    return address.isInSubnet(QHostAddress("10.0.0.0"), 8)
        || address.isInSubnet(QHostAddress("172.16.0.0"), 12)
        || address.isInSubnet(QHostAddress("192.168.0.0"), 16)
        || address.isInSubnet(QHostAddress("100.64.0.0"), 10)
        || address.isInSubnet(QHostAddress("fc00::"), 7);
}

bool LocalChannel::setStatus(QString text, bool result) {
    status_ = std::move(text);
    emit stateChanged();
    return result;
}

bool LocalChannel::setControlStatus(QString text, bool result) {
    controlStatus_ = std::move(text); emit controlChanged(); return result;
}

bool LocalChannel::initialize(bool hostOnly) {
    hostOnly_ = hostOnly_ || hostOnly;
    if (hostOnly_) {
        autoJoinTimer_.stop();
        controlTimer_.stop();
        closeControl();
        closeClients();
        publishRemoteAvailability();
    }
    if (ready()) {
        if (!startHost()) return false;
        if (!hostOnly_ && !remoteMode() && chatHostId_.isEmpty()) openChat(ownId(), "127.0.0.1", port());
        startDiscovery();
        return true;
    }
    if (initializing_) return true;
    initializing_ = true;
    setStatus(tr("Loading protected device identity."));
    const auto slot = QStringLiteral("device/") + QString::fromLatin1(QCryptographicHash::hash(
        QFileInfo(storageFile_).absoluteFilePath().toUtf8(), QCryptographicHash::Sha256).toHex());
    return TlsIdentity::load(slot, this, [this](std::optional<TlsIdentity> identity, QString error) {
        initializing_ = false;
        if (!identity) { setStatus(error, false); return; }
        identity_ = std::move(identity);
        setStatus(tr("Ready for local channels."));
        initialize(hostOnly_);
    });
}

bool LocalChannel::persist(const QSet<QString>& approved, const QHash<QString, Attempt>& attempts, bool allowed,
                           const QHash<QString, QString>& pins, const QJsonObject& control, const QJsonObject& security,
                           const QJsonObject* channels) {
    const auto& saved = channels ? *channels : savedChannels_;
    QJsonArray approvals;
    for (const auto& id : approved) approvals.append(id);
    QJsonObject retries;
    for (auto it = attempts.begin(); it != attempts.end(); ++it)
        retries.insert(it.key(), QJsonObject{{"count", it->count}, {"next", it->next}});
    QJsonObject addresses;
    for (auto it = pins.begin(); it != pins.end(); ++it) addresses.insert(it.key(), it.value());
    QJsonObject object{{"version", 1}, {"channelName", channelName_}, {"botName", botName_}, {"servicePort", configuredPort_}, {"messageLifetimeDays", messageLifetimeDays_}, {"approved", approvals},
        {"attempts", retries}, {"requestsAllowed", allowed}, {"endpointPins", addresses}, {"control", control}, {"security", security}, {"channels", saved}};
    if (!service_) object.insert("ownedChannels", QJsonArray::fromStringList(ownedIds_));
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (data.size() > 1024 * 1024)
        return setStatus(tr("The local permission store is full."), false);
    QSaveFile file(storageFile_);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        return setStatus(tr("Channel permissions could not be saved: %1").arg(file.errorString()), false);
    approved_ = approved;
    attempts_ = attempts;
    requestsAllowed_ = allowed;
    endpointPins_ = pins;
    const bool wasRemote = remoteMode();
    control_ = control;
    security_ = security;
    savedChannels_ = saved;
    updateControlIntent();
    if (wasRemote != remoteMode()) publishRemoteAvailability();
    emit stateChanged();
    if (channels) emit hostsChanged();
    return true;
}

int LocalChannel::passwordRetrySeconds() const {
    const auto* c = chatClient();
    return c ? int(std::max<qint64>(0, c->passwordRetryAt - controlClock_.elapsed() + 999) / 1000) : 0;
}

QByteArray LocalChannel::passwordContext(const QString& host) const {
    return "SquadSpeak/password/v1/" + ownId().toUtf8() + '/' + host.toUtf8();
}

bool LocalChannel::hashPassword(QByteArray password, QByteArray salt, std::function<void(QByteArray, QString)> completion) {
    auto* device = service_ ? service_ : this;
    if (device->passwordChecks_ >= 2) return false;
    ++device->passwordChecks_;
    auto* watcher = new QFutureWatcher<QByteArray>(device);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, device, [device, owner = QPointer<LocalChannel>(this), watcher, completion = std::move(completion)] {
        --device->passwordChecks_;
        QByteArray hash;
        QString error;
        try { hash = watcher->result(); }
        catch (const std::exception& failure) { error = tr(failure.what()); }
        watcher->deleteLater();
        if (owner) completion(std::move(hash), std::move(error));
    });
    QPromise<QByteArray> promise;
    watcher->setFuture(promise.future());
    device->passwordWorkers_.start([password = std::move(password), salt = std::move(salt), promise = std::move(promise)]() mutable {
        promise.start();
        try { promise.addResult(TlsIdentity::passwordHash(password, salt)); }
        catch (...) { promise.setException(std::current_exception()); }
        password.fill(0);
        promise.finish();
    });
    return true;
}

bool LocalChannel::validHostPassword(const QString& password) {
    return password.toUtf8().size() <= 1024 && QString::fromUtf8(password.toUtf8()) == password
        && std::none_of(password.begin(), password.end(), [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; });
}

bool LocalChannel::setHostPassword(const QString& password) {
    if (passwordBusy_) return false;
    if (!validHostPassword(password))
        return setStatus(tr("The password may contain at most 1024 UTF-8 bytes and no control characters."), false);
    if (password.isEmpty()) {
        auto next = security_; next.insert("verifier", "");
        if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, next)) return false;
        for (auto* socket : peers_.keys())
            if (!peers_.value(socket).controller && !peers_.value(socket).joined && !peers_.value(socket).pending
                && !peers_.value(socket).name.isEmpty()) admitPeer(socket);
        setStatus(tr("Channel password removed. Device permissions remain."));
        emit hostPasswordSaved(true);
        return true;
    }
    QByteArray salt;
    try { salt = TlsIdentity::newKey(); }
    catch (const std::exception& error) { return setStatus(tr(error.what()), false); }
    passwordBusy_ = true;
    if (!hashPassword(password.toUtf8(), salt, [this, salt](QByteArray hash, QString error) {
        passwordBusy_ = false;
        if (!error.isEmpty()) { setStatus(error, false); emit hostPasswordSaved(false); return; }
        auto next = security_; next.insert("verifier", QString::fromLatin1((salt + hash).toBase64()));
        if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, next)) { emit hostPasswordSaved(false); return; }
        for (auto* socket : peers_.keys())
            if (!peers_.value(socket).controller && !peers_.value(socket).joined && !peers_.value(socket).passwordChecking
                && !peers_.value(socket).name.isEmpty()) requirePassword(socket);
        setStatus(tr("Password saved. It applies on the next join."));
        emit hostPasswordSaved(true);
    })) {
        passwordBusy_ = false;
        return setStatus(tr("Password verification is busy. Please wait briefly."), false);
    }
    return setStatus(tr("Saving channel password."));
}

void LocalChannel::requirePassword(QSslSocket* socket, qint64 retryMs, bool busy) {
    if (!peers_.contains(socket) || peers_.value(socket).closing) return;
    const auto pending = peers_[socket].pending;
    peers_[socket].pending = false;
    writeMessage(socket, {{"type", "join"}, {"result", "password"}, {"retryMs", retryMs}, {"busy", busy}});
    if (pending) emit requestsChanged();
}

void LocalChannel::checkPassword(QSslSocket* socket, const QJsonObject& message) {
    auto& peer = peers_[socket];
    if (peer.passwordChecking || peer.pending) { socket->abort(); return; }
    const auto password = message.value("password").toString();
    if (!message.value("password").isString() || password.isEmpty() || password.toUtf8().size() > 1024
        || QString::fromUtf8(password.toUtf8()) != password) { socket->abort(); return; }
    const auto verifier = QByteArray::fromBase64(security_.value("verifier").toString().toLatin1());
    if (verifier.isEmpty()) { admitPeer(socket); return; }
    const QString key = "pw:" + peer.id;
    const auto now = clock_();
    if (attempts_.value(key).next > now) { requirePassword(socket, attempts_.value(key).next - now); return; }
    const QPointer<QSslSocket> guarded(socket);
    peer.passwordChecking = true;
    if (!hashPassword(password.toUtf8(), verifier.first(32), [this, guarded, verifier, key](QByteArray hash, QString error) {
        if (!guarded || !peers_.contains(guarded) || peers_.value(guarded).closing) return;
        peers_[guarded].passwordChecking = false;
        if (peers_[guarded].joined) return;
        if (!error.isEmpty()) { setStatus(error, false); guarded->abort(); return; }
        if (verifier != QByteArray::fromBase64(security_.value("verifier").toString().toLatin1())) {
            admitPeer(guarded); return;
        }
        if (CRYPTO_memcmp(hash.constData(), verifier.constData() + 32, 32) != 0) {
            const auto now = clock_();
            auto attempts = attempts_;
            for (auto it = attempts.begin(); it != attempts.end();) {
                if (now - it->next > 86400000) it = attempts.erase(it); else ++it;
            }
            const auto count = std::min(attempts.value(key).count + 1, 6);
            const auto wait = std::min<qint64>(600000, 30000LL << (count - 1));
            attempts.insert(key, {count, now + wait});
            if (!persist(approved_, attempts, requestsAllowed_, endpointPins_, control_, security_)) { guarded->abort(); return; }
            requirePassword(guarded, wait); return;
        }
        peers_[guarded].passwordVerified = verifier;
        admitPeer(guarded);
    })) {
        peer.passwordChecking = false;
        requirePassword(socket, 1000, true);
    }
}

bool LocalChannel::submitPassword(Client& c, const QString& password, bool remember) {
    if (!c.passwordRequired || c.passwordRetryAt > controlClock_.elapsed() || !c.socket || !c.socket->isEncrypted()) return false;
    if (password.isEmpty() || password.toUtf8().size() > 1024 || QString::fromUtf8(password.toUtf8()) != password)
        return setClientStatus(c, tr("Enter a password with at most 1024 UTF-8 bytes."), false);
    c.password = password; c.rememberPassword = remember; c.passwordSent = true;
    c.passwordRequired = false;
    if (!writeMessage(c.socket, {{"type", "password"}, {"password", password}})) {
        c.passwordRequired = true; return setClientStatus(c, tr("Password could not be sent."), false);
    }
    return setClientStatus(c, tr("Checking password."));
}

bool LocalChannel::savePassword(Client& c, const QString& password, bool remember) {
    auto saved = security_.value("saved").toObject();
    try {
        if (remember && !password.isEmpty()) {
            const auto context = passwordContext(c.id);
            const auto sealed = TlsIdentity::seal(password.toUtf8(), identity_->deriveKey(context, "channel password"), context);
            saved.insert(c.id, QString::fromLatin1(sealed.toBase64()));
        } else saved.remove(c.id);
    } catch (const std::exception& error) { return setClientStatus(c, tr(error.what()), false); }
    auto next = security_; next.insert("saved", saved);
    if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, next))
        return setClientStatus(c, tr("Connected, but the channel password could not be saved."), false);
    return true;
}

bool LocalChannel::forgetPassword() {
    auto* c = chatClient();
    if (!c || !savePassword(*c, {}, false)) return false;
    c->rememberPassword = false;
    return setClientStatus(*c, tr("Saved password removed. The current connection remains active."));
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

bool LocalChannel::startDiscovery() {
    if (service_) return false;
    if (!ready()) return setStatus(tr("Device identity is not ready yet."), false);
    if (discovery_.state() == QAbstractSocket::BoundState) return true;
    heartbeat_.start();
    if (!discovery_.bind(QHostAddress::AnyIPv4, discoveryPort,
                         QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        discoveryError_ = tr("Local channel discovery unavailable: %1").arg(discovery_.errorString());
        emit hostsChanged(); return false;
    }
    discovery_.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    discoveryInterfaces_.clear();
    discovery_.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    announce();
    return discovery_.state() == QAbstractSocket::BoundState;
}

QList<LocalChannel::ScanTarget> LocalChannel::discoveryTargets() const {
    QList<ScanTarget> result;
    QSet<QString> seen;
    const auto add = [&](const QString& address, quint16 port) {
        const auto key = address + ':' + QString::number(port);
        if (!seen.contains(key)) { seen.insert(key); result.append({address, port}); }
    };
    for (quint16 port = 48763; port <= 48783; ++port) add("127.0.0.1", port);
    if (server_.isListening() && server_.serverAddress().isLoopback()) return result;

    QList<QString> addresses;
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp)
            || !interface.flags().testFlag(QNetworkInterface::IsRunning)) continue;
        for (const auto& entry : interface.addressEntries()) {
            const auto ip = entry.ip();
            const auto value = ip.toIPv4Address();
            const bool privateAddress = (value >= 0x0a000000u && value <= 0x0affffffu)
                || (value >= 0xac100000u && value <= 0xac1fffffu)
                || (value >= 0xc0a80000u && value <= 0xc0a8ffffu);
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || !privateAddress || ip.isLoopback()) continue;
            const auto mask = entry.netmask().toIPv4Address();
            if (!mask || !value) continue;
            int prefix = 0;
            for (quint32 bit = 0x80000000u; bit && (mask & bit); bit >>= 1) ++prefix;
            const int width = std::max(prefix, 24);
            const quint32 network = value & (0xffffffffu << (32 - width));
            const quint32 count = 1u << (32 - width);
            for (quint32 offset = 1; offset + 1 < count && offset <= 254; ++offset) {
                const auto candidate = QHostAddress(network + offset).toString();
                if (!addresses.contains(candidate)) addresses.append(candidate);
            }
        }
    }
    for (const auto& address : addresses) add(address, defaultPort);
    for (const auto& address : addresses)
        for (quint16 port = 48763; port <= 48783; ++port) if (port != defaultPort) add(address, port);
    return result;
}

bool LocalChannel::setDiscoverySearch(bool enabled) {
    if (!enabled) {
        clearDiscoverySearch();
        discoverySearching_ = false;
        for (auto it = hosts_.begin(); it != hosts_.end();) {
            it->scanned = false;
            if (it.key() != ownId() && !it->direct && clock_() - it->seen > 6000) it = hosts_.erase(it);
            else ++it;
        }
        emit hostsChanged();
        emit discoverySearchChanged();
        return true;
    }
    if (discoverySearching_) return true;
    if (!ready()) return setStatus(tr("Device identity is not ready yet."), false);
    discoverySearchQueue_ = discoveryTargets();
    discoverySearching_ = true;
    emit discoverySearchChanged();
    pumpDiscoverySearch();
    return true;
}

void LocalChannel::clearDiscoverySearch() {
    discoverySearchQueue_.clear();
    const auto sockets = discoverySearchProbes_.keys();
    discoverySearchProbes_.clear();
    for (auto* socket : sockets) {
        socket->abort();
        socket->deleteLater();
    }
}

void LocalChannel::finishDiscoveryProbe(QSslSocket* socket) {
    if (!discoverySearchProbes_.contains(socket)) return;
    discoverySearchProbes_.remove(socket);
    socket->abort();
    socket->deleteLater();
    if (discoverySearching_) pumpDiscoverySearch();
}

void LocalChannel::pumpDiscoverySearch() {
    if (!discoverySearching_) return;
    while (discoverySearchProbes_.size() < 16 && !discoverySearchQueue_.isEmpty()) {
        const auto target = discoverySearchQueue_.takeFirst();
        auto* socket = new QSslSocket(this);
        discoverySearchProbes_.insert(socket, {target, {}});
        socket->setReadBufferSize(2 * maximumFrame);
        socket->setSslConfiguration(identity_->configuration());
        connect(socket, &QSslSocket::sslErrors, this, [this, socket](const QList<QSslError>& errors) {
            if (discoverySearchProbes_.contains(socket) && !TlsIdentity::peerId(socket->peerCertificate()).isEmpty()
                && acceptableCertificateErrors(errors)) socket->ignoreSslErrors(errors);
        });
        connect(socket, &QSslSocket::encrypted, this, [this, socket] {
            if (discoverySearchProbes_.contains(socket)) writeMessage(socket, {{"type", "discover"}, {"version", 1}});
        });
        connect(socket, &QSslSocket::readyRead, this, [this, socket] {
            auto it = discoverySearchProbes_.find(socket);
            if (it == discoverySearchProbes_.end()) return;
            auto buffer = std::move(it->buffer);
            const auto targetPort = it->target.port;
            readMessages(socket, buffer, [this, socket, targetPort](const QJsonObject& message) {
                const auto it = discoverySearchProbes_.find(socket);
                if (it == discoverySearchProbes_.end()) return;
                const auto id = TlsIdentity::peerId(socket->peerCertificate());
                if (message.value("type") == "discovery" && message.value("version").toInt() == 1
                    && message.value("hosting").toBool() && validId(id) && id != ownId()
                    && displayName(message.value("channel")) && (hosts_.size() < 1024 || hosts_.contains(id))) {
                    receiveDirectory(id, message, socket->peerAddress().toString(), targetPort, false, true);
                }
                finishDiscoveryProbe(socket);
            });
            if (discoverySearchProbes_.contains(socket)) discoverySearchProbes_[socket].buffer = std::move(buffer);
        });
        connect(socket, &QSslSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
            finishDiscoveryProbe(socket);
        });
        connect(socket, &QSslSocket::disconnected, this, [this, socket] { finishDiscoveryProbe(socket); });
        QTimer::singleShot(1500, socket, [this, socket] { finishDiscoveryProbe(socket); });
        socket->connectToHostEncrypted(target.address, target.port, target.address, QIODevice::ReadWrite,
                                       QAbstractSocket::IPv4Protocol);
    }
    if (discoverySearchQueue_.isEmpty() && discoverySearchProbes_.isEmpty()) {
        discoverySearching_ = false;
        emit discoverySearchChanged();
    }
}

int LocalChannel::chatLifetimeDays() const {
    const auto* c = chatClient();
    return c && c->accepted ? c->messageLifetimeDays : 0;
}

QJsonObject LocalChannel::hostConfiguration() const {
    auto approvals = approved_.values(); std::sort(approvals.begin(), approvals.end());
    auto blocked = security_.value("blocked").toObject().keys(); std::sort(blocked.begin(), blocked.end());
    return {{"channelName", channelName_}, {"botName", botName_}, {"port", configuredPort()},
        {"messageLifetimeDays", messageLifetimeDays()}, {"requestsAllowed", requestsAllowed()},
        {"approvedClients", QJsonArray::fromStringList(approvals)}, {"blockedClients", QJsonArray::fromStringList(blocked)}};
}

bool LocalChannel::configureHost(const QJsonObject& configuration) {
    static const QSet<QString> fields{"channelName", "botName", "port", "messageLifetimeDays", "requestsAllowed", "approvedClients", "blockedClients"};
    for (auto it = configuration.begin(); it != configuration.end(); ++it)
        if (!fields.contains(it.key())) return setStatus(tr("Unknown host setting: %1").arg(it.key()), false);
    auto name = channelName_;
    auto botName = botName_;
    if (configuration.contains("botName")) {
        try { botName = VoiceSession::validatedName(configuration.value("botName").toString()); }
        catch (const std::invalid_argument& error) { return setStatus(tr(error.what()), false); }
    }
    if (configuration.contains("channelName")) {
        try { name = VoiceSession::validatedName(configuration.value("channelName").toString()); }
        catch (const std::invalid_argument& error) { return setStatus(tr(error.what()), false); }
    }
    const auto port = configuration.value("port").toInt(configuredPort_);
    if (configuration.contains("port") && (!configuration.value("port").isDouble()
        || configuration.value("port").toDouble() != port || port < 1 || port > 65535))
        return setStatus(tr("Port must be between 1 and 65535."), false);
    const auto days = configuration.value("messageLifetimeDays").toInt(messageLifetimeDays_);
    if (configuration.contains("messageLifetimeDays") && (!configuration.value("messageLifetimeDays").isDouble()
        || configuration.value("messageLifetimeDays").toDouble() != days
        || !ChatHistory::validLifetime(qint64(days) * ChatHistory::lifetime)))
        return setStatus(tr("Invalid message lifetime."), false);
    if (configuration.contains("messageLifetimeDays") && days > 30 && !session_.supporterEnabled())
        return setStatus(tr("Choose 24 hours, 7 days or 30 days."), false);
    if (configuration.contains("requestsAllowed") && !configuration.value("requestsAllowed").isBool())
        return setStatus(tr("requestsAllowed must be true or false."), false);
    auto approvals = approved_;
    auto security = security_;
    auto blocked = security.value("blocked").toObject();
    for (const auto* field : {"approvedClients", "blockedClients"}) {
        if (!configuration.contains(field)) continue;
        if (!configuration.value(field).isArray()) return setStatus(tr("%1 must be a list of device IDs.").arg(field), false);
        const auto entries = configuration.value(field).toArray();
        if (entries.size() > 4096) return setStatus(tr("A host preset may contain at most 4096 device IDs per list."), false);
        for (const auto& entry : entries) {
            const auto id = entry.toString();
            if (!validId(id) || id == ownId()) return setStatus(tr("Invalid device identity."), false);
            if (QString::fromLatin1(field) == "approvedClients") approvals.insert(id);
            else if (!blocked.contains(id)) blocked.insert(id, id.left(12));
        }
    }
    security.insert("blocked", blocked);
    if (service_ && configuration.contains("port")) return setStatus(tr("All own channels use the device's shared port."), false);
    const auto previousName = channelName_;
    const auto previousBotName = botName_;
    const auto previousPort = configuredPort_;
    const auto previousDays = messageLifetimeDays_;
    channelName_ = name; botName_ = botName; configuredPort_ = port; messageLifetimeDays_ = days;
    if (!persist(approvals, attempts_, configuration.value("requestsAllowed").toBool(requestsAllowed_), endpointPins_, control_, security)) {
        channelName_ = previousName; botName_ = previousBotName; configuredPort_ = previousPort; messageLifetimeDays_ = previousDays;
        emit stateChanged(); return false;
    }
    for (const auto& entry : configuration.value("blockedClients").toArray()) disconnectMember(entry.toString(), "blocked");
    for (const auto& entry : configuration.value("approvedClients").toArray())
        if (!blocked.contains(entry.toString())) applyDecision(entry.toString(), true);
    if (days != previousDays) for (auto* socket : peers_.keys()) sendChatKey(socket);
    if (hosting()) { announce(); broadcastRoster(); }
    return true;
}

bool LocalChannel::setChannelName(const QString& name) {
    return configureHost({{"channelName", name}});
}
bool LocalChannel::setBotName(const QString& name) { return configureHost({{"botName", name}}); }

bool LocalChannel::setConfiguredPort(int port) {
    if (service_) return service_->setConfiguredPort(port);
    return configureHost({{"port", port}}) && setStatus(tr("Port saved. Applies after restarting the app."));
}

bool LocalChannel::setMessageLifetimeDays(int days) {
    return configureHost({{"messageLifetimeDays", days}}) && setStatus(tr("Lifetime saved for new messages."));
}

bool LocalChannel::startHost() { return listen(QHostAddress::Any, quint16(configuredPort_)); }

bool LocalChannel::startService(const QHostAddress& address, quint16 port) {
    if (service_) return service_->startService(address, port);
    if (!ready()) return setStatus(tr("Device identity is not ready yet."), false);
    if (server_.isListening()) return true;
    server_.setSslConfiguration(identity_->configuration());
    server_.setHandshakeTimeout(5000);
    if (!server_.listen(address, port)) return setStatus(tr("Device access on port %1 is unavailable: %2").arg(port).arg(server_.errorString()), false);
    emit stateChanged();
    if (!hostOnly_) QTimer::singleShot(0, this, &LocalChannel::tryAutoJoin);
    return true;
}

bool LocalChannel::listen(const QHostAddress& address, quint16 port) {
    if (hosting()) return true;
    if (!startService(address, port)) return false;
    try {
        history_ = std::make_unique<ChatHistory>(storageFile_ + ".chat", *identity_, clock_());
        hostChatKey_ = TlsIdentity::newKey();
        hostChatEpoch_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    } catch (const std::exception& error) { return setStatus(tr(error.what()), false); }
    hosting_ = true;
    botLastActive_ = clock_(); botSleeping_ = false;
    if (!service_ && !syncOwnedChannels()) { stopHost(); return false; }
    hosts_.insert(channelId(), {channelName_, "127.0.0.1", this->port(), clock_(), false, false, ownId()});
    announce();
    emit hostsChanged();
    return setStatus(tr("Own channel opened."));
}

bool LocalChannel::stopHost() {
    pendingDepartureReasons_ = {};
    for (const auto& child : owned_) child->stopHost();
    setScreenSharing(false);
    hosting_ = false;
    const auto sockets = peers_.keys();
    for (auto* socket : sockets) if (!peers_.value(socket).controller) socket->abort();
    history_.reset(); hostChatKey_.fill(0); hostChatKey_.clear(); hostChatEpoch_.clear();
    hosts_.remove(channelId());
    if (service_) { service_->hosts_.remove(channelId()); emit service_->hostsChanged(); }
    emit ownedChannelsChanged();
    emit hostsChanged();
    return setStatus(tr("Own channel closed."));
}

QVariantList LocalChannel::hosts() const {
    QVariantList result;
    for (auto it = hosts_.begin(); it != hosts_.end(); ++it)
        result.append(QVariantMap{{"id", it.key()}, {"deviceId", it->deviceId.isEmpty() ? it.key() : it->deviceId}, {"name", it->name}, {"address", it->address}, {"port", it->port}});
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.toMap().value("name").toString().localeAwareCompare(b.toMap().value("name").toString()) < 0;
    });
    return result;
}

QVariantList LocalChannel::availableHosts() const {
    QSet<QString> ids;
    QHash<QString, QString> endpoints;
    ids.insert(channelId());
    for (const auto& id : ownedIds_) ids.insert(id);
    for (const auto& value : savedChannels()) {
        const auto entry = value.toMap();
        const auto id = entry.value("id").toString();
        ids.insert(id);
        endpoints.insert(endpointKey(entry.value("address").toString(), quint16(entry.value("port").toInt())), entry.value("deviceId", id).toString());
    }
    QVariantList result;
    for (const auto& value : hosts()) {
        const auto entry = value.toMap(); const auto id = entry.value("id").toString();
        const auto device = entry.value("deviceId", id).toString();
        const auto endpoint = endpointKey(entry.value("address").toString(), quint16(entry.value("port").toInt()));
        if (ids.contains(id) || (endpoints.contains(endpoint) && endpoints.value(endpoint) != device)) continue;
        endpoints.insert(endpoint, device); ids.insert(id);
        result.append(entry);
    }
    return result;
}

QVariantList LocalChannel::savedChannels() const {
    auto entries = savedChannels_;
    const auto addOwn = [&entries](const LocalChannel& channel) {
        auto own = entries.value(channel.channelId()).toObject();
        own.insert("name", channel.channelName_); own.insert("address", "127.0.0.1"); own.insert("port", channel.port());
        own.insert("deviceId", channel.ownId()); own.insert("owned", true); own.insert("hosting", channel.hosting());
        if (!own.contains("autoJoin")) own.insert("autoJoin", false);
        if (!own.contains("lastJoined")) own.insert("lastJoined", 0);
        entries.insert(channel.channelId(), own);
    };
    if (hosting()) addOwn(*this);
    for (const auto& child : owned_) addOwn(*child);
    for (const auto& c : clients_) {
        if (entries.contains(c->id) || c->access == "blocked") continue;
        const auto name = hosts_.contains(c->id) ? hosts_.value(c->id).name : c->name;
        entries.insert(c->id, QJsonObject{{"name", name.isEmpty() ? tr("Channel") : name},
            {"address", c->address}, {"port", c->port}, {"deviceId", c->deviceId}, {"autoJoin", false}, {"lastJoined", 0}});
    }
    QVariantList result;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        auto entry = it.value().toObject().toVariantMap();
        const auto c = clients_.value(it.key());
        entry.insert("id", it.key());
        entry.insert("online", c && c->accepted);
        entry.insert("screen", c && c->accepted && c->screenAvailable);
        const auto* owner = ownChannel(it.key());
        entry.insert("lifetimeDays", owner ? owner->messageLifetimeDays() : c && c->accepted ? c->messageLifetimeDays : 0);
        entry.insert("access", c ? c->access : QStringLiteral("saved"));
        entry.insert("members", c && c->accepted ? humanMembers(c->members) : QVariantList{});
        result.append(entry);
    }
    std::sort(result.begin(), result.end(), [](const QVariant& a, const QVariant& b) {
        const auto left = a.toMap(), right = b.toMap();
        if (left.value("lastJoined") != right.value("lastJoined"))
            return left.value("lastJoined").toLongLong() > right.value("lastJoined").toLongLong();
        return left.value("id").toString() < right.value("id").toString();
    });
    return result;
}

bool LocalChannel::rememberChannel(Client& c, const QString& name) {
    if (!c.accepted || !displayName(name)) return false;
    auto next = savedChannels_;
    auto entry = next.value(c.id).toObject();
    entry.insert("name", name); entry.insert("address", c.address); entry.insert("port", c.port); entry.insert("deviceId", c.deviceId);
    if (!entry.contains("autoJoin")) entry.insert("autoJoin", false);
    if (!entry.contains("lastJoined")) entry.insert("lastJoined", clock_());
    if (next.value(c.id).toObject() == entry) return true;
    next.insert(c.id, entry);
    return persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &next);
}

bool LocalChannel::setAutoJoin(const QString& id, bool enabled) {
    if (!savedChannels_.contains(id)) return setStatus(tr("Add this channel before enabling auto-join."), false);
    auto next = savedChannels_; auto entry = next.value(id).toObject(); entry.insert("autoJoin", enabled); next.insert(id, entry);
    if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &next)) return false;
    if (enabled) { pausedAutoJoin_.remove(id); triedAutoJoin_.remove(id); }
    return true;
}

bool LocalChannel::removeChannel(const QString& id) {
    if (!savedChannels_.contains(id) && !clients_.contains(id)) return false;
    auto next = savedChannels_; next.remove(id);
    if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &next)) return false;
    unavailableControlTargets_.remove(id);
    if (joinedHostId_ == id) leave();
    closeChat(id);
    return true;
}

bool LocalChannel::joinSaved(const QString& id) {
    if (const auto* owner = ownChannel(id))
        return owner->hosting() ? join(id, "127.0.0.1", port()) : setStatus(tr("Channel unavailable"), false);
    if (!savedChannels_.contains(id)) {
        if (auto c = clients_.value(id)) return join(id, c->address, c->port);
    }
    if (!savedChannels_.contains(id)) return setStatus(tr("This channel is not saved."), false);
    const auto [address, port] = channelEndpoint(id);
    return join(id, address, port);
}

void LocalChannel::tryAutoJoin() {
    if (service_) return;
    if (hostOnly_ || !ready() || (clients_.contains(joinedHostId_) && clients_.value(joinedHostId_)->socket) || joined() || (!automaticAttempt_ && !joinedHostId_.isEmpty()) || remoteMode()) return;
    for (const auto& value : savedChannels()) {
        const auto entry = value.toMap(); const auto id = entry.value("id").toString();
        if (!entry.value("autoJoin").toBool() || pausedAutoJoin_.contains(id) || triedAutoJoin_.contains(id)) continue;
        triedAutoJoin_.insert(id);
        const auto [address, port] = channelEndpoint(id);
        if (!beginJoin(id, address, port, true))
            QTimer::singleShot(0, this, &LocalChannel::tryAutoJoin);
        return;
    }
    if (automaticAttempt_) leaveChannel(false);
}

void LocalChannel::advanceAutoJoin(bool pause) {
    if (pause) pausedAutoJoin_.insert(joinedHostId_);
    if (auto c = clients_.value(joinedHostId_)) { c->reconnectWanted = false; closeClient(*c); }
    QTimer::singleShot(0, this, &LocalChannel::tryAutoJoin);
}

QVariantList LocalChannel::requests() const {
    QVariantList result;
    QSet<QString> seen;
    for (const auto& peer : peers_) {
        if (!peer.pending || peer.controller || seen.contains(peer.id)) continue;
        seen.insert(peer.id);
        result.append(QVariantMap{{"id", peer.id}, {"name", peer.name}, {"avatarId", peer.avatar},
            {"address", peer.address}, {"channel", peer.channel}, {"port", peer.servicePort},
            {"hostId", channelId()}, {"hostName", channelName_}});
    }
    if (!service_) for (const auto& child : owned_) result.append(child->requests());
    return result;
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

void LocalChannel::announce() {
    if (service_) { service_->announce(); return; }
    if (heartbeat_.isActive() && discovery_.state() != QAbstractSocket::BoundState) {
        startDiscovery(); return;
    }
    const auto now = clock_();
    for (auto it = hosts_.begin(); it != hosts_.end();) {
        if (it.key() != ownId() && !it->direct && !it->scanned && now - it->seen > 6000) it = hosts_.erase(it);
        else ++it;
    }
    if (discovery_.state() == QAbstractSocket::BoundState) {
        const auto current = discoveryInterfaces();
        const auto addresses = [](const auto& interface) {
            QList<QHostAddress> result;
            for (const auto& entry : interface.addressEntries())
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) result.append(entry.ip());
            return result;
        };
        const auto same = [&](const auto& a, const auto& b) {
            // An interface index can be reused, and an existing adapter can
            // receive a new address without changing its name or index.
            return a.index() == b.index() && a.name() == b.name()
                && a.hardwareAddress() == b.hardwareAddress() && addresses(a) == addresses(b);
        };
        if (std::any_of(discoveryInterfaces_.cbegin(), discoveryInterfaces_.cend(), [&](const auto& previous) {
                return std::none_of(current.cbegin(), current.cend(),
                    [&](const auto& value) { return same(previous, value); });
            })) {
            // A removed address may no longer be usable for leaving its old
            // group. Rebinding drops every stale membership on all backends.
            discovery_.close();
            startDiscovery();
            return;
        }
        for (const auto& interface : current) {
            if (std::any_of(discoveryInterfaces_.cbegin(), discoveryInterfaces_.cend(),
                    [&](const auto& value) { return same(interface, value); })) continue;
            if (discovery_.joinMulticastGroup(discoveryGroup, interface)) discoveryInterfaces_.append(interface);
            else qCDebug(discoveryLog) << "join" << interface.name() << discovery_.errorString();
        }
        if (discoveryInterfaces_.isEmpty()) {
            discovery_.close();
            discoveryError_ = tr("The network does not allow local multicast discovery.");
            emit hostsChanged(); return;
        }
        discoveryError_.clear();
    }
    if (hosting()) {
        for (const auto& entry : channelDirectory()) {
            const auto value = entry.toObject();
            hosts_.insert(value.value("id").toString(), {value.value("name").toString(), "127.0.0.1", port(), now, false, false, ownId()});
        }
        if (discovery_.state() == QAbstractSocket::BoundState) {
            const auto data = QJsonDocument(QJsonObject{{"protocol", "squadspeak/1"}, {"id", ownId()},
                {"name", channelName_}, {"port", port()}, {"channels", channelDirectory()}}).toJson(QJsonDocument::Compact);
            bool sentAny = false;
            QString failure;
            for (const auto& interface : discoveryInterfaces_) {
                discovery_.setMulticastInterface(interface);
                const auto sent = discovery_.writeDatagram(data, discoveryGroup, discoveryPort);
                const auto nativeError = errno;
                if (sent >= 0) sentAny = true;
                else failure = discovery_.errorString();
                qCDebug(discoveryLog) << "announce" << interface.name() << sent
                    << (sent < 0 ? discovery_.errorString() : QString{}) << "socket" << discovery_.error()
                    << "errno" << (sent < 0 ? nativeError : 0);
            }
            discoveryError_ = sentAny ? QString{} : tr("Local channel discovery unavailable: %1").arg(failure);
            if (!sentAny) discovery_.close();
        }
    }
    emit hostsChanged();
}

void LocalChannel::receiveDiscovery() {
    while (discovery_.hasPendingDatagrams()) {
        const auto datagram = discovery_.receiveDatagram(8192);
        qCDebug(discoveryLog) << "received" << datagram.senderAddress() << datagram.data().size() << "local" << localAddress(datagram.senderAddress());
        if (!localAddress(datagram.senderAddress())) continue;
        const auto object = QJsonDocument::fromJson(datagram.data()).object();
        const auto id = object.value("id").toString();
        const auto port = object.value("port").toInt();
        if (object.value("protocol") != "squadspeak/1" || !validId(id) || id == ownId()
            || !displayName(object.value("name")) || port < 1 || port > 65535) continue;
        if (hosts_.size() >= 1024 && !hosts_.contains(id)) continue;
        auto directory = object; directory.insert("channel", object.value("name"));
        if (!receiveDirectory(id, directory, datagram.senderAddress().toString(), quint16(port), false, hosts_.value(id).scanned)) continue;
        for (const auto& c : clients_) if (c->deviceId == id && c->reconnectWanted && hosts_.contains(c->id)) {
            // Preserve explicit DNS names across endpoint changes on every channel.
            if (!QHostAddress(c->address).isNull()) {
                c->address = datagram.senderAddress().toString(); c->port = port;
            }
        }
        emit hostsChanged();
    }
}

bool LocalChannel::acceptableCertificateErrors(const QList<QSslError>& errors) {
    return std::all_of(errors.begin(), errors.end(), [](const auto& error) {
        // Local trust is established by the public-key pin and host admission,
        // not by public certificate authorities or DNS names.
        return error.error() == QSslError::SelfSignedCertificate
            || error.error() == QSslError::CertificateUntrusted
            || error.error() == QSslError::HostNameMismatch;
    });
}

bool LocalChannel::writeMessage(QSslSocket* socket, const QJsonObject& object) {
    if (!socket || !socket->isEncrypted()) return false;
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (data.size() > maximumFrame || queuedBytes(socket) > 4 * maximumFrame) { socket->abort(); return false; }
    QByteArray frame(4, '\0');
    qToBigEndian<quint32>(quint32(data.size()), frame.data());
    frame.append(data);
    return socket->write(frame) == frame.size();
}

bool LocalChannel::readMessages(QSslSocket* socket, QByteArray& buffer,
                                const std::function<void(const QJsonObject&)>& receive) {
    buffer.append(socket->readAll());
    while (buffer.size() >= 4) {
        const auto size = qFromBigEndian<quint32>(buffer.constData());
        if (!size || size > maximumFrame) { socket->abort(); return false; }
        if (buffer.size() < size + 4) break;
        QJsonParseError error;
        const auto message = QJsonDocument::fromJson(buffer.mid(4, size), &error);
        buffer.remove(0, size + 4);
        if (error.error != QJsonParseError::NoError || !message.isObject()
            || !validMessageType(message.object().value("type").toString())) { socket->abort(); return false; }
        receive(message.object());
        // Remote EOF leaves readable data open; abort() and a local close stop parsing.
        if (!socket->isOpen() || socket->state() == QAbstractSocket::ClosingState) return false;
    }
    return true;
}

void LocalChannel::acceptConnections() {
    while (server_.hasPendingConnections()) {
        auto* socket = qobject_cast<QSslSocket*>(server_.nextPendingConnection());
        if (!socket) continue;
        int pending = 0;
        const auto count = [&pending](const LocalChannel& channel) {
            for (const auto& peer : channel.peers_) if (!peer.joined) ++pending;
        };
        count(*this); for (const auto& child : owned_) count(*child);
        if (pending >= maximumPendingPeers) { socket->abort(); socket->deleteLater(); continue; }
        acceptSocket(socket);
        readPeer(socket);
    }
}

void LocalChannel::acceptSocket(QSslSocket* socket) {
    const auto id = TlsIdentity::peerId(socket->peerCertificate());
    if (!validId(id) || !socket->isEncrypted()) { socket->abort(); socket->deleteLater(); return; }
    socket->setParent(this);
    socket->setReadBufferSize(2 * maximumFrame);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    Peer peer; peer.id = id; peer.lastActivity = controlClock_.elapsed();
    peers_.insert(socket, std::move(peer));
    connect(socket, &QSslSocket::readyRead, this, [this, socket] { readPeer(socket); });
    connect(socket, &QSslSocket::disconnected, this, [this, socket] {
        const auto previous = peers_.take(socket);
        socket->deleteLater();
        endMembership(previous);
    });
    QTimer::singleShot(5000, socket, [this, socket] {
        if (peers_.contains(socket) && peers_.value(socket).name.isEmpty()) socket->abort();
    });
}

void LocalChannel::dispatchMessage(QSslSocket* socket, const QJsonObject& message) {
    if (!peers_.contains(socket) || peers_.value(socket).closing) return;
    const auto type = message.value("type").toString();
    if ((type == "hello" || type == "screenHello") && peers_.value(socket).name.isEmpty() && message.contains("targetChannel")) {
        const auto id = message.value("targetChannel").toString();
        if (!validId(id)) { socket->abort(); return; }
        if (id != channelId()) {
            const auto child = service_ ? nullptr : ownChannel(id);
            if (!child || !child->hosting()) { socket->abort(); return; }
            peers_.remove(socket); socket->disconnect(this);
            child->acceptSocket(socket); child->hostMessage(socket, message); return;
        }
    }
    hostMessage(socket, message);
}

void LocalChannel::readPeer(QSslSocket* socket) {
    if (!peers_.contains(socket)) return;
    if (peers_.value(socket).closing) { socket->readAll(); return; }
    auto buffer = std::move(peers_[socket].buffer);
    readMessages(socket, buffer, [socket](const QJsonObject& message) {
        auto* owner = qobject_cast<LocalChannel*>(socket->parent());
        if (!owner) { socket->abort(); return; }
        if (owner->viewers_.contains(socket)) owner->viewerMessage(socket, message);
        else owner->dispatchMessage(socket, message);
    });
    auto* owner = qobject_cast<LocalChannel*>(socket->parent());
    if (!owner) return;
    if (owner->peers_.contains(socket) && !owner->peers_.value(socket).closing) owner->peers_[socket].buffer = std::move(buffer);
    else if (owner->viewers_.contains(socket)) owner->viewers_[socket].buffer = std::move(buffer);
}

void LocalChannel::acceptPeer(QSslSocket* socket) {
    if (!peers_.contains(socket) || peers_.value(socket).closing) return;
    const auto id = peers_[socket].id;
    const bool controller = peers_[socket].controller;
    if (!controller && security_.value("blocked").toObject().contains(id)) {
        disconnectMember(id, "blocked"); return;
    }
    if (!controller && !peers_[socket].joined && id != ownId() && passwordProtected()
        && peers_[socket].passwordVerified != QByteArray::fromBase64(security_.value("verifier").toString().toLatin1())) {
        requirePassword(socket); return;
    }
    auto occupied = mediaParticipants(); occupied.remove(id);
    if (!controller && !peers_[socket].observer && occupied.size() >= maximumMediaParticipants) {
        const bool wasPending = std::exchange(peers_[socket].pending, false);
        writeMessage(socket, {{"type", "join"}, {"result", "full"}});
        finishPeer(socket);
        if (wasPending) emit requestsChanged();
        return;
    }
    // One logical device occupies one participant slot even after a fast reconnect.
    bool wasInVoice = false;
    for (auto* other : peers_.keys()) {
        if (other == socket || peers_.value(other).id != id || peers_.value(other).controller != controller) continue;
        wasInVoice |= peers_.value(other).joined && !peers_.value(other).observer;
        peers_[other].joined = false;
        peers_[other].pending = false;
        writeMessage(other, {{"type", controller ? "control" : "join"}, {"result", "replaced"}});
        finishPeer(other);
    }
    if (!peers_.contains(socket)) return;
    const bool wasPending = peers_[socket].pending;
    if (!controller && history_) {
        try { history_->updateProfile(id, peers_[socket].name, peers_[socket].avatar); }
        catch (const std::exception& error) { setStatus(tr(error.what()), false); socket->abort(); return; }
    }
    peers_[socket].joined = true;
    peers_[socket].pending = false;
    peers_[socket].lastSpoke = clock_();
    if (controller) {
        const auto allowed = control_.value("controllers").toObject().value(peers_[socket].id).toObject().value("remote").toBool()
            && (!peers_[socket].capabilitiesAdvertised || peers_[socket].capabilities.contains("remote-control"));
        writeMessage(socket, {{"type", "control"}, {"result", "accepted"}, {"remoteAllowed", allowed}, {"capabilities", capabilityAdvertisement()}});
        if (allowed) sendRemoteView(socket);
    } else writeMessage(socket, {{"type", "join"}, {"result", "accepted"}, {"adaptiveAudio", peers_[socket].adaptiveAudio}, {"capabilities", capabilityAdvertisement()}});
    if (wasPending) emit requestsChanged();
    if (!controller) {
        sendChatKey(socket); broadcastRoster(); startMedia(socket);
        if (!peers_.value(socket).observer && !wasInVoice) publishMembership("joined", id, peers_.value(socket).name);
        else if (peers_.value(socket).observer && wasInVoice) publishMembership("left", id, peers_.value(socket).name);
        const auto* device = service_ ? service_ : this;
        if (peers_.value(socket).remoteOffers) writeMessage(socket, {{"type", "remoteOffer"},
            {"allowed", device->control_.value("controllers").toObject().value(id).toObject().value("remote").toBool()
                && (!peers_.value(socket).capabilitiesAdvertised || peers_.value(socket).capabilities.contains("remote-control"))},
            {"available", !device->hostOnly_ && !device->remoteMode()}});
    }
}

void LocalChannel::hostMessage(QSslSocket* socket, const QJsonObject& message) {
    if (!peers_.contains(socket) || peers_.value(socket).closing) return;
    peers_[socket].lastActivity = controlClock_.elapsed();
    const auto type = message.value("type").toString();
    if (peers_[socket].joined && unknownMessageType(type)) return;
    if (type == "screenHello" && peers_[socket].name.isEmpty() && message.value("version").toInt() == 1) {
        QSet<QString> capabilities;
        if (!parseCapabilities(message, &capabilities)
            || (message.contains("capabilities") && !capabilities.contains("screen-share"))) { socket->abort(); return; }
        const auto minimum = message.value("minimumTier").toInt(0);
        if (message.contains("minimumTier") && message.value("minimumTier").toDouble(-1) != minimum) { socket->abort(); return; }
        if (message.contains("audio") && !message.value("audio").isBool()) { socket->abort(); return; }
        acceptViewer(socket, minimum, capabilities.contains("screen-audio"), message.value("audio").toBool(), capabilities.contains("udp-screen")); return;
    }
    const auto id = peers_[socket].id;
    if (type == "identify" && peers_[socket].name.isEmpty() && message.value("version").toInt() == 1) {
        writeMessage(socket, {{"type", "identity"}, {"version", 1}, {"name", hostOnly_ ? channelName_ : session_.userName()},
            {"channel", channelName_}, {"hosting", hosting()}, {"channels", channelDirectory()}});
        finishPeer(socket);
        return;
    }
    if (type == "discover" && peers_[socket].name.isEmpty() && message.value("version").toInt() == 1) {
        QJsonObject response{{"type", "discovery"}, {"version", 1}, {"hosting", hosting()}};
        if (hosting()) { response.insert("channel", channelName_); response.insert("channels", channelDirectory()); }
        writeMessage(socket, response);
        finishPeer(socket);
        return;
    }
    if (type == "controlHello") {
        if (hostOnly_ || remoteMode()) {
            writeMessage(socket, {{"type", "control"}, {"result", "unavailable"}});
            finishPeer(socket); return;
        }
        const auto revision = message.value("revision").toInteger(-1);
        if (!peers_[socket].name.isEmpty() || message.value("version").toInt() != 1
            || !displayName(message.value("name")) || !message.value("held").isBool() || !message.value("pairing").isBool()
            || revision < 0 || revision > 9007199254740991LL || message.value("revision").toDouble(-1) != double(revision)) {
            socket->abort(); return;
        }
        auto& peer = peers_[socket];
        if (!parseCapabilities(message, &peer.capabilities)) { socket->abort(); return; }
        peer.capabilitiesAdvertised = message.contains("capabilities");
        peer.name = message.value("name").toString(); peer.controller = true;
        peer.held = message.value("held").toBool(); peer.revision = revision;
        if (!message.value("pairing").toBool() || control_.value("controllers").toObject().value(id).toObject().value("remote").toBool()) {
            if (!control_.value("controllers").toObject().contains(id)) {
                writeMessage(socket, {{"type", "control"}, {"result", "revoked"}}); finishPeer(socket); return;
            }
            if (acceptControlState(socket, message)) acceptPeer(socket);
            return;
        }
        const auto now = clock_();
        const QString attemptKey = "ptt:" + id;
        const auto previous = attempts_.value(attemptKey);
        const auto address = socket->peerAddress().toString();
        for (auto it = requestWindows_.begin(); it != requestWindows_.end();) {
            if (now - it->first >= 30000) it = requestWindows_.erase(it); else ++it;
        }
        auto& window = requestWindows_[address];
        if (window.second == 0) window.first = now;
        if (++window.second > 3) {
            writeMessage(socket, {{"type", "control"}, {"result", "rejected"}, {"retryMs", 30000 - (now - window.first)}});
            finishPeer(socket); return;
        }
        auto attempts = attempts_;
        for (auto it = attempts.begin(); it != attempts.end();) {
            if (now - it->next > 86400000) it = attempts.erase(it); else ++it;
        }
        const auto count = std::min(previous.count + 1, 6);
        attempts.insert(attemptKey, {count, now + std::min<qint64>(600000, 30000LL << (count - 1))});
        if (!persist(approved_, attempts, requestsAllowed_, endpointPins_, control_, security_)) { socket->abort(); return; }
        if (now < previous.next) {
            writeMessage(socket, {{"type", "control"}, {"result", "rejected"}, {"retryMs", attempts.value(attemptKey).next - now}});
            finishPeer(socket); return;
        }
        if (!peers_.contains(socket)) return;
        peers_[socket].pending = true;
        writeMessage(socket, {{"type", "control"}, {"result", "pending"}});
        emit requestsChanged(); return;
    }
    if (peers_[socket].controller) {
        if (type == "controlPing" && peers_[socket].pending) { writeMessage(socket, {{"type", "controlWait"}}); return; }
        if (type == "remoteViewAck") {
            auto& peer = peers_[socket];
            const auto offset = message.value("offset").toInteger(-1);
            if (!peer.joined) { socket->abort(); return; }
            if (!control_.value("controllers").toObject().value(id).toObject().value("remote").toBool()
                || !peer.remoteSnapshotWaiting || message.value("id").toString() != peer.remoteSnapshotId) return;
            if (offset != peer.remoteSnapshotOffset || message.value("offset").toDouble(-1) != double(offset)) { socket->abort(); return; }
            peer.remoteSnapshotOffset += std::min<qint64>(remoteChunkSize, peer.remoteSnapshot.size() - peer.remoteSnapshotOffset);
            if (peer.remoteSnapshotOffset >= peer.remoteSnapshot.size()) {
                const auto snapshotId = peer.remoteSnapshotId;
                const bool dirty = peer.remoteSnapshotDirty;
                peer.remoteSnapshot.clear(); peer.remoteSnapshotWaiting = false; peer.remoteSnapshotDirty = false;
                writeMessage(socket, {{"type", "remoteViewEnd"}, {"id", snapshotId}});
                if (dirty) QTimer::singleShot(0, this, [this, guard = QPointer<QSslSocket>(socket)] { if (guard) sendRemoteView(guard); });
            } else sendRemoteViewChunk(socket);
            return;
        }
        if (type == "remoteImageGet") {
            auto& peer = peers_[socket];
            if (!peer.joined) { socket->abort(); return; }
            if (!control_.value("controllers").toObject().value(id).toObject().value("remote").toBool()) return;
            if (!validId(message.value("hash").toString()) || message.value("offset").toInteger(-1) < 0
                || message.value("offset").toDouble(-1) != double(message.value("offset").toInteger(-1))) { socket->abort(); return; }
            peer.remoteImageRequest = message; sendRemoteImage(socket); return;
        }
        if (type == "remoteAction") {
            if (!peers_[socket].joined) { socket->abort(); return; }
            if (!control_.value("controllers").toObject().value(id).toObject().value("remote").toBool()) return;
            handleRemoteAction(socket, message); return;
        }
        if (type != "pttState" || !peers_[socket].joined) { socket->abort(); return; }
        if (acceptControlState(socket, message)) writeMessage(socket, {{"type", "controlAck"}, {"revision", message.value("revision")}});
        return;
    }
    if (!hosting()) { socket->abort(); return; }
    if (type == "ping" && !peers_[socket].name.isEmpty()) {
        writeMessage(socket, {{"type", "pong"}}); return;
    }
    if (type == "hello") {
        if (peers_[socket].joined || !peers_[socket].name.isEmpty() || message.value("version").toInt() != 1
            || !displayName(message.value("name")) || !message.value("available").isBool()
            || !message.value("muted").isBool() || !validPresenceDetails(message)
            || (message.contains("observer") && !message.value("observer").isBool())
            || (message.contains("adaptiveAudio") && !message.value("adaptiveAudio").isBool())
            || (message.contains("remoteOffers") && !message.value("remoteOffers").isBool())
            || !parseCapabilities(message, &peers_[socket].capabilities)) { socket->abort(); return; }
        peers_[socket].capabilitiesAdvertised = message.contains("capabilities");
        peers_[socket].adaptiveAudio = message.value("adaptiveAudio").toBool()
            && (!peers_[socket].capabilitiesAdvertised || peers_[socket].capabilities.contains("adaptive-audio"));
        const auto advertisedChannel = message.value("channel");
        const auto advertisedPort = message.value("port");
        if (advertisedChannel.isUndefined() != advertisedPort.isUndefined()
            || (!advertisedChannel.isUndefined() && (!advertisedChannel.isString() || !displayName(advertisedChannel)
                || !advertisedPort.isDouble() || advertisedPort.toDouble() != advertisedPort.toInt()
                || advertisedPort.toInt() < 1 || advertisedPort.toInt() > 65535))) { socket->abort(); return; }
        peers_[socket].observer = message.value("observer").toBool();
        peers_[socket].remoteOffers = message.value("remoteOffers").toBool()
            && (!peers_[socket].capabilitiesAdvertised || peers_[socket].capabilities.contains("remote-control"));
        peers_[socket].name = message.value("name").toString();
        peers_[socket].available = message.value("available").toBool();
        peers_[socket].muted = message.value("muted").toBool();
        peers_[socket].deafened = message.value("deafened").toBool();
        peers_[socket].avatar = message.value("avatarId").toString(message.value("avatar").toString("mossling"));
        peers_[socket].address = socket->peerAddress().toString();
        peers_[socket].channel = advertisedChannel.toString();
        peers_[socket].servicePort = quint16(advertisedPort.toInt());
        admitPeer(socket);
        return;
    }
    if (type == "password" && !peers_[socket].name.isEmpty() && !peers_[socket].joined) {
        checkPassword(socket, message); return;
    }
    if (!peers_[socket].joined) { socket->abort(); return; }
    if (type == "media") {
        const auto media = peers_[socket].media;
        if (!media || (message.value("kind") == "source" && message.value("id") != id) || !media->receive(message)) socket->abort();
    } else if (type == "voiceLeave") {
        const bool wasInVoice = !peers_[socket].observer;
        peers_[socket].observer = true; peers_[socket].muted = true;
        peers_[socket].relay.reset();
        broadcastRoster();
        if (wasInVoice) publishMembership("left", id, peers_[socket].name);
    } else if (type == "audioAck") {
        auto& peer = peers_[socket];
        if (!peer.adaptiveAudio) { socket->abort(); return; }
        if (peer.audioProbeSent < 0) return;
        if (message.value("serial").toDouble(-1) != double(peer.audioProbe)) { socket->abort(); return; }
        const auto delay = controlClock_.elapsed() - peer.audioProbeSent;
        peer.audioProbeSent = -1;
        qCDebug(mediaLog) << "audio probe" << peer.audioProbe << "rtt_ms" << delay
                         << "queued_bytes" << queuedBytes(socket) << "tier" << peer.audioTier;
        if (!peer.media || !peer.media->udpActive()) sampleAudioLink(socket, delay > 250 || queuedBytes(socket) > 16384 ? -1 : delay < 120 && queuedBytes(socket) < 4096 ? 1 : 0);
    } else if (type == "presence") {
        if (!displayName(message.value("name")) || !message.value("available").isBool()
            || !message.value("muted").isBool() || !validPresenceDetails(message)) { socket->abort(); return; }
        const auto name = message.value("name").toString();
        const auto avatar = message.value("avatarId").toString(message.value("avatar").toString("mossling"));
        if (history_ && (peers_[socket].name != name || peers_[socket].avatar != avatar)) {
            try { history_->updateProfile(id, name, avatar); }
            catch (const std::exception& error) { setStatus(tr(error.what()), false); socket->abort(); return; }
        }
        peers_[socket].name = name;
        peers_[socket].available = message.value("available").toBool();
        peers_[socket].muted = message.value("muted").toBool();
        peers_[socket].deafened = message.value("deafened").toBool();
        peers_[socket].avatar = message.value("avatarId").toString(message.value("avatar").toString("mossling"));
        broadcastRoster();
    } else if (type == "chat") {
        hostChat(socket, message);
    } else if (type == "audio") {
        const auto encoded = message.value("data").toString().toLatin1();
        const auto packet = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!packet) { socket->abort(); return; }
        hostAudio(socket, packet.decoded);
    } else socket->abort();
}

void LocalChannel::startMedia(QSslSocket* socket) {
    auto& peer = peers_[socket];
    if (peer.media || (peer.observer && !receivesScreenAudio(peer)) || peer.controller || !peer.joined || !peer.capabilities.contains("udp-audio")) return;
    const auto media = std::make_shared<squad::MediaTransport>(true, port(), socket->peerAddress(), peer.servicePort);
    peer.media = media;
    const auto guard = QPointer<QSslSocket>(socket);
    connect(media.get(), &squad::MediaTransport::signaling, this, [this, guard](QJsonObject message) {
        if (!guard || !peers_.value(guard).joined) return;
        if (message.value("kind") == "frame" && queuedBytes(guard) > 32768) return;
        message.insert("type", "media"); writeMessage(guard, message);
    });
    connect(media.get(), &squad::MediaTransport::audio, this, [this, guard](const QString& source, const QByteArray& packet, int missing) {
        if (!guard || !peers_.value(guard).joined) return;
        if (peers_.value(guard).id != source) { guard->abort(); return; }
        hostAudio(guard, packet, missing);
    });
    connect(media.get(), &squad::MediaTransport::quality, this, [this, guard](int condition) {
        if (guard && peers_.value(guard).joined && (!peers_.value(guard).observer || receivesScreenAudio(peers_.value(guard))))
            sampleAudioLink(guard, condition);
    });
}

void LocalChannel::startMedia(Client& client) {
    if (client.media || !client.accepted || !client.capabilities.contains("udp-audio")) return;
    const auto media = std::make_shared<squad::MediaTransport>(false, 0, client.socket->peerAddress(), client.port);
    client.media = media;
    const auto id = client.id; const auto guard = client.socket;
    connect(media.get(), &squad::MediaTransport::signaling, this, [this, id, guard](QJsonObject message) {
        const auto c = clients_.value(id);
        if (!guard || !c || c->socket != guard || !c->accepted) return;
        if (message.value("kind") == "frame" && queuedBytes(guard) > 32768) return;
        message.insert("type", "media"); writeMessage(guard, message);
    });
    connect(media.get(), &squad::MediaTransport::audio, this, [this, id, guard](const QString& source, const QByteArray& packet, int missing) {
        const auto c = clients_.value(id);
        if (!guard || !c || c->socket != guard || !c->accepted) return;
        const auto member = std::find_if(c->members.begin(), c->members.end(), [&](const auto& entry) {
            return entry.toMap().value("id").toString() == source;
        });
        if (member == c->members.end()) return;
        const bool voice = c->voice && c->id == joinedHostId_;
        const bool screen = member->toMap().value("music").toBool() && receivesScreenAudio(*c);
        if (voice || screen) emit audioReceived(source, packet, missing);
    });
}

void LocalChannel::hostAudio(QSslSocket* socket, const QByteArray& packet, int missing) {
    if (!peers_.contains(socket) || !peers_.value(socket).joined) return;
    auto& peer = peers_[socket];
    if (peer.observer || peer.controller || packet.isEmpty() || packet.size() > 4000
        || opus_packet_get_nb_samples(reinterpret_cast<const unsigned char*>(packet.constData()), int(packet.size()), 48000) != 960) {
        socket->abort(); return;
    }
    if (peer.muted || !peer.available) return;
    peer.lastSpoke = clock_();
    if (peer.sleeping) { peer.sleeping = false; broadcastRoster(); }
    relayAudio(socket, packet, false, missing);
}

void LocalChannel::sampleAudioLink(QSslSocket* socket, int condition) {
    auto& peer = peers_[socket];
    const auto now = controlClock_.elapsed();
    if (now - peer.audioSampled < 750) return;
    peer.audioSampled = now;
    peer.badAudioSamples = condition < 0 ? peer.badAudioSamples + 1 : 0;
    peer.goodAudioSamples = condition > 0 ? peer.goodAudioSamples + 1 : 0;
    const auto previous = peer.audioTier;
    if (peer.badAudioSamples >= 2) { peer.audioTier = std::min(3, peer.audioTier + 1); peer.badAudioSamples = 0; }
    if (peer.goodAudioSamples >= 10) { peer.audioTier = std::max(0, peer.audioTier - 1); peer.goodAudioSamples = 0; }
    qCDebug(mediaLog) << "audio quality condition" << condition << "good" << peer.goodAudioSamples
                     << "bad" << peer.badAudioSamples << "tier" << peer.audioTier;
    if (peer.audioTier != previous)
        writeMessage(socket, {{"type", "audioQuality"}, {"bitrate", audioBitrates[size_t(peer.audioTier)]}});
}

void LocalChannel::relayAudio(QSslSocket* socket, const QByteArray& packet, bool screenAudio, int missing) {
    const auto eligible = [this, screenAudio](const Peer& peer) {
        return peer.joined && !peer.controller && (screenAudio ? receivesScreenAudio(peer) : !peer.observer);
    };
    std::array<bool, 4> needed{};
    const auto receivers = peers_.keys();
    for (auto* other : receivers) {
        const auto& peer = peers_[other];
        if (other != socket && eligible(peer)) needed[size_t(peer.audioTier)] = true;
    }
    std::array<QByteArray, 4> packets; packets[0] = packet;
    auto& relay = socket ? peers_[socket].relay : musicRelay_;
    const auto senderId = socket ? peers_[socket].id : musicId();
    if (needed[1] || needed[2] || needed[3]) {
        try {
            if (!relay) relay = std::make_shared<Relay>();
            packets = relay->packets(packet, needed, missing);
        } catch (const std::exception& error) {
            setStatus(tr(error.what()), false); if (socket) socket->abort(); return;
        }
    } else relay.reset();
    std::array<QJsonObject, 4> payloads;
    for (auto* other : receivers) {
        if (!peers_.contains(other)) continue;
        const auto& peer = peers_[other];
        if (other == socket || !eligible(peer)) continue;
        if (screenAudio && peer.capabilities.contains("udp-screen")) startMedia(other);
        if (peer.media) {
            const auto transport = peer.media;
            transport->send(senderId, packets[size_t(peer.audioTier)], missing);
            continue;
        }
        // Bound new audio behind a congested TCP connection. Text still uses the
        // reliable stream; old voice must not grow an unbounded playback queue.
        if (peer.adaptiveAudio && queuedBytes(other) > 32768) { sampleAudioLink(other, -1); continue; }
        auto& payload = payloads[size_t(peer.audioTier)];
        if (payload.isEmpty()) payload = {{"type", screenAudio ? "screenAudio" : "audio"}, {"sender", senderId},
            {"data", QString::fromLatin1(packets[size_t(peer.audioTier)].toBase64())}, {"bitrate", audioBitrates[size_t(peer.audioTier)]}};
        writeMessage(other, payload);
    }
}

void LocalChannel::admitPeer(QSslSocket* socket) {
    if (!peers_.contains(socket) || peers_.value(socket).closing) return;
    const auto id = peers_[socket].id;
    if (security_.value("blocked").toObject().contains(id)) {
        disconnectMember(id, "blocked"); return;
    }
    const auto verifier = QByteArray::fromBase64(security_.value("verifier").toString().toLatin1());
    if (id != ownId() && !verifier.isEmpty() && peers_[socket].passwordVerified != verifier) {
        requirePassword(socket); return;
    }
    if (hostOnly_) { acceptPeer(socket); return; }
    if (id == ownId() || approved_.contains(id)) { acceptPeer(socket); return; }
    const auto now = clock_();
    for (auto it = requestWindows_.begin(); it != requestWindows_.end();) {
        if (now - it->first >= 30000) it = requestWindows_.erase(it);
        else ++it;
    }
    const auto address = socket->peerAddress().toString();
    auto& window = requestWindows_[address];
    if (window.second == 0) window.first = now;
    if (++window.second > 3) {
        writeMessage(socket, {{"type", "join"}, {"result", "rejected"}, {"retryMs", 30000 - (now - window.first)}});
        finishPeer(socket);
        return;
    }
    const auto previous = attempts_.value(id);
    auto attempts = attempts_;
    for (auto it = attempts.begin(); it != attempts.end();) {
        if (now - it->next > 86400000) it = attempts.erase(it);
        else ++it;
    }
    const auto count = std::min(previous.count + 1, 6);
    attempts.insert(id, {count, now + std::min<qint64>(600000, 30000LL << (count - 1))});
    if (!persist(approved_, attempts, requestsAllowed_, endpointPins_, control_, security_)) { socket->abort(); return; }
    if (!requestsAllowed_ || now < previous.next) {
        writeMessage(socket, {{"type", "join"}, {"result", "rejected"}, {"retryMs", attempts_[id].next - now}});
        finishPeer(socket);
        return;
    }
    writeMessage(socket, {{"type", "join"}, {"result", "pending"}});
    if (!peers_.contains(socket)) return;
    peers_[socket].pending = true;
    emit requestsChanged();
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

bool LocalChannel::decide(const QString& id, bool allow) {
    if (!validId(id)) return setStatus(tr("Invalid device identity."), false);
    if (allow && security_.value("blocked").toObject().contains(id))
        return setStatus(tr("Unblock this member before allowing access."), false);
    auto approvals = approved_;
    if (allow) approvals.insert(id); else approvals.remove(id);
    if (!persist(approvals, attempts_, requestsAllowed_, endpointPins_, control_, security_)) return false;
    applyDecision(id, allow);
    return true;
}

void LocalChannel::applyDecision(const QString& id, bool allow) {
    if (!allow) { disconnectMember(id, "rejected"); return; }
    for (auto* socket : peers_.keys()) {
        if (peers_.value(socket).id != id || peers_.value(socket).controller) continue;
        if (!peers_[socket].name.isEmpty() && !peers_[socket].joined) acceptPeer(socket);
    }
    emit requestsChanged();
}

void LocalChannel::endMembership(const Peer& peer) {
    if (peer.pending) emit requestsChanged();
    if (peer.joined && !peer.controller) {
        closeViewers(peer.id);
        rotateChatKey(); broadcastRoster();
        if (!peer.observer) publishMembership("left", peer.id, peer.name);
    }
}

void LocalChannel::finishPeer(QSslSocket* socket) {
    auto it = peers_.find(socket);
    if (it == peers_.end() || it->closing) return;
    // Keep receiving until the client closes after its final response. Closing
    // immediately can discard that response when inbound bytes race TCP EOF.
    // Maintenance bounds peers that never close, before older clients' own
    // timeout. Discarded traffic cannot refresh it or regain admission.
    const auto previous = *it;
    it->closing = true; it->joined = false; it->pending = false;
    it->lastActivity = controlClock_.elapsed();
    it->buffer.clear(); it->imageRequest = {}; it->imageUpload.clear();
    it->remoteSnapshot.clear(); it->remoteImageRequest = {}; it->relay.reset(); it->media.reset();
    endMembership(previous);
}

bool LocalChannel::disconnectMember(const QString& id, const QString& reason) {
    closeViewers(id);
    bool found = false;
    bool member = false;
    QString voiceName;
    for (auto* socket : peers_.keys()) {
        if (peers_.value(socket).id != id || peers_.value(socket).controller) continue;
        found = true; member |= peers_[socket].joined;
        if (peers_[socket].joined && !peers_[socket].observer) voiceName = peers_[socket].name;
        peers_[socket].joined = false; peers_[socket].pending = false;
        peers_[socket].imageRequest = {}; peers_[socket].imageUpload.clear();
        writeMessage(socket, {{"type", "join"}, {"result", reason},
            {"retryMs", std::max<qint64>(0, attempts_.value(id).next - clock_())}});
        finishPeer(socket);
    }
    if (member) rotateChatKey();
    if (member && !voiceName.isEmpty() && (reason == "kicked" || reason == "blocked"))
        pendingDepartureReasons_.insert(id, reason == "kicked" ? QStringLiteral("kicked") : QStringLiteral("banned"));
    broadcastRoster(); emit requestsChanged();
    if (!voiceName.isEmpty()) publishMembership(reason == "kicked" ? "kicked" : reason == "blocked" ? "banned" : "left", id, voiceName);
    return found;
}

bool LocalChannel::kick(const QString& id) {
    if (!validId(id) || id == ownId()) return setStatus(tr("Select another member to remove."), false);
    return disconnectMember(id, "kicked") || setStatus(tr("This member is no longer connected."), false);
}

bool LocalChannel::setBlocked(const QString& id, bool blocked) {
    if (!validId(id) || id == ownId()) return setStatus(tr("Select another member to block."), false);
    auto entries = security_.value("blocked").toObject();
    if (blocked) {
        QString name = entries.value(id).toString(id.left(12));
        for (const auto& peer : peers_) if (peer.id == id && !peer.name.isEmpty()) { name = peer.name; break; }
        entries.insert(id, name);
    } else entries.remove(id);
    auto next = security_; next.insert("blocked", entries);
    if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, next)) return false;
    if (blocked) disconnectMember(id, "blocked");
    return true;
}

QVariantList LocalChannel::blockedClients() const {
    QVariantList result;
    const auto entries = security_.value("blocked").toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it)
        result.append(QVariantMap{{"id", it.key()}, {"name", it.value().toString()}});
    return result;
}

QVariantList LocalChannel::hostParticipants() const {
    auto result = hostClients();
    result.removeIf([](const auto& member) { return !member.toMap().value("voice").toBool(); });
    return result;
}

QVariantList LocalChannel::hostClients() const {
    QVariantList result;
    for (const auto& peer : peers_) if (peer.joined && !peer.controller)
        result.append(QVariantMap{{"id", peer.id}, {"name", peer.name}, {"available", peer.available}, {"muted", peer.muted},
            {"deafened", peer.deafened}, {"avatar", VoiceSession::avatarFallback(peer.avatar)}, {"avatarId", peer.avatar}, {"sleeping", peer.sleeping}, {"voice", !peer.observer}});
    std::sort(result.begin(), result.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("id").toString() < b.toMap().value("id").toString();
    });
    return result;
}

QVariantMap LocalChannel::hostBot() const {
    return {{"id", musicId()}, {"name", botName_}, {"available", true},
        {"muted", !screenAudio_ && musicActive_ && musicState_ != "playing"}, {"deafened", false}, {"avatar", "system"},
        {"avatarId", "system"}, {"sleeping", botSleeping_}, {"voice", false}, {"music", true},
        {"musicActive", musicActive_ || screenAudio_}, {"musicState", screenAudio_ ? QStringLiteral("playing") : musicState_}, {"station", musicName_}};
}

bool LocalChannel::setRequestsAllowed(bool allowed) { return configureHost({{"requestsAllowed", allowed}}); }

void LocalChannel::broadcastRoster() {
    if (rosterPending_) return;
    rosterPending_ = true;
    QTimer::singleShot(0, this, [this] {
        rosterPending_ = false;
        const auto departures = std::exchange(pendingDepartureReasons_, {});
        auto members = QJsonArray::fromVariantList(hostParticipants());
        if (hosting()) members.append(QJsonObject::fromVariantMap(hostBot()));
        QJsonArray online, profiles;
        auto clients = hostClients();
        if (hosting()) clients.append(hostBot());
        for (const auto& client : clients) {
            const auto member = client.toMap(); online.append(member.value("id").toString());
            profiles.append(QJsonObject{{"id", member.value("id").toString()}, {"name", member.value("name").toString()},
                {"avatar", member.value("avatar").toString()}, {"avatarId", member.value("avatarId").toString()}});
        }
        QJsonObject roster{{"type", "roster"}, {"host", channelId()}, {"name", channelName_}, {"members", members}, {"online", online}, {"profiles", profiles}, {"screen", screenSharing_}, {"screenAudio", screenAudio_}};
        if (!departures.isEmpty()) roster.insert("departures", departures);
        QStringList sources;
        for (const auto& member : members) sources.append(member.toObject().value("id").toString());
        for (auto* socket : peers_.keys()) if (peers_.value(socket).joined && !peers_.value(socket).controller) {
            if (const auto media = peers_.value(socket).media) media->retainSources(sources);
            writeMessage(socket, roster);
        }
        emit hostParticipantsChanged();
    });
}

bool LocalChannel::join(const QString& id, const QString& address, int port) {
    return beginJoin(id, address, port, false);
}

bool LocalChannel::beginJoin(const QString& id, const QString& address, int port, bool automatic) {
    return openConnection(id, address, port, true, automatic);
}

bool LocalChannel::joinAddress(const QString& endpoint) { return accessAddress(endpoint, AddressAction::Join); }
bool LocalChannel::openAddress(const QString& endpoint) { return accessAddress(endpoint, AddressAction::Browse); }
bool LocalChannel::approveAddress(const QString& endpoint) { return accessAddress(endpoint, AddressAction::Approve); }
bool LocalChannel::pairAddress(const QString& endpoint) {
    if (session_.pttInputHeld()) return setControlStatus(tr("Release the held transmit state first."), false);
    return accessAddress(endpoint, AddressAction::Pair);
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

void LocalChannel::finishDirect() {
    if (!direct_) return;
    auto* socket = direct_.data();
    direct_ = nullptr;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    directBuffer_.clear();
    emit stateChanged();
}

bool LocalChannel::accessAddress(const QString& endpoint, AddressAction action) {
    if (!ready() || directBusy()) return setStatus(tr("Device access is busy or not ready."), false);
    auto text = endpoint.trimmed();
    QString addressText = text, portText;
    bool explicitPort = false, bracketed = false;
    if (text.startsWith('[')) {
        bracketed = true;
        const auto close = text.indexOf(']');
        if (close < 0 || (close != text.size() - 1 && text.mid(close + 1, 1) != ":"))
            return setStatus(tr("Enter IPv6 addresses with a port as [address]:port."), false);
        addressText = text.mid(1, close - 1);
        explicitPort = close != text.size() - 1;
        if (explicitPort) portText = text.mid(close + 2);
    } else if (text.count(':') == 1) {
        const auto colon = text.indexOf(':');
        addressText = text.left(colon); portText = text.mid(colon + 1); explicitPort = true;
    }
    bool validPort = true;
    const int port = explicitPort ? portText.toInt(&validPort) : defaultPort;
    const QHostAddress literal(addressText);
    if (text.size() > 300 || !validEndpointHost(addressText)
        || (bracketed && literal.protocol() != QAbstractSocket::IPv6Protocol)
        || !validPort || port < 1 || port > 65535 || (explicitPort && (portText.isEmpty()
            || !std::all_of(portText.begin(), portText.end(), [](QChar c) { return c >= '0' && c <= '9'; }))))
        return setStatus(tr("Enter a valid host or IP address, optionally with a port from 1 to 65535."), false);
    addressText = canonicalEndpointHost(addressText);
    const QString key = endpointKey(addressText, quint16(port));
    const auto expected = endpointPins_.value(key);
    auto* socket = new QSslSocket(this);
    direct_ = socket;
    directBuffer_.clear();
    socket->setReadBufferSize(2 * maximumFrame);
    socket->setSslConfiguration(identity_->configuration());
    const auto identityMatches = [socket, expected] {
        const auto id = TlsIdentity::peerId(socket->peerCertificate());
        return validId(id) && (expected.isEmpty() || id == expected);
    };
    connect(socket, &QSslSocket::sslErrors, this, [this, socket, identityMatches](const QList<QSslError>& errors) {
        socket->setProperty("squadTlsSeen", true);
        if (!identityMatches()) {
            finishDirect();
            setStatus(tr("A different device identity responds at this address. No connection or new permission will be created."), false);
        } else if (acceptableCertificateErrors(errors)) socket->ignoreSslErrors(errors);
    });
    connect(socket, &QSslSocket::encrypted, this, [this, socket, identityMatches] {
        socket->setProperty("squadTlsSeen", true);
        if (!identityMatches()) {
            finishDirect();
            setStatus(tr("The saved device identity does not match."), false);
            return;
        }
        writeMessage(socket, {{"type", "identify"}, {"version", 1}});
    });
    connect(socket, &QSslSocket::readyRead, this, [this, socket, key, addressText, port, action] {
        auto buffer = std::move(directBuffer_);
        readMessages(socket, buffer, [this, socket, key, addressText, port, action](const QJsonObject& message) {
            if (direct_ != socket) return;
            if (message.value("type") != "identity" || message.value("version").toInt() != 1
                || !displayName(message.value("name")) || !displayName(message.value("channel")) || !message.value("hosting").isBool()) {
                finishDirect(); setStatus(tr("The peer did not report valid device access."), false); return;
            }
            const auto id = TlsIdentity::peerId(socket->peerCertificate());
            if (action == AddressAction::Approve && security_.value("blocked").toObject().contains(id)) {
                finishDirect(); setStatus(tr("Unblock this member before allowing access."), false); return;
            }
            if ((action == AddressAction::Join || action == AddressAction::Browse) && !message.value("hosting").toBool()) {
                finishDirect(); setStatus(tr("The device is reachable but has not opened a channel."), false); return;
            }
            auto pins = endpointPins_; pins.insert(key, id);
            auto approvals = approved_;
            if (action == AddressAction::Approve) approvals.insert(id);
            if (!persist(approvals, attempts_, requestsAllowed_, pins, control_, security_)) { finishDirect(); return; }
            finishDirect();
            if (action == AddressAction::Pair) {
                startControl(id, addressText, quint16(port), message.value("name").toString());
            } else if (action == AddressAction::Approve) {
                applyDecision(id, true);
                setStatus(tr("%1 is permitted for your channel.").arg(message.value("name").toString()));
            } else {
                if (!receiveDirectory(id, message, addressText, quint16(port), true, false)) {
                    setStatus(tr("The peer reported an invalid channel list."), false); return;
                }
                const auto directory = message.value("channels").toArray();
                if (directory.size() > 1) { setStatus(tr("Choose a channel from this host.")); return; }
                const auto selected = directory.isEmpty() ? id : directory.first().toObject().value("id").toString();
                if (action == AddressAction::Browse) openChat(selected, addressText, port);
                else join(selected, addressText, port);
            }
        });
        if (direct_ == socket) directBuffer_ = std::move(buffer);
    });
    const bool preferIpv4 = QHostAddress(addressText).isNull();
    connect(socket, &QSslSocket::errorOccurred, this, [this, socket, addressText, port, ipv4Attempt = preferIpv4](QAbstractSocket::SocketError error) mutable {
        if (direct_ != socket) return;
        if (ipv4Attempt && addressConnectionFailure(error, socket)) {
            ipv4Attempt = false;
            socket->setProperty("squadIpv6Retry", true);
            QTimer::singleShot(0, socket, [this, socket, addressText, port] {
                if (direct_ != socket) return;
                socket->setProperty("squadIpv6Retry", false);
                connectEndpoint(socket, addressText, quint16(port), QAbstractSocket::IPv6Protocol);
            });
            return;
        }
        const auto reason = socket->errorString();
        finishDirect(); setStatus(tr("Device access failed: %1").arg(reason), false);
    });
    connect(socket, &QSslSocket::disconnected, this, [this, socket] {
        if (direct_ != socket) return;
        if (socket->property("squadIpv6Retry").toBool()) return;
        finishDirect(); setStatus(tr("Device access closed without a valid response."), false);
    });
    QTimer::singleShot(5000, socket, [this, socket] {
        if (direct_ != socket) return;
        finishDirect(); setStatus(tr("Device was not reachable in time. Check the address, port, and firewall."), false);
    });
    setStatus(tr("Checking device identity at %1.").arg(key));
    connectEndpoint(socket, addressText, quint16(port),
        preferIpv4 ? QAbstractSocket::IPv4Protocol : QAbstractSocket::AnyIPProtocol);
    return true;
}

void LocalChannel::connectClient(Client& c) {
    if (!c.reconnectWanted || !ready() || c.socket) return;
    const auto connection = clients_.value(c.id);
    auto* socket = new QSslSocket(this);
    c.socket = socket;
    c.passwordSent = false; c.passwordRequired = false; c.access = "connecting";
    c.reply = c.probe = controlClock_.elapsed();
    c.buffer.clear();
    c.chatKey.fill(0); c.chatKey.clear(); c.chatEpoch.clear();
    emit chatChanged();
    socket->setReadBufferSize(2 * maximumFrame);
    socket->setSslConfiguration(identity_->configuration());
    connect(socket, &QSslSocket::sslErrors, this, [this, socket, connection](const QList<QSslError>& errors) {
        socket->setProperty("squadTlsSeen", true);
        auto& c = *connection;
        if (TlsIdentity::peerId(socket->peerCertificate()) == c.deviceId && acceptableCertificateErrors(errors))
            socket->ignoreSslErrors(errors);
        else {
            c.reconnectWanted = false; setClientStatus(c, tr("Channel identity could not be confirmed."), false);
            if (c.voice && automaticAttempt_) advanceAutoJoin(true);
        }
    });
    connect(socket, &QSslSocket::encrypted, this, [this, socket, connection] {
        socket->setProperty("squadTlsSeen", true);
        auto& c = *connection;
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        if (TlsIdentity::peerId(socket->peerCertificate()) != c.deviceId) {
            if (c.voice && automaticAttempt_) { advanceAutoJoin(true); return; }
            c.reconnectWanted = false; socket->abort(); setClientStatus(c, tr("The channel identity does not match."), false); return;
        }
        QJsonObject hello{{"type", "hello"}, {"version", 1}, {"targetChannel", c.id}, {"name", session_.userName()},
            {"available", session_.available()}, {"muted", !session_.transmissionAllowed() || !session_.inputReady()},
            {"deafened", session_.deafened() || !session_.outputReady()}, {"avatar", VoiceSession::avatarFallback(session_.avatar())},
            {"avatarId", session_.avatar()}, {"observer", !c.voice}, {"adaptiveAudio", true}, {"remoteOffers", true}, {"capabilities", capabilityAdvertisement()}};
        if (hosting()) { hello.insert("channel", channelName_); hello.insert("port", int(server_.serverPort())); }
        writeMessage(socket, hello);
    });
    const auto receive = [this, socket, connection] {
        auto& c = *connection;
        if (c.socket != socket || !socket->isOpen()) return;
        auto buffer = std::move(c.buffer);
        readMessages(socket, buffer, [this, &c](const QJsonObject& message) { clientMessage(c, message); });
        if (c.socket == socket) c.buffer = std::move(buffer);
    };
    connect(socket, &QSslSocket::readyRead, this, receive);
    connect(socket, &QSslSocket::disconnected, this, [this, socket, connection, receive] {
        auto& c = *connection;
        if (c.socket != socket) return;
        if (socket->property("squadIpv6Retry").toBool()) return;
        receive();
        if (c.socket != socket) return;
        if (c.voice && automaticAttempt_) { advanceAutoJoin(false); return; }
        closeClient(c);
        if (c.reconnectWanted) setClientStatus(c, tr("Connection interrupted. Retrying."));
    });
    const bool preferIpv4 = QHostAddress(c.address).isNull();
    connect(socket, &QSslSocket::errorOccurred, this, [this, socket, connection, ipv4Attempt = preferIpv4](QAbstractSocket::SocketError error) mutable {
        auto& c = *connection;
        if (c.socket != socket) return;
        // TLS can report EOF before delivering its final decrypted messages.
        // disconnected() drains those messages before deciding whether to retry.
        if (error == QAbstractSocket::RemoteHostClosedError) return;
        if (ipv4Attempt && c.reconnectWanted && !c.accepted && addressConnectionFailure(error, socket)) {
            ipv4Attempt = false;
            socket->setProperty("squadIpv6Retry", true);
            QTimer::singleShot(0, socket, [socket, connection] {
                if (connection->socket != socket || !connection->reconnectWanted) return;
                socket->setProperty("squadIpv6Retry", false);
                connectEndpoint(socket, connection->address, connection->port, QAbstractSocket::IPv6Protocol);
            });
            return;
        }
        if (c.voice && automaticAttempt_) { advanceAutoJoin(false); return; }
        const auto reason = socket->errorString();
        if (socket->state() == QAbstractSocket::UnconnectedState) closeClient(c);
        setClientStatus(c, tr("Connection failed: %1").arg(reason), false);
    });
    setClientStatus(c, tr("Establishing encrypted connection."));
    connectEndpoint(socket, c.address, c.port,
        preferIpv4 ? QAbstractSocket::IPv4Protocol : QAbstractSocket::AnyIPProtocol);
}

void LocalChannel::closeClient(Client& c) {
    c.passwordAutoRetry = false; c.media.reset();
    closeScreen(c); c.screenAvailable = false;
    if (c.socket) {
        auto* socket = c.socket.data(); c.socket = nullptr;
        socket->disconnect(this); socket->abort(); socket->deleteLater();
    }
    if (c.accepted) c.access = "offline";
    c.accepted = false; c.rosterSeen = false; c.members.clear(); c.onlineIds.clear(); c.presenceKnown = false; c.buffer.clear();
    c.adaptiveAudio = false; c.audioBitrate = 32;
    c.historyRequest.clear(); c.messages = {}; c.hasOlder = false; c.hasNewer = false;
    c.chatKey.fill(0); c.chatKey.clear(); c.capabilities.clear(); c.capabilitiesAdvertised = false;
    c.reconnectAt = controlClock_.elapsed() + 2000;
    emit chatChanged(); emit participantsChanged(); emit stateChanged(); emit hostsChanged();
}

bool LocalChannel::leave() {
    return leaveChannel(true);
}

bool LocalChannel::leaveChannel(bool pauseAutoJoin) {
    if (joined()) emit channelEvent("leave");
    if (pauseAutoJoin && !joinedHostId_.isEmpty()) pausedAutoJoin_.insert(joinedHostId_);
    automaticAttempt_ = false;
    finishDirect();
    const auto id = std::exchange(joinedHostId_, {});
    if (auto c = clients_.value(id)) {
        c->voice = false;
        if (c->accepted) writeMessage(c->socket, {{"type", "voiceLeave"}});
        else { c->reconnectWanted = false; c->passwordRequired = false; closeClient(*c); }
    }
    emit participantsChanged(); emit hostsChanged(); emit stateChanged();
    return setStatus(tr("Not joined to a voice channel."));
}

void LocalChannel::clientMessage(Client& c, const QJsonObject& message) {
    c.reply = controlClock_.elapsed();
    const auto type = message.value("type").toString();
    if (c.accepted && unknownMessageType(type)) return;
    if (type == "pong") return;
    if (type == "remoteOffer") {
        if (!c.accepted || !message.value("allowed").isBool()
            || (message.contains("available") && !message.value("available").isBool())
            || !receiveRemoteOffer(c.id, message.value("allowed").toBool(), message.value("available").toBool(true))) {
            if (c.socket) c.socket->abort();
        }
        return;
    }
    if ((type == "audioProbe" || type == "audioQuality") && c.accepted && c.adaptiveAudio) {
        if (type == "audioProbe") {
            const auto serial = message.value("serial").toInteger(-1);
            if (serial < 1 || serial > 9007199254740991LL || message.value("serial").toDouble(-1) != double(serial)) { if (c.socket) c.socket->abort(); return; }
            writeMessage(c.socket, {{"type", "audioAck"}, {"serial", serial}});
        } else {
            const auto bitrate = message.value("bitrate").toInt(-1);
            if (std::find(audioBitrates.begin(), audioBitrates.end(), bitrate) == audioBitrates.end()) { if (c.socket) c.socket->abort(); return; }
            if (c.audioBitrate != bitrate) { c.audioBitrate = bitrate; emit stateChanged(); }
        }
        return;
    }
    if (type == "join") {
        c.capabilitiesAdvertised = message.contains("capabilities");
        if (!parseCapabilities(message, &c.capabilities)) { if (c.socket) c.socket->abort(); return; }
        const auto result = message.value("result").toString();
        c.access = result;
        emit hostsChanged();
        c.passwordAutoRetry = false;
        if (result == "password") {
            if (c.voice && automaticAttempt_ && (c.password.isEmpty() || (c.passwordSent && !message.value("busy").toBool()))) {
                advanceAutoJoin(true); return;
            }
            c.reconnectWanted = false;
            c.passwordRequired = true;
            c.passwordRetryAt = controlClock_.elapsed() + std::clamp<qint64>(message.value("retryMs").toInteger(), 0, 600000);
            if (message.value("busy").toBool() && !c.password.isEmpty()) {
                c.passwordAutoRetry = true;
                setClientStatus(c, tr("The host is reviewing other joins. Your join will be retried shortly."));
            } else setClientStatus(c, c.passwordSent ? tr("Password not accepted. Check it and enter it again.") : tr("This channel requires a password."));
            if (!c.passwordSent && !c.password.isEmpty() && c.passwordRetryAt <= controlClock_.elapsed())
                submitPassword(c, c.password, c.rememberPassword);
            return;
        }
        c.passwordRequired = false;
        const bool wasJoined = c.accepted;
        c.accepted = result == "accepted";
        if (c.voice && c.accepted && !wasJoined) emit channelEvent("join");
        c.reconnectWanted = c.accepted;
        if (!c.accepted) {
            c.media.reset(); c.members.clear(); c.onlineIds.clear(); c.presenceKnown = false; c.messages = {}; c.historyRequest.clear(); c.hasOlder = false; c.hasNewer = false;
            if (c.id == chatHostId_) { imageCache_.clear(); imageQueue_.clear(); downloadingImage_.clear(); downloadedImage_.clear(); ++imageRevision_; emit imagesChanged(); }
            emit participantsChanged();
            c.chatKey.fill(0); c.chatKey.clear();
            if (!c.pending.isEmpty()) c.chatError = tr("Delivery was not confirmed. The draft is retained.");
            c.pending = {}; c.pendingImage.clear(); emit chatChanged();
        }
        if (c.accepted) {
            c.adaptiveAudio = message.value("adaptiveAudio").toBool()
                && (!c.capabilitiesAdvertised || c.capabilities.contains("adaptive-audio")); c.audioBitrate = 32;
            if (c.voice) startMedia(c);
            if (c.voice) automaticAttempt_ = false;
            auto next = savedChannels_;
            auto entry = next.value(c.id).toObject();
            if (c.voice || !entry.contains("lastJoined")) entry.insert("lastJoined", clock_());
            entry.insert("name", hosts_.contains(c.id) ? hosts_.value(c.id).name
                : c.name.isEmpty() ? entry.value("name").toString(tr("Channel")) : c.name);
            entry.insert("address", c.address); entry.insert("port", c.port);
            if (!entry.contains("autoJoin")) entry.insert("autoJoin", false);
            next.insert(c.id, entry);
            if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &next)) { sendPresence(); return; }
            if (c.passwordSent && !savePassword(c, c.password, c.rememberPassword)) {
                sendPresence(); return;
            }
            setClientStatus(c, {}); sendPresence();
        }
        else if (result == "pending") setClientStatus(c, tr("Waiting for host approval."));
        else if (result == "full") setClientStatus(c, tr("The channel is full. Text chat is still available."), false);
        else if (result == "kicked" || result == "blocked") {
            if (c.voice && wasJoined) emit channelEvent(result == "kicked" ? "kick" : "ban");
            pausedAutoJoin_.insert(c.id);
            if (result == "blocked") {
                auto next = savedChannels_; next.remove(c.id);
                if (!persist(approved_, attempts_, requestsAllowed_, endpointPins_, control_, security_, &next)) return;
                unavailableControlTargets_.remove(c.id);
            }
            c.members.clear(); emit participantsChanged();
            setClientStatus(c, result == "kicked" ? tr("You were removed from this channel. You can join again.")
                : tr("You are blocked from this channel."), false);
        }
        else if (result == "replaced") {
            c.members.clear();
            emit participantsChanged();
            setClientStatus(c, tr("This device is in the channel through a newer connection."));
        }
        else setClientStatus(c, tr("Join rejected. A new request is possible in %1 seconds at the earliest.")
            .arg((message.value("retryMs").toInteger() + 999) / 1000), false);
        if (c.voice && automaticAttempt_ && !c.accepted) advanceAutoJoin(true);
        if (c.socket && !c.accepted && result != "pending") c.socket->disconnectFromHost();
    } else if ((type == "chatKey" || type == "chat") && c.accepted) {
        clientChat(c, message);
    } else if (type == "roster" && c.accepted) {
        if (message.value("host").toString() != c.id || !message.value("members").isArray()) {
            if (c.socket) c.socket->abort();
            return;
        }
        if (!validMembers(message.value("members"))) { if (c.socket) c.socket->abort(); return; }
        QStringList onlineIds;
        if (message.contains("online")) {
            if (!message.value("online").isArray()) { if (c.socket) c.socket->abort(); return; }
            for (const auto value : message.value("online").toArray()) {
                const auto id = value.toString();
                if (!validId(id) || onlineIds.contains(id)) { if (c.socket) c.socket->abort(); return; }
                onlineIds.append(id);
            }
        }
        c.screenAvailable = message.value("screen").toBool()
            && (!c.capabilitiesAdvertised || c.capabilities.contains("screen-share"));
        c.screenAudioAvailable = c.screenAvailable && message.value("screenAudio").toBool() && c.capabilities.contains("screen-audio");
        if (!c.screenAvailable) closeScreen(c);
        else if (c.screenWanted && !c.screenSocket) connectScreen(c);
        emit screenChanged();
        c.presenceKnown = message.contains("online"); c.onlineIds = onlineIds;
        if (message.contains("profiles")) {
            if (!message.value("profiles").isArray()) { if (c.socket) c.socket->abort(); return; }
            QHash<QString, QJsonObject> profiles;
            for (const auto value : message.value("profiles").toArray()) {
                const auto profile = value.toObject(); const auto id = profile.value("id").toString();
                if (!onlineIds.contains(id) || profiles.contains(id) || !displayName(profile.value("name"))
                    || !VoiceSession::validAvatar(profile.value("avatar").toString())
                    || !VoiceSession::validAvatar(profile.value("avatarId").toString())) {
                    if (c.socket) c.socket->abort();
                    return;
                }
                profiles.insert(id, profile);
            }
            bool changed = false;
            for (qsizetype i = 0; i < c.messages.size(); ++i) {
                auto record = c.messages.at(i).toObject(); const auto profile = profiles.value(record.value("sender").toString());
                if (profile.isEmpty()) continue;
                const auto previous = record;
                for (const auto* key : {"name", "avatar", "avatarId"}) record.insert(key, profile.value(key));
                if (record != previous) { c.messages[i] = record; changed = true; }
            }
            if (changed) emit chatChanged();
        }
        const auto members = message.value("members").toArray().toVariantList();
        QSet<QString> ids;
        for (const auto& member : humanMembers(members)) ids.insert(member.toMap().value("id").toString());
        if (c.voice && c.rosterSeen) {
            QSet<QString> previous;
            for (const auto& member : humanMembers(c.members)) previous.insert(member.toMap().value("id").toString());
            if (!(ids - previous).isEmpty()) emit channelEvent("memberJoin");
            bool memberLeave = false, kicked = false, banned = false;
            const auto departures = message.value("departures").toObject();
            for (const auto& removed : previous - ids) {
                const auto kind = departures.value(removed).toString();
                if (kind == "kicked") kicked = true;
                else if (kind == "banned") banned = true;
                else memberLeave = true;
            }
            if (memberLeave) emit channelEvent("memberLeave");
            if (kicked) emit channelEvent("kick");
            if (banned) emit channelEvent("ban");
        }
        c.rosterSeen = true;
        c.members = members;
        if (c.media) {
            auto sources = ids.values(); sources.append(ownId());
            for (const auto& member : members) if (member.toMap().value("music").toBool()) sources.append(member.toMap().value("id").toString());
            c.media->retainSources(sources);
        }
        emit hostsChanged();
        if (displayName(message.value("name"))) rememberChannel(c, message.value("name").toString());
        if (hosts_.contains(c.id) && displayName(message.value("name"))) {
            hosts_[c.id].name = message.value("name").toString(); emit hostsChanged();
        }
        emit participantsChanged();
    } else if (type == "media" && c.accepted) {
        // A screen-only viewer receives media without gaining microphone rights.
        // Keep this connection until chat closes, so late packets and reopening
        // cannot race a second negotiation on the same authenticated channel.
        startMedia(c);
        const auto media = c.media;
        if (!media || !media->receive(message)) { if (c.socket) c.socket->abort(); }
    } else if ((type == "audio" || type == "screenAudio") && c.accepted) {
        const bool screenAudio = type == "screenAudio";
        if (screenAudio ? !receivesScreenAudio(c) : (!c.voice || c.id != joinedHostId_)) return;
        const auto id = message.value("sender").toString();
        const auto packet = QByteArray::fromBase64Encoding(message.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        const bool known = std::any_of(c.members.begin(), c.members.end(), [&](const auto& entry) {
            const auto member = entry.toMap();
            return member.value("id").toString() == id && (!screenAudio || member.value("music").toBool());
        });
        if (known && packet && !packet.decoded.isEmpty() && packet.decoded.size() <= 8192) {
            if (c.adaptiveAudio) {
                const auto bitrate = message.value("bitrate").toInt(-1);
                if (std::find(audioBitrates.begin(), audioBitrates.end(), bitrate) == audioBitrates.end()) { if (c.socket) c.socket->abort(); return; }
                if (c.audioBitrate != bitrate) { c.audioBitrate = bitrate; emit stateChanged(); }
            }
            emit audioReceived(id, packet.decoded);
        }
    } else { if (c.socket) c.socket->abort(); }
}

void LocalChannel::sendPresence() {
    for (const auto& c : clients_) {
        if (!c->accepted) continue;
        writeMessage(c->socket, {{"type", "presence"}, {"name", session_.userName()},
            {"available", session_.available()}, {"muted", !c->voice || !session_.transmissionAllowed() || !session_.inputReady()},
            {"deafened", session_.deafened() || !session_.outputReady()}, {"avatar", VoiceSession::avatarFallback(session_.avatar())}, {"avatarId", session_.avatar()}});
    }
}

bool LocalChannel::sendAudio(const QByteArray& packet) {
    const auto c = clients_.value(joinedHostId_);
    if (remoteMode() || !c || !c->accepted || !c->voice || !session_.transmissionAllowed() || packet.isEmpty() || packet.size() > 8192) return false;
    if (c->media) return c->media->send(ownId(), packet);
    return writeMessage(c->socket, {{"type", "audio"}, {"data", QString::fromLatin1(packet.toBase64())}});
}

QString LocalChannel::musicId() const {
    return QString::fromLatin1(QCryptographicHash::hash(channelId().toLatin1() + "/system", QCryptographicHash::Sha256).toHex());
}

bool LocalChannel::setMusicState(const QString& name, const QString& state, bool active) {
    if (active && (!displayName(name) || (state != "playing" && state != "connecting"
        && state != "reconnecting" && state != "unavailable"))) return false;
    if (musicName_ == name && musicState_ == state && musicActive_ == active) return true;
    musicName_ = name; musicState_ = state; musicActive_ = active;
    if (!active) musicRelay_.reset();
    if (hosting()) broadcastRoster();
    return true;
}

bool LocalChannel::sendMusic(const QByteArray& packet) {
    if (screenSharing_) return false;
    if (!hosting() || !musicActive_ || musicState_ != "playing" || packet.isEmpty() || packet.size() > 4000
        || opus_packet_get_nb_samples(reinterpret_cast<const unsigned char*>(packet.constData()), int(packet.size()), 48000) != 960) return false;
    botLastActive_ = clock_();
    if (botSleeping_) { botSleeping_ = false; broadcastRoster(); }
    relayAudio(nullptr, packet);
    return true;
}

bool LocalChannel::sendScreenAudio(const QByteArray& packet) {
    if (!screenSharing_ || !screenAudio_ || packet.isEmpty() || packet.size() > 4000
        || opus_packet_get_nb_samples(reinterpret_cast<const unsigned char*>(packet.constData()), int(packet.size()), 48000) != 960) return false;
    botLastActive_ = clock_();
    if (botSleeping_) { botSleeping_ = false; broadcastRoster(); }
    relayAudio(nullptr, packet, true); return true;
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
    bool present = false;
    for (const auto& value : (remoteMode() ? remoteView_.value("messages").toList() : messages())) if (value.toMap().value("image").toMap().value("hash").toString() == hash) { present = true; break; }
    if (!present) return false;
    if (imageCache_.contains(hash) || downloadingImage_ == hash || imageQueue_.contains(hash)) return true;
    if (imageQueue_.size() >= 64) return false;
    imageQueue_.append(hash); downloadImage(); return true;
}

void LocalChannel::downloadImage() {
    if ((remoteMode() ? !remoteAllowed_ || !remoteView_.value("chatReady").toBool() : !chatReady()) || !downloadingImage_.isEmpty()) return;
    while (!imageQueue_.isEmpty()) {
        const auto hash = imageQueue_.takeFirst();
        bool present = false;
        for (const auto& value : (remoteMode() ? remoteView_.value("messages").toList() : messages())) if (value.toMap().value("image").toMap().value("hash").toString() == hash) { present = true; break; }
        if (!present || imageCache_.contains(hash)) continue;
        downloadingImage_ = hash; downloadedImage_.clear();
        sendImageRequest(hash, 0); return;
    }
}

QString LocalChannel::imageSource(const QString& hash) const {
    const auto* png = imageCache_.object(hash);
    if (!png) return {};
    bool present = false;
    for (const auto& value : (remoteMode() ? remoteView_.value("messages").toList() : messages())) if (value.toMap().value("image").toMap().value("hash").toString() == hash) { present = true; break; }
    return present ? "data:image/png;base64," + QString::fromLatin1(png->toBase64()) : QString{};
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
