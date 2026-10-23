#include "media_transport.hpp"
#include "local_channel.hpp"
#include <QTemporaryDir>
#include "event_wait.hpp"
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QTest>
#include <QTimer>
#include <QSemaphore>
#include <QThread>
#include <QScopeGuard>
#include <QtEndian>
#include <rtc/global.hpp>
#include <plog/Converters/UTF8Converter.h>
#include <plog/Logger.h>
#include <opus.h>
#include <array>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
quint16 availableUdpPort() {
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::Any, 0, QAbstractSocket::DontShareAddress)) {
        qWarning() << "Unable to reserve UDP test port:" << socket.errorString();
        return 0;
    }
    const auto port = socket.localPort();
    socket.close();
    return port;
}

class DatagramLink final : public QObject {
    QUdpSocket socket_;
    quint16 target_, client_ = 0;
public:
    bool blocked = false, corrupt = false;
    int audioPackets = 0, dropEvery = 0;
    bool bindingAnswered = false;
    QByteArray lastAudio, lastBinding;
    explicit DatagramLink(quint16 target) : target_(target) {
        if (!socket_.bind(QHostAddress::LocalHost)) throw std::runtime_error("UDP fixture cannot bind");
        connect(&socket_, &QUdpSocket::readyRead, this, [this] {
            while (socket_.hasPendingDatagrams()) {
                const auto datagram = socket_.receiveDatagram();
                const bool downstream = datagram.senderPort() == target_;
                if (!downstream) client_ = quint16(datagram.senderPort());
                if (blocked) continue;
                auto bytes = datagram.data();
                if (downstream && bytes.size() >= 20 && bytes.first(2) == QByteArray::fromHex("0101"))
                    bindingAnswered = true;
                if (!downstream && bytes.size() >= 20 && bytes.first(2) == QByteArray::fromHex("0001"))
                    lastBinding = bytes;
                const bool audio = bytes.size() > 12 && (quint8(bytes[0]) >> 6) == 2 && (quint8(bytes[1]) & 127) == 111;
                if (audio && downstream) {
                    ++audioPackets; lastAudio = bytes;
                    if (dropEvery && audioPackets % dropEvery == 0) continue;
                    if (corrupt) bytes[bytes.size() - 1] = char(quint8(bytes.back()) ^ 1);
                }
                socket_.writeDatagram(bytes, QHostAddress::LocalHost, downstream ? client_ : target_);
            }
        });
    }
    quint16 port() const { return socket_.localPort(); }
    void setTarget(quint16 target) { target_ = target; }
    qint64 sendToHost(const QByteArray& bytes) { return socket_.writeDatagram(bytes, QHostAddress::LocalHost, target_); }
    void replay() { socket_.writeDatagram(lastAudio, QHostAddress::LocalHost, client_); }
};
const QString first(64, 'a'), second(64, 'b');
QByteArray speech() {
    int error = 0;
    const auto encoder = std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>(
        opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error), opus_encoder_destroy);
    if (!encoder || error) return {};
    std::array<float, 960> samples{};
    for (size_t n = 0; n < samples.size(); ++n) samples[n] = float(std::sin(double(n) * 0.031) * 0.3);
    QByteArray packet(4000, '\0');
    const auto count = opus_encode_float(encoder.get(), samples.data(), 960,
        reinterpret_cast<unsigned char*>(packet.data()), int(packet.size()));
    return count > 0 ? packet.first(count) : QByteArray{};
}

// Observe and stall native transport boundaries without replacing the transport.
class SendGate final {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, released = false, onOwner = false;
        int remaining = 1;
        std::vector<int> packetSizes;
        size_t received = 0;
        std::string boundary = "Protected SRTP packet, size=";
        const std::thread::id owner = std::this_thread::get_id();
    };
    struct Observer final : plog::IAppender {
        std::shared_ptr<State> state;
        void write(const plog::Record& record) override {
            const auto s = std::atomic_load(&state);
            if (!s) return;
            const auto message = plog::UTF8Converter::convert(record.getMessage());
            if (message.find("Unprotected SRTP packet, size=") != std::string::npos) {
                std::lock_guard lock(s->mutex);
                ++s->received; s->changed.notify_all(); return;
            }
            if (message.find(s->boundary) == std::string::npos) return;
            std::unique_lock lock(s->mutex);
            const auto size = message.find("size=");
            s->packetSizes.push_back(size == std::string::npos ? 0 : std::stoi(message.substr(size + 5)));
            s->changed.notify_all();
            if (s->remaining == 0) return;
            --s->remaining;
            s->entered = true;
            s->onOwner = std::this_thread::get_id() == s->owner;
            if (!s->onOwner) s->changed.wait(lock, [&] { return s->released; });
        }
    };
    static Observer& observer() {
        // plog retains appenders for the process lifetime. Install before any
        // transports start. Its callback must not serialize unrelated senders.
        static auto* value = [] {
            auto* result = new Observer;
            plog::get()->addAppender(result);
            return result;
        }();
        return *value;
    }
    const std::shared_ptr<State> state_ = std::make_shared<State>();
public:
    static void initialize() { rtc::InitLogger(rtc::LogLevel::Info); observer(); }
    explicit SendGate(std::string boundary = "Protected SRTP packet, size=", int blockedSenders = 1) {
        state_->remaining = blockedSenders;
        state_->boundary = std::move(boundary);
        std::atomic_store(&observer().state, state_);
        rtc::InitLogger(rtc::LogLevel::Verbose);
    }
    ~SendGate() { release(); std::atomic_store(&observer().state, std::shared_ptr<State>{}); rtc::InitLogger(rtc::LogLevel::Info); }
    bool entered() const { std::lock_guard lock(state_->mutex); return state_->entered; }
    std::vector<int> packetSizes() const { std::lock_guard lock(state_->mutex); return state_->packetSizes; }
    bool waitForPackets(size_t count) {
        std::unique_lock lock(state_->mutex);
        return state_->changed.wait_for(lock, std::chrono::seconds(1), [&] { return state_->packetSizes.size() >= count; });
    }
    bool waitForReceived(size_t count) {
        std::unique_lock lock(state_->mutex);
        return state_->changed.wait_for(lock, std::chrono::seconds(1), [&] { return state_->received >= count; });
    }
    bool onOwner() const { std::lock_guard lock(state_->mutex); return state_->onOwner; }
    void release() {
        { std::lock_guard lock(state_->mutex); state_->released = true; }
        state_->changed.notify_all();
    }
};
}
class MediaTransportTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        SendGate::initialize();
    }
    void dataDatagramsCarryBinaryPayloads_data() {
        QTest::addColumn<QHostAddress>("address");
        QTest::newRow("ipv4") << QHostAddress(QHostAddress::LocalHost);
        QTest::newRow("ipv6") << QHostAddress(QHostAddress::LocalHostIPv6);
    }
    void dataDatagramsCarryBinaryPayloads() {
        QFETCH(QHostAddress, address);
        const auto port = availableUdpPort(); QVERIFY(port);
        squad::MediaTransport host(true, port, address, 0, squad::MediaTransport::Medium::Data);
        squad::MediaTransport client(false, 0, address, port, squad::MediaTransport::Medium::Data);
        bool valid = true;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) { valid &= client.receive(message); });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) { valid &= host.receive(message); });
        QSignalSpy atHost(&host, &squad::MediaTransport::datagramReceived), atClient(&client, &squad::MediaTransport::datagramReceived);
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        QByteArray payload(16384, '\0');
        for (qsizetype index = 0; index < payload.size(); ++index) payload[index] = char(index * 37);
        QVERIFY(host.sendDatagram(payload));
        const QByteArray reverse = QByteArray::fromHex("00ff01");
        QVERIFY(client.sendDatagram(reverse));
        QTRY_COMPARE(atClient.size(), 1);
        QTRY_COMPARE(atHost.size(), 1);
        QCOMPARE(atClient.first().first().toByteArray(), payload);
        QCOMPARE(atHost.first().first().toByteArray(), reverse);
        QVERIFY(!host.sendDatagram({}));
        QVERIFY(!host.sendDatagram(QByteArray(16385, 'x')));
        QVERIFY(valid);
    }

    void dataDatagramsRefuseDuringUdpBlackoutAndRecover() {
        DatagramLink link(0);
        const auto port = availableUdpPort(); QVERIFY(port); link.setTarget(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, link.port(), squad::MediaTransport::Medium::Data);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, link.port(), squad::MediaTransport::Medium::Data);
        bool valid = true;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) { valid &= client.receive(message); });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) { valid &= host.receive(message); });
        QSignalSpy atClient(&client, &squad::MediaTransport::datagramReceived);
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        link.blocked = true;
        QTRY_VERIFY_WITH_TIMEOUT(!host.udpActive(), 3000);
        QVERIFY(!host.sendDatagram(QByteArray("blocked", 7)));
        link.blocked = false;
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 5000);
        QVERIFY(host.sendDatagram(QByteArray("recovered", 9)));
        QTRY_COMPARE(atClient.size(), 1);
        QCOMPARE(atClient.first().first().toByteArray(), QByteArray("recovered", 9));
        QVERIFY(valid);
    }

    void reconnectReusesPortsBeforeOldNativeConnectionIsDestroyed() {
        using Transport = squad::MediaTransport;
        const auto port = availableUdpPort(); QVERIFY(port);
        quint16 clientPort;
        do { clientPort = availableUdpPort(); QVERIFY(clientPort); } while (clientPort == port);
        auto host = std::make_unique<Transport>(true, port, QHostAddress::LocalHost, 0, Transport::Medium::Data);
        auto client = std::make_unique<Transport>(false, clientPort, QHostAddress::LocalHost, port, Transport::Medium::Data);
        const auto connectPeers = [&] {
            connect(host.get(), &Transport::signaling, client.get(), [peer = client.get()](const auto& message) { QVERIFY(peer->receive(message)); });
            connect(client.get(), &Transport::signaling, host.get(), [peer = host.get()](const auto& message) { QVERIFY(peer->receive(message)); });
        };
        connectPeers();
        QTRY_VERIFY_WITH_TIMEOUT(host->udpActive() && client->udpActive(), 10000);
        SendGate teardown("Destroying ICE transport");
        host.reset();
        QVERIFY(teardown.waitForPackets(1));
        QVERIFY(!teardown.onOwner());
        client.reset();
        host = std::make_unique<Transport>(true, port, QHostAddress::LocalHost, 0, Transport::Medium::Data);
        client = std::make_unique<Transport>(false, clientPort, QHostAddress::LocalHost, port, Transport::Medium::Data);
        connectPeers();
        QTRY_VERIFY_WITH_TIMEOUT(host->udpActive() && client->udpActive(), 3000);
        QSignalSpy received(client.get(), &Transport::datagramReceived);
        QVERIFY(host->sendDatagram("reconnected"));
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received.first().first().toByteArray(), QByteArray("reconnected"));
    }

    void forgedBindingCannotRedirectExistingEndpoint() {
        using Transport = squad::MediaTransport;
        const auto port = availableUdpPort(); QVERIFY(port);
        DatagramLink firstLink(port), secondLink(port);
        Transport firstHost(true, port, QHostAddress::LocalHost, 0, Transport::Medium::Data);
        Transport firstClient(false, 0, QHostAddress::LocalHost, firstLink.port(), Transport::Medium::Data);
        Transport secondHost(true, port, QHostAddress::LocalHost, 0, Transport::Medium::Data);
        Transport secondClient(false, 0, QHostAddress::LocalHost, secondLink.port(), Transport::Medium::Data);
        for (auto pair : {std::pair{&firstHost, &firstClient}, std::pair{&secondHost, &secondClient}}) {
            connect(pair.first, &Transport::signaling, pair.second, [peer = pair.second](const auto& message) { QVERIFY(peer->receive(message)); });
            connect(pair.second, &Transport::signaling, pair.first, [peer = pair.first](const auto& message) { QVERIFY(peer->receive(message)); });
        }
        QVERIFY(waitForEvents([&] { return firstHost.udpActive() && firstClient.udpActive()
            && secondHost.udpActive() && secondClient.udpActive(); }, 10000));
        auto forged = secondLink.lastBinding;
        QVERIFY(forged.size() > 52);
        QCOMPARE(forged.sliced(forged.size() - 8, 4), QByteArray::fromHex("80280004"));
        forged.chop(8); // Fingerprints are optional. Leave invalid message integrity.
        QCOMPARE(forged.sliced(forged.size() - 24, 4), QByteArray::fromHex("00080014"));
        forged[forged.size() - 1] = char(quint8(forged.back()) ^ 1);
        qToBigEndian<quint16>(quint16(forged.size() - 20), forged.data() + 2);
        SendGate rejected("STUN integrity check failed"); rejected.release();
        QCOMPARE(firstLink.sendToHost(forged), forged.size());
        QVERIFY(waitForEvents([&] { return rejected.entered(); }, 1000));
        QSignalSpy firstReceived(&firstHost, &Transport::datagramReceived);
        QSignalSpy secondReceived(&secondHost, &Transport::datagramReceived);
        // Liveness expires independently for each peer. Service both relays
        // continuously and wait for the sender's current readiness before use.
        QVERIFY(waitForEvents([&] { return firstClient.udpActive(); }));
        QVERIFY(firstClient.sendDatagram("original peer"));
        QVERIFY(waitForEvents(&firstHost, &Transport::datagramReceived, [&] { return firstReceived.size() == 1; }));
        QCOMPARE(firstReceived.first().first().toByteArray(), QByteArray("original peer"));
        QCOMPARE(secondReceived.size(), 0);
        QVERIFY(waitForEvents([&] { return secondClient.udpActive(); }));
        QVERIFY(secondClient.sendDatagram("other peer"));
        QVERIFY(waitForEvents(&secondHost, &Transport::datagramReceived, [&] { return secondReceived.size() == 1; }));
        QCOMPARE(secondReceived.first().first().toByteArray(), QByteArray("other peer"));
        QCOMPARE(firstReceived.size(), 1);
    }

    void dataDatagramsReleaseQueuedMessagesWhenPeerCloses_data() {
        QTest::addColumn<bool>("reuseClientPort");
        QTest::newRow("ephemeral-client") << false;
        QTest::newRow("reused-client") << true;
    }
    void dataDatagramsReleaseQueuedMessagesWhenPeerCloses() {
        QFETCH(bool, reuseClientPort);
        const QByteArray payload(16384, 'x');
        const auto port = availableUdpPort(); QVERIFY(port);
        quint16 clientPort = 0;
        if (reuseClientPort) {
            do { clientPort = availableUdpPort(); QVERIFY(clientPort); } while (clientPort == port);
        }
        rtc::InitLogger(rtc::LogLevel::Debug);
        const auto restoreLogging = qScopeGuard([] { rtc::InitLogger(rtc::LogLevel::Info); });
        for (int repeat = 0; repeat < 20; ++repeat) {
            qInfo() << "Reconnect iteration" << repeat << "host/client ports" << port << clientPort;
            squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0, squad::MediaTransport::Medium::Data);
            auto client = std::make_unique<squad::MediaTransport>(false, clientPort, QHostAddress::LocalHost, port,
                squad::MediaTransport::Medium::Data);
            connect(&host, &squad::MediaTransport::signaling, client.get(), [&](const auto& message) { QVERIFY(client->receive(message)); });
            connect(client.get(), &squad::MediaTransport::signaling, &host, [&](const auto& message) { QVERIFY(host.receive(message)); });
            QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client->udpActive(), 10000);
            QVERIFY(host.sendDatagram(payload));
            client.reset();
            // The peer's SCTP shutdown may arrive during this send. Queued
            // messages must be released even when the library aborts the send.
            host.sendDatagram(payload);
            QTRY_VERIFY_WITH_TIMEOUT(!host.udpActive(), 3000);
        }
    }

    void datagramHandshakeCanPrecedeReliableAnswer() {
        const auto port = availableUdpPort(); QVERIFY(port);
        DatagramLink link(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0, squad::MediaTransport::Medium::Data);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, link.port(), squad::MediaTransport::Medium::Data);
        QJsonObject answer;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) { QVERIFY(client.receive(message)); });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) { answer = message; });
        QVERIFY(waitForEvents([&] { return !answer.isEmpty() && link.bindingAnswered; }, 5000));
        QVERIFY(host.receive(answer));
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        QSignalSpy received(&client, &squad::MediaTransport::datagramReceived);
        QVERIFY(host.sendDatagram("ready"));
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received.first().first().toByteArray(), QByteArray("ready"));
    }

    void sharedSocketServesIpv4AndIpv6Peers() {
        using Transport = squad::MediaTransport;
        const auto port = availableUdpPort(); QVERIFY(port);
        Transport ipv4Host(true, port, QHostAddress::LocalHost, 0, Transport::Medium::Data);
        Transport ipv6Host(true, port, QHostAddress::LocalHostIPv6, 0, Transport::Medium::Data);
        Transport ipv4Client(false, 0, QHostAddress::LocalHost, port, Transport::Medium::Data);
        Transport ipv6Client(false, 0, QHostAddress::LocalHostIPv6, port, Transport::Medium::Data);
        const auto connectPair = [&](Transport& host, Transport& client) {
            connect(&host, &Transport::signaling, &client, [&client](const auto& message) { QVERIFY(client.receive(message)); });
            connect(&client, &Transport::signaling, &host, [&host](const auto& message) { QVERIFY(host.receive(message)); });
        };
        connectPair(ipv4Host, ipv4Client); connectPair(ipv6Host, ipv6Client);
        QVERIFY(waitForEvents([&] { return ipv4Host.udpActive() && ipv6Host.udpActive()
            && ipv4Client.udpActive() && ipv6Client.udpActive(); }, 10000));
        QSignalSpy ipv4AtClient(&ipv4Client, &Transport::datagramReceived), ipv6AtClient(&ipv6Client, &Transport::datagramReceived);
        QSignalSpy ipv4AtHost(&ipv4Host, &Transport::datagramReceived), ipv6AtHost(&ipv6Host, &Transport::datagramReceived);
        QVERIFY(ipv4Host.sendDatagram("four")); QVERIFY(ipv6Host.sendDatagram("six"));
        QVERIFY(ipv4Client.sendDatagram("four back")); QVERIFY(ipv6Client.sendDatagram("six back"));
        QVERIFY(waitForEvents([&] { return ipv4AtClient.size() == 1 && ipv6AtClient.size() == 1
            && ipv4AtHost.size() == 1 && ipv6AtHost.size() == 1; }));
        QCOMPARE(ipv4AtClient.first().first().toByteArray(), QByteArray("four"));
        QCOMPARE(ipv6AtClient.first().first().toByteArray(), QByteArray("six"));
        QCOMPARE(ipv4AtHost.first().first().toByteArray(), QByteArray("four back"));
        QCOMPARE(ipv6AtHost.first().first().toByteArray(), QByteArray("six back"));
    }
    void encryptedDatagramsCarryIndependentSources_data() {
        QTest::addColumn<QHostAddress>("address");
        QTest::newRow("ipv4") << QHostAddress(QHostAddress::LocalHost);
        QTest::newRow("ipv6") << QHostAddress(QHostAddress::LocalHostIPv6);
    }
    void encryptedDatagramsCarryIndependentSources() {
        QFETCH(QHostAddress, address);
        const auto port = availableUdpPort(); QVERIFY(port);
        squad::MediaTransport host(true, port, address, 0);
        squad::MediaTransport client(false, 0, address, port);
        bool valid = true; int fallback = 0;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) {
            fallback += message.value("kind") == "frame"; valid &= client.receive(message);
        });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) {
            fallback += message.value("kind") == "frame"; valid &= host.receive(message);
        });
        QSignalSpy atHost(&host, &squad::MediaTransport::audio), atClient(&client, &squad::MediaTransport::audio);
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        const auto packet = speech(); QVERIFY(!packet.isEmpty());
        QSet<QString> sources;
        for (int index = 0; index < 70; ++index) {
            const auto source = QString("%1").arg(index, 64, 16, QLatin1Char('0'));
            QVERIFY(host.send(source, packet));
            QVERIFY(waitForEvents(&client, &squad::MediaTransport::audio, [&] { return atClient.size() == index + 1; }));
            QCOMPARE(atClient.last().at(0).toString(), source);
            QCOMPARE(atClient.last().at(1).toByteArray(), packet);
            sources.insert(source);
        }
        QVERIFY(client.send(first, packet)); QTRY_COMPARE(atHost.size(), 1);
        QCOMPARE(sources.size(), 70); QCOMPARE(fallback, 0); QVERIFY(valid);
    }
    void datagramLossTamperingReplayAndRecovery() {
        DatagramLink link(0);
        const auto port = availableUdpPort(); QVERIFY(port); link.setTarget(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, link.port());
        int fallback = 0; bool valid = true;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) {
            fallback += message.value("kind") == "frame"; valid &= client.receive(message);
        });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) { valid &= host.receive(message); });
        QSignalSpy heard(&client, &squad::MediaTransport::audio);
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        const auto packet = speech(); QVERIFY(host.send(first, packet)); QTRY_COMPARE(heard.size(), 1);
        QVERIFY(!link.lastAudio.contains(packet));
        link.replay(); QTest::qWait(80); QCOMPARE(heard.size(), 1);
        QTest::qSleep(400); // Force expiry of the last UDP liveness response.
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 2000);
        const auto beforeCorruption = link.audioPackets;
        link.corrupt = true; QVERIFY(host.send(first, packet)); QCOMPARE(fallback, 0);
        QTRY_COMPARE(link.audioPackets, beforeCorruption + 1);
        QTest::qWait(80); QCOMPARE(heard.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 2000);
        link.corrupt = false; QVERIFY(host.send(first, packet)); QTRY_COMPARE(heard.size(), 2);
        QCOMPARE(heard.last().at(2).toInt(), 1); QCOMPARE(fallback, 0);
        link.blocked = true; QTRY_VERIFY(!host.udpActive());
        QVERIFY(host.send(first, packet)); QTRY_COMPARE(heard.size(), 3); QCOMPARE(fallback, 1);
        link.blocked = false; QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 5000);
        QVERIFY(host.send(first, packet)); QTRY_COMPARE(heard.size(), 4); QCOMPARE(fallback, 1); QVERIFY(valid);
    }
    void prolongedBlockedUdpRenegotiatesWithoutLosingReliableAudio_data() {
        QTest::addColumn<int>("recoverAt");
        QTest::newRow("first-attempt") << 1;
        QTest::newRow("backoff") << 2;
    }
    void prolongedBlockedUdpRenegotiatesWithoutLosingReliableAudio() {
        QFETCH(int, recoverAt);
        QElapsedTimer recoveryClock; recoveryClock.start();
        QList<qint64> attempts;
        DatagramLink link(0); link.blocked = true;
        const auto port = availableUdpPort(); QVERIFY(port); link.setTarget(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, link.port());
        int restarts = 0, fallback = 0;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) {
            if (message.value("kind") == "restart") { attempts.append(recoveryClock.elapsed()); ++restarts; link.blocked = restarts < recoverAt; }
            fallback += message.value("kind") == "frame";
            QVERIFY(client.receive(message));
        });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) { QVERIFY(host.receive(message)); });
        QVERIFY(!host.receive({{"kind", "restart"}, {"epoch", 1}}));
        QVERIFY(!client.receive({{"kind", "restart"}, {"epoch", 2}}));
        QVERIFY(!client.receive({{"kind", "restart"}, {"epoch", 0.5}}));
        QSignalSpy heard(&client, &squad::MediaTransport::audio);
        const auto packet = speech();
        QVERIFY(host.send(first, packet)); QTRY_COMPARE(heard.size(), 1);
        QCOMPARE(fallback, 1);
        QVERIFY(waitForEvents([&] { return host.udpActive() && client.udpActive(); }, recoverAt == 1 ? 10000 : 22000));
        QCOMPARE(restarts, recoverAt);
        QVERIFY(attempts.first() >= 5000);
        if (recoverAt == 2) QVERIFY(attempts.last() - attempts.first() >= 10000);
        QVERIFY(host.send(first, packet)); QTRY_COMPARE(heard.size(), 2);
        QCOMPARE(fallback, 1); QCOMPARE(heard.last().at(2).toInt(), 0);
        // Simulate an overloaded UI thread that cannot service probe callbacks.
        QTest::qSleep(400);
        QVERIFY(!client.udpActive());
        // Duplicate and late restart messages do not tear down the recovered path.
        QSignalSpy renegotiation(&client, &squad::MediaTransport::signaling);
        QVERIFY(client.receive({{"kind", "restart"}, {"epoch", recoverAt}}));
        QVERIFY(client.receive({{"kind", "restart"}, {"epoch", 0}}));
        // A delayed test callback can age the 300 ms liveness probe. Wait for
        // a fresh acknowledgement, but reject any actual renegotiation.
        QTRY_VERIFY_WITH_TIMEOUT(client.udpActive(), 2000);
        QVERIFY(renegotiation.isEmpty());
    }
    void mismatchedDtlsFingerprintCannotOpenMedia() {
        DatagramLink link(0);
        const auto port = availableUdpPort(); QVERIFY(port); link.setTarget(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0), client(false, 0, QHostAddress::LocalHost, link.port());
        bool altered = false; int fallback = 0;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](QJsonObject message) {
            if (message.value("kind") == "description") {
                auto sdp = message.value("sdp").toString();
                const QString marker("a=fingerprint:sha-256 "); const auto start = sdp.indexOf(marker);
                QVERIFY(start >= 0); const auto index = start + marker.size();
                sdp[index] = sdp.at(index) == '0' ? '1' : '0'; message.insert("sdp", sdp); altered = true;
            }
            fallback += message.value("kind") == "frame"; QVERIFY(client.receive(message));
        });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) { QVERIFY(host.receive(message)); });
        QTRY_VERIFY(altered); QTest::qWait(1000);
        QVERIFY(!host.udpActive()); QVERIFY(!client.udpActive());
        QSignalSpy heard(&client, &squad::MediaTransport::audio);
        QVERIFY(host.send(first, speech())); QTRY_COMPARE(heard.size(), 1);
        QCOMPARE(fallback, 1); QCOMPARE(link.audioPackets, 0);
    }
    void onlyTheImpairedReceiverReportsLoss() {
        DatagramLink slow(0), fast(0);
        QUdpSocket firstSocket, secondSocket;
        QVERIFY2(firstSocket.bind(QHostAddress::Any, 0, QAbstractSocket::DontShareAddress),
                 qPrintable(firstSocket.errorString()));
        QVERIFY2(secondSocket.bind(QHostAddress::Any, 0, QAbstractSocket::DontShareAddress),
                 qPrintable(secondSocket.errorString()));
        const auto firstPort = firstSocket.localPort(), secondPort = secondSocket.localPort();
        firstSocket.close(); secondSocket.close();
        slow.setTarget(firstPort); fast.setTarget(secondPort);
        squad::MediaTransport a(true, firstPort, QHostAddress::LocalHost, 0), b(false, 0, QHostAddress::LocalHost, slow.port());
        squad::MediaTransport c(true, secondPort, QHostAddress::LocalHost, 0), d(false, 0, QHostAddress::LocalHost, fast.port());
        connect(&a, &squad::MediaTransport::signaling, &b, [&](const auto& message) { QVERIFY(b.receive(message)); });
        connect(&b, &squad::MediaTransport::signaling, &a, [&](const auto& message) { QVERIFY(a.receive(message)); });
        connect(&c, &squad::MediaTransport::signaling, &d, [&](const auto& message) { QVERIFY(d.receive(message)); });
        connect(&d, &squad::MediaTransport::signaling, &c, [&](const auto& message) { QVERIFY(c.receive(message)); });
        QTRY_VERIFY_WITH_TIMEOUT(a.udpActive() && b.udpActive() && c.udpActive() && d.udpActive(), 10000);
        QSignalSpy impaired(&a, &squad::MediaTransport::quality), healthy(&c, &squad::MediaTransport::quality);
        QSignalSpy impairedAudio(&b, &squad::MediaTransport::audio), healthyAudio(&d, &squad::MediaTransport::audio);
        const auto packet = speech();
        QVERIFY(a.send(first, packet)); QVERIFY(c.send(first, packet));
        QVERIFY(waitForEvents([&] { return impairedAudio.count() > 0 && healthyAudio.count() > 0
            && slow.audioPackets > 0 && fast.audioPackets > 0; }, 3000));
        impaired.clear(); healthy.clear();
        slow.dropEvery = 3;
        QTimer sender; sender.setInterval(20);
        connect(&sender, &QTimer::timeout, this, [&] { QVERIFY(a.send(first, packet)); QVERIFY(c.send(first, packet)); });
        sender.start();
        // Both receivers must report a measured condition. An empty healthy
        // spy or a transient handshake delay does not establish isolation.
        QVERIFY(waitForEvents([&] { return !impaired.isEmpty() && !healthy.isEmpty()
            && impaired.last().first().toInt() < 0 && healthy.last().first().toInt() > 0; }, 3000));
        slow.dropEvery = 0; impaired.clear();
        QVERIFY(waitForEvents([&] { return std::any_of(impaired.begin(), impaired.end(),
            [](const auto& event) { return event.first().toInt() > 0; }); }, 3000));
    }
    void sourceAcknowledgmentPrecedesDatagrams_data() {
        QTest::addColumn<bool>("expiredProbe");
        QTest::newRow("current-probe") << false;
        QTest::newRow("expired-probe") << true;
    }
    void sourceAcknowledgmentPrecedesDatagrams() {
        QFETCH(bool, expiredProbe);
        const auto port = availableUdpPort(); QVERIFY(port);
        DatagramLink link(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, link.port());
        QJsonObject acknowledgment; int fallback = 0; bool valid = true;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) {
            fallback += message.value("kind") == "frame"; valid &= client.receive(message);
        });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) {
            if (message.value("kind") == "sourceAck") acknowledgment = message;
            else valid &= host.receive(message);
        });
        QSignalSpy heard(&client, &squad::MediaTransport::audio);
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        const auto packet = speech(); QVERIFY(host.send(first, packet));
        QTRY_COMPARE(heard.size(), 1); QCOMPARE(fallback, 1);
        QCOMPARE(link.audioPackets, 0);
        if (expiredProbe) {
            link.blocked = true;
            QTRY_VERIFY_WITH_TIMEOUT(!host.udpActive(), 3000);
            link.blocked = false;
        }
        QVERIFY(!acknowledgment.isEmpty()); QVERIFY(host.receive(acknowledgment));
        // Source approval and path liveness are independent. The first delivery
        // can outlive a probe, so require a fresh response before expecting UDP.
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 2000);
        QVERIFY(host.send(first, packet)); QTRY_COMPARE(heard.size(), 2);
        QCOMPARE(fallback, 1); QCOMPARE(link.audioPackets, 1); QVERIFY(valid);
        QCOMPARE(heard.last().at(1).toByteArray(), packet);
        QCOMPARE(heard.last().at(2).toInt(), 0);
    }
    void audioPlayoutMemoryStaysBoundedAcrossSources() {
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, 0);
        auto packet = speech(); const auto size = packet.size(); packet.resize(4000);
        QCOMPARE(opus_packet_pad(reinterpret_cast<unsigned char*>(packet.data()), int(size), int(packet.size())), OPUS_OK);
        const auto encoded = QString::fromLatin1(packet.toBase64());
        QSignalSpy heard(&client, &squad::MediaTransport::audio);
        for (int index = 1; index <= 200; ++index) {
            const auto id = QString::number(index, 16).rightJustified(64, '0');
            QVERIFY(client.receive({{"kind", "source"}, {"ssrc", index}, {"id", id}, {"sequence", 1}}));
            QVERIFY(client.receive({{"kind", "frame"}, {"ssrc", index}, {"sequence", 1}, {"data", encoded}}));
        }
        // No event-loop turn was allowed above, so all input competes for the
        // same bounded playout window. Excess work is shed, not accumulated.
        QTRY_COMPARE(heard.size(), (512 * 1024) / 4000);
        client.retainSources({}); heard.clear();
        QVERIFY(client.receive({{"kind", "source"}, {"ssrc", 1}, {"id", first}, {"sequence", 2}}));
        QVERIFY(client.receive({{"kind", "frame"}, {"ssrc", 1}, {"sequence", 2}, {"data", encoded}}));
        QTRY_COMPARE(heard.size(), 1);
    }

    void sourceChurnReusesBoundedEncryptedStreams() {
        const auto port = availableUdpPort(); QVERIFY(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0), client(false, 0, QHostAddress::LocalHost, port);
        QSet<qint64> streams; int fallback = 0;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) {
            if (message.value("kind") == "source") streams.insert(message.value("ssrc").toInteger());
            fallback += message.value("kind") == "frame";
            QVERIFY(client.receive(message));
        });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) { QVERIFY(host.receive(message)); });
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        const auto packet = speech(); QSignalSpy heard(&client, &squad::MediaTransport::audio);
        for (int index = 0; index < 70; ++index) {
            const auto source = QString("%1").arg(index, 64, 16, QLatin1Char('0'));
            host.retainSources({source}); client.retainSources({source});
            QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 5000);
            QVERIFY(host.send(source, packet));
            QVERIFY(waitForEvents(&client, &squad::MediaTransport::audio, [&] { return heard.size() == index + 1; }));
            QCOMPARE(heard.last().at(0).toString(), source); QCOMPARE(heard.last().at(2).toInt(), 0);
        }
        QCOMPARE(streams.size(), 1); QCOMPARE(fallback, 0);
    }
    void delayedOwnerUsesNetworkArrivalTime_data() {
        QTest::addColumn<bool>("deadlinePassed");
        QTest::addColumn<bool>("gap");
        QTest::newRow("due-frames") << true << false;
        QTest::newRow("contiguous-burst") << false << false;
        QTest::newRow("gap-before-burst") << false << true;
    }
    void delayedOwnerUsesNetworkArrivalTime() {
        QFETCH(bool, deadlinePassed);
        QFETCH(bool, gap);
        const auto port = availableUdpPort(); QVERIFY(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0);
        QThread thread;
        QObject context;
        context.moveToThread(&thread); thread.start();
        std::unique_ptr<squad::MediaTransport> client;
        QSemaphore entered, resume;
        bool blocked = false;
        const auto cleanup = qScopeGuard([&] {
            if (blocked) resume.release();
            QMetaObject::invokeMethod(&context, [&] {
                client.reset(); context.moveToThread(QCoreApplication::instance()->thread());
            }, Qt::BlockingQueuedConnection);
            thread.quit(); thread.wait();
        });
        QMetaObject::invokeMethod(&context, [&] {
            client = std::make_unique<squad::MediaTransport>(false, 0, QHostAddress::LocalHost, port);
        }, Qt::BlockingQueuedConnection);
        int fallback = 0;
        connect(&host, &squad::MediaTransport::signaling, &host, [&](const auto& message) { fallback += message.value("kind") == "frame"; });
        connect(&host, &squad::MediaTransport::signaling, client.get(), [&](const auto& message) { QVERIFY(client->receive(message)); });
        connect(client.get(), &squad::MediaTransport::signaling, &host, [&](const auto& message) { QVERIFY(host.receive(message)); });
        int heard = 0, missing = 0;
        connect(client.get(), &squad::MediaTransport::audio, &host, [&](const auto&, const auto&, int gaps) { ++heard; missing += gaps; });
        QVERIFY(waitForEvents([&] { return host.udpActive(); }, 10000));
        const auto packet = speech(); QVERIFY(host.send(first, packet));
        QVERIFY(waitForEvents([&] { return heard == 1; }));
        const auto setupFallback = fallback;
        blocked = true;
        QMetaObject::invokeMethod(client.get(), [&] { entered.release(); resume.acquire(); }, Qt::QueuedConnection);
        QVERIFY(entered.tryAcquire(1, 1000));
        // Observe each authenticated UDP receipt while the owner is suspended.
        // Pacing with sleeps can expire the path probe on a busy runner and
        // accidentally exercise reliable delivery instead of the reorder queue.
        SendGate gate; gate.release();
        for (size_t i = 0; i < 8; ++i) {
            QVERIFY(host.send(first, packet, gap && i == 0 ? 1 : 0));
            QVERIFY(gate.waitForReceived(i + 1));
        }
        // Let the received packets' actual reorder interval expire.
        if (deadlinePassed) QTest::qWait(25);
        resume.release(); blocked = false;
        QVERIFY(waitForEvents([&] { return heard == 9; }));
        QCOMPARE(missing, gap ? 1 : 0);
        QCOMPARE(fallback, setupFallback);
    }
    void audioSendsStayOffTheOwnerThreadAndBoundBacklog() {
        const auto port = availableUdpPort(); QVERIFY(port);
        DatagramLink link(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, link.port());
        int fallback = 0;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) {
            fallback += message.value("kind") == "frame";
            QVERIFY(client.receive(message));
        });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) { QVERIFY(host.receive(message)); });
        QSignalSpy heard(&client, &squad::MediaTransport::audio);
        QSignalSpy quality(&host, &squad::MediaTransport::quality);
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        const auto packet = speech();
        SendGate gate;
        QVERIFY(host.send(first, packet));
        QTRY_VERIFY_WITH_TIMEOUT(gate.entered(), 1000);
        QVERIFY2(!gate.onOwner(), "SRTP send blocks the Qt owner thread");
        for (int i = 0; i < 1000; ++i) QVERIFY(host.send(first, packet));
        gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 3000);
        QVERIFY(host.send(second, packet));
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(heard.begin(), heard.end(), [](const auto& event) {
            return event.first().toString() == second;
        }), 3000);
        // One in-flight frame, at most six queued frames of that speaker, then
        // the marker. Queue pressure must not spill old speech into TLS.
        QVERIFY(link.audioPackets <= 8);
        QCOMPARE(fallback, 0);
        QCOMPARE(heard.last().at(1).toByteArray(), packet);
        QVERIFY(std::any_of(heard.begin(), heard.end(), [](const auto& event) { return event.at(2).toInt() > 0; }));
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(quality.begin(), quality.end(), [](const auto& event) {
            return event.first().toInt() < 0;
        }), 3000);
    }
    void busyAudioConnectionDoesNotStarveAnotherConnection() {
        const auto port = availableUdpPort(); QVERIFY(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, port);
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) { QVERIFY(client.receive(message)); });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) { QVERIFY(host.receive(message)); });
        QSignalSpy atHost(&host, &squad::MediaTransport::audio), atClient(&client, &squad::MediaTransport::audio);
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        auto packet = speech();
        const auto originalSize = int(packet.size());
        packet.resize(1000);
        QCOMPARE(opus_packet_pad(reinterpret_cast<unsigned char*>(packet.data()), originalSize, 1000), OPUS_OK);
        auto marker = packet; marker.resize(1100);
        QCOMPARE(opus_packet_pad(reinterpret_cast<unsigned char*>(marker.data()), 1000, 1100), OPUS_OK);
        QVERIFY(host.send(first, packet)); QVERIFY(client.send(second, marker));
        QTRY_COMPARE(atHost.size(), 1); QTRY_COMPARE(atClient.size(), 1);
        QTRY_VERIFY(host.udpActive() && client.udpActive());
        atHost.clear(); atClient.clear();
        SendGate gate;
        QVERIFY(host.send(first, packet));
        QVERIFY(waitForEvents([&] { return gate.entered(); }, 1000));
        QVERIFY(!gate.onOwner());
        for (int index = 0; index < 6; ++index) QVERIFY(host.send(first, packet));
        QVERIFY(client.send(second, marker));
        const bool progressedWhileBlocked = gate.waitForPackets(2);
        gate.release();
        QVERIFY2(progressedWhileBlocked, "A blocked connection prevented another connection from sending");
        QVERIFY(waitForEvents([&] { return !atHost.isEmpty(); }, 3000));
        QCOMPARE(atHost.first().at(1).toByteArray(), marker);
        QVERIFY(host.send(first, marker));
        QVERIFY(waitForEvents([&] {
            return std::any_of(atClient.begin(), atClient.end(), [&](const auto& event) { return event.at(1).toByteArray() == marker; });
        }, 3000));
        const auto sizes = gate.packetSizes();
        const auto position = std::find(sizes.begin(), sizes.end(), sizes.front() + 100);
        QVERIFY(position != sizes.end());
        QCOMPARE(position - sizes.begin(), 1);
    }
    void hostProtectsExistingMediaDuringSharedSenderOverload_data() {
        QTest::addColumn<int>("blockedSenders");
        QTest::newRow("one-slow-connection") << 1;
        QTest::newRow("shared-sender-stalled") << 2;
    }
    void hostProtectsExistingMediaDuringSharedSenderOverload() {
        QFETCH(int, blockedSenders);
        QTemporaryDir dir;
        VoiceSession hostSession(dir.filePath("host")), guestSession(dir.filePath("guest"));
        LocalChannel owner(hostSession, dir.filePath("host.channel"), TlsIdentity::create());
        LocalChannel guest(guestSession, dir.filePath("guest.channel"), TlsIdentity::create());
        QVERIFY(owner.listen(QHostAddress::LocalHost));
        QVERIFY(owner.decide(guest.ownId(), true));
        QVERIFY(owner.join(owner.ownId(), "127.0.0.1", owner.port())); QTRY_VERIFY(owner.joined());
        QVERIFY(owner.setScreenSharing(true));
        QVERIFY(guest.openChat(owner.ownId(), "127.0.0.1", owner.port())); QTRY_VERIFY(guest.chatReady());
        QTRY_VERIFY(guest.screenInfo(owner.ownId()).value("available").toBool());
        // Use the host's existing shared endpoint, as additional channels do.
        // A separately probed, then released port is not reserved for this pair.
        const auto port = owner.port();
        squad::MediaTransport sender(true, port, QHostAddress::LocalHost, 0);
        squad::MediaTransport receiver(false, 0, QHostAddress::LocalHost, port);
        connect(&sender, &squad::MediaTransport::signaling, &receiver, [&](const auto& m) { QVERIFY(receiver.receive(m)); });
        connect(&receiver, &squad::MediaTransport::signaling, &sender, [&](const auto& m) { QVERIFY(sender.receive(m)); });
        QTRY_VERIFY2(sender.udpActive() && receiver.udpActive(), qPrintable(QString("UDP handshake on host port %1").arg(port)));
        const auto packet = speech();
        QSignalSpy delivered(&receiver, &squad::MediaTransport::audio);
        QVERIFY(sender.send(first, packet)); QTRY_COMPARE(delivered.size(), 1);
        SendGate gate("Protected SRTP packet, size=", blockedSenders);
        QVERIFY(sender.send(first, packet));
        QVERIFY(waitForEvents([&] { return gate.entered(); }, 1000));
        for (int i = 0; i < 6; ++i) QVERIFY(sender.send(first, packet));
        QVERIFY(receiver.send(second, packet));
        QVERIFY(gate.waitForPackets(2));
        for (int i = 0; i < 6; ++i) QVERIFY(receiver.send(second, packet));
        QElapsedTimer deadline; deadline.start();
        // Expire actual queued RTP packets while keeping the UI/network thread running.
        QVERIFY(waitForEvents([&] { return deadline.elapsed() >= 200; }, 2000));
        QVERIFY(guest.watchScreen(owner.ownId(), true));
        if (blockedSenders == 2) {
            QTRY_COMPARE(guest.screenInfo(owner.ownId()).value("status").toString(), QString("busy"));
            QVERIFY(guest.chatReady());
            QVERIFY(guest.sendChat("Chat remains available during overload")); QTRY_VERIFY(!guest.chatPending());
        } else QTRY_COMPARE(guest.screenInfo(owner.ownId()).value("tier").toInt(), 1);
        QVERIFY(guest.join(owner.ownId(), "127.0.0.1", owner.port()));
        if (blockedSenders == 2) {
            QTRY_VERIFY(guest.status().contains("busy"));
            QVERIFY(!guest.joined());
        } else QTRY_VERIFY(guest.joined());
        QVERIFY(owner.joined());
        gate.release();
        if (blockedSenders == 2) {
            QTimer retry;
            connect(&retry, &QTimer::timeout, &guest, [&] {
                if (!guest.joined()) guest.join(owner.ownId(), "127.0.0.1", owner.port());
            });
            retry.start(250);
            QTRY_VERIFY_WITH_TIMEOUT(guest.joined(), 10000);
            QTRY_VERIFY(guest.screenInfo(owner.ownId()).value("available").toBool());
            QVERIFY(guest.watchScreen(owner.ownId(), true));
            QTRY_COMPARE(guest.screenInfo(owner.ownId()).value("tier").toInt(), 1);
        }
        QVERIFY(owner.joined());
    }
    void queuedAudioCannotOutliveItsSourceOrDeadline_data() {
        QTest::addColumn<bool>("removeSource");
        QTest::newRow("source-removed") << true;
        QTest::newRow("deadline-expired") << false;
    }
    void queuedAudioCannotOutliveItsSourceOrDeadline() {
        QFETCH(bool, removeSource);
        const auto port = availableUdpPort(); QVERIFY(port);
        DatagramLink link(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, link.port());
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) { QVERIFY(client.receive(message)); });
        connect(&client, &squad::MediaTransport::signaling, &host, [&](const auto& message) { QVERIFY(host.receive(message)); });
        QSignalSpy heard(&client, &squad::MediaTransport::audio);
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        const auto packet = speech();
        SendGate gate;
        QVERIFY(host.send(first, packet));
        QTRY_VERIFY_WITH_TIMEOUT(gate.entered(), 1000);
        QVERIFY(!gate.onOwner());
        for (int i = 0; i < 6; ++i) QVERIFY(host.send(first, packet));
        if (removeSource) {
            host.retainSources({}); client.retainSources({});
        } else {
            QElapsedTimer stalled; stalled.start();
            QTRY_VERIFY_WITH_TIMEOUT(stalled.elapsed() >= 350, 1000);
        }
        gate.release();
        // The intentional stall can expire UDP liveness too. Wait for a real
        // probe reply before using UDP delivery as the completion marker.
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 3000);
        QVERIFY(host.send(second, packet));
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(heard.begin(), heard.end(), [](const auto& event) {
            return event.first().toString() == second;
        }), 3000);
        // The packet already being sent can arrive. Queued old packets cannot.
        QCOMPARE(link.audioPackets, 2);
        if (removeSource) QCOMPARE(heard.size(), 1);
    }
    void audioQueuesReleaseWhenPeerCloses() {
        const auto packet = speech();
        for (int repeat = 0; repeat < 10; ++repeat) {
            const auto port = availableUdpPort(); QVERIFY(port);
            squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0);
            auto client = std::make_unique<squad::MediaTransport>(false, 0, QHostAddress::LocalHost, port);
            connect(&host, &squad::MediaTransport::signaling, client.get(), [&](const auto& message) { QVERIFY(client->receive(message)); });
            connect(client.get(), &squad::MediaTransport::signaling, &host, [&](const auto& message) { QVERIFY(host.receive(message)); });
            QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client->udpActive(), 10000);
            for (int i = 0; i < 100; ++i) {
                QVERIFY(host.send(first, packet));
                QVERIFY(client->send(second, packet));
            }
            client.reset();
            QTRY_VERIFY_WITH_TIMEOUT(!host.udpActive(), 3000);
        }
    }
    void sequenceWrapReorderingAndRebindingRejectOldFrames() {
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, 0);
        QSignalSpy heard(&client, &squad::MediaTransport::audio); const auto packet = speech();
        QVERIFY(client.receive({{"kind", "source"}, {"ssrc", 42}, {"id", first}, {"sequence", 65534}}));
        const auto frame = [&](int sequence) { return client.receive({{"kind", "frame"}, {"ssrc", 42}, {"sequence", sequence}, {"data", QString::fromLatin1(packet.toBase64())}}); };
        QVERIFY(frame(65534)); QTRY_COMPARE(heard.size(), 1);
        QVERIFY(frame(0)); QTRY_COMPARE(heard.size(), 2); QCOMPARE(heard.last().at(2).toInt(), 1);
        QVERIFY(frame(65535)); QTest::qWait(30); QCOMPARE(heard.size(), 2);
        client.retainSources({});
        QVERIFY(client.receive({{"kind", "source"}, {"ssrc", 42}, {"id", second}, {"sequence", 3}}));
        QVERIFY(frame(2)); QTest::qWait(30); QCOMPARE(heard.size(), 2);
        QVERIFY(frame(3)); QTRY_COMPARE(heard.size(), 3); QCOMPARE(heard.last().at(0).toString(), second);
        QVERIFY(!client.receive({{"kind", "source"}, {"ssrc", 43}, {"id", second}, {"sequence", 4}}));
        QVERIFY(!client.receive({{"kind", "sourceAck"}, {"ssrc", 42}, {"sequence", 1.5}}));
    }
    void dueAudioSurvivesDelayedTimerDelivery_data() {
        QTest::addColumn<int>("duringDelivery");
        QTest::newRow("continue") << 0;
        QTest::newRow("remove-source") << 1;
        QTest::newRow("destroy-receiver") << 2;
    }
    void dueAudioSurvivesDelayedTimerDelivery() {
        QFETCH(int, duringDelivery);
        auto client = std::make_unique<squad::MediaTransport>(false, 0, QHostAddress::LocalHost, 0);
        const auto packet = speech(); QVERIFY(!packet.isEmpty());
        QVERIFY(client->receive({{"kind", "source"}, {"ssrc", 42}, {"id", first}, {"sequence", 1}}));
        int heard = 0, missing = 0;
        connect(client.get(), &squad::MediaTransport::audio, this, [&](const auto& source, const auto& bytes, int gaps) {
            QCOMPARE(source, first); QCOMPARE(bytes, packet);
            ++heard; missing += gaps;
            if (duringDelivery == 1) client->retainSources({});
            if (duringDelivery == 2) client.reset();
        });
        // Reliable frames are due immediately. Deliver another network batch
        // before Qt dispatches the pending playout timer, without sleeping.
        QJsonObject frame{{"kind", "frame"}, {"ssrc", 42}, {"data", QString::fromLatin1(packet.toBase64())}};
        for (int sequence = 1; sequence <= 7; ++sequence) {
            frame.insert("sequence", sequence);
            QVERIFY(client->receive(frame));
        }
        QCOMPARE(heard, duringDelivery ? 1 : 6);
        if (!duringDelivery) QTRY_COMPARE(heard, 7);
        QCOMPARE(missing, 0);
    }
    void continuousIncomingAudioDoesNotPostponePlayout() {
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, 0);
        const auto packet = speech(); QVERIFY(!packet.isEmpty());
        QVERIFY(client.receive({{"kind", "source"}, {"ssrc", 42}, {"id", first}, {"sequence", 1}}));
        QJsonObject frame{{"kind", "frame"}, {"ssrc", 42}, {"data", QString::fromLatin1(packet.toBase64())}};
        QTimer incoming;
        bool valid = true, heardWhileReceiving = false;
        quint16 sequence = 0;
        connect(&incoming, &QTimer::timeout, &client, [&] {
            frame.insert("sequence", ++sequence);
            valid &= client.receive(frame);
        });
        connect(&client, &squad::MediaTransport::audio, &incoming, [&] {
            heardWhileReceiving |= incoming.isActive();
        });
        // Continuously ready input must not restart the pending playout timer.
        // A zero timer keeps supplying work without blocking the Qt event loop.
        incoming.start(0);
        QTRY_VERIFY_WITH_TIMEOUT(heardWhileReceiving, 1000);
        incoming.stop();
        QVERIFY(valid);
    }
    void receiversCanRemoveSourcesAndDestroyTransportDuringDelivery() {
        auto host = std::make_unique<squad::MediaTransport>(true, 0, QHostAddress::LocalHost, 0);
        auto client = std::make_unique<squad::MediaTransport>(false, 0, QHostAddress::LocalHost, 0);
        connect(host.get(), &squad::MediaTransport::signaling, client.get(), [&](const auto& message) {
            if (message.value("kind") != "description") QVERIFY(client->receive(message));
        });
        int heard = 0;
        connect(client.get(), &squad::MediaTransport::audio, this, [&](const auto&, const auto&, int) {
            ++heard; client->retainSources({}); client.reset();
        });
        const auto packet = speech(); QVERIFY(host->send(first, packet)); QVERIFY(host->send(second, packet));
        QTRY_VERIFY(!client); QCOMPARE(heard, 1);
    }
    void additiveExtensionsDoNotBreakAudio() {
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, 0);
        QVERIFY(client.receive({{"kind", "camera.v2"}, {"future", true}}));
        QVERIFY(!client.receive({{"kind", "invalid kind"}}));
        QVERIFY(!client.receive({{"kind", 42}}));
        const auto packet = speech(); QSignalSpy heard(&client, &squad::MediaTransport::audio);
        QVERIFY(client.receive({{"kind", "source"}, {"ssrc", 1}, {"id", first}, {"sequence", 1}, {"newOption", true}}));
        QVERIFY(client.receive({{"kind", "frame"}, {"ssrc", 1}, {"sequence", 1}, {"data", QString::fromLatin1(packet.toBase64())}, {"newOption", true}}));
        QTRY_COMPARE(heard.size(), 1);
    }
    void unavailableUdpRetainsReliableAudio() {
        squad::MediaTransport host(true, 0, QHostAddress::LocalHost, 0);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, 0);
        bool valid = true;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) {
            if (message.value("kind") != "description") valid &= client.receive(message);
        });
        QSignalSpy atClient(&client, &squad::MediaTransport::audio);
        const auto packet = speech(); QVERIFY(host.send(first, packet));
        QTRY_COMPARE(atClient.size(), 1); QCOMPARE(atClient.first().at(1).toByteArray(), packet);
        QVERIFY(valid); QVERIFY(!host.udpActive());
        QVERIFY(!host.send("invalid", packet)); QVERIFY(!host.send(first, "invalid"));
        QVERIFY(!client.receive({{"kind", "frame"}, {"ssrc", 42}, {"sequence", 1}, {"data", QString::fromLatin1(packet.toBase64())}}));
    }
};
QTEST_GUILESS_MAIN(MediaTransportTests)
#include "media_transport_tests.moc"
