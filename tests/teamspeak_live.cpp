#include "local_channel.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QCommandLineParser>
#include <QTest>
#include <QSignalSpy>
#include <QTimer>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QUrl>
#include <opus.h>
#include <cmath>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser arguments;
    arguments.setApplicationDescription("Two test clients. No microphone or audio output is opened.");
    arguments.addHelpOption();
    arguments.addPositionalArgument("server", "An explicitly authorized TS3 server, with optional :port.");
    arguments.addOption({"state-dir", "Reuse the two test identities in this directory.", "directory"});
    arguments.addOption({"channel", "Numeric channel ID, or 'empty' to select an unoccupied channel.", "id"});
    arguments.addOption({"reconnect", "Interrupt only these test clients' UDP traffic and verify automatic recovery."});
    arguments.addOption({"text-only", "Check text and recovery without sending audio."});
    arguments.addOption({"observer", "One additional listener who explicitly agreed to hear the short test tone.", "nickname"});
    arguments.process(app);
    if (arguments.positionalArguments().size() != 1 || !arguments.isSet("state-dir")) arguments.showHelp(2);
    const auto endpoint = arguments.positionalArguments().first();
    const auto target = arguments.value("channel");
    const auto observer = arguments.value("observer");
    QUdpSocket proxy;
    QHash<quint16, QUdpSocket*> routes;
    bool blocked = false;
    auto connectionAddress = endpoint;
    if (arguments.isSet("reconnect")) {
        const QUrl url(endpoint.startsWith("ts3://") ? endpoint : "ts3://" + endpoint);
        const auto addresses = QHostInfo::fromName(url.host()).addresses();
        if (addresses.isEmpty() || url.port(9987) < 1 || !proxy.bind(QHostAddress::LocalHost, 0)) return 13;
        const auto serverAddress = addresses.first();
        const auto serverPort = quint16(url.port(9987));
        QObject::connect(&proxy, &QUdpSocket::readyRead, &proxy, [&, serverAddress, serverPort] {
            while (proxy.hasPendingDatagrams()) {
                const auto packet = proxy.receiveDatagram();
                if (blocked) continue;
                const auto clientPort = packet.senderPort();
                auto* route = routes.value(clientPort);
                if (!route) {
                    route = new QUdpSocket(&proxy);
                    route->connectToHost(serverAddress, serverPort);
                    routes.insert(clientPort, route);
                    QObject::connect(route, &QUdpSocket::readyRead, &proxy, [&, route, clientPort] {
                        while (route->hasPendingDatagrams()) {
                            const auto reply = route->receiveDatagram();
                            if (!blocked) proxy.writeDatagram(reply.data(), QHostAddress::LocalHost, clientPort);
                        }
                    });
                }
                route->write(packet.data());
            }
        });
        connectionAddress = "127.0.0.1:" + QString::number(proxy.localPort());
    }
    const QString root = QDir(arguments.value("state-dir")).absolutePath();
    QDir().mkpath(root);
    VoiceSession sa(root + "/a.ini"), sb(root + "/b.ini");
    sa.setUserName("SquadSpeak test A"); sb.setUserName("SquadSpeak test B");
    sa.setMuted(true); sb.setMuted(true); sa.setDeafened(false); sb.setDeafened(false);
    sa.setAudioReadiness(true, true); sb.setAudioReadiness(true, true);
    LocalChannel a(sa, root + "/a.channel", TlsIdentity::loadFile(root + "/a.identity", true));
    LocalChannel b(sb, root + "/b.channel", TlsIdentity::loadFile(root + "/b.identity", true));
    auto check = [](bool okay, const char* label) { std::printf("%s: %s\n", label, okay ? "PASS" : "FAIL"); std::fflush(stdout); return okay; };
    if (!check(a.addTeamSpeak(connectionAddress) && b.addTeamSpeak(connectionAddress), "start")) return 1;
    if (!check(QTest::qWaitFor([&] { return a.joined() && b.joined() && a.participants().size() >= 2 && b.participants().size() >= 2; }, 20000), "two real server connections")) {
        std::fprintf(stderr, "A: %s\nB: %s\n", qPrintable(a.status()), qPrintable(b.status())); return 2;
    }
    QString peer;
    for (const auto& value : a.participants()) if (value.toMap().value("name") == "SquadSpeak test B") peer = value.toMap().value("id").toString();
    if (!check(!peer.isEmpty() && a.openPrivateChat(a.joinedHostId(), peer), "private recipient")) return 3;
    const QString text = "SquadSpeak private integration check";
    QSignalSpy receipt(&a, &LocalChannel::chatSent);
    if (!check(a.sendChat(text) && QTest::qWaitFor([&] { return !receipt.empty() && !b.privateChats().empty(); }, 10000), "private server acknowledgement")) return 4;
    const auto conversation = b.privateChats().first().toMap();
    if (!check(b.openSavedChat(conversation.value("id").toString()) && !b.messages().empty() && b.messages().last().toMap().value("text") == text, "private end-to-end delivery")) return 5;
    a.openSavedChat(a.joinedHostId()); b.openSavedChat(b.joinedHostId());
    if (!target.isEmpty()) {
        bool moved = false;
        for (const auto& item : a.savedChannels()) {
            const auto candidate = item.toMap();
            if (candidate.value("protocol") != "teamspeak" || !candidate.value("online").toBool()
                || candidate.value("tsChannel").toString().isEmpty()
                || (target != "empty" && candidate.value("tsChannel") != target) || candidate.value("password").toBool()
                || !candidate.value("members").toList().isEmpty()) continue;
            const auto id = candidate.value("id").toString();
            if (a.joinSaved(id) && b.joinSaved(id) && QTest::qWaitFor([&] {
                return a.joinedHostId() == id && b.joinedHostId() == id && a.participants().size() == 2 && b.participants().size() == 2;
            }, 5000)) { moved = true; break; }
            std::fprintf(stderr, "Move to %s (%s) failed: A=%s B=%s, joined=%s/%s, members=%lld/%lld, status=%s/%s\n",
                qPrintable(candidate.value("name").toString()), qPrintable(candidate.value("tsChannel").toString()),
                qPrintable(a.chatError()), qPrintable(b.chatError()), qPrintable(a.joinedHostId()), qPrintable(b.joinedHostId()),
                (long long)a.participants().size(), (long long)b.participants().size(), qPrintable(a.status()), qPrintable(b.status()));
        }
        if (!check(moved, "moved to empty test channel")) return 12;
    }
    a.openSavedChat(a.joinedHostId()); b.openSavedChat(b.joinedHostId());
    std::printf("Channel members A=%lld B=%lld\n", (long long)a.participants().size(), (long long)b.participants().size());
    const auto expectedMembers = observer.isEmpty() ? 2 : 3;
    const auto allowed = [&](const QVariantList& members) {
        if (members.size() != expectedMembers) return false;
        for (const auto& value : members) {
            const auto name = value.toMap().value("name").toString();
            if (name != "SquadSpeak test A" && name != "SquadSpeak test B" && name != observer) return false;
        }
        return true;
    };
    if (!allowed(a.participants()) || !allowed(b.participants())) {
        std::puts("Audio and channel message withheld because other server members are present."); a.leave(); b.leave(); return 6;
    }
    receipt.clear();
    const auto channelMessage = [&](const QString& message) {
        receipt.clear();
        if (!allowed(a.participants()) || !allowed(b.participants()) || !a.sendChat(message)) return false;
        return QTest::qWaitFor([&] {
            return !receipt.empty() && !b.messages().empty() && b.messages().last().toMap().value("text") == message;
        }, 10000);
    };
    if (observer.isEmpty() && !check(channelMessage("SquadSpeak isolated channel check"), "channel end-to-end delivery")) {
        std::fprintf(stderr, "Chat A: %s\nChat B: %s\n", qPrintable(a.chatError()), qPrintable(b.chatError()));
        return 7;
    }
    if (arguments.isSet("reconnect")) {
        const auto selected = a.joinedHostId();
        const auto conversationId = conversation.value("id").toString();
        blocked = true;
        if (!check(QTest::qWaitFor([&] { return !a.chatReady() && !b.chatReady(); }, 60000), "outage detected")) return 14;
        blocked = false;
        if (!check(QTest::qWaitFor([&] {
            return a.chatReady() && b.chatReady() && a.joinedHostId() == selected && b.joinedHostId() == selected
                && allowed(a.participants()) && allowed(b.participants());
        }, 45000), "automatic same-channel recovery")) return 15;
        if (!check(b.openSavedChat(conversationId) && !b.messages().empty() && b.messages().last().toMap().value("text") == text,
            "private history survives recovery")) return 16;
        b.openSavedChat(b.joinedHostId());
        if (!check(sa.muted() && sb.muted(), "mute state survives recovery")) return 17;
        if (observer.isEmpty() && !check(channelMessage("SquadSpeak recovered channel check"), "channel delivery after recovery")) return 18;
    }
    if (arguments.isSet("text-only")) { a.leave(); b.leave(); return 0; }
    sa.setPushToTalk(true); sa.setPttKeyHeld(false); sa.setMuted(false);
    if (!check(!a.sendAudio("withheld"), "released PTT blocks outgoing audio")) return 11;
    sa.setPttKeyHeld(true);
    if (!check(QTest::qWaitFor([&] { for (const auto& value : b.participants()) { const auto member = value.toMap(); if (member.value("name") == "SquadSpeak test A" && !member.value("muted").toBool()) return true; } return false; }, 5000), "unmuted presence")) { std::fprintf(stderr, "A status: %s, ready %d available %d mute %d\nB status: %s\n", qPrintable(a.status()), sa.inputReady(), sa.available(), sa.muted(), qPrintable(b.status())); return 8; }
    int error = 0;
    OpusEncoder* encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
    OpusDecoder* decoder = opus_decoder_create(48000, 1, &error);
    if (!encoder || !decoder || error) return 9;
    int sent = 0, received = 0; double energy = 0;
    QObject::connect(&b, &LocalChannel::audioReceived, &app, [&](const QString&, const QByteArray& packet, int) {
        float pcm[5760]; const int count = opus_decode_float(decoder, reinterpret_cast<const unsigned char*>(packet.constData()), packet.size(), pcm, 5760, 0);
        if (count > 0) { ++received; for (int i = 0; i < count; ++i) energy += pcm[i] * pcm[i]; }
    });
    QSignalSpy ownEcho(&a, &LocalChannel::audioReceived);
    QTimer send;
    QObject::connect(&send, &QTimer::timeout, &app, [&] {
        if (sent >= 20 || !allowed(a.participants()) || !allowed(b.participants())) { send.stop(); return; }
        float pcm[960]; for (int i = 0; i < 960; ++i) pcm[i] = float(0.08 * std::sin((sent * 960 + i) * 2 * 3.141592653589793 * 440 / 48000));
        QByteArray packet(4096, Qt::Uninitialized);
        const int count = opus_encode_float(encoder, pcm, 960, reinterpret_cast<unsigned char*>(packet.data()), packet.size());
        if (count > 0) { packet.resize(count); if (a.sendAudio(packet)) ++sent; }
    });
    send.start(20);
    const bool sound = QTest::qWaitFor([&] { return received >= 5 && energy > 0.01; }, 6000);
    std::printf("Audio sent=%d received=%d energy=%f ownEcho=%lld allowed=%d status=%s\n", sent, received, energy, (long long)ownEcho.size(), sa.transmissionAllowed(), qPrintable(a.status()));
    send.stop(); sa.setMuted(true); a.leave(); b.leave();
    opus_encoder_destroy(encoder); opus_decoder_destroy(decoder);
    return check(sound && ownEcho.isEmpty(), "Opus audio delivered without self echo") ? 0 : 10;
}
