#include "media_transport.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <rtc/rtc.hpp>
#include <array>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <opus.h>

namespace squad {
namespace {
Q_LOGGING_CATEGORY(mediaTransportLog, "squadspeak.media.transport", QtWarningMsg)
Q_LOGGING_CATEGORY(mediaSenderLog, "squadspeak.media.sender", QtWarningMsg)
// Bound authenticated source metadata separately from queued audio bytes.
// A 2 MiB roster cannot contain more distinct 64-byte device IDs.
constexpr int maximumSources = 2 * 1024 * 1024 / 64;
constexpr qsizetype maximumPendingAudio = 512 * 1024;
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
    struct Sender;
    static Sender& sender();
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
    qsizetype queuedBytes = 0, pendingBytes = 0;
    size_t mailboxDrops = 0, lateFrames = 0, pendingDrops = 0;

    State(MediaTransport* value, bool offer, quint16 port, QHostAddress address, quint16 remote, Medium mode)
        : owner(value), offerer(offer), medium(mode), localPort(port), remotePort(remote), remoteAddress(std::move(address)) { clock.start(); }

    // Hold the mutex through invokeMethod: destruction invalidates owner under
    // this same lock. Queued lambdas cannot outlive the QObject receiver.
    static void post(const std::weak_ptr<State>& weak, quint64 generation, qsizetype size, std::function<void(State&)> action) {
        const auto state = weak.lock();
        if (!state) return;
        std::lock_guard lock(state->mutex);
        if (!state->owner) return;
        if (size > 65536 || state->mailbox.size() >= 256 || state->queuedBytes + size > 512 * 1024) {
            ++state->mailboxDrops; return;
        }
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
    bool write(const std::shared_ptr<State>& self, const QByteArray& bytes);
};

// Track::send encrypts and writes synchronously. Normally one worker drains the
// shared socket. A second can serve other connections if a send stalls, without
// making every short datagram contend on the socket. QObject state stays owned
// by its original thread, with at most one send per connection in flight.
struct MediaTransport::State::Sender final {
    using Clock = std::chrono::steady_clock;
    static constexpr auto backupDelay = std::chrono::milliseconds(5);
    struct Packet {
        std::shared_ptr<rtc::Channel> channel;
        QByteArray bytes;
        quint32 ssrc;
        Clock::time_point expires;
        std::function<void(bool)> failed;
    };
    std::mutex mutex;
    std::condition_variable changed, backupChanged;
    std::map<rtc::Channel*, std::deque<Packet>> queues;
    decltype(queues)::iterator turn = queues.end();
    size_t queued = 0;
    size_t processedCount = 0, expiredCount = 0, shedCount = 0, failureCount = 0, maximumQueued = 0;
    Clock::time_point reportAt = Clock::now(), overloadedUntil{};
    long long sendMicros = 0, maximumSendMicros = 0;
    std::map<std::pair<quintptr, quint32>, int> counts;
    std::array<rtc::Channel*, 2> active{};
    Clock::time_point primaryStarted{};
    bool backupWaiting = false;
    bool stopping = false;
    std::array<std::thread, 2> workers;
    Sender() {
        try {
            for (size_t i = 0; i < workers.size(); ++i)
                workers[i] = std::thread([this, i] { run(i); });
        } catch (...) {
            stop();
            throw;
        }
    }
    bool busy(rtc::Channel* channel) const {
        return std::find(active.begin(), active.end(), channel) != active.end();
    }
    auto ready() {
        if (queues.empty()) return queues.end();
        auto current = turn;
        do {
            if (!busy(current->first)) return current;
            if (++current == queues.end()) current = queues.begin();
        } while (current != turn);
        return queues.end();
    }
    bool samplePressure() {
        const auto now = Clock::now();
        int delayed = 0;
        for (const auto& [channel, queue] : queues) {
            if (!queue.empty() && queue.front().expires <= now && ++delayed == 2) break;
        }
        // One stalled destination is isolated by the second sender. Admission
        // pauses only when work for multiple destinations misses its deadline.
        if (delayed >= 2) overloadedUntil = now + std::chrono::seconds(5);
        return now < overloadedUntil;
    }
    bool overloaded() {
        std::lock_guard lock(mutex);
        return samplePressure();
    }
    void run(size_t worker) {
        // Speech expires after 120 ms. Mark this worker as latency-sensitive
        // even when the hosting app has no foreground window.
        QThread::currentThread()->setServiceLevel(QThread::QualityOfService::High);
        std::unique_lock lock(mutex);
        for (;;) {
            if (worker != 0) backupWaiting = true;
            auto& available = worker == 0 ? changed : backupChanged;
            available.wait(lock, [this, worker] {
                return stopping || (ready() != queues.end() && (worker == 0 || active[0]));
            });
            if (worker != 0) backupWaiting = false;
            if (stopping) return;
            if (worker != 0) {
                while (active[0] && ready() != queues.end() && Clock::now() < primaryStarted + backupDelay) {
                    // A healthy primary may finish thousands of sends while
                    // this deadline is pending. Only shutdown needs to wake it.
                    backupChanged.wait_until(lock, primaryStarted + backupDelay, [this] { return stopping; });
                    if (stopping) return;
                }
                if (!active[0] || ready() == queues.end()) continue;
            }
            auto current = ready();
            turn = std::next(current);
            auto& queue = current->second;
            if (queue.front().expires <= Clock::now()) samplePressure();
            uncount(queue.front());
            auto packet = std::move(queue.front()); queue.pop_front(); --queued;
            removeEmpty(current);
            active[worker] = packet.channel.get();
            if (worker == 0) {
                primaryStarted = Clock::now();
                if (backupWaiting) backupChanged.notify_one();
            }
            lock.unlock();
            const auto sendStarted = Clock::now();
            const bool expired = sendStarted >= packet.expires;
            bool failure = false;
            if (!expired) {
                bool sent = false, fatal = false;
                try {
                    sent = packet.channel->send(reinterpret_cast<const rtc::byte*>(packet.bytes.constData()), size_t(packet.bytes.size()));
                } catch (const std::exception& error) { qCDebug(mediaTransportLog) << error.what(); fatal = true; }
                if (!sent) { failure = true; packet.failed(fatal); }
            }
            lock.lock();
            active[worker] = nullptr;
            if (mediaSenderLog().isDebugEnabled()) {
                ++processedCount;
                expiredCount += expired;
                failureCount += failure;
                const auto now = Clock::now();
                const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now - sendStarted).count();
                sendMicros += micros; maximumSendMicros = std::max<long long>(maximumSendMicros, micros);
                if (now - reportAt >= std::chrono::seconds(1)) {
                    qCDebug(mediaSenderLog) << "RTP dequeued" << processedCount << "expired" << expiredCount << "shed" << shedCount
                            << "failed" << failureCount << "queue max" << maximumQueued << "send us mean/max"
                            << sendMicros / std::max<size_t>(1, processedCount) << maximumSendMicros;
                    processedCount = expiredCount = shedCount = failureCount = maximumQueued = 0;
                    sendMicros = maximumSendMicros = 0; reportAt = now;
                }
            }
            changed.notify_all();
            if (backupWaiting) backupChanged.notify_one();
        }
    }

    ~Sender() { stop(); }
    void removeEmpty(decltype(queues)::iterator current) {
        if (current->second.empty()) {
            if (turn == current) ++turn;
            queues.erase(current);
        }
        if (turn == queues.end()) turn = queues.begin();
    }
    void uncount(const Packet& packet) {
        const auto it = counts.find({quintptr(packet.channel.get()), packet.ssrc});
        if (--it->second == 0) counts.erase(it);
    }
    void stop() {
        { std::lock_guard lock(mutex); stopping = true; queues.clear(); turn = queues.end(); queued = 0; counts.clear(); }
        changed.notify_all();
        backupChanged.notify_one();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    }
    void cancel(const std::shared_ptr<rtc::Channel>& channel, std::optional<quint32> ssrc = {}) {
        std::unique_lock lock(mutex);
        const auto current = queues.find(channel.get());
        if (current != queues.end()) {
            std::erase_if(current->second, [&](const Packet& packet) {
                if (ssrc && packet.ssrc != *ssrc) return false;
                uncount(packet); --queued; return true;
            });
            removeEmpty(current);
        }
        // At most one datagram per connection is in flight. Never close its native
        // track/connection concurrently with encryption or socket access.
        if (!ssrc) changed.wait(lock, [&] { return !busy(channel.get()); });
    }
    bool enqueue(const std::shared_ptr<rtc::Channel>& channel, QByteArray bytes, std::function<void(bool)> failed) {
        const auto ssrc = quint8(bytes[1]) == 111 ? qFromBigEndian<quint32>(bytes.constData() + 8) : 0;
        std::lock_guard lock(mutex);
        if (stopping) return false;
        const auto key = std::pair{quintptr(channel.get()), ssrc};
        const auto count = counts.find(key);
        if (count != counts.end() && count->second == 6) {
            samplePressure();
            auto& queue = queues.at(channel.get());
            const auto oldest = std::find_if(queue.begin(), queue.end(), [&](const Packet& packet) {
                return packet.ssrc == ssrc;
            });
            uncount(*oldest); queue.erase(oldest); --queued; ++shedCount;
        }
        // Six 20 ms frames per source, at most 4096 small RTP packets globally.
        // Shed old speech under overload instead of growing latency or TLS work.
        if (queued == 4096) {
            const auto oldest = std::min_element(queues.begin(), queues.end(), [](const auto& a, const auto& b) {
                return a.second.front().expires < b.second.front().expires;
            });
            auto& queue = oldest->second;
            uncount(queue.front()); queue.pop_front(); --queued; ++shedCount;
            removeEmpty(oldest);
        }
        const auto [current, added] = queues.try_emplace(channel.get());
        if (turn == queues.end()) turn = current;
        ++counts[key]; ++queued; maximumQueued = std::max(maximumQueued, queued);
        current->second.push_back({channel, std::move(bytes), ssrc, Clock::now() + std::chrono::milliseconds(120), std::move(failed)});
        if (added) {
            changed.notify_all();
            if (backupWaiting) backupChanged.notify_one();
        }
        return true;
    }
};

MediaTransport::State::Sender& MediaTransport::State::sender() {
    static Sender instance;
    return instance;
}

bool MediaTransport::State::write(const std::shared_ptr<State>& self, const QByteArray& bytes) {
    if (!channel || !channel->isOpen() || (medium == Medium::Data && channel->bufferedAmount() > 32768)) return false;
    if (medium == Medium::Audio) {
        const auto ssrc = quint8(bytes[1]) == 111 ? qFromBigEndian<quint32>(bytes.constData() + 8) : 0;
        const auto stream = outgoing.value(ssrc);
        return sender().enqueue(channel, bytes, [weak = std::weak_ptr(self), epoch = generation, bytes, ssrc,
                                                source = stream.source, start = stream.start](bool fatal) {
            State::post(weak, epoch, bytes.size(), [bytes, ssrc, source, start, fatal](State& s) {
                s.failed |= fatal;
                const auto stream = s.outgoing.constFind(ssrc);
                if (ssrc && stream != s.outgoing.cend() && stream->source == source && stream->start == start)
                    emit s.owner->signaling({{"kind", "frame"}, {"ssrc", double(ssrc)},
                        {"sequence", qFromBigEndian<quint16>(bytes.constData() + 2)},
                        {"data", QString::fromLatin1(bytes.mid(12).toBase64())}});
            });
        });
    }
    try {
        channel->send(reinterpret_cast<const rtc::byte*>(bytes.constData()), size_t(bytes.size()));
        return true; // Data-channel false means queued, not rejected.
    } catch (const std::exception& error) { qCDebug(mediaTransportLog) << error.what(); failed = true; return false; }
}

MediaTransport::MediaTransport(bool offerer, quint16 localPort, QHostAddress address, quint16 remotePort, Medium medium, QObject* parent)
    : QObject(parent), state_(std::make_shared<State>(this, offerer, localPort, address, remotePort, medium)) {
    static const bool runtime = [] {
        rtc::SetThreadPoolSize(2);
        rtc::SctpSettings sctp; sctp.recvBufferSize = 262144; sctp.sendBufferSize = 262144;
        rtc::SetSctpSettings(sctp);
        State::sender();
        qAddPostRoutine([] { State::sender().stop(); rtc::Cleanup().wait(); });
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
    { std::lock_guard lock(s->mutex);
        if (s->mailboxDrops || s->lateFrames || s->pendingDrops)
            qCDebug(mediaSenderLog) << "RTP receive drops; mailbox/late/playout" << s->mailboxDrops << s->lateFrames << s->pendingDrops;
        s->owner = nullptr; s->mailbox.clear(); s->queuedBytes = 0;
    }
    timer_.stop();
    stopUdp();
}

void MediaTransport::stopUdp() {
    auto& s = *state_;
    ++s.generation;
    s.failed = true; s.acknowledged = -10000;
    if (s.channel) {
        if (s.medium == Medium::Audio) State::sender().cancel(s.channel);
        s.channel->resetCallbacks(); s.channel->close(); s.channel.reset();
    }
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
            const auto state = weak.lock();
            if (!state) return;
            const auto arrived = state->clock.elapsed();
            QByteArray packet(reinterpret_cast<const char*>(bytes->data()), qsizetype(bytes->size()));
            const auto size = packet.size();
            State::post(weak, generation, size, [packet = std::move(packet), arrived](State& s) { s.owner->packet(packet, arrived); });
        });
        if (s.offerer) s.connection->setLocalDescription(rtc::Description::Type::Offer);
        timer_.start();
    } catch (const std::exception& error) {
        qCWarning(mediaTransportLog) << "UDP initialization failed; retaining TLS:" << error.what(); stopUdp();
    }
}

void MediaTransport::restartUdp() {
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
        for (const auto& [serial, frame] : stream.pending) s.pendingBytes -= frame.bytes.size();
        stream.pending.clear();
        emit signaling({{"kind", "sourceAck"}, {"ssrc", double(ssrc)}, {"sequence", start}});
        return true;
    }
    if (kind != "frame" || !s.incoming.contains(ssrc)) return false;
    const auto seq = message.value("sequence").toInt(-1);
    const auto data = QByteArray::fromBase64Encoding(message.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (seq < 0 || seq > UINT16_MAX || message.value("sequence").toDouble() != seq || !data || !validOpus(data.decoded)) return false;
    accept(ssrc, quint16(seq), data.decoded, false, s.clock.elapsed());
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
        if (s.write(keepAlive, bytes)) return true;
    }
    emit signaling({{"kind", "frame"}, {"ssrc", double(found.key())}, {"sequence", stream.sequence}, {"data", QString::fromLatin1(opus.toBase64())}});
    return true;
}

bool MediaTransport::overloaded() { return State::sender().overloaded(); }

bool MediaTransport::sendDatagram(const QByteArray& bytes) {
    return state_->medium == Medium::Data && !bytes.isEmpty() && bytes.size() <= 16384
        && udpActive() && state_->write(state_, bytes);
}

void MediaTransport::retainSources(const QStringList& sources) {
    auto& s = *state_;
    if (s.medium == Medium::Audio && s.channel)
        for (auto it = s.outgoing.cbegin(); it != s.outgoing.cend(); ++it)
            if (!sources.contains(it->source)) State::sender().cancel(s.channel, it.key());
    for (auto* streams : {&s.incoming, &s.outgoing})
        for (auto& stream : *streams) if (!sources.contains(stream.source)) {
            for (const auto& [serial, frame] : stream.pending) s.pendingBytes -= frame.bytes.size();
            stream.source.clear(); stream.confirmed = false; stream.pending.clear();
        }
}

void MediaTransport::accept(quint32 ssrc, quint16 sequence, const QByteArray& opus, bool datagram, qint64 arrived) {
    auto& s = *state_;
    auto it = s.incoming.find(ssrc);
    if (it == s.incoming.end() || it->source.isEmpty()) return;
    // A full reorder window advances past a missing packet. Preserve the
    // received speech instead of discarding it while waiting for that gap.
    if (it->pending.size() == 6) {
        const auto source = it->source;
        tick();
        if (!s.owner) return;
        it = s.incoming.find(ssrc);
        if (it == s.incoming.end() || it->source != source) return;
    }
    auto& stream = *it;
    const qint64 serial = stream.highest < 0 ? sequence : stream.highest + qint16(sequence - quint16(stream.highest));
    if (serial <= stream.delivered || stream.pending.contains(serial) || (stream.highest >= 0 && serial < stream.highest - 6)) {
        ++s.lateFrames; return;
    }
    if (datagram) { ++s.received; s.expected += quint32(stream.highest < 0 ? 1 : std::max<qint64>(0, serial - stream.highest)); }
    stream.highest = std::max(stream.highest, serial);
    if (stream.pending.size() == 6) {
        s.pendingBytes -= stream.pending.begin()->second.bytes.size();
        stream.pending.erase(stream.pending.begin()); ++s.pendingDrops;
    }
    if (s.pendingBytes + opus.size() > maximumPendingAudio) { ++s.pendingDrops; return; }
    s.pendingBytes += opus.size();
    // Owner-thread stalls must not restart the network reorder interval.
    stream.pending.emplace(serial, State::Stream::Frame{opus, arrived + (datagram ? 20 : 0)});
    if (!timer_.isActive() || timer_.interval() != 5) timer_.start(5);
}

void MediaTransport::packet(const QByteArray& bytes, qint64 arrived) {
    const auto keepAlive = state_;
    auto& s = *keepAlive;
    if (bytes.size() == 24 && quint8(bytes[0]) == 0x80 && quint8(bytes[1]) == 204
        && qFromBigEndian<quint16>(bytes.constData() + 2) == 5 && bytes.mid(8, 4) == "SQSP") {
        const auto serial = qFromBigEndian<quint32>(bytes.constData() + 12);
        if (quint8(bytes[7]) == 0) {
            auto answer = bytes; answer[7] = 1;
            qToBigEndian(s.received, answer.data() + 16); qToBigEndian(s.expected, answer.data() + 20);
            s.received = s.expected = 0; s.write(keepAlive, answer);
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
    accept(qFromBigEndian<quint32>(bytes.constData() + 8), qFromBigEndian<quint16>(bytes.constData() + 2), opus, true, arrived);
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
        while (!stream.pending.empty() && (stream.pending.size() == 6
                || stream.pending.begin()->first == stream.delivered + 1
                || stream.pending.begin()->second.due <= now)) {
            auto node = stream.pending.extract(stream.pending.begin());
            s.pendingBytes -= node.mapped().bytes.size();
            const auto missing = stream.delivered < 0 ? 0 : int(std::min<qint64>(6, node.key() - stream.delivered - 1));
            stream.delivered = node.key();
            ready.append({it.key(), stream.source, std::move(node.mapped().bytes), missing});
        }
        pending |= !stream.pending.empty();
    }
    timer_.setInterval(pending ? 5 : 100);
    for (const auto& frame : ready) {
        const auto stream = s.incoming.constFind(frame.ssrc);
        if (stream != s.incoming.cend() && stream->source == frame.source)
            emit audio(frame.source, frame.bytes, frame.missing);
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
    qToBigEndian(++s.probe, probe.data() + 12); s.write(keepAlive, probe);
}
}
