#include "local_channel.hpp"
#include "voice_mixer.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <vector>
#include <opus.h>

namespace {
// Minimal independent protocol peer for configuring and observing the real
// server. The application adapter is never substituted by the test fixture.
QByteArray varint(quint64 value) {
    QByteArray bytes;
    do { const auto part = quint8(value & 127); value >>= 7; bytes.append(char(part | (value ? 128 : 0))); } while (value);
    return bytes;
}
QByteArray number(int field, quint64 value) { return varint(quint64(field) << 3) + varint(value); }
QByteArray bytes(int field, const QByteArray& value) { return varint((quint64(field) << 3) | 2) + varint(value.size()) + value; }
QMap<int, QVariant> fields(const QByteArray& data) {
    QMap<int, QVariant> result;
    qsizetype cursor = 0;
    const auto read = [&]() {
        quint64 value = 0;
        for (int shift = 0; shift < 64 && cursor < data.size(); shift += 7) {
            const auto byte = quint8(data[cursor++]); value |= quint64(byte & 127) << shift;
            if (!(byte & 128)) return value;
        }
        cursor = data.size(); return quint64(0);
    };
    while (cursor < data.size()) {
        const auto tag = read();
        if (!tag) break;
        if ((tag & 7) == 0) result[int(tag >> 3)] = qulonglong(read());
        else if ((tag & 7) == 2) {
            const auto length = read();
            if (length > quint64(data.size() - cursor)) break;
            result[int(tag >> 3)] = data.mid(cursor, qsizetype(length)); cursor += qsizetype(length);
        } else if ((tag & 7) == 1) cursor += 8;
        else if ((tag & 7) == 5) cursor += 4;
        else break;
    }
    return result;
}
QByteArray voiceNumber(quint32 value) {
    if (value < 128) return QByteArray(1, char(value));
    if (value < 16384) return QByteArray(1, char(0x80 | (value >> 8))) + char(value);
    if (value < 2097152) return QByteArray(1, char(0xc0 | (value >> 16))) + char(value >> 8) + char(value);
    return QByteArray(1, char(0xe0 | (value >> 24))) + char(value >> 16) + char(value >> 8) + char(value);
}
std::optional<QPair<quint32, qsizetype>> readVoiceNumber(const QByteArray& packet, qsizetype offset) {
    if (offset >= packet.size()) return {};
    const auto first = quint8(packet[offset++]);
    int count = 1;
    quint32 value = first;
    if (first & 0x80) {
        if ((first & 0xc0) == 0x80) { value = first & 0x3f; count = 2; }
        else if ((first & 0xe0) == 0xc0) { value = first & 0x1f; count = 3; }
        else if ((first & 0xf0) == 0xe0) { value = first & 0x0f; count = 4; }
        else return {};
    }
    for (int i = 1; i < count; ++i) {
        if (offset >= packet.size()) return {};
        value = (value << 8) | quint8(packet[offset++]);
    }
    return qMakePair(value, offset);
}
QByteArray voicePacket(quint32 sequence, const QByteArray& opus, bool final = false) {
    const auto size = quint32(opus.size()) | (final ? 0x2000 : 0);
    return QByteArray(1, char(4 << 5)) + voiceNumber(sequence) + voiceNumber(size) + opus;
}
QByteArray opusFrame(int samples, int tone = 440) {
    int error = OPUS_OK;
    auto* encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
    if (!encoder || error != OPUS_OK) return {};
    std::vector<float> pcm(static_cast<size_t>(samples), 0.0f);
    for (int i = 0; i < samples; ++i)
        pcm[size_t(i)] = float(0.1 * std::sin(2 * std::numbers::pi * tone * i / 48000));
    QByteArray encoded(1275, Qt::Uninitialized);
    const auto length = opus_encode_float(encoder, pcm.data(), samples,
        reinterpret_cast<unsigned char*>(encoded.data()), opus_int32(encoded.size()));
    opus_encoder_destroy(encoder);
    if (length <= 0) return {};
    encoded.resize(length);
    return encoded;
}
struct DecodedVoice final { quint32 session = 0; quint32 sequence = 0; QByteArray opus; bool final = false; };
std::optional<DecodedVoice> decodeVoice(const QByteArray& packet, bool serverToClient) {
    if (packet.isEmpty() || (quint8(packet[0]) >> 5) != 4) return {};
    qsizetype offset = 1;
    quint32 session = 0;
    if (serverToClient) {
        const auto read = readVoiceNumber(packet, offset);
        if (!read) return {};
        session = read->first; offset = read->second;
    }
    const auto sequence = readVoiceNumber(packet, offset);
    if (!sequence) return {};
    offset = sequence->second;
    const auto length = readVoiceNumber(packet, offset);
    if (!length) return {};
    offset = length->second;
    const auto size = length->first & 0x1fff;
    if (size > quint32(packet.size() - offset) || offset + size != packet.size()) return {};
    return DecodedVoice{session, sequence->first, packet.mid(offset, size), bool(length->first & 0x2000)};
}
class ProtocolPeer final : public QObject {
public:
    QSslSocket socket;
    QByteArray pending;
    QList<QPair<int, QByteArray>> received;
    QMap<quint32, QMap<int, QVariant>> users;
    QMap<QString, quint32> channels;
    quint32 own = 0;
    bool synced = false;
    ProtocolPeer() {
        connect(&socket, &QSslSocket::sslErrors, &socket, [this](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
        connect(&socket, &QSslSocket::readyRead, this, [this] {
            pending += socket.readAll();
            while (pending.size() >= 6) {
                const auto type = qFromBigEndian<quint16>(pending.constData());
                const auto length = qFromBigEndian<quint32>(pending.constData() + 2);
                if (length > 1024 * 1024) { socket.abort(); return; }
                if (pending.size() < length + 6) return;
                const auto message = pending.mid(6, length); pending.remove(0, length + 6);
                received.append({type, message}); if (received.size() > 512) received.removeFirst();
                const auto data = fields(message);
                if (type == 5) { own = data.value(1).toUInt(); synced = true; }
                if (type == 7 && data.contains(1) && data.contains(3)) channels[QString::fromUtf8(data.value(3).toByteArray())] = data.value(1).toUInt();
                if (type == 9 && data.contains(1)) for (auto it = data.cbegin(); it != data.cend(); ++it) users[data.value(1).toUInt()][it.key()] = it.value();
                if (type == 8) users.remove(data.value(1).toUInt());
            }
        });
    }
    bool open(const QString& address, const QString& user, const QString& password) {
        const QUrl endpoint("mumble://" + address);
        QTimer retry;
        connect(&retry, &QTimer::timeout, &socket, [&] {
            if (socket.state() == QAbstractSocket::UnconnectedState) socket.connectToHostEncrypted(endpoint.host(), quint16(endpoint.port()));
        });
        socket.connectToHostEncrypted(endpoint.host(), quint16(endpoint.port()));
        retry.start(100);
        const auto connected = QTest::qWaitFor([&] { return socket.isEncrypted(); }, 15000);
        retry.stop();
        if (!connected) return false;
        send(0, number(1, 0x010400) + bytes(2, "Independent Mumble test peer"));
        send(2, bytes(1, user.toUtf8()) + bytes(2, password.toUtf8()) + number(5, 1));
        return QTest::qWaitFor([&] { return synced; }, 10000);
    }
    bool send(quint16 type, const QByteArray& payload) {
        if (!socket.isEncrypted() || payload.size() > 1024 * 1024) return false;
        QByteArray packet(6, Qt::Uninitialized);
        qToBigEndian(type, packet.data()); qToBigEndian(quint32(payload.size()), packet.data() + 2);
        packet += payload;
        return socket.write(packet) == packet.size();
    }
    bool createChannel(const QString& name, quint32 parent) {
        send(7, number(2, parent) + bytes(3, name.toUtf8()));
        return QTest::qWaitFor([&] { return channels.contains(name); }, 5000);
    }
    bool move(quint32 channel) {
        send(9, number(5, channel));
        return QTest::qWaitFor([&] { return users.value(own).value(5).toUInt() == channel; }, 5000);
    }
    quint32 sessionNamed(const QString& name) const {
        for (auto it = users.cbegin(); it != users.cend(); ++it)
            if (QString::fromUtf8(it.value().value(3).toByteArray()) == name) return it.key();
        return 0;
    }
    QString certificateHash(quint32 session) const {
        return QString::fromUtf8(users.value(session).value(15).toByteArray());
    }
    bool hasText(const QString& text) const {
        return std::any_of(received.cbegin(), received.cend(), [&](const auto& item) {
            return item.first == 11 && QString::fromUtf8(fields(item.second).value(5).toByteArray()) == text;
        });
    }
    bool sendChannelText(quint32 channel, const QString& text) {
        return send(11, number(3, channel) + bytes(5, text.toUtf8()));
    }
    bool sendPrivateText(quint32 session, const QString& text) {
        return send(11, number(2, session) + bytes(5, text.toUtf8()));
    }
    bool sendVoice(quint32 sequence, const QByteArray& opus, bool final = false) {
        return send(1, voicePacket(sequence, opus, final));
    }
    quint32 goodPacketsFrom(quint32 session) const {
        for (auto it = received.crbegin(); it != received.crend(); ++it) {
            if (it->first != 22) continue;
            const auto stats = fields(it->second);
            if (stats.value(1).toUInt() == session && stats.contains(4))
                return fields(stats.value(4).toByteArray()).value(1).toUInt();
        }
        return 0;
    }
    bool waitForGoodPackets(quint32 session, quint32 minimum, int timeoutMs = 10000) {
        QTimer polling;
        polling.setInterval(500);
        connect(&polling, &QTimer::timeout, this, [this, session] {
            send(22, number(1, session) + number(2, 1));
        });
        polling.start();
        send(22, number(1, session) + number(2, 1));
        const auto okay = QTest::qWaitFor([&] { return goodPacketsFrom(session) >= minimum; }, timeoutMs);
        polling.stop();
        return okay;
    }
    QList<DecodedVoice> voiceFrom(quint32 session) const {
        QList<DecodedVoice> result;
        for (const auto& item : received) if (item.first == 1) {
            const auto decoded = decodeVoice(item.second, true);
            if (decoded && decoded->session == session) result.append(*decoded);
        }
        return result;
    }
    bool acl(quint32 channel, const QByteArray& token = {}) {
        QByteArray list = bytes(4, bytes(5, "all") + number(7, 4));
        if (!token.isEmpty()) list += bytes(4, bytes(5, "#" + token) + number(6, 4));
        send(13, number(1, channel) + number(2, 1) + list);
        const auto before = received.size();
        send(13, number(1, channel) + number(5, 1));
        const auto accepted = QTest::qWaitFor([&] {
            for (int i = before; i < received.size(); ++i) if (received[i].first == 13 && fields(received[i].second).value(1).toUInt() == channel) return true;
            return false;
        }, 5000);
        if (!accepted) for (int i = before; i < received.size(); ++i) qWarning() << received[i].first << fields(received[i].second);
        return accepted;
    }
};
struct Peer final {
    VoiceSession session;
    LocalChannel channel;
    Peer(const QString& path, const QString& name, const TlsIdentity& identity)
        : session(path + ".ini"), channel(session, path + ".json", identity) {
        session.setUserName(name); session.setPushToTalk(false);
        session.setAudioReadiness(true, true); session.setMuted(false); session.setDeafened(false);
    }
};
QVariantMap channelNamed(const LocalChannel& client, const QString& name) {
    for (const auto& value : client.savedChannels()) if (value.toMap().value("protocol") == "mumble" && value.toMap().value("name") == name) return value.toMap();
    return {};
}
QString memberNamed(const LocalChannel& client, const QString& name) {
    for (const auto& value : client.participants()) if (value.toMap().value("name") == name) return value.toMap().value("id").toString();
    return {};
}
}

class MumbleTests final : public QObject {
    Q_OBJECT
    QString endpoint = qEnvironmentVariable("SQUAD_MUMBLE_ENDPOINT");
    QString password = qEnvironmentVariable("SQUAD_MUMBLE_PASSWORD");
    ProtocolPeer admin;
private slots:
    void initTestCase() {
        QVERIFY2(!endpoint.isEmpty() && !password.isEmpty(), "Run this suite through tests/mumble_server.py.");
        QVERIFY2(admin.open(endpoint, "SuperUser", password), qPrintable(admin.socket.errorString()));
        QVERIFY(admin.createChannel("Lounge", 0));
        QVERIFY(admin.createChannel("Studio", admin.channels.value("Lounge")));
        QVERIFY(admin.createChannel("Token", 0)); QVERIFY(admin.acl(admin.channels.value("Token"), "open-sesame"));
        QVERIFY(admin.createChannel("Private", 0)); QVERIFY(admin.acl(admin.channels.value("Private")));
    }
    void passwordDirectoryIdentityAndRepeatJoin() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto identity = TlsIdentity::create();
        QString saved, remoteId, certificateHash;
        quint32 remoteSession = 0;
        {
            Peer client(directory.filePath("a"), "Mumble A", identity);
            QVERIFY(client.channel.openAddress(endpoint));
            QTRY_VERIFY_WITH_TIMEOUT(client.channel.passwordRequired(), 10000);
            QVERIFY(client.channel.submitPassword("incorrect-server-password", false));
            QTRY_VERIFY_WITH_TIMEOUT(client.channel.passwordRequired(), 10000);
            QVERIFY(client.channel.submitPassword(password, true));
            QTRY_VERIFY_WITH_TIMEOUT(client.channel.joined(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT((remoteSession = admin.sessionNamed("Mumble A")) != 0, 5000);
            certificateHash = admin.certificateHash(remoteSession);
            QVERIFY(!certificateHash.isEmpty());
            QTRY_VERIFY(!channelNamed(client.channel, "Studio").isEmpty());
            const auto studio = channelNamed(client.channel, "Studio");
            QCOMPARE(studio.value("parent"), channelNamed(client.channel, "Lounge").value("id"));
            saved = studio.value("id").toString();
            QVERIFY(client.channel.joinSaved(saved)); QTRY_COMPARE(client.channel.joinedHostId(), saved);
            remoteId = client.channel.ownId();
            const auto count = admin.users.size();
            QVERIFY(client.channel.joinSaved(saved));
            QCOMPARE(admin.users.size(), count);
            QVERIFY(client.channel.leave()); QTRY_VERIFY(!client.channel.joined());
            QTRY_VERIFY_WITH_TIMEOUT(!admin.users.contains(remoteSession), 5000);
        }
        Peer restored(directory.filePath("a"), "Mumble A", identity);
        QVERIFY(restored.channel.joinSaved(saved));
        QTRY_COMPARE_WITH_TIMEOUT(restored.channel.joinedHostId(), saved, 10000);
        QTRY_VERIFY(restored.channel.joined());
        QCOMPARE(restored.channel.ownId(), remoteId);
        quint32 restoredSession = 0;
        QTRY_VERIFY_WITH_TIMEOUT((restoredSession = admin.sessionNamed("Mumble A")) != 0, 5000);
        QCOMPARE(admin.certificateHash(restoredSession), certificateHash);
        QVERIFY(restored.channel.leave());
        QTRY_VERIFY_WITH_TIMEOUT(!admin.users.contains(restoredSession), 5000);
        QFile state(directory.filePath("a.json")); QVERIFY(state.open(QIODevice::ReadOnly));
        const auto savedState = state.readAll(); QVERIFY(!savedState.contains(password.toUtf8()));
        QVERIFY(savedState.contains("mumblePins"));
    }
    void changedServerKeyFailsBeforeAuthentication() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto identity = TlsIdentity::create();
        const auto path = directory.filePath("pinned");
        QString saved, server;
        {
            Peer first(path, "Pin original", identity);
            QVERIFY(first.channel.addMumble(endpoint, password));
            QTRY_VERIFY(first.channel.joined());
            saved = first.channel.joinedHostId();
            for (const auto& entry : first.channel.savedChannels())
                if (entry.toMap().value("id") == saved) server = entry.toMap().value("deviceId").toString();
            QVERIFY(!server.isEmpty());
            QVERIFY(first.channel.leave());
            QTRY_COMPARE(admin.sessionNamed("Pin original"), quint32(0));
        }
        QFile file(path + ".json"); QVERIFY(file.open(QIODevice::ReadOnly));
        auto state = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        auto security = state.value("security").toObject();
        auto pins = security.value("mumblePins").toObject();
        QVERIFY(pins.contains(server)); pins.insert(server, QString(64, '0'));
        security.insert("mumblePins", pins); state.insert("security", security);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(QJsonDocument(state).toJson()) > 0); file.close();
        Peer rejected(path, "Must not authenticate", identity);
        QVERIFY(rejected.channel.joinSaved(saved));
        QTRY_VERIFY_WITH_TIMEOUT(rejected.channel.status().contains("key changed"), 10000);
        QVERIFY(!rejected.channel.joined());
        QCOMPARE(admin.sessionNamed("Must not authenticate"), quint32(0));
    }
    void hostnameUsesAReachableResolvedAddress() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        Peer client(directory.filePath("dns"), "DNS client", TlsIdentity::create());
        QVERIFY(client.channel.addMumble("localhost:" + endpoint.section(':', -1), password));
        QTRY_VERIFY2_WITH_TIMEOUT(client.channel.joined(), qPrintable(client.channel.status()), 10000);
        QVERIFY(client.channel.leave());
    }
    void channelPermissionsAndSavedAccessToken() {
        QTemporaryDir directory; const auto identity = TlsIdentity::create();
        QString tokenChannel;
        {
            Peer client(directory.filePath("a"), "Mumble token", identity);
            QVERIFY(client.channel.addMumble(endpoint, password)); QTRY_VERIFY(client.channel.joined());
            const auto current = client.channel.joinedHostId();
            const auto blocked = channelNamed(client.channel, "Private").value("id").toString();
            QVERIFY(!blocked.isEmpty()); QVERIFY(client.channel.joinSaved(blocked));
            QTRY_VERIFY(client.channel.passwordRequired()); QCOMPARE(client.channel.joinedHostId(), current);
            tokenChannel = channelNamed(client.channel, "Token").value("id").toString();
            QVERIFY(client.channel.joinSaved(tokenChannel)); QTRY_VERIFY(client.channel.passwordRequired());
            QVERIFY(client.channel.submitPassword("wrong-token", false)); QTRY_VERIFY(client.channel.passwordRequired());
            QCOMPARE(client.channel.joinedHostId(), current);
            QVERIFY(client.channel.submitPassword("open-sesame", true));
            QTRY_COMPARE(client.channel.joinedHostId(), tokenChannel); QVERIFY(client.channel.leave());
        }
        Peer restored(directory.filePath("a"), "Mumble token", identity);
        QVERIFY(restored.channel.joinSaved(tokenChannel));
        QTRY_COMPARE_WITH_TIMEOUT(restored.channel.joinedHostId(), tokenChannel, 10000);
        QTRY_VERIFY(restored.channel.joined());
    }
    void channelAndPrivateMessagesReuseTheSharedChat() {
        QTemporaryDir directory;
        Peer a(directory.filePath("a"), "Chat A", TlsIdentity::create());
        Peer b(directory.filePath("b"), "Chat B", TlsIdentity::create());
        QVERIFY(a.channel.addMumble(endpoint, password)); QVERIFY(b.channel.addMumble(endpoint, password));
        QTRY_VERIFY(a.channel.joined() && b.channel.joined());
        const auto studio = channelNamed(a.channel, "Studio").value("id").toString();
        QVERIFY(a.channel.joinSaved(studio)); QVERIFY(b.channel.joinSaved(studio));
        QTRY_COMPARE(a.channel.joinedHostId(), studio); QTRY_COMPARE(b.channel.joinedHostId(), studio);
        QTRY_VERIFY(!memberNamed(a.channel, "Chat B").isEmpty());
        const auto bId = memberNamed(a.channel, "Chat B");
        QVERIFY(a.channel.sendChat("Channel hello"));
        QTRY_VERIFY(!b.channel.messages().isEmpty());
        QCOMPARE(b.channel.messages().last().toMap().value("text").toString(), QString("Channel hello"));
        QVERIFY(a.channel.canPrivateChat(studio, bId)); QVERIFY(a.channel.openPrivateChat(studio, bId));
        QVERIFY(a.channel.sendChat("Private hello"));
        QTRY_COMPARE(b.channel.privateChats().size(), 1);
        const auto conversation = b.channel.privateChats().first().toMap();
        QVERIFY(b.channel.openSavedChat(conversation.value("id").toString()));
        QTRY_VERIFY(!b.channel.messages().isEmpty());
        QCOMPARE(b.channel.messages().last().toMap().value("text").toString(), QString("Private hello"));
        QVERIFY(b.channel.sendChat("Private reply"));
        QTRY_COMPARE(a.channel.messages().last().toMap().value("text").toString(), QString("Private reply"));
        QVERIFY(a.channel.leave());
        QTRY_VERIFY(!b.channel.privateChats().first().toMap().value("peerOnline").toBool());
        QVERIFY(!b.channel.sendChat("Offline must fail"));
    }
    void opusAudioIsDeliveredWithoutEchoOrTranscoding() {
        QTemporaryDir directory;
        Peer a(directory.filePath("a"), "Audio A", TlsIdentity::create());
        Peer b(directory.filePath("b"), "Audio B", TlsIdentity::create());
        QVERIFY(a.channel.addMumble(endpoint, password)); QVERIFY(b.channel.addMumble(endpoint, password));
        QTRY_VERIFY(a.channel.joined() && b.channel.joined());
        const auto studio = channelNamed(a.channel, "Studio").value("id").toString();
        QVERIFY(a.channel.joinSaved(studio)); QVERIFY(b.channel.joinSaved(studio));
        QTRY_COMPARE(a.channel.joinedHostId(), studio); QTRY_COMPARE(b.channel.joinedHostId(), studio);
        QTRY_VERIFY(!memberNamed(b.channel, "Audio A").isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(admin.sessionNamed("Audio A") != 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(admin.sessionNamed("Audio B") != 0, 5000);
        const auto aSession = admin.sessionNamed("Audio A");
        const auto bSession = admin.sessionNamed("Audio B");
        QVERIFY(admin.waitForGoodPackets(aSession, 1));
        QVERIFY(admin.waitForGoodPackets(bSession, 1));
        const auto udpBaseline = admin.goodPacketsFrom(aSession);
        squad::VoiceMixer encoder(2), receiver(2); receiver.setAutomatic(false);
        QList<QByteArray> sent, received;
        QList<int> receivedSamples;
        QList<bool> receivedFinal;
        int missing = 0;
        connect(&b.channel, &LocalChannel::audioReceived, &b.channel,
            [&](const QString& id, const QByteArray& packet, int lost, int quantum, bool last) {
                if (!packet.isEmpty()) {
                    received.append(packet);
                    receivedSamples.append(opus_packet_get_nb_samples(
                        reinterpret_cast<const unsigned char*>(packet.constData()), int(packet.size()), 48000));
                }
                receivedFinal.append(last);
                missing += lost;
                QVERIFY(receiver.receive(id, packet, 0, lost, quantum, last));
            });
        QSignalSpy echo(&a.channel, &LocalChannel::audioReceived);
        QTimer sender; sender.setTimerType(Qt::PreciseTimer);
        connect(&sender, &QTimer::timeout, &a.channel, [&] {
            std::vector<float> input(1920);
            for (int i = 0; i < 960; ++i) {
                input[i * 2] = float(0.1 * std::sin(2 * std::numbers::pi * 440 * (sent.size() * 960 + i) / 48000));
                input[i * 2 + 1] = float(0.07 * std::sin(2 * std::numbers::pi * 997 * (sent.size() * 960 + i) / 48000));
            }
            const auto packet = encoder.encode(input, 48000).first(); sent.append(packet);
            QVERIFY(a.channel.sendAudio(packet));
            if (sent.size() == 40) { sender.stop(); QVERIFY(a.channel.sendAudio({})); }
        });
        sender.start(20);
        const auto complete = QTest::qWaitFor([&] { return received.size() == 40; }, 10000);
        if (!complete) {
            QList<int> indices;
            for (const auto& packet : received) indices.append(int(sent.indexOf(packet)));
            qWarning() << "Received frame indices" << indices << "missing ticks" << missing
                << "server UDP packets" << admin.goodPacketsFrom(aSession) << "baseline" << udpBaseline;
        }
        QVERIFY(complete);
        QVERIFY(admin.waitForGoodPackets(aSession, udpBaseline + 40));
        QCOMPARE(received, sent); QCOMPARE(missing, 0); QCOMPARE(echo.size(), 0);
        QVERIFY(a.channel.sendAudio(opusFrame(480, 659)));
        QTRY_COMPARE_WITH_TIMEOUT(received.size(), 41, 5000);
        QCOMPARE(receivedSamples.last(), 480);
        QVERIFY(a.channel.sendAudio({}));
        QTRY_VERIFY_WITH_TIMEOUT(receivedFinal.contains(true), 5000);
        QVERIFY(a.session.setMuted(true)); QVERIFY(!a.channel.sendAudio(sent.first()));
    }
    void htmlMessagesRemainReadableWithoutLoadingResources() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QTcpServer resources;
        QVERIFY(resources.listen(QHostAddress::LocalHost, 0));
        ProtocolPeer peer;
        QVERIFY(peer.open(endpoint, "HTML peer", password));
        Peer app(directory.filePath("app"), "HTML app", TlsIdentity::create());
        QVERIFY(app.channel.addMumble(endpoint, password));
        QTRY_VERIFY_WITH_TIMEOUT(app.channel.joined(), 10000);
        const auto html = QString("<b>Hello &amp; welcome</b><br/>Second line<img src=\"http://127.0.0.1:%1/image.png\"/>")
            .arg(resources.serverPort());
        QVERIFY(peer.sendChannelText(0, html));
        QTRY_VERIFY_WITH_TIMEOUT(!app.channel.messages().isEmpty(), 5000);
        QCOMPARE(app.channel.messages().last().toMap().value("text").toString(),
            QString("Hello & welcome\nSecond line"));
        QVERIFY(!resources.hasPendingConnections());
        const QString literal = "<tag> & \"quoted\"\nNext line";
        QVERIFY(app.channel.sendChat(literal));
        QTRY_VERIFY_WITH_TIMEOUT(peer.hasText("&lt;tag&gt; &amp; &quot;quoted&quot;<br/>Next line"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(app.channel.messages().last().toMap().value("text").toString(), literal, 5000);
    }
    void independentPeerChatVoiceAndPushToTalk() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto studio = admin.channels.value("Studio");
        ProtocolPeer peer;
        QVERIFY(peer.open(endpoint, "Independent peer", password));
        QVERIFY(peer.move(studio));
        Peer app(directory.filePath("app"), "SquadSpeak app", TlsIdentity::create());
        QVERIFY(app.channel.addMumble(endpoint, password));
        QTRY_VERIFY_WITH_TIMEOUT(app.channel.joined(), 10000);
        const auto savedStudio = channelNamed(app.channel, "Studio").value("id").toString();
        QVERIFY(!savedStudio.isEmpty());
        QVERIFY(app.channel.joinSaved(savedStudio));
        QTRY_COMPARE_WITH_TIMEOUT(app.channel.joinedHostId(), savedStudio, 10000);
        const auto peerId = memberNamed(app.channel, "Independent peer");
        QVERIFY(!peerId.isEmpty());
        const auto peerSession = peer.sessionNamed("Independent peer");
        QTRY_VERIFY_WITH_TIMEOUT(peer.sessionNamed("SquadSpeak app") != 0, 5000);
        const auto appSession = peer.sessionNamed("SquadSpeak app");

        QVERIFY(peer.sendChannelText(studio, "Peer to app channel"));
        QTRY_VERIFY_WITH_TIMEOUT(!app.channel.messages().isEmpty(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(app.channel.messages().last().toMap().value("text").toString(),
            QString("Peer to app channel"), 5000);
        QVERIFY(app.channel.sendChat("App to peer channel"));
        QTRY_VERIFY_WITH_TIMEOUT(peer.hasText("App to peer channel"), 5000);

        QVERIFY(app.channel.canPrivateChat(savedStudio, peerId));
        QVERIFY(app.channel.openPrivateChat(savedStudio, peerId));
        QVERIFY(app.channel.sendChat("App to peer private"));
        QTRY_VERIFY_WITH_TIMEOUT(peer.hasText("App to peer private"), 5000);
        QVERIFY(peer.sendPrivateText(appSession, "Peer to app private"));
        QTRY_COMPARE_WITH_TIMEOUT(app.channel.privateChats().size(), 1, 5000);
        const auto conversation = app.channel.privateChats().first().toMap();
        QVERIFY(app.channel.openSavedChat(conversation.value("id").toString()));
        QTRY_COMPARE_WITH_TIMEOUT(app.channel.messages().last().toMap().value("text").toString(),
            QString("Peer to app private"), 5000);

        QList<QByteArray> fromPeer;
        QList<int> durations;
        QList<bool> finals;
        connect(&app.channel, &LocalChannel::audioReceived, &app.channel,
            [&](const QString& id, const QByteArray& packet, int, int, bool last) {
                if (!packet.isEmpty()) {
                    fromPeer.append(packet);
                    durations.append(opus_packet_get_nb_samples(
                        reinterpret_cast<const unsigned char*>(packet.constData()), int(packet.size()), 48000));
                }
                finals.append(last);
                QVERIFY(id == peerId);
            });
        QVERIFY(peer.sendVoice(0, opusFrame(960, 330)));
        QVERIFY(peer.sendVoice(2, opusFrame(480, 550)));
        QVERIFY(peer.sendVoice(3, QByteArray{}, true));
        // Mumble's empty end marker consumes no sequence ticks. A new talk
        // burst can arrive in the same socket read at exactly that sequence.
        QVERIFY(peer.sendVoice(3, opusFrame(960, 770)));
        QVERIFY(peer.sendVoice(5, QByteArray{}, true));
        QTRY_COMPARE_WITH_TIMEOUT(fromPeer.size(), 3, 5000);
        QCOMPARE(durations, QList<int>({960, 480, 960}));
        QVERIFY(finals.contains(true));

        QVERIFY(app.session.setPushToTalk(true));
        QVERIFY(!app.session.pttHeld());
        const auto outgoingLong = opusFrame(960, 880);
        const auto outgoingShort = opusFrame(480, 990);
        QVERIFY(!app.channel.sendAudio(outgoingLong));
        QVERIFY(app.session.setPttButtonHeld(true));
        QVERIFY(app.channel.sendAudio(outgoingLong));
        QVERIFY(app.channel.sendAudio(outgoingShort));
        QVERIFY(app.channel.sendAudio({}));
        QVERIFY(app.session.setPttButtonHeld(false));
        QTRY_VERIFY_WITH_TIMEOUT(peer.voiceFrom(appSession).size() >= 3, 5000);
        const auto appVoice = peer.voiceFrom(appSession);
        QCOMPARE(appVoice[appVoice.size() - 3].opus, outgoingLong);
        QCOMPARE(appVoice[appVoice.size() - 2].opus, outgoingShort);
        QCOMPARE(appVoice.last().opus.size(), 0);
        QVERIFY(appVoice.last().final);
        QVERIFY(app.session.setMuted(true));
        QVERIFY(!app.channel.sendAudio(outgoingLong));
        QVERIFY(app.channel.leave());
        QTRY_VERIFY_WITH_TIMEOUT(!peer.users.contains(appSession), 5000);
    }
};
QTEST_GUILESS_MAIN(MumbleTests)
#include "mumble_tests.moc"
