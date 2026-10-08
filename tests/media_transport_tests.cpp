#include "media_transport.hpp"
#include <QSignalSpy>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QTest>
#include <QTimer>
#include <rtc/global.hpp>
#include <opus.h>
#include <array>
#include <cmath>
#include <memory>

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
    QByteArray lastAudio;
    explicit DatagramLink(quint16 target) : target_(target) {
        if (!socket_.bind(QHostAddress::LocalHost)) throw std::runtime_error("UDP fixture cannot bind");
        connect(&socket_, &QUdpSocket::readyRead, this, [this] {
            while (socket_.hasPendingDatagrams()) {
                const auto datagram = socket_.receiveDatagram();
                const bool downstream = datagram.senderPort() == target_;
                if (!downstream) client_ = quint16(datagram.senderPort());
                if (blocked) continue;
                auto bytes = datagram.data();
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
    bool receivedClientData() const { return client_ != 0; }
    void setTarget(quint16 target) { target_ = target; }
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
}
class MediaTransportTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        rtc::InitLogger(rtc::LogLevel::Info);
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

    void dataDatagramsReleaseQueuedMessagesWhenPeerCloses() {
        const QByteArray payload(16384, 'x');
        const auto port = availableUdpPort(); QVERIFY(port);
        for (int repeat = 0; repeat < 20; ++repeat) {
            squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0, squad::MediaTransport::Medium::Data);
            auto client = std::make_unique<squad::MediaTransport>(false, 0, QHostAddress::LocalHost, port,
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
        QTRY_VERIFY_WITH_TIMEOUT(!answer.isEmpty() && link.receivedClientData(), 5000);
        QVERIFY(host.receive(answer));
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 10000);
        QSignalSpy received(&client, &squad::MediaTransport::datagramReceived);
        QVERIFY(host.sendDatagram("ready"));
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received.first().first().toByteArray(), QByteArray("ready"));
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
        QVERIFY(host.send(first, packet)); QVERIFY(host.send(second, packet)); QVERIFY(client.send(first, packet));
        QTRY_COMPARE(atClient.size(), 2); QTRY_COMPARE(atHost.size(), 1);
        QCOMPARE(atClient.at(0).at(1).toByteArray(), packet); QCOMPARE(atClient.at(1).at(1).toByteArray(), packet);
        QSet<QString> sources{atClient.at(0).at(0).toString(), atClient.at(1).at(0).toString()};
        QCOMPARE(sources, (QSet<QString>{first, second})); QCOMPARE(fallback, 0); QVERIFY(valid);
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
    void prolongedBlockedUdpRenegotiatesWithoutLosingReliableAudio() {
        DatagramLink link(0); link.blocked = true;
        const auto port = availableUdpPort(); QVERIFY(port); link.setTarget(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, link.port());
        int restarts = 0, fallback = 0;
        connect(&host, &squad::MediaTransport::signaling, &client, [&](const auto& message) {
            if (message.value("kind") == "restart") { ++restarts; link.blocked = false; }
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
        QTRY_VERIFY_WITH_TIMEOUT(host.udpActive() && client.udpActive(), 38000);
        QCOMPARE(restarts, 1);
        QVERIFY(host.send(first, packet)); QTRY_COMPARE(heard.size(), 2);
        QCOMPARE(fallback, 1); QCOMPARE(heard.last().at(2).toInt(), 0);
        // Simulate an overloaded UI thread that cannot service probe callbacks.
        QTest::qSleep(400);
        QVERIFY(!client.udpActive());
        // Duplicate and late restart messages do not tear down the recovered path.
        QSignalSpy renegotiation(&client, &squad::MediaTransport::signaling);
        QVERIFY(client.receive({{"kind", "restart"}, {"epoch", 1}}));
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
        slow.dropEvery = 3; const auto packet = speech();
        QTimer sender; sender.setInterval(20);
        connect(&sender, &QTimer::timeout, this, [&] { QVERIFY(a.send(first, packet)); QVERIFY(c.send(first, packet)); });
        sender.start();
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(impaired.begin(), impaired.end(), [](const auto& event) { return event.first().toInt() < 0; }), 3000);
        QVERIFY(std::none_of(healthy.begin(), healthy.end(), [](const auto& event) { return event.first().toInt() < 0; }));
        slow.dropEvery = 0; impaired.clear();
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(impaired.begin(), impaired.end(), [](const auto& event) { return event.first().toInt() > 0; }), 3000);
    }
    void sourceAcknowledgmentPrecedesDatagrams() {
        const auto port = availableUdpPort(); QVERIFY(port);
        squad::MediaTransport host(true, port, QHostAddress::LocalHost, 0);
        squad::MediaTransport client(false, 0, QHostAddress::LocalHost, port);
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
        QVERIFY(!acknowledgment.isEmpty()); QVERIFY(host.receive(acknowledgment));
        QVERIFY(host.send(first, packet)); QTRY_COMPARE(heard.size(), 2);
        QCOMPARE(fallback, 1); QVERIFY(valid);
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
            QVERIFY(host.send(source, packet)); QTRY_COMPARE(heard.size(), index + 1);
            QCOMPARE(heard.last().at(0).toString(), source); QCOMPARE(heard.last().at(2).toInt(), 0);
        }
        QCOMPARE(streams.size(), 1); QCOMPARE(fallback, 0);
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
