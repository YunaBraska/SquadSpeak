#include "local_channel.hpp"
#include "voice_mixer.hpp"
#include <QElapsedTimer>
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
#include <QScopeGuard>
#include <QtEndian>

static int stress(const QString& endpoint, const QString& root, const QString& target, const QString& observer, int batchMs) {
    constexpr int clients = 3, speakers = 2, bursts = 8, frames = 100, pause = 20;
    struct Peer {
        VoiceSession session;
        LocalChannel channel;
        squad::VoiceMixer encoder{2};
        std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder{
            opus_decoder_create(48000, 2, nullptr), opus_decoder_destroy};
        std::array<QList<QByteArray>, speakers> received;
        QList<QByteArray> sent;
        int missing = 0;
        Peer(const QString& path, int index)
            : session(path + ".ini"), channel(session, path + ".channel", TlsIdentity::loadFile(path + ".identity", true)) {
            session.setUserName(QStringLiteral("SquadSpeak load %1").arg(index + 1));
            session.setPushToTalk(false);
            session.setMuted(true); session.setDeafened(false); session.setAudioReadiness(true, true);
        }
    };
    std::vector<std::unique_ptr<Peer>> peers;
    const auto cleanup = qScopeGuard([&] { for (auto& peer : peers) peer->channel.leave(); });
    QDir().mkpath(root);
    QString selected;
    for (int index = 0; index < clients; ++index) {
        const auto profile = index < 2 ? QString(index == 0 ? "/a" : "/b") : QStringLiteral("/load-%1").arg(index);
        auto& peer = *peers.emplace_back(std::make_unique<Peer>(root + profile, index));
        if (!peer.channel.addTeamSpeak(endpoint) || !QTest::qWaitFor([&] { return peer.channel.joined(); }, 20000)) {
            std::fprintf(stderr, "Client %d could not connect: %s\n", index + 1, qPrintable(peer.channel.status())); return 20;
        }
        if (!QTest::qWaitFor([&] {
            for (const auto& item : peer.channel.savedChannels())
                if (item.toMap().value("tsChannel") == target && item.toMap().value("online").toBool()) return true;
            return false;
        }, 10000)) { std::puts("Selected stress channel was not received."); return 21; }
        if (index == 0) {
            for (const auto& item : peer.channel.savedChannels()) {
                const auto row = item.toMap();
                const auto members = row.value("members").toList();
                const bool safe = std::all_of(members.cbegin(), members.cend(), [&](const auto& member) {
                    const auto name = member.toMap().value("name").toString();
                    return name == peer.session.userName() || (!observer.isEmpty() && name == observer);
                });
                if (row.value("protocol") == "teamspeak" && row.value("online").toBool() && row.value("tsChannel") == target
                    && safe && !row.value("password").toBool()) selected = row.value("id").toString();
            }
            if (selected.isEmpty()) { std::puts("Stress test requires an empty, explicitly selected channel."); return 21; }
        }
        if ((peer.channel.joinedHostId() != selected && !peer.channel.joinSaved(selected))
            || !QTest::qWaitFor([&] { return peer.channel.joinedHostId() == selected; }, 5000)) {
            std::fprintf(stderr, "Client %d could not join test channel: %s\n", index + 1, qPrintable(peer.channel.status())); return 22;
        }
        std::printf("Client %d joined isolated test channel.\n", index + 1); std::fflush(stdout);
    }
    const auto isolated = [&] {
        for (const auto& peer : peers) {
            const auto members = peer->channel.participants();
            const bool observing = !observer.isEmpty() && std::any_of(members.cbegin(), members.cend(), [&](const auto& member) {
                return member.toMap().value("name") == observer;
            });
            if (members.size() != clients + (observing ? 1 : 0)) return false;
            for (const auto& member : members) {
                const auto row = member.toMap();
                const auto name = row.value("name").toString();
                if (name == observer && !row.value("deafened").toBool()) return false;
                if (name != observer && std::none_of(peers.cbegin(), peers.cend(), [&](const auto& expected) { return expected->session.userName() == name; })) return false;
            }
        }
        return true;
    };
    if (!QTest::qWaitFor(isolated, 10000)) {
        for (int index = 0; index < clients; ++index) {
            std::fprintf(stderr, "Client %d roster:", index + 1);
            for (const auto& member : peers[index]->channel.participants())
                std::fprintf(stderr, " [%s]", qPrintable(member.toMap().value("name").toString()));
            std::fputc('\n', stderr);
        }
        return 23;
    }
    for (int receiver = 0; receiver < clients; ++receiver) {
        QHash<QString, int> identities;
        for (const auto& member : peers[receiver]->channel.participants()) {
            const auto row = member.toMap();
            for (int sender = 0; sender < speakers; ++sender)
                if (row.value("name") == peers[sender]->session.userName()) identities.insert(row.value("id").toString(), sender);
        }
        QObject::connect(&peers[receiver]->channel, &LocalChannel::audioReceived, &peers[receiver]->channel,
            [&, receiver, identities](const QString& id, const QByteArray& packet, int missing) {
                const int sender = identities.value(id, -1);
                if (sender < 0 || sender == receiver) { peers[receiver]->missing++; return; }
                peers[receiver]->received[sender].append(packet);
                peers[receiver]->missing += missing;
            });
    }
    for (int sender = 0; sender < speakers; ++sender) {
        peers[sender]->encoder.setPacketLimit(peers[sender]->channel.audioPacketLimit());
        peers[sender]->session.setMuted(false);
    }
    if (!QTest::qWaitFor([&] {
        if (!isolated()) return false;
        for (const auto& peer : peers) {
            for (const auto& member : peer->channel.participants()) {
                const auto row = member.toMap();
                for (int sender = 0; sender < speakers; ++sender)
                    if (row.value("name") == peers[sender]->session.userName() && row.value("muted").toBool()) return false;
            }
        }
        return true;
    }, 5000)) { std::puts("Speakers were not unmuted by the server."); return 23; }
    QElapsedTimer clock; clock.start();
    QTimer send; send.setTimerType(Qt::PreciseTimer);
    int tick = 0;
    bool accepted = true;
    constexpr int ticks = bursts * (frames + pause);
    QObject::connect(&send, &QTimer::timeout, &send, [&] {
        if (!isolated()) { std::puts("Channel membership or observer mute changed."); accepted = false; send.stop(); return; }
        const int due = std::min(ticks, int(clock.elapsed() / 20));
        while (tick < due) {
            for (int sender = 0; sender < speakers; ++sender) {
                auto& peer = *peers[sender];
                if (tick % (frames + pause) < frames) {
                    std::array<float, 1920> pcm{};
                    for (auto packet : peer.encoder.encode(pcm, 48000)) {
                        // Opus padding loads the transport without audible samples.
                        // A sequence in the ignored padding makes every packet distinct.
                        const int size = packet.size();
                        packet.resize(peer.channel.audioPacketLimit());
                        if (packet.size() - size < 16 || opus_packet_pad(
                            reinterpret_cast<unsigned char*>(packet.data()), size, packet.size()) != OPUS_OK) {
                            accepted = false; send.stop(); return;
                        }
                        qToBigEndian<quint32>(sender, packet.data() + packet.size() - 8);
                        qToBigEndian<quint32>(tick, packet.data() + packet.size() - 4);
                        std::array<float, 1920> decoded{};
                        if (!peer.decoder || opus_decode_float(peer.decoder.get(),
                            reinterpret_cast<const unsigned char*>(packet.constData()), packet.size(), decoded.data(), 960, 0) != 960
                            || !std::all_of(decoded.begin(), decoded.end(), [](float value) { return std::isfinite(value) && std::abs(value) < 1e-6f; })) {
                            accepted = false; send.stop(); return;
                        }
                        if (!peer.channel.sendAudio(packet)) {
                            std::fprintf(stderr, "Client %d rejected frame %d: %s, allowed=%d, bytes=%lld, limit=%d\n",
                                sender + 1, tick, qPrintable(peer.channel.status()), peer.session.transmissionAllowed(),
                                (long long)packet.size(), peer.channel.audioPacketLimit());
                            accepted = false; send.stop(); return;
                        }
                        peer.sent.append(packet);
                    }
                } else if (tick % (frames + pause) == frames) accepted &= peer.channel.sendAudio({});
            }
            ++tick;
        }
        if (tick == ticks) send.stop();
    });
    send.start(batchMs);
    const bool finished = QTest::qWaitFor([&] {
        if (!accepted) return true;
        if (tick != ticks) return false;
        for (int receiver = 0; receiver < clients; ++receiver)
            for (int sender = 0; sender < speakers; ++sender)
                if (receiver != sender && peers[receiver]->received[sender].size() != bursts * frames) return false;
        return true;
    }, ticks * 20 + 5000);
    bool okay = finished && accepted;
    for (int receiver = 0; receiver < clients; ++receiver) {
        auto& peer = *peers[receiver];
        okay &= peer.missing == 0;
        for (int sender = 0; sender < speakers; ++sender) {
            if (receiver == sender) continue;
            const bool exact = peer.received[sender] == peers[sender]->sent;
            okay &= exact;
            std::printf("receiver=%d sender=%d sent=%lld received=%lld exact=%d concealed=%d\n", receiver + 1, sender + 1,
                (long long)peers[sender]->sent.size(), (long long)peer.received[sender].size(), exact, peer.missing);
        }
    }
    std::printf("Three clients, two concurrent stereo speakers, %d ms capture batches: %s\n", batchMs, okay ? "PASS" : "FAIL");
    return okay ? 0 : 24;
}

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
    arguments.addOption({"capture-batch-ms", "Audio callback interval, between 20 and 100 ms.", "milliseconds", "20"});
    arguments.addOption({"stress", "Three clients, two padded silent streams, 19 seconds in an isolated numeric --channel. Any observer must be deafened."});
    arguments.process(app);
    if (arguments.positionalArguments().size() != 1 || !arguments.isSet("state-dir")) arguments.showHelp(2);
    const auto endpoint = arguments.positionalArguments().first();
    const auto target = arguments.value("channel");
    const auto observer = arguments.value("observer");
    bool validInterval = false;
    const auto batchMs = arguments.value("capture-batch-ms").toInt(&validInterval);
    if (!validInterval || batchMs < 20 || batchMs > 100) arguments.showHelp(2);
    if (arguments.isSet("stress")) {
        bool validChannel = false;
        target.toULongLong(&validChannel);
        if (!validChannel || arguments.isSet("reconnect") || arguments.isSet("text-only")) arguments.showHelp(2);
        return stress(endpoint, QDir(arguments.value("state-dir")).absolutePath(), target, observer, batchMs);
    }
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
    sa.setAvatar("mechanic"); sb.setAvatar("courier");
    sa.setMuted(true); sb.setMuted(true); sa.setDeafened(false); sb.setDeafened(false);
    sa.setAudioReadiness(true, true); sb.setAudioReadiness(true, true);
    LocalChannel a(sa, root + "/a.channel", TlsIdentity::loadFile(root + "/a.identity", true));
    LocalChannel b(sb, root + "/b.channel", TlsIdentity::loadFile(root + "/b.identity", true));
    auto check = [](bool okay, const char* label) { std::printf("%s: %s\n", label, okay ? "PASS" : "FAIL"); std::fflush(stdout); return okay; };
    if (!check(a.addTeamSpeak(connectionAddress) && b.addTeamSpeak(connectionAddress), "start")) return 1;
    const auto hasPeer = [](const LocalChannel& channel, const QString& name) {
        const auto members = channel.participants();
        return std::any_of(members.cbegin(), members.cend(), [&](const auto& value) {
            return value.toMap().value("name").toString() == name;
        });
    };
    if (!check(QTest::qWaitFor([&] { return a.joined() && b.joined()
        && hasPeer(a, "SquadSpeak test B") && hasPeer(b, "SquadSpeak test A"); }, 20000), "two real server connections")) {
        std::fprintf(stderr, "A: %s\nB: %s\n", qPrintable(a.status()), qPrintable(b.status())); return 2;
    }
    const auto squadBadges = [&](const LocalChannel& channel) {
        int matched = 0;
        for (const auto& value : channel.participants()) {
            const auto member = value.toMap();
            if (member.value("name") != "SquadSpeak test A" && member.value("name") != "SquadSpeak test B") continue;
            if (member.value("teamSpeak").toBool()) return false;
            if (member.value("avatarId") != (member.value("name") == sa.userName() ? sa.avatar() : sb.avatar())) return false;
            ++matched;
        }
        return matched == 2;
    };
    if (!check(QTest::qWaitFor([&] { return squadBadges(a) && squadBadges(b); }, 5000), "client identity metadata")) return 24;
    sa.setAvatar("mossling");
    if (!check(QTest::qWaitFor([&] { return squadBadges(a) && squadBadges(b); }, 5000), "live portrait update")) return 24;
    QString peer;
    for (const auto& value : a.participants()) if (value.toMap().value("name") == "SquadSpeak test B") peer = value.toMap().value("id").toString();
    if (!check(!peer.isEmpty() && a.openPrivateChat(a.joinedHostId(), peer), "private recipient")) return 3;
    const QString text = "SquadSpeak private integration check";
    QSignalSpy receipt(&a, &LocalChannel::chatSent);
    if (!check(a.sendChat(text) && QTest::qWaitFor([&] { return !receipt.empty() && !b.privateChats().empty(); }, 10000), "private server acknowledgement")) return 4;
    const auto conversation = b.privateChats().first().toMap();
    if (!check(b.openSavedChat(conversation.value("id").toString()) && !b.messages().empty() && b.messages().last().toMap().value("text") == text, "private end-to-end delivery")) return 5;
    if (!check(!a.messages().last().toMap().value("teamSpeak").toBool()
        && !b.messages().last().toMap().value("teamSpeak").toBool(), "private client badges")) return 24;
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
    squad::VoiceMixer encoder(2);
    encoder.setPacketLimit(a.audioPacketLimit());
    int error = 0;
    const std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
        opus_decoder_create(48000, 2, &error), opus_decoder_destroy);
    if (!decoder || error) return 9;
    QList<QByteArray> transmitted, delivered;
    int missing = 0, ends = 0, decoded = 0, tick = 0;
    double energy = 0;
    QElapsedTimer clock; clock.start();
    QList<qint64> arrivals;
    QObject::connect(&b, &LocalChannel::audioReceived, &app, [&](const QString&, const QByteArray& packet, int lost) {
        delivered.append(packet); arrivals.append(clock.elapsed()); missing += lost;
        std::array<float, 1920> pcm{};
        const int count = opus_decode_float(decoder.get(), reinterpret_cast<const unsigned char*>(packet.constData()), int(packet.size()), pcm.data(), 960, 0);
        if (count == 960) { ++decoded; for (const auto sample : pcm) energy += sample * sample; }
    });
    QSignalSpy ownEcho(&a, &LocalChannel::audioReceived);
    QTimer send;
    send.setTimerType(Qt::PreciseTimer);
    constexpr int bursts = 4, speechFrames = 50, pauseFrames = 10;
    constexpr int ticks = bursts * (speechFrames + pauseFrames);
    bool accepted = true;
    QObject::connect(&send, &QTimer::timeout, &app, [&] {
        if (!allowed(a.participants()) || !allowed(b.participants())) { accepted = false; send.stop(); return; }
        // Use elapsed sample time. Delayed callbacks may deliver a short batch,
        // as the real microphone does, rather than silently slowing the fixture.
        const int due = std::min(ticks, int(clock.elapsed() / 20));
        while (tick < due) {
            const int withinBurst = tick % (speechFrames + pauseFrames);
            if (withinBurst < speechFrames) {
                std::array<float, 1920> pcm{};
                for (int i = 0; i < 960; ++i)
                    pcm[size_t(i) * 2] = pcm[size_t(i) * 2 + 1] = float(0.08 * std::sin((tick * 960 + i) * 2 * 3.141592653589793 * 440 / 48000));
                const auto packets = encoder.encode(pcm, 48000);
                for (const auto& packet : packets) {
                    accepted &= a.sendAudio(packet);
                    transmitted.append(packet);
                }
            } else if (withinBurst == speechFrames) {
                // The first and third pause model PTT release. The others model VAD.
                if ((tick / (speechFrames + pauseFrames)) % 2 == 0) sa.setPttKeyHeld(false);
                accepted &= a.sendAudio({}); ++ends;
                accepted &= !a.sendAudio({});
            }
            ++tick;
            if (tick % (speechFrames + pauseFrames) == 0) sa.setPttKeyHeld(true);
        }
        if (tick == ticks) send.stop();
    });
    send.start(batchMs);
    const bool sound = QTest::qWaitFor([&] {
        return !accepted || (tick == ticks && delivered.size() == bursts * speechFrames);
    }, 8000);
    qint64 longestGap = 0;
    qsizetype expected = 0, previous = -1;
    for (qsizetype i = 0; i < delivered.size(); ++i) {
        const auto& packet = delivered[i];
        while (expected < transmitted.size() && transmitted[expected] != packet) {
            std::printf("Missing sent frame %lld\n", (long long)expected); ++expected;
        }
        if (previous >= 0 && previous / speechFrames == expected / speechFrames)
            longestGap = std::max(longestGap, arrivals[i] - arrivals[i - 1]);
        previous = expected++;
    }
    while (expected < transmitted.size()) { std::printf("Missing trailing frame %lld\n", (long long)expected); ++expected; }
    std::printf("Audio sent=%lld received=%lld decoded=%d concealed=%d ends=%d longest-gap-ms=%lld energy=%f ownEcho=%lld accepted=%d status=%s\n",
        (long long)transmitted.size(), (long long)delivered.size(), decoded, missing, ends,
        (long long)longestGap, energy, (long long)ownEcho.size(), accepted, qPrintable(a.status()));
    send.stop(); sa.setMuted(true); a.sendAudio({}); a.leave(); b.leave();
    return check(sound && accepted && delivered == transmitted && decoded == bursts * speechFrames
        && missing == 0 && ends == bursts && energy > 0.01 && ownEcho.isEmpty(),
        "complete Opus bursts and pauses without invented loss or self echo") ? 0 : 10;
}
