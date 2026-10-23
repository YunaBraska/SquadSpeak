#include "local_channel.hpp"
#include "voice_mixer.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QQueue>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <QScopeGuard>
#include <QTest>
#include <QSignalSpy>
#include <QVariant>
#include <cstring>
#include <cmath>
#include <numbers>
#include <numeric>

namespace {
QMutex fakeMutex;
QQueue<QByteArray> fakeEvents;
QByteArray lastOptions;
QByteArray lastCommand;
QByteArray lastChatCommand;
QByteArray pendingRequest;
QByteArray lastAudio;
int openCount = 0;
int closeCount = 0;
int audioCount = 0;
int presenceCount = 0;
int joinCount = 0;

QByteArray stateEvent(bool includePeer = false) {
    QJsonArray members{QJsonObject{{"id", "1"}, {"uid", "member-uid"}, {"name", "Me"}, {"channel", "1"}, {"inputMuted", false}, {"outputMuted", false}}};
    if (includePeer) members.append(QJsonObject{{"id", "7"}, {"uid", "remote-uid"}, {"name", "Remote"}, {"channel", "1"}, {"inputMuted", false}, {"outputMuted", false}});
    return QJsonDocument(QJsonObject{
        {"type", "state"}, {"directoryComplete", true},
        {"server", QJsonObject{{"name", "Fake TS"}, {"uid", "server-uid"}}},
        {"self", "1"}, {"channel", "1"}, {"canSend", true},
        {"channels", QJsonArray{
            QJsonObject{{"id", "1"}, {"parent", "0"}, {"order", "0"}, {"name", "Root"}, {"password", false}, {"codec", 4}},
            QJsonObject{{"id", "2"}, {"parent", "1"}, {"order", "1"}, {"name", "Nested"}, {"password", true}, {"codec", 5}}
        }},
        {"members", members}
    }).toJson(QJsonDocument::Compact);
}

void resetFake() {
    const QMutexLocker lock(&fakeMutex);
    fakeEvents.clear(); lastOptions.clear(); lastCommand.clear(); lastChatCommand.clear(); pendingRequest.clear(); lastAudio.clear(); openCount = 0; closeCount = 0; audioCount = 0; presenceCount = 0; joinCount = 0;
}
}

extern "C" int ss_ts_identity(uint8_t* out, size_t capacity) {
    const QByteArray value = "{\"identity\":true}";
    if (!out || capacity < size_t(value.size())) return -value.size();
    memcpy(out, value.constData(), size_t(value.size()));
    return value.size();
}
extern "C" void* ss_ts_open(const uint8_t* data, size_t size) {
    const QMutexLocker lock(&fakeMutex);
    lastOptions = QByteArray(reinterpret_cast<const char*>(data), qsizetype(size));
    ++openCount;
    return reinterpret_cast<void*>(quintptr(1));
}
extern "C" int ss_ts_command(void*, const uint8_t* data, size_t size) {
    const QByteArray command(reinterpret_cast<const char*>(data), qsizetype(size));
    const QMutexLocker lock(&fakeMutex);
    lastCommand = command;
    const auto object = QJsonDocument::fromJson(command).object();
    if (object.value("action") == "presence") ++presenceCount;
    if (object.value("action") == "join") ++joinCount;
    if (object.value("action").toString() == "chat") {
        lastChatCommand = command;
        pendingRequest = object.value("request").toString().toUtf8();
    }
    return 1;
}
extern "C" int ss_ts_audio(void*, const uint8_t* data, size_t size) {
    ++audioCount;
    lastAudio = QByteArray(reinterpret_cast<const char*>(data), qsizetype(size));
    return 1;
}
extern "C" int ss_ts_next(void*, uint8_t* out, size_t capacity) {
    const QMutexLocker lock(&fakeMutex);
    if (fakeEvents.isEmpty()) return 0;
    const QByteArray json = fakeEvents.head();
    const QByteArray event = json.startsWith(char(2)) ? json : QByteArray(1, char(1)) + json;
    if (capacity < size_t(event.size())) return -event.size();
    fakeEvents.dequeue();
    memcpy(out, event.constData(), size_t(event.size()));
    return event.size();
}
extern "C" void ss_ts_close(void*) { ++closeCount; }
void acknowledgePendingChat() {
    const QMutexLocker lock(&fakeMutex);
    fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "result"}, {"request", QString::fromUtf8(pendingRequest)}, {"ok", true}, {"passwordRequired", false}}).toJson(QJsonDocument::Compact));
    pendingRequest.clear();
}

class TeamSpeakTests final : public QObject {
    Q_OBJECT
private slots:
    void addressDetectsTeamSpeak_data() {
        QTest::addColumn<bool>("explicitPort");
        QTest::newRow("default-port") << false;
        QTest::newRow("explicit-port") << true;
    }

    void addressDetectsTeamSpeak() {
        QFETCH(bool, explicitPort);
        resetFake();
        QTcpServer unused;
        QVERIFY(unused.listen(QHostAddress::LocalHost, explicitPort ? 0 : LocalChannel::defaultPort));
        const auto port = unused.serverPort();
        unused.close();
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.openAddress(explicitPort ? "127.0.0.1:" + QString::number(port) : "127.0.0.1"));
        QTRY_COMPARE(openCount, 1);
        const auto options = QJsonDocument::fromJson(lastOptions).object();
        QCOMPARE(options.value("address").toString(), "127.0.0.1:" + QString::number(explicitPort ? port : 9987));
        QVERIFY(options.value("password").toString().isEmpty());
        fakeEvents.enqueue(stateEvent());
        QTRY_VERIFY(channel.joined());
        QVERIFY(channel.teamSpeakChat());
        QVERIFY(channel.leave());
    }

    void nativeNameResolutionFailureStillTriesTeamSpeak() {
        resetFake();
        QTcpServer resolver;
        QVERIFY(resolver.listen(QHostAddress::LocalHost, 0));
        connect(&resolver, &QTcpServer::newConnection, &resolver, [&] {
            auto* socket = resolver.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket, greeted = false]() mutable {
                if (!greeted) {
                    if (socket->bytesAvailable() < 2) return;
                    const auto header = socket->peek(2);
                    if (socket->bytesAvailable() < 2 + quint8(header[1])) return;
                    socket->read(2 + quint8(header[1]));
                    socket->write(QByteArray::fromHex("0500")); greeted = true;
                } else {
                    // SOCKS5's host-unreachable reply exercises Qt's resolver error path.
                    socket->readAll();
                    socket->write(QByteArray::fromHex("05040001000000000000"));
                    socket->disconnectFromHost();
                }
            });
        });
        const auto previous = QNetworkProxy::applicationProxy();
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", resolver.serverPort()));
        const auto restore = qScopeGuard([&] { QNetworkProxy::setApplicationProxy(previous); });
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.openAddress("voice.example"));
        QTRY_COMPARE(openCount, 1);
        QCOMPARE(QJsonDocument::fromJson(lastOptions).object().value("address").toString(), QString("voice.example:9987"));
    }

    void nativeAddressKeepsChatOnlyAccessAndRejectsIdentityChanges() {
        resetFake();
        QTemporaryDir dir;
        VoiceSession hostSession(dir.filePath("host-session")), session(dir.filePath("session"));
        auto host = std::make_unique<LocalChannel>(hostSession, dir.filePath("host"), TlsIdentity::create());
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(host->listen(QHostAddress::LocalHost, 0));
        const auto port = host->port();
        const auto address = "127.0.0.1:" + QString::number(port);
        QVERIFY(host->decide(channel.ownId(), true));
        QVERIFY(channel.openAddress(address));
        QTRY_VERIFY(channel.chatReady());
        QVERIFY(!channel.joined());
        QCOMPARE(openCount, 0);
        host.reset();
        QTRY_VERIFY(!channel.chatReady());
        VoiceSession replacementSession(dir.filePath("replacement-session"));
        LocalChannel replacement(replacementSession, dir.filePath("replacement"), TlsIdentity::create());
        QVERIFY(replacement.listen(QHostAddress::LocalHost, port));
        QVERIFY(channel.openAddress(address));
        QTRY_VERIFY(!channel.directBusy());
        QVERIFY(channel.status().contains("identity"));
        QCOMPARE(openCount, 0);
        QVERIFY(!channel.teamSpeakChat());
    }

    void remotePttUsesTheTargetsTeamSpeakConnection() {
        resetFake();
        QTemporaryDir dir;
        VoiceSession targetSession(dir.filePath("target-session")), controllerSession(dir.filePath("controller-session"));
        LocalChannel target(targetSession, dir.filePath("target"), TlsIdentity::create());
        LocalChannel controller(controllerSession, dir.filePath("controller"), TlsIdentity::create());
        QVERIFY(target.startService(QHostAddress::LocalHost, 0));
        fakeEvents.enqueue(stateEvent());
        QVERIFY(target.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(target.joined());
        const auto connectedIdentity = QJsonDocument::fromJson(lastOptions).object().value("identity");
        QVERIFY(targetSession.setPushToTalk(true));
        QVERIFY(targetSession.setMuted(false));
        QVERIFY(controller.pairAddress("127.0.0.1:" + QString::number(target.port())));
        QTRY_COMPARE(target.controlRequests().size(), 1);
        QVERIFY(target.decideControl(controller.ownId(), true));
        QTRY_VERIFY(controller.controlConnected());
        QVERIFY(!target.sendAudio("packet"));
        QVERIFY(controllerSession.setPttKeyHeld(true));
        QTRY_VERIFY(targetSession.transmissionAllowed());
        QVERIFY(target.sendAudio("packet"));
        QCOMPARE(audioCount, 1);
        QCOMPARE(openCount, 1);
        QCOMPARE(QJsonDocument::fromJson(lastOptions).object().value("identity"), connectedIdentity);
        QVERIFY(!controller.joined());
        QVERIFY(!controller.sendAudio("packet"));
        QVERIFY(controllerSession.setPttKeyHeld(false));
        QTRY_VERIFY(!targetSession.transmissionAllowed());
        QVERIFY(!target.sendAudio("packet"));
        QCOMPARE(audioCount, 1);
    }

    void invalidEndpointIsRejected() {
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.initialize());
        QVERIFY(!channel.addTeamSpeak("http://not-team-speak", {}));
        QVERIFY(!channel.teamSpeakChat());
    }

    void stateSnapshotBuildsNestedSavedChannels() {
        resetFake();
        {
            const QMutexLocker lock(&fakeMutex);
            fakeEvents.enqueue(stateEvent());
        }
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.initialize());
        QVERIFY(channel.addTeamSpeak("ts3://127.0.0.1:9987", {}));
        QTRY_VERIFY_WITH_TIMEOUT(channel.chatReady(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(channel.savedChannels().size() >= 2, 1000);
        bool nested = false;
        for (const auto& value : channel.savedChannels()) {
            const auto map = value.toMap();
            if (map.value("name") == "Nested" && !map.value("parent").toString().isEmpty()) nested = true;
        }
        QVERIFY(nested);
        QMutexLocker lock(&fakeMutex);
        QVERIFY(lastOptions.contains("inputMuted"));
    }

    void presenceSkipsLocalPreferencesAndRestoresAfterReconnect() {
        resetFake(); fakeEvents.enqueue(stateEvent());
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        QVERIFY(session.setMuted(false));
        const auto before = presenceCount;
        QVERIFY(session.setTheme("dark"));
        QVERIFY(session.setPushToTalk(true));
        QVERIFY(session.setPttKeyHeld(true));
        QCOMPARE(presenceCount, before);
        QVERIFY(session.setDeafened(true));
        QCOMPARE(presenceCount, before + 1);
        fakeEvents.enqueue("{\"type\":\"reconnecting\"}");
        QTRY_VERIFY(!channel.joined());
        QVERIFY(session.setMuted(true));
        QCOMPARE(presenceCount, before + 1);
        fakeEvents.enqueue(stateEvent());
        QTRY_VERIFY(channel.joined());
        QCOMPARE(presenceCount, before + 2);
        const auto current = QJsonDocument::fromJson(lastCommand).object();
        QVERIFY(current.value("inputMuted").toBool());
        QVERIFY(current.value("outputMuted").toBool());
    }

    void portraitsFollowIdentityAcrossNicknameAndClientIdChanges() {
        resetFake();
        auto state = QJsonDocument::fromJson(stateEvent(true)).object();
        auto members = state.value("members").toArray();
        for (int i = 8; i < 28; ++i) members.append(QJsonObject{{"id", QString::number(i)},
            {"uid", QString("peer-%1").arg(i)}, {"name", QString("Peer %1").arg(i)}, {"channel", "1"}});
        state.insert("members", members);
        fakeEvents.enqueue(QJsonDocument(state).toJson(QJsonDocument::Compact));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        QVERIFY(session.setAvatar("coral-diver"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_COMPARE(channel.participants().size(), 22);
        QSet<QString> portraits;
        QString peer, avatar;
        for (const auto& value : channel.participants()) {
            const auto member = value.toMap();
            QCOMPARE(member.value("teamSpeak").toBool(), member.value("id") != channel.ownId());
            const auto selected = member.value("avatarId").toString();
            QVERIFY(VoiceSession::avatars().contains(selected));
            portraits.insert(selected);
            if (member.value("id") == channel.ownId()) QCOMPARE(selected, session.avatar());
            if (member.value("tsUid") == "remote-uid") { peer = member.value("id").toString(); avatar = selected; }
        }
        QVERIFY(portraits.size() > 1);
        auto renamed = members.at(1).toObject();
        renamed.insert("id", "70"); renamed.insert("name", "Renamed"); members[1] = renamed;
        state.insert("members", members);
        fakeEvents.enqueue(QJsonDocument(state).toJson(QJsonDocument::Compact));
        QTRY_VERIFY([&] {
            for (const auto& value : channel.participants()) {
                const auto member = value.toMap();
                if (member.value("id") == peer) return member.value("name") == "Renamed" && member.value("avatarId") == avatar;
            }
            return false;
        }());
        QVERIFY(channel.openPrivateChat(channel.joinedHostId(), peer));
        fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "message"}, {"mode", "private"}, {"fromId", "70"},
            {"fromName", "Renamed"}, {"text", "Same portrait"}}).toJson(QJsonDocument::Compact));
        QTRY_COMPARE(channel.messages().size(), 1);
        QCOMPARE(channel.messages().first().toMap().value("avatarId").toString(), avatar);
        QVERIFY(channel.messages().first().toMap().value("teamSpeak").toBool());
    }

    void clientBadgesFollowTheApplicationAcrossChatsAndReconnects() {
        resetFake();
        auto state = QJsonDocument::fromJson(stateEvent(true)).object();
        auto members = state.value("members").toArray();
        auto other = members.at(1).toObject();
        other.insert("squadSpeak", true); members[1] = other;
        members.append(QJsonObject{{"id", "8"}, {"uid", "official-uid"}, {"name", "SquadSpeak nickname"}, {"channel", "1"}});
        state.insert("members", members);
        fakeEvents.enqueue(QJsonDocument(state).toJson(QJsonDocument::Compact));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_COMPARE(channel.participants().size(), 3);
        QString peer;
        for (const auto& value : channel.participants()) {
            const auto member = value.toMap();
            QCOMPARE(member.value("teamSpeak").toBool(), member.value("tsClient") == "8");
            if (member.value("tsClient") == "7") peer = member.value("id").toString();
        }
        const auto joined = channel.joinedHostId();
        for (const auto& mode : {QStringLiteral("channel"), QStringLiteral("private")}) {
            if (mode == "private") QVERIFY(channel.openPrivateChat(joined, peer));
            const auto previous = channel.messages().size();
            fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "message"}, {"mode", mode}, {"channel", "1"},
                {"fromId", "7"}, {"fromUid", "remote-uid"}, {"fromName", "Remote"}, {"text", "Hello"}}).toJson(QJsonDocument::Compact));
            QTRY_COMPARE(channel.messages().size(), previous + 1);
            QVERIFY(!channel.messages().last().toMap().value("teamSpeak").toBool());
            QVERIFY(channel.sendChat("Reply")); acknowledgePendingChat();
            QTRY_COMPARE(channel.messages().size(), previous + 2);
            QVERIFY(!channel.messages().last().toMap().value("teamSpeak").toBool());
        }
        fakeEvents.enqueue("{\"type\":\"reconnecting\"}");
        QTRY_VERIFY(!channel.joined());
        other.insert("id", "70"); other.insert("name", "Renamed"); members[1] = other;
        state.insert("members", members);
        fakeEvents.enqueue(QJsonDocument(state).toJson(QJsonDocument::Compact));
        QTRY_VERIFY(channel.joined());
        QVERIFY(channel.openSavedChat(joined));
        QTRY_VERIFY([&] {
            for (const auto& value : channel.participants()) {
                const auto member = value.toMap();
                if (member.value("id") == peer) return member.value("tsClient") == "70" && !member.value("teamSpeak").toBool();
            }
            return false;
        }());
    }

    void repeatedJoinReusesConnection() {
        resetFake();
        {
            const QMutexLocker lock(&fakeMutex);
            fakeEvents.enqueue(stateEvent());
        }
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.initialize());
        QVERIFY(channel.addTeamSpeak("ts3://127.0.0.1:9987", {}));
        QTRY_VERIFY_WITH_TIMEOUT(channel.chatReady(), 1000);
        const int firstOpenCount = openCount;
        QVERIFY(channel.addTeamSpeak("ts3://127.0.0.1:9987", {}));
        QCOMPARE(openCount, firstOpenCount);
    }

    void identityAndServerPinPersistAcrossReconnect() {
        resetFake();
        {
            const QMutexLocker lock(&fakeMutex);
            fakeEvents.enqueue(stateEvent());
        }
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.initialize());
        QVERIFY(channel.addTeamSpeak("ts3://127.0.0.1:9987", {}));
        QTRY_VERIFY_WITH_TIMEOUT(channel.chatReady(), 1000);
        QVERIFY(channel.leave());
        {
            const QMutexLocker lock(&fakeMutex);
            fakeEvents.enqueue(stateEvent());
        }
        QVERIFY(channel.addTeamSpeak("ts3://127.0.0.1:9987", {}));
        QTRY_VERIFY_WITH_TIMEOUT(channel.chatReady(), 1000);
        QMutexLocker lock(&fakeMutex);
        QVERIFY(openCount >= 2);
        QVERIFY(lastOptions.contains("serverKey"));
        QVERIFY(lastOptions.contains("server-uid"));
        QVERIFY(lastOptions.contains("identity"));
    }

    void chatIsRecordedOnlyAfterServerAck() {
        resetFake();
        {
            const QMutexLocker lock(&fakeMutex);
            fakeEvents.enqueue(stateEvent());
        }
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        QVERIFY(session.setUserName("Tester"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.initialize());
        QVERIFY(channel.addTeamSpeak("ts3://127.0.0.1:9987", {}));
        QTRY_VERIFY_WITH_TIMEOUT(channel.chatReady(), 1000);
        QVERIFY(channel.sendChat("hello"));
        QVERIFY(channel.messages().isEmpty());
        acknowledgePendingChat();
        QTRY_VERIFY_WITH_TIMEOUT(!channel.messages().isEmpty(), 1000);
        QMutexLocker lock(&fakeMutex);
        QVERIFY(lastChatCommand.contains("\"action\":\"chat\""));
    }

    void unreadMessagesRequireFreshCurrentChatRead_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("channel") << QString("channel");
        QTest::newRow("server") << QString("server");
    }

    void unreadMessagesRequireFreshCurrentChatRead() {
        QFETCH(QString, mode);
        resetFake();
        fakeEvents.enqueue(stateEvent(true));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.initialize());
        QVERIFY(channel.addTeamSpeak("127.0.0.1:9987"));
        QTRY_VERIFY_WITH_TIMEOUT(channel.chatReady(), 1000);
        const auto host = channel.joinedHostId();
        QString peer;
        for (const auto& value : channel.participants())
            if (value.toMap().value("tsUid") == "remote-uid") peer = value.toMap().value("id").toString();
        QVERIFY(!peer.isEmpty());
        const auto unread = [&](const QString& id) {
            for (const auto& value : channel.savedChannels())
                if (value.toMap().value("id") == id) return value.toMap().value("unread").toBool();
            for (const auto& value : channel.privateChats())
                if (value.toMap().value("id") == id) return value.toMap().value("unread").toBool();
            return false;
        };
        QVERIFY(!unread(host));
        fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "message"}, {"mode", mode}, {"channel", "1"},
            {"fromId", "7"}, {"fromUid", "remote-uid"}, {"fromName", "Remote"}, {"text", "new"}}).toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(unread(host), 1000);
        const auto sequence = channel.messages().first().toMap().value("sequence").toLongLong();
        QVERIFY(sequence > 0);
        QVERIFY(!channel.markChatRead(host, -1));
        QVERIFY(!channel.markChatRead(host, sequence - 1));
        QVERIFY(!channel.markChatRead(host, sequence + 1));
        QVERIFY(!channel.markChatRead(QStringLiteral("wrong"), sequence));
        QVERIFY(channel.markChatRead(host, sequence));
        QVERIFY(!unread(host));
        QVERIFY(channel.markChatRead(host, sequence));

        QVERIFY(channel.sendChat("self"));
        acknowledgePendingChat();
        QTRY_VERIFY_WITH_TIMEOUT(!channel.messages().isEmpty(), 1000);
        QVERIFY(!unread(host));

        QVERIFY(channel.openPrivateChat(host, peer));
        QTRY_VERIFY_WITH_TIMEOUT(channel.chatReady(), 1000);
        fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "message"}, {"mode", "private"}, {"fromId", "7"},
            {"fromUid", "remote-uid"}, {"fromName", "Remote"}, {"text", "private"}}).toJson(QJsonDocument::Compact));
        QTRY_VERIFY_WITH_TIMEOUT(channel.messages().size() == 1 && unread(channel.chatHostId()), 1000);
        const auto privateHost = channel.chatHostId();
        const auto privateSequence = channel.messages().first().toMap().value("sequence").toLongLong();
        QVERIFY(!channel.markChatRead(host, privateSequence));
        QVERIFY(channel.markChatRead(privateHost, privateSequence));
        QVERIFY(!unread(privateHost));
    }

    void privateMessageUsesStableRemoteIdentity_data() {
        QTest::addColumn<QString>("messageUid");
        QTest::newRow("message-identity") << QString("remote-uid");
        QTest::newRow("roster-identity") << QString{};
    }

    void privateMessageUsesStableRemoteIdentity() {
        QFETCH(QString, messageUid);
        resetFake();
        {
            const QMutexLocker lock(&fakeMutex);
            fakeEvents.enqueue(stateEvent(true));
        }
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.initialize());
        QVERIFY(channel.addTeamSpeak("ts3://127.0.0.1:9987", {}));
        QTRY_VERIFY_WITH_TIMEOUT(channel.chatReady(), 1000);
        const auto server = QString::fromLatin1(QCryptographicHash::hash("ts3/server/127.0.0.1:9987", QCryptographicHash::Sha256).toHex());
        const auto host = QString::fromLatin1(QCryptographicHash::hash((server + "/channel/1").toUtf8(), QCryptographicHash::Sha256).toHex());
        const auto peer = QString::fromLatin1(QCryptographicHash::hash((server + "/member/remote-uid").toUtf8(), QCryptographicHash::Sha256).toHex());
        QVERIFY(channel.openPrivateChat(host, peer));
        QTRY_VERIFY_WITH_TIMEOUT(channel.chatReady(), 1000);
        {
            const QMutexLocker lock(&fakeMutex);
            fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "message"}, {"mode", "private"}, {"fromId", "7"}, {"fromUid", messageUid}, {"fromName", "Remote"}, {"text", "hello privately"}}).toJson(QJsonDocument::Compact));
        }
        QTRY_VERIFY_WITH_TIMEOUT(channel.messages().size() == 1, 1000);
        QCOMPARE(channel.messages().first().toMap().value("sender").toString(), peer);
        QCOMPARE(channel.messages().first().toMap().value("text").toString(), QString("hello privately"));
        QCOMPARE(channel.privateChats().size(), 1);
        QVERIFY(channel.sendChat("reply"));
        {
            const QMutexLocker lock(&fakeMutex);
            const auto command = QJsonDocument::fromJson(lastChatCommand).object();
            QCOMPARE(command.value("target").toString(), QString("private"));
            QCOMPARE(command.value("client").toInt(), 7);
        }
        acknowledgePendingChat();
        QTRY_COMPARE_WITH_TIMEOUT(channel.messages().size(), 2, 1000);
    }

    void encodedVoiceReachesBridgeUnchanged_data() {
        QTest::addColumn<int>("codec");
        QTest::newRow("opus-voice") << 4;
        QTest::newRow("opus-music") << 5;
    }

    void encodedVoiceReachesBridgeUnchanged() {
        QFETCH(int, codec);
        resetFake();
        auto state = QJsonDocument::fromJson(stateEvent()).object();
        auto channels = state.value("channels").toArray();
        auto current = channels[0].toObject(); current.insert("codec", codec); channels[0] = current;
        state.insert("channels", channels);
        fakeEvents.enqueue(QJsonDocument(state).toJson(QJsonDocument::Compact));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        session.setMuted(false); session.setPushToTalk(true); session.setAudioReadiness(true, true);
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        QVERIFY(session.setPttKeyHeld(true));
        squad::VoiceMixer capture(2);
        QCOMPARE(channel.audioPacketLimit(), 484);
        capture.setPacketLimit(channel.audioPacketLimit());
        int error = 0;
        const int outputChannels = codec == 4 ? 1 : 2;
        const std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
            opus_decoder_create(48000, outputChannels, &error), opus_decoder_destroy);
        QVERIFY(decoder && error == OPUS_OK);
        qint64 bytes = 0;
        double outputEnergy = 0;
        std::array<float, 1920> input{}, output{};
        for (int frame = 0; frame < 100; ++frame) {
            for (int sample = 0; sample < 960; ++sample) {
                const auto time = double(frame * 960 + sample) / 48000;
                float value = 0;
                for (const int frequency : {173, 439, 997, 2039, 4001, 7907, 12101, 17011})
                    value += float(0.06 * std::sin(2 * std::numbers::pi * frequency * time));
                input[size_t(sample) * 2] = input[size_t(sample) * 2 + 1] = value;
            }
            const auto packets = capture.encode(input, 48000);
            QCOMPARE(packets.size(), 1);
            QVERIFY2(packets.first().size() <= 484, qPrintable(QString::number(packets.first().size())));
            QVERIFY(channel.sendAudio(packets.first()));
            QCOMPARE(lastAudio, packets.first());
            const auto* data = reinterpret_cast<const unsigned char*>(lastAudio.constData());
            QCOMPARE(opus_packet_get_nb_samples(data, int(lastAudio.size()), 48000), 960);
            QCOMPARE(opus_packet_get_bandwidth(data), OPUS_BANDWIDTH_FULLBAND);
            QCOMPARE(opus_decode_float(decoder.get(), data, int(lastAudio.size()), output.data(), 960, 0), 960);
            outputEnergy += std::inner_product(output.begin(), output.begin() + 960 * outputChannels, output.begin(), 0.0);
            bytes += lastAudio.size();
        }
        QCOMPARE(audioCount, 100);
        QVERIFY(!channel.sendAudio(QByteArray(485, 'x')));
        QCOMPARE(audioCount, 100);
        const auto bitrate = bytes * 4;
        QVERIFY2(bitrate > 96000 && bitrate < 180000, qPrintable(QString::number(bitrate)));
        const auto rms = std::sqrt(outputEnergy / (96000 * outputChannels));
        QVERIFY2(rms > 0.10 && rms < 0.14, qPrintable(QString::number(rms)));
        QVERIFY(session.setPttKeyHeld(false));
        QVERIFY(!channel.sendAudio(lastAudio));
        QCOMPARE(audioCount, 100);
        QVERIFY(channel.leave());
        QCOMPARE(channel.audioPacketLimit(), 4000);
        capture.setPacketLimit(channel.audioPacketLimit()).resetCapture();
        QVERIFY(capture.encode(input, 48000).first().size() > 484);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, capture.setPacketLimit(0));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, capture.setPacketLimit(4001));
    }

    void pttAndReceiveGatesUseExistingSession() {
        resetFake();
        fakeEvents.enqueue(stateEvent(true));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        session.setMuted(false); session.setPushToTalk(true); session.setAudioReadiness(true, true);
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        QVERIFY(!channel.sendAudio("opus"));
        session.setPttKeyHeld(true);
        QVERIFY(channel.sendAudio("opus"));
        QCOMPARE(audioCount, 1);
        session.setPttKeyHeld(false);
        QVERIFY(!channel.sendAudio("opus"));
        QSignalSpy received(&channel, &LocalChannel::audioReceived);
        const auto packet = [](int from, int sequence) {
            QByteArray data; data.append(char(2)); data.append(char(0)); data.append(char(from));
            data.append(char(0)); data.append(char(sequence)); data.append(char(4)); data.append("opus"); return data;
        };
        fakeEvents.enqueue(packet(1, 0)); // Never replay our own voice.
        fakeEvents.enqueue(packet(7, 0));
        fakeEvents.enqueue(packet(7, 0)); // Duplicate packet.
        fakeEvents.enqueue(packet(7, 2));
        QTRY_COMPARE(received.size(), 2);
        QCOMPARE(received.last().at(2).toInt(), 1);
        session.setDeafened(true);
        fakeEvents.enqueue(packet(7, 3));
        fakeEvents.enqueue(stateEvent(true));
        QSignalSpy changed(&channel, &LocalChannel::hostsChanged);
        QTRY_VERIFY(!changed.empty());
        QCOMPARE(received.size(), 2);
    }

    void reorderedVoicePreservesEveryPacket_data() {
        QTest::addColumn<int>("firstSequence");
        QTest::newRow("ordinary") << 100;
        QTest::newRow("wrap") << 65534;
    }
    void reorderedVoicePreservesEveryPacket() {
        QFETCH(int, firstSequence);
        resetFake(); fakeEvents.enqueue(stateEvent(true));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        QSignalSpy received(&channel, &LocalChannel::audioReceived);
        const auto enqueue = [&](int offset, const QByteArray& payload) {
            const auto sequence = quint16(firstSequence + offset);
            QByteArray event(6, '\0'); event[0] = 2; event[2] = 7; event[5] = 4;
            event[3] = char(sequence >> 8); event[4] = char(sequence & 255);
            fakeEvents.enqueue(event + payload);
        };
        enqueue(0, "start");
        QTRY_COMPARE(received.size(), 1);
        // Real TS3 capture: a later packet precedes several earlier packets.
        enqueue(4, "fourth"); enqueue(2, "second"); enqueue(1, "first");
        enqueue(3, "third"); enqueue(2, "second");
        enqueue(5, {}); enqueue(7, "last"); enqueue(6, "next");
        QTRY_COMPARE(received.size(), 7);
        const QList<QByteArray> expected{"start", "first", "second", "third", "fourth", "next", "last"};
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QCOMPARE(received[i][1].toByteArray(), expected[i]);
            QCOMPARE(received[i][2].toInt(), 0);
        }
        enqueue(1, "first"); // Already played packets cannot replay.
        fakeEvents.enqueue(stateEvent(true));
        QSignalSpy changed(&channel, &LocalChannel::hostsChanged);
        QTRY_VERIFY(!changed.empty());
        QCOMPARE(received.size(), 7);
        for (int offset = 20; offset <= 25; ++offset) enqueue(offset, QByteArray::number(offset));
        enqueue(19, "oldest"); // Under queue pressure, include the newly arrived oldest packet.
        QTRY_COMPARE(received.size(), 14);
        QCOMPARE(received[7][1].toByteArray(), QByteArray("oldest"));
        QCOMPARE(received[7][2].toInt(), 6); // A real gap still has bounded concealment.
        for (int i = 8; i < 14; ++i) {
            QCOMPARE(received[i][1].toByteArray(), QByteArray::number(i + 12));
            QCOMPARE(received[i][2].toInt(), 0);
        }
    }

    void queuedVoiceCannotSurviveMembershipChanges_data() {
        QTest::addColumn<QString>("change");
        for (const auto* value : {"deafen", "peer-left", "peer-muted", "channel", "reconnect"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }
    void queuedVoiceCannotSurviveMembershipChanges() {
        QFETCH(QString, change);
        resetFake(); fakeEvents.enqueue(stateEvent(true));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        QSignalSpy received(&channel, &LocalChannel::audioReceived);
        const auto enqueue = [&](int sequence, const QByteArray& payload) {
            QByteArray event(6, '\0'); event[0] = 2; event[2] = 7; event[4] = char(sequence); event[5] = 4;
            fakeEvents.enqueue(event + payload);
        };
        enqueue(0, "first");
        QTRY_COMPARE(received.size(), 1);
        enqueue(2, "discard"); // Gap keeps this packet queued until the state change.
        auto state = QJsonDocument::fromJson(stateEvent(true)).object();
        auto members = state.value("members").toArray();
        if (change == "peer-left") members.removeAt(1);
        if (change == "peer-muted") {
            auto peer = members[1].toObject(); peer.insert("inputMuted", true); members[1] = peer;
        }
        if (change == "channel") state.insert("channel", "2");
        state.insert("members", members);
        if (change == "reconnect") fakeEvents.enqueue("{\"type\":\"reconnecting\"}");
        bool deafened = false;
        const auto toggle = connect(&channel, &LocalChannel::hostsChanged, &channel, [&] {
            if (change != "deafen" || deafened) return;
            deafened = true;
            session.setDeafened(true);
            session.setDeafened(false);
        });
        fakeEvents.enqueue(QJsonDocument(state).toJson(QJsonDocument::Compact));
        fakeEvents.enqueue(stateEvent(true));
        enqueue(3, "fresh");
        QTRY_COMPARE(received.size(), 2);
        disconnect(toggle);
        QCOMPARE(received.last()[1].toByteArray(), QByteArray("fresh"));
        QCOMPARE(received.last()[2].toInt(), 0);
    }

    void speechEndIsSentOnceEvenAfterPttRelease() {
        resetFake(); fakeEvents.enqueue(stateEvent(true));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        session.setMuted(false); session.setPushToTalk(true); session.setAudioReadiness(true, true);
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        QVERIFY(!channel.sendAudio({}));
        QCOMPARE(audioCount, 0);
        session.setPttKeyHeld(true);
        QVERIFY(channel.sendAudio("opus"));
        session.setPttKeyHeld(false);
        QVERIFY(channel.sendAudio({}));
        QVERIFY(lastAudio.isEmpty());
        QCOMPARE(audioCount, 2);
        QVERIFY(!channel.sendAudio({}));
        QVERIFY(!channel.sendAudio("opus"));
        QCOMPARE(audioCount, 2);
        session.setPttKeyHeld(true);
        QVERIFY(channel.sendAudio("next"));
        session.setMuted(true);
        QVERIFY(channel.sendAudio({}));
        QCOMPARE(audioCount, 4);
    }

    void speechEndDoesNotInventLostVoiceFrames_data() {
        QTest::addColumn<int>("endSize");
        QTest::newRow("empty-end") << 0;
        QTest::newRow("one-byte-end") << 1;
    }

    void speechEndDoesNotInventLostVoiceFrames() {
        QFETCH(int, endSize);
        resetFake(); fakeEvents.enqueue(stateEvent(true));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        QSignalSpy received(&channel, &LocalChannel::audioReceived);
        const auto enqueue = [&](int sequence, const QByteArray& payload) {
            QByteArray event; event.append(char(2)); event.append(char(0)); event.append(char(7));
            event.append(char(sequence >> 8)); event.append(char(sequence & 0xff)); event.append(char(4));
            fakeEvents.enqueue(event + payload);
        };
        enqueue(65534, "first");
        enqueue(65535, QByteArray(endSize, '\0'));
        enqueue(65535, QByteArray(endSize, '\0')); // Repeated ends are harmless.
        enqueue(0, "second");
        enqueue(65535, QByteArray(endSize, '\0')); // Late end cannot erase newer audio.
        enqueue(2, "third"); // This is a real gap and must still be concealed.
        QTRY_COMPARE(received.size(), 3);
        QCOMPARE(received[0][1].toByteArray(), QByteArray("first"));
        QCOMPARE(received[1][1].toByteArray(), QByteArray("second"));
        QCOMPARE(received[1][2].toInt(), 0);
        QCOMPARE(received[2][2].toInt(), 1);
    }

    void voicePlaybackRecoversAfterPacketLoss_data() {
        QTest::addColumn<int>("lost");
        QTest::addColumn<int>("firstSequence");
        QTest::addColumn<bool>("speechPause");
        for (bool pause : {false, true}) {
            const auto suffix = pause ? "-pause" : "-continuous";
            for (int lost : {0, 6, 7, 10, 50})
                QTest::newRow(qPrintable(QString::number(lost) + "-lost" + suffix)) << lost << 100 << pause;
            QTest::newRow(qPrintable(QString("sequence-wrap") + suffix)) << 10 << 65530 << pause;
        }
    }

    void voicePlaybackRecoversAfterPacketLoss() {
        QFETCH(int, lost);
        QFETCH(int, firstSequence);
        QFETCH(bool, speechPause);
        resetFake(); fakeEvents.enqueue(stateEvent(true));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        squad::VoiceMixer source(2), receiver(2);
        receiver.setAutomatic(false);
        std::array<float, 1920> samples{};
        QList<QByteArray> packets;
        for (int frame = 0; frame < lost + 3; ++frame) {
            for (size_t i = 0; i < samples.size() / 2; ++i)
                samples[i * 2] = samples[i * 2 + 1] = float(0.2 * std::sin(2 * std::numbers::pi * 440 * double(frame * 960 + i) / 48000));
            packets.append(source.encode(samples, 48000));
        }
        QCOMPARE(packets.size(), lost + 3);
        QList<bool> decoded;
        QList<double> levels;
        qint64 now = 1000;
        connect(&channel, &LocalChannel::audioReceived, &channel, [&](const QString& peer, const QByteArray& packet, int missing) {
            if (speechPause) now += 1000;
            decoded.append(receiver.receive(peer, packet, now, missing));
            std::vector<float> output;
            // Continuous loss is concealed. After a drained speech pause,
            // playback resumes directly with the newly received voice frame.
            for (int frame = 0; frame < (speechPause ? 1 : missing + 1); ++frame, now += 20)
                output = receiver.render(48000, now);
            levels.append(std::sqrt(std::inner_product(output.begin(), output.end(), output.begin(), 0.0) / double(output.size())));
            const auto drained = receiver.render(48000, now);
            QVERIFY(std::all_of(drained.begin(), drained.end(), [](float sample) { return sample == 0; }));
        });
        const auto enqueue = [&](int offset) {
            const auto sequence = quint16(firstSequence + offset);
            QByteArray event; event.append(char(2)); event.append(char(0)); event.append(char(7));
            event.append(char(sequence >> 8)); event.append(char(sequence & 0xff)); event.append(char(4));
            event.append(packets[offset]);
            const QMutexLocker lock(&fakeMutex);
            fakeEvents.enqueue(event);
        };
        enqueue(0);
        enqueue(lost + 1);
        enqueue(lost + 1); // Duplicate and older frames cannot replay sound.
        enqueue(0);
        enqueue(lost + 2);
        QTRY_COMPARE(decoded.size(), 3);
        QCOMPARE(decoded, (QList<bool>{true, true, true}));
        for (const auto level : levels) QVERIFY2(level > 0.001, qPrintable(QString::number(level)));
    }

    void savedChannelJoinWaitsForIncrementalServerDirectory() {
        resetFake();
        auto state = QJsonDocument::fromJson(stateEvent()).object();
        auto channels = state.value("channels").toArray();
        auto nestedInfo = channels[1].toObject(); nestedInfo.insert("password", false);
        channels[1] = nestedInfo; state.insert("channels", channels);
        fakeEvents.enqueue(QJsonDocument(state).toJson());
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        const auto root = channel.joinedHostId();
        QString nested;
        for (const auto& value : channel.savedChannels())
            if (value.toMap().value("name") == "Nested") nested = value.toMap().value("id").toString();
        QVERIFY(!nested.isEmpty());
        QVERIFY(channel.setAutoJoin(nested, true));
        QVERIFY(channel.leave());
        QVERIFY(channel.joinSaved(nested));
        auto partial = state; partial.insert("directoryComplete", false);
        partial.insert("channel", ""); partial.insert("members", QJsonArray{});
        fakeEvents.enqueue(QJsonDocument(partial).toJson());
        QTRY_VERIFY(fakeEvents.isEmpty());
        QCOMPARE(joinCount, 0);
        QVERIFY(!channel.joined());
        partial.insert("channels", QJsonArray{});
        fakeEvents.enqueue(QJsonDocument(partial).toJson());
        partial = state; partial.insert("directoryComplete", false); partial.insert("channels", QJsonArray{channels[0]});
        fakeEvents.enqueue(QJsonDocument(partial).toJson());
        QTRY_COMPARE(channel.joinedHostId(), root);
        QCOMPARE(joinCount, 0);
        fakeEvents.enqueue(QJsonDocument(state).toJson());
        QTRY_COMPARE(joinCount, 1);
        const auto saved = channel.savedChannels();
        QVERIFY(std::any_of(saved.cbegin(), saved.cend(), [&](const auto& item) {
            const auto entry = item.toMap(); return entry.value("id") == nested && entry.value("autoJoin").toBool();
        }));
        QCOMPARE(QJsonDocument::fromJson(lastCommand).object().value("channel").toString(), QString("2"));
        const auto request = QJsonDocument::fromJson(lastCommand).object().value("request");
        // Repeated server updates and clicks must not duplicate a pending move.
        fakeEvents.enqueue(QJsonDocument(state).toJson());
        QVERIFY(channel.joinSaved(nested));
        fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "result"}, {"request", request},
            {"ok", false}, {"error", "PermissionsClientInsufficient"}}).toJson());
        QTRY_COMPARE(channel.chatError(), QString("Insufficient permissions."));
        QCOMPARE(joinCount, 1);
        QCOMPARE(channel.joinedHostId(), root);
        QVERIFY(channel.joinSaved(nested));
        QCOMPARE(joinCount, 2);
        QVERIFY(channel.chatError().isEmpty());
        state.insert("channel", "2");
        fakeEvents.enqueue(QJsonDocument(state).toJson());
        fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "result"}, {"request", request}, {"ok", true}}).toJson());
        QTRY_COMPARE(channel.joinedHostId(), nested);
        QVERIFY(channel.chatReady());
        QVERIFY(channel.joinSaved(nested));
        QCOMPARE(joinCount, 2);
        QCOMPARE(openCount, 2);
        partial.insert("directoryComplete", true);
        fakeEvents.enqueue(QJsonDocument(partial).toJson());
        QTRY_VERIFY([&] { const auto current = channel.savedChannels();
            return std::none_of(current.cbegin(), current.cend(),
                [&](const auto& item) { return item.toMap().value("id") == nested; }); }());
    }

    void rejectedChannelMoveKeepsCurrentVoiceAndCanRetryPassword_data() {
        QTest::addColumn<QString>("error");
        QTest::addColumn<bool>("password");
        QTest::newRow("password") << QString("ChannelInvalidPassword") << true;
        QTest::newRow("permissions") << QString("PermissionsClientInsufficient") << false;
    }
    void rejectedChannelMoveKeepsCurrentVoiceAndCanRetryPassword() {
        QFETCH(QString, error); QFETCH(bool, password);
        resetFake(); fakeEvents.enqueue(stateEvent());
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        const auto active = channel.joinedHostId();
        QString nested;
        for (const auto& entry : channel.savedChannels()) if (entry.toMap().value("name") == "Nested") nested = entry.toMap().value("id").toString();
        QVERIFY(!nested.isEmpty());
        QVERIFY(channel.joinSaved(nested));
        QVERIFY(channel.passwordRequired());
        QVERIFY(channel.submitPassword("wrong", true));
        const auto request = QJsonDocument::fromJson(lastCommand).object().value("request");
        fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "result"}, {"request", request},
            {"ok", false}, {"passwordRequired", password}, {"error", error}}).toJson());
        QTRY_COMPARE(channel.chatError(), password ? error : QString("Insufficient permissions."));
        QCOMPARE(channel.passwordRequired(), password);
        QCOMPARE(channel.joinedHostId(), active);
        if (!password) return;
        QVERIFY(channel.submitPassword("correct", true));
        QCOMPARE(QJsonDocument::fromJson(lastCommand).object().value("password").toString(), QString("correct"));
        QCOMPARE(openCount, 1);
    }

    void mutedAudioIsNotSentAndLeaveClosesConnection() {
        resetFake();
        {
            const QMutexLocker lock(&fakeMutex);
            fakeEvents.enqueue(stateEvent());
        }
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        QVERIFY(session.setMuted(true));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.initialize());
        QVERIFY(channel.addTeamSpeak("ts3://127.0.0.1:9987", {}));
        QTRY_VERIFY_WITH_TIMEOUT(channel.chatReady(), 1000);
        QVERIFY(!channel.sendAudio(QByteArray("opus")));
        QCOMPARE(audioCount, 0);
        QVERIFY(channel.leave());
        QCOMPARE(closeCount, 1);
        QVERIFY(!channel.chatReady());
    }

    void closedConnectionClearsParticipationAndCanReconnect() {
        resetFake(); fakeEvents.enqueue(stateEvent());
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        const auto active = channel.joinedHostId();
        fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "closed"}, {"message", "Connection lost"}}).toJson());
        QTRY_COMPARE(closeCount, 1);
        QVERIFY(channel.joinedHostId().isEmpty());
        QVERIFY(!channel.chatReady());
        QVERIFY(channel.participants().isEmpty());
        fakeEvents.enqueue(stateEvent());
        QVERIFY(channel.joinSaved(active));
        QTRY_VERIFY(channel.joined());
        QCOMPARE(openCount, 2);
        QCOMPARE(channel.joinedHostId(), active);
    }

    void serverPasswordRetryAndChannelPasswordStaySeparate() {
        resetFake();
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1", "wrong-server-password"));
        fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "closed"}, {"message", "ServerInvalidPassword"}, {"passwordRequired", true}}).toJson());
        QTRY_VERIFY(channel.passwordRequired());
        fakeEvents.enqueue(stateEvent());
        QVERIFY(channel.submitPassword("server-password", true));
        QTRY_VERIFY(channel.joined());
        const auto root = channel.joinedHostId();
        QCOMPARE(QJsonDocument::fromJson(lastOptions).object().value("password").toString(), QString("server-password"));
        QString nested;
        for (const auto& entry : channel.savedChannels()) if (entry.toMap().value("name") == "Nested") nested = entry.toMap().value("id").toString();
        QVERIFY(channel.joinSaved(nested));
        QVERIFY(channel.submitPassword("channel-password", true));
        const auto request = QJsonDocument::fromJson(lastCommand).object().value("request");
        fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "result"}, {"request", request}, {"ok", true}}).toJson());
        QTRY_VERIFY(channel.passwordSaved());
        QVERIFY(channel.leave());
        fakeEvents.enqueue(stateEvent());
        QVERIFY(channel.joinSaved(root));
        QTRY_VERIFY(channel.joined());
        QCOMPARE(QJsonDocument::fromJson(lastOptions).object().value("password").toString(), QString("server-password"));
        QVERIFY(channel.joinSaved(nested));
        QCOMPARE(QJsonDocument::fromJson(lastCommand).object().value("password").toString(), QString("channel-password"));
    }

    void unsupportedCodecAndServerRestrictionsKeepTextAvailable() {
        resetFake();
        auto state = QJsonDocument::fromJson(stateEvent()).object();
        state.insert("canSend", false); state.insert("talkAllowed", false);
        fakeEvents.enqueue(QJsonDocument(state).toJson());
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        session.setMuted(false); session.setPushToTalk(false); session.setAudioReadiness(true, true);
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        QVERIFY(channel.chatReady());
        QVERIFY(!channel.sendAudio("opus"));
        QVERIFY(channel.status().contains("Speaking is not allowed"));
        state.insert("canSend", true); state.insert("talkAllowed", true);
        auto channels = state.value("channels").toArray();
        auto active = channels[0].toObject(); active.insert("codec", 3); channels[0] = active;
        state.insert("channels", channels);
        fakeEvents.enqueue(QJsonDocument(state).toJson());
        QTRY_VERIFY(channel.status().contains("Unsupported audio codec"));
        QVERIFY(!channel.sendAudio("opus"));
        QVERIFY(channel.sendChat("Text still works"));
        acknowledgePendingChat();
        QTRY_COMPARE(channel.messages().size(), 1);
    }

    void privateRecipientMustBelongToTheSelectedServer() {
        resetFake(); fakeEvents.enqueue(stateEvent(true));
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        const auto oldHost = channel.joinedHostId();
        fakeEvents.enqueue(stateEvent(true));
        QVERIFY(channel.addTeamSpeak("127.0.0.2"));
        QTRY_VERIFY(channel.joined() && channel.joinedHostId() != oldHost);
        QString peer;
        for (const auto& value : channel.participants()) {
            const auto member = value.toMap();
            if (member.value("name") == "Remote") peer = member.value("id").toString();
        }
        QVERIFY(!peer.isEmpty());
        QVERIFY(!channel.canPrivateChat(oldHost, peer));
        QVERIFY(channel.canPrivateChat(channel.joinedHostId(), peer));
        QVERIFY(channel.openPrivateChat(channel.joinedHostId(), peer));
    }

    void successfulJoinUpdatesAutoJoinPriorityOnlyOnce() {
        resetFake(); fakeEvents.enqueue(stateEvent());
        QTemporaryDir dir;
        qint64 now = 100000;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create(), [&] { return now; });
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        const auto joinedAt = [&] {
            for (const auto& entry : channel.savedChannels())
                if (entry.toMap().value("id") == channel.joinedHostId()) return entry.toMap().value("lastJoined").toLongLong();
            return qint64(-1);
        };
        QCOMPARE(joinedAt(), now);
        now += 5000;
        QSignalSpy changed(&channel, &LocalChannel::hostsChanged);
        fakeEvents.enqueue(stateEvent(true));
        QTRY_VERIFY(!changed.empty());
        QCOMPARE(joinedAt(), qint64(100000));
    }

    void cachedPrivateMessagesExpireWithoutAnotherServerEvent() {
        resetFake(); fakeEvents.enqueue(stateEvent(true));
        QTemporaryDir dir;
        qint64 now = 100000;
        VoiceSession session(dir.filePath("session"));
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create(), [&] { return now; });
        QVERIFY(channel.addTeamSpeak("127.0.0.1"));
        QTRY_VERIFY(channel.joined());
        fakeEvents.enqueue(QJsonDocument(QJsonObject{{"type", "message"}, {"mode", "private"}, {"fromId", "7"},
            {"fromUid", "remote-uid"}, {"fromName", "Remote"}, {"text", "Expires while idle"}}).toJson());
        QTRY_COMPARE(channel.privateChats().size(), 1);
        const auto conversation = channel.privateChats().first().toMap();
        QVERIFY(channel.openSavedChat(conversation.value("id").toString()));
        QCOMPARE(channel.messages().size(), 1);
        QSignalSpy messagesChanged(&channel, &LocalChannel::chatChanged);
        QSignalSpy listChanged(&channel, &LocalChannel::hostsChanged);
        now += ChatHistory::lifetime;
        QTRY_VERIFY_WITH_TIMEOUT(!messagesChanged.empty() && !listChanged.empty(), 2000);
        QVERIFY(channel.messages().isEmpty());
        QVERIFY(!channel.privateChats().first().toMap().value("hasMessages").toBool());
    }
};

QTEST_MAIN(TeamSpeakTests)
#include "teamspeak_tests.moc"
