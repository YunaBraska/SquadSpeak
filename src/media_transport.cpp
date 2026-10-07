#include "media_transport.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QtEndian>
#include <rtc/rtc.hpp>
#include <deque>
#include <map>
#include <mutex>
#include <opus.h>

namespace squad {
namespace {
Q_LOGGING_CATEGORY(mediaTransportLog, "squadspeak.media.transport", QtWarningMsg)
constexpr int maximumSources = 65;
constexpr int maximumOpus = 4000;
constexpr int maximumDatagram = 1200;
bool sourceId(const QString& id) {
    return id.size() == 64 && std::all_of(id.begin(), id.end(), [](QChar c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool validOpus(const QByteArray& bytes) {
    return !bytes.isEmpty() && bytes.size() <= maximumOpus &&
        opus_packet_get_nb_samples(reinterpret_cast<const unsigned char*>(bytes.constData()), int(bytes.size()), 48000) == 960;
}
}

struct MediaTransport::State final {
    struct Stream {
        QString source;
        bool confirmed = false;
        quint16 sequence = 0, start = 0;
        quint32 timestamp = 0;
        qint64 highest = -1, delivered = -1;
        struct Frame { QByteArray bytes; qint64 due; };
        std::map<qint64, Frame> pending;
    };
    MediaTransport* owner;
    const bool offerer;
    const Medium medium;
    const quint16 localPort, remotePort;
    const QHostAddress remoteAddress;
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::Channel> channel;
    QHash<quint32, Stream> outgoing, incoming;
    QElapsedTimer clock;
    qint64 acknowledged = -10000, probeAt = -1, probeSent = -1000, qualityAt = 0;
    quint32 probe = 0, received = 0, expected = 0;
    quint64 reportedReceived = 0, reportedExpected = 0;
    bool descriptionSeen = false, failed = false;
    quint32 epoch = 0;
    quint64 generation = 0;
    qint64 attempted = 0;
    std::mutex mutex;
    std::deque<std::function<void()>> mailbox;
    qsizetype queuedBytes = 0;

    State(MediaTransport* value, bool offer, quint16 port, QHostAddress address, quint16 remote, Medium mode)
        : owner(value), offerer(offer), medium(mode), localPort(port), remotePort(remote), remoteAddress(std::move(address)) { clock.start(); }

    // Hold the mutex through invokeMethod: destruction invalidates owner under
    // this same lock. Queued lambdas cannot outlive the QObject receiver.
    static void post(const std::weak_ptr<State>& weak, quint64 generation, qsizetype size, std::function<void(State&)> action) {
        const auto state = weak.lock();
        if (!state) return;
        std::lock_guard lock(state->mutex);
        if (!state->owner || size > 65536 || state->mailbox.size() >= 256 || state->queuedBytes + size > 512 * 1024) return;
        const bool schedule = state->mailbox.empty();
        state->queuedBytes += size;
        state->mailbox.emplace_back([weak, generation, action = std::move(action)] { if (auto s = weak.lock(); s && s->generation == generation) action(*s); });
        if (!schedule) return;
        QMetaObject::invokeMethod(state->owner, [weak] {
            const auto s = weak.lock(); if (!s) return;
            std::deque<std::function<void()>> pending;
            { std::lock_guard guard(s->mutex); if (!s->owner) return; pending.swap(s->mailbox); s->queuedBytes = 0; }
            for (auto& call : pending) {
                if (!s->owner) break;
                call();
            }
        }, Qt::QueuedConnection);
    }
    bool write(const QByteArray& bytes) {
        if (!channel || !channel->isOpen() || (medium == Medium::Data && channel->bufferedAmount() > 32768)) return false;
        try {
            const bool sent = channel->send(reinterpret_cast<const rtc::byte*>(bytes.constData()), size_t(bytes.size()));
            return medium == Medium::Data || sent; // Data-channel false means queued, not rejected.
        }
        catch (const std::exception& error) { qCDebug(mediaTransportLog) << error.what(); failed = true; return false; }
    }
};

MediaTransport::MediaTransport(bool offerer, quint16 localPort, QHostAddress address, quint16 remotePort, Medium medium, QObject* parent)
    : QObject(parent), state_(std::make_shared<State>(this, offerer, localPort, address, remotePort, medium)) {
    static const bool runtime = [] {
        rtc::SetThreadPoolSize(2);
        rtc::SctpSettings sctp; sctp.recvBufferSize = 262144; sctp.sendBufferSize = 262144;
        rtc::SetSctpSettings(sctp);
        qAddPostRoutine([] { rtc::Cleanup().wait(); });
        return true;
    }();
    Q_UNUSED(runtime);
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &MediaTransport::tick);
    timer_.start();
    QTimer::singleShot(0, this, &MediaTransport::start);
}

MediaTransport::~MediaTransport() {
    const auto s = state_;
    { std::lock_guard lock(s->mutex); s->owner = nullptr; s->mailbox.clear(); s->queuedBytes = 0; }
    timer_.stop();
    stopUdp();
}

void MediaTransport::stopUdp() {
    auto& s = *state_;
    s.failed = true; s.acknowledged = -10000;
    if (s.channel) { s.channel->resetCallbacks(); s.channel->close(); s.channel.reset(); }
    if (s.connection) { s.connection->resetCallbacks(); s.connection->close(); s.connection.reset(); }
}

void MediaTransport::start() {
    auto& s = *state_;
    if (s.connection || s.failed) return;
    s.attempted = s.clock.elapsed();
    try {
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        config.enableIceUdpMux = s.localPort != 0;
        if (s.localPort) config.portRangeBegin = config.portRangeEnd = s.localPort;
        config.mtu = maximumDatagram;
        config.maxMessageSize = 16384;
        s.connection = std::make_shared<rtc::PeerConnection>(config);
        const auto weak = std::weak_ptr(state_);
        const auto generation = s.generation;
        s.connection->onGatheringStateChange([weak, generation](rtc::PeerConnection::GatheringState status) {
            if (status != rtc::PeerConnection::GatheringState::Complete) return;
            State::post(weak, generation, 0, [](State& s) {
                if (!s.connection || !s.connection->localDescription()) return;
                const auto description = *s.connection->localDescription();
                emit s.owner->signaling({{"kind", "description"}, {"epoch", double(s.epoch)}, {"sdp", QString::fromStdString(std::string(description))}});
            });
        });
        s.connection->onStateChange([weak, generation](rtc::PeerConnection::State status) {
            State::post(weak, generation, 0, [status](State& s) {
                if (status == rtc::PeerConnection::State::Failed || status == rtc::PeerConnection::State::Closed) s.failed = true;
            });
        });
        if (s.medium == Medium::Audio) {
            rtc::Description::Audio description("voice", rtc::Description::Direction::SendRecv);
            description.addOpusCodec(111);
            // One RTP track carries independent SSRCs; mappings arrive over TLS.
            description.addSSRC(1, "voice");
            s.channel = s.connection->addTrack(description);
        } else {
            rtc::DataChannelInit init;
            init.negotiated = true; init.id = 0;
            init.reliability.unordered = true;
            init.reliability.maxPacketLifeTime = std::chrono::milliseconds(100);
            s.channel = s.connection->createDataChannel("screen", init);
            s.connection->onDataChannel([](const auto& unexpected) { unexpected->close(); });
        }
        s.channel->setBufferedAmountLowThreshold(16384);
        s.channel->onBufferedAmountLow([weak, generation] {
            State::post(weak, generation, 0, [](State& s) { emit s.owner->writable(); });
        });
        s.channel->onMessage([weak, generation](rtc::message_variant value) {
            const auto* bytes = std::get_if<rtc::binary>(&value);
            if (!bytes || bytes->size() > 16384) return;
            QByteArray packet(reinterpret_cast<const char*>(bytes->data()), qsizetype(bytes->size()));
            const auto size = packet.size();
            State::post(weak, generation, size, [packet = std::move(packet)](State& s) { s.owner->packet(packet); });
        });
        if (s.offerer) s.connection->setLocalDescription(rtc::Description::Type::Offer);
        timer_.start();
    } catch (const std::exception& error) {
        qCWarning(mediaTransportLog) << "UDP initialization failed; retaining TLS:" << error.what(); stopUdp();
    }
}

void MediaTransport::restartUdp() {
    ++state_->generation;
    stopUdp();
    state_->failed = false; state_->descriptionSeen = false;
    state_->probeAt = -1; state_->reportedReceived = state_->reportedExpected = 0;
    timer_.start(100); start();
}

bool MediaTransport::udpActive() const {
    const auto& s = *state_;
    return !s.failed && s.channel && s.channel->isOpen() && s.clock.elapsed() - s.acknowledged < 300;
}

bool MediaTransport::receive(const QJsonObject& message) {
    const auto keepAlive = state_;
    auto& s = *state_;
    const auto kind = message.value("kind").toString();
    static const QRegularExpression name(QStringLiteral("^[A-Za-z][A-Za-z0-9._-]*$"));
    if (kind.size() > 64 || !name.match(kind).hasMatch()) return false;
    if (kind != "restart" && kind != "description" && kind != "source" && kind != "sourceAck" && kind != "frame") return true;
    if (kind == "restart" || kind == "description") {
        const auto epoch = message.value("epoch").toInteger(-1);
        if (epoch < 0 || epoch > UINT32_MAX || message.value("epoch").toDouble(-1) != double(epoch)) return false;
        if (epoch < s.epoch) return true;
        if (kind == "restart") {
            if (s.offerer || epoch > qint64(s.epoch) + 1) return false;
            if (epoch != s.epoch) { s.epoch = quint32(epoch); restartUdp(); }
            return true;
        }
        if (epoch != s.epoch) return false;
    }
    if (kind == "description") {
        const auto text = message.value("sdp");
        if (s.descriptionSeen || !text.isString() || text.toString().size() > 32768) return false;
        s.descriptionSeen = true;
        if (!s.connection && !s.failed) start();
        if (!s.connection || s.failed) return true;
        try {
            // Use the authenticated TCP endpoint for the UDP route, including
            // forwarded external ports. No public STUN service or private-IP probing.
            QStringList lines;
            for (const auto& line : text.toString().split('\n'))
                if (!line.startsWith("a=candidate:") && !line.startsWith("a=end-of-candidates")) lines.append(line);
            rtc::Description description(lines.join('\n').toStdString(), s.offerer ? "answer" : "offer");
            if (!description.fingerprint() || description.mediaCount() != 1 || (s.medium == Medium::Audio ? !description.hasAudioOrVideo() : !description.hasApplication())) return false;
            s.connection->setRemoteDescription(description);
            if (!s.offerer) {
                s.connection->setLocalDescription(rtc::Description::Type::Answer);
                const auto candidate = QString("candidate:1 1 UDP 2130706431 %1 %2 typ host")
                    .arg(s.remoteAddress.toString()).arg(s.remotePort);
                s.connection->addRemoteCandidate(rtc::Candidate(candidate.toStdString(), description.bundleMid()));
            }
            return true;
        } catch (const std::exception& error) {
            qCWarning(mediaTransportLog) << "UDP negotiation failed; retaining TLS:" << error.what(); stopUdp(); return true;
        }
    }
    if (s.medium != Medium::Audio) return false;
    const auto number = message.value("ssrc").toInteger();
    if (number < 1 || number > UINT32_MAX || message.value("ssrc").toDouble() != double(number)) return false;
    const auto ssrc = quint32(number);
    if (kind == "source" || kind == "sourceAck") {
        const auto start = message.value("sequence").toInt(-1);
        if (start < 0 || start > UINT16_MAX || message.value("sequence").toDouble() != start) return false;
        if (kind == "sourceAck") {
            auto found = s.outgoing.find(ssrc);
            if (found != s.outgoing.end() && !found->source.isEmpty() && found->start == start) found->confirmed = true;
            return true;
        }
        const auto id = message.value("id").toString();
        const auto found = s.incoming.constFind(ssrc);
        if (!sourceId(id) || (found != s.incoming.cend() && !found->source.isEmpty())
            || (found == s.incoming.cend() && s.incoming.size() >= maximumSources)) return false;
        for (const auto& stream : s.incoming) if (stream.source == id) return false;
        auto& stream = s.incoming[ssrc]; stream.source = id;
        stream.highest = (stream.highest < 0 ? start : stream.highest + qint16(start - quint16(stream.highest))) - 1;
        stream.delivered = stream.highest;
        stream.pending.clear();
        emit signaling({{"kind", "sourceAck"}, {"ssrc", double(ssrc)}, {"sequence", start}});
        return true;
    }
    if (kind != "frame" || !s.incoming.contains(ssrc)) return false;
    const auto seq = message.value("sequence").toInt(-1);
    const auto data = QByteArray::fromBase64Encoding(message.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (seq < 0 || seq > UINT16_MAX || message.value("sequence").toDouble() != seq || !data || !validOpus(data.decoded)) return false;
    accept(ssrc, quint16(seq), data.decoded, false);
    return true;
}

bool MediaTransport::send(const QString& source, const QByteArray& opus, int missing) {
    if (state_->medium != Medium::Audio || !sourceId(source) || !validOpus(opus) || missing < 0 || missing > 6) return false;
    const auto keepAlive = state_;
    auto& s = *keepAlive;
    auto found = std::find_if(s.outgoing.begin(), s.outgoing.end(), [&](const auto& entry) { return entry.source == source; });
    if (found == s.outgoing.end()) {
        found = std::find_if(s.outgoing.begin(), s.outgoing.end(), [](const auto& entry) { return entry.source.isEmpty(); });
        if (found == s.outgoing.end()) {
            if (s.outgoing.size() >= maximumSources) return false;
            quint32 ssrc;
            do { ssrc = QRandomGenerator::global()->generate(); } while (!ssrc || s.outgoing.contains(ssrc));
            State::Stream stream;
            stream.sequence = quint16(QRandomGenerator::global()->generate()); stream.timestamp = QRandomGenerator::global()->generate();
            found = s.outgoing.insert(ssrc, stream);
        }
        // Reuse bounded RTP slots without resetting their SRTP sequence. New
        // bindings fence off old packets, so previous speakers cannot reappear.
        found->source = source; found->start = quint16(found->sequence + missing + 1);
        const auto ssrc = found.key();
        emit signaling({{"kind", "source"}, {"ssrc", double(ssrc)}, {"id", source}, {"sequence", found->start}});
        if (!s.owner) return false;
        found = s.outgoing.find(ssrc);
        if (found == s.outgoing.end() || found->source != source) return false;
    }
    auto& stream = *found;
    stream.sequence = quint16(stream.sequence + missing + 1); stream.timestamp += quint32(missing + 1) * 960;
    if (stream.confirmed && udpActive() && opus.size() + 12 <= maximumDatagram) {
        QByteArray bytes(12, '\0'); bytes[0] = char(0x80); bytes[1] = 111;
        qToBigEndian(stream.sequence, bytes.data() + 2); qToBigEndian(stream.timestamp, bytes.data() + 4);
        qToBigEndian(found.key(), bytes.data() + 8); bytes.append(opus);
        if (s.write(bytes)) return true;
    }
    emit signaling({{"kind", "frame"}, {"ssrc", double(found.key())}, {"sequence", stream.sequence}, {"data", QString::fromLatin1(opus.toBase64())}});
    return true;
}

bool MediaTransport::sendDatagram(const QByteArray& bytes) {
    return state_->medium == Medium::Data && !bytes.isEmpty() && bytes.size() <= 16384
        && udpActive() && state_->write(bytes);
}

void MediaTransport::retainSources(const QStringList& sources) {
    auto& s = *state_;
    for (auto* streams : {&s.incoming, &s.outgoing})
        for (auto& stream : *streams) if (!sources.contains(stream.source)) {
            stream.source.clear(); stream.confirmed = false; stream.pending.clear();
        }
}

void MediaTransport::accept(quint32 ssrc, quint16 sequence, const QByteArray& opus, bool datagram) {
    auto& s = *state_;
    auto it = s.incoming.find(ssrc);
    if (it == s.incoming.end() || it->source.isEmpty()) return;
    auto& stream = *it;
    const qint64 serial = stream.highest < 0 ? sequence : stream.highest + qint16(sequence - quint16(stream.highest));
    if (serial <= stream.delivered || stream.pending.contains(serial) || (stream.highest >= 0 && serial < stream.highest - 6)) return;
    if (datagram) { ++s.received; s.expected += quint32(stream.highest < 0 ? 1 : std::max<qint64>(0, serial - stream.highest)); }
    stream.highest = std::max(stream.highest, serial);
    if (stream.pending.size() == 6) stream.pending.erase(stream.pending.begin());
    stream.pending.emplace(serial, State::Stream::Frame{opus, s.clock.elapsed() + (datagram ? 20 : 0)});
    if (!timer_.isActive() || timer_.interval() != 5) timer_.start(5);
}

void MediaTransport::packet(const QByteArray& bytes) {
    const auto keepAlive = state_;
    auto& s = *keepAlive;
    if (bytes.size() == 24 && quint8(bytes[0]) == 0x80 && quint8(bytes[1]) == 204
        && qFromBigEndian<quint16>(bytes.constData() + 2) == 5 && bytes.mid(8, 4) == "SQSP") {
        const auto serial = qFromBigEndian<quint32>(bytes.constData() + 12);
        if (quint8(bytes[7]) == 0) {
            auto answer = bytes; answer[7] = 1;
            qToBigEndian(s.received, answer.data() + 16); qToBigEndian(s.expected, answer.data() + 20);
            s.received = s.expected = 0; s.write(answer);
        } else if (quint8(bytes[7]) == 1 && serial == s.probe && s.probeAt >= 0) {
            const auto now = s.clock.elapsed(), rtt = now - s.probeAt;
            const bool recovered = now - s.acknowledged >= 300;
            s.acknowledged = now; s.probeAt = -1;
            s.reportedReceived += qFromBigEndian<quint32>(bytes.constData() + 16);
            s.reportedExpected += qFromBigEndian<quint32>(bytes.constData() + 20);
            if (now - s.qualityAt >= 750) {
                const auto condition = rtt > 250 || (s.reportedExpected > 5 && s.reportedReceived < s.reportedExpected * 0.95) ? -1 : rtt < 120 ? 1 : 0;
                s.reportedReceived = s.reportedExpected = 0; s.qualityAt = now;
                qCDebug(mediaTransportLog) << "UDP probe rtt_ms" << rtt << "condition" << condition;
                emit quality(condition); if (!s.owner) return;
            }
            if (recovered && s.medium == Medium::Data) emit writable();
        }
        return;
    }
    if (s.medium == Medium::Data) { emit datagramReceived(bytes); return; }
    if (bytes.size() < 13 || quint8(bytes[0]) != 0x80 || (quint8(bytes[1]) & 0x7f) != 111) return;
    const auto opus = bytes.mid(12); if (!validOpus(opus)) return;
    accept(qFromBigEndian<quint32>(bytes.constData() + 8), qFromBigEndian<quint16>(bytes.constData() + 2), opus, true);
}

void MediaTransport::tick() {
    const auto keepAlive = state_;
    auto& s = *keepAlive;
    const auto now = s.clock.elapsed();
    if (s.offerer && now - s.attempted >= 30000 && !udpActive()
        && (s.failed || !s.channel || !s.channel->isOpen() || now - s.acknowledged > 5000)) {
        if (s.epoch == UINT32_MAX) return;
        ++s.epoch;
        emit signaling({{"kind", "restart"}, {"epoch", double(s.epoch)}});
        if (!s.owner) return;
        restartUdp(); return;
    }
    struct Ready { quint32 ssrc; QString source; QByteArray bytes; int missing; };
    QList<Ready> ready;
    bool pending = false;
    for (auto it = s.incoming.begin(); it != s.incoming.end(); ++it) {
        auto& stream = *it;
        while (!stream.pending.empty() && stream.pending.begin()->second.due <= now) {
            auto node = stream.pending.extract(stream.pending.begin());
            const auto missing = stream.delivered < 0 ? 0 : int(std::min<qint64>(6, node.key() - stream.delivered - 1));
            stream.delivered = node.key();
            ready.append({it.key(), stream.source, std::move(node.mapped().bytes), missing});
        }
        pending |= !stream.pending.empty();
    }
    timer_.setInterval(pending ? 5 : 100);
    for (const auto& frame : ready) {
        if (s.incoming.value(frame.ssrc).source == frame.source) emit audio(frame.source, frame.bytes, frame.missing);
        if (!s.owner) return;
    }
    if (s.failed || !s.channel || !s.channel->isOpen()) {
        if (!s.offerer && s.failed && std::all_of(s.incoming.begin(), s.incoming.end(), [](const auto& stream) { return stream.pending.empty(); })) timer_.stop();
        return;
    }
    if (s.probeAt >= 0 && now - s.probeAt < 300) return;
    if (now - s.probeSent < 100) return;
    if (now - s.acknowledged >= 300 && now - s.qualityAt >= 750) { s.qualityAt = now; emit quality(-1); if (!s.owner) return; }
    s.probeAt = s.probeSent = now;
    QByteArray probe(24, '\0'); probe[0] = char(0x80); probe[1] = char(204);
    qToBigEndian<quint16>(5, probe.data() + 2); probe.replace(8, 4, "SQSP");
    qToBigEndian(++s.probe, probe.data() + 12); s.write(probe);
}
}
