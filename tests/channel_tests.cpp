#include "local_channel.hpp"
#include "voice_mixer.hpp"
#include "video_codec.hpp"
#include "chat_content.hpp"
#include "license.hpp"
#include "event_wait.hpp"
#include <QImage>
#include <QCborValue>
#include <QLoggingCategory>
#include <QFutureWatcher>
#include <QPromise>
#if QT_CONFIG(process)
#include <QProcess>
#endif
#include <QThreadPool>
#include <QtEndian>
#include <QBuffer>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <iostream>
#include <thread>
#include <array>
#include <numbers>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#if defined(Q_OS_IOS)
#include <qtkeychain/keychain.h>
#include <QGuiApplication>
#endif
#include <QTcpServer>
#include <QTcpSocket>
#include <QSslSocket>
#include <QSslKey>
#include <QSslServer>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QTranslator>
#include <QScopeGuard>
#include <deque>
#include <exception>
#include <memory>
#include <numeric>
#include <utility>
#include <opus.h>

namespace {
// Sanitizers change scheduling for the host and simulated clients.
// Release builds own the realtime thresholds; both builds verify delivery.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool instrumentedTiming = true;
#elif defined(__has_feature)
constexpr bool instrumentedTiming = __has_feature(address_sanitizer);
#else
constexpr bool instrumentedTiming = false;
#endif

struct Device final {
    VoiceSession session;
    LocalChannel channel;
    Device(const QString& path, const QString& name, TlsIdentity identity = TlsIdentity::create(),
           std::function<qint64()> clock = QDateTime::currentMSecsSinceEpoch)
        : session(path + ".session"), channel(session, path + ".channel", std::move(identity), std::move(clock)) {
        if (qEnvironmentVariableIsSet("SQUAD_TEST_TRACE"))
            QObject::connect(&channel, &LocalChannel::stateChanged, &channel, [this, name] { qInfo() << name << channel.status(); });
        if (!session.setUserName(name)) throw std::runtime_error("Fixture name failed");
    }
    bool join(LocalChannel& host) { return channel.join(host.ownId(), "127.0.0.1", host.port()); }
    bool listenWithUdp(const QHostAddress& address = QHostAddress::LocalHost) {
        for (int attempt = 0; attempt < 32; ++attempt) {
            QTcpServer reservation;
            if (!reservation.listen(QHostAddress::Any, 0)) {
                qWarning() << "Unable to reserve device access port:" << reservation.errorString();
                return false;
            }
            const auto port = reservation.serverPort();
            reservation.close();
            if (!channel.startService(address, port)) continue;
            return channel.listen(address, port);
        }
        qWarning() << "Unable to find a TCP/UDP media port after 32 attempts:" << channel.status();
        return false;
    }
};
#if QT_CONFIG(process)
int controlledProcess(QCoreApplication& application, std::function<void(const QJsonObject&)> command = {}) {
    std::thread input([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
            QMetaObject::invokeMethod(&application, [&, request] { if (command) command(request); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(&application, [&] {
            qInfo() << "Capacity command input closed";
            application.quit();
        }, Qt::QueuedConnection);
    });
    const auto result = application.exec();
    qInfo() << "Capacity event loop stopped";
    input.join();
    return result;
}
#endif
QVariantMap participant(const LocalChannel& channel, const QString& id) {
    for (const auto& entry : channel.participants())
        if (entry.toMap().value("id").toString() == id) return entry.toMap();
    return {};
}
QVariantList memberMessages(const LocalChannel& channel) {
    QVariantList records;
    for (const auto& record : channel.messages())
        if (!record.toMap().contains("event")) records.append(record);
    return records;
}
QByteArray voicePacket(double frequency = 440) {
    squad::VoiceMixer encoder(2);
    std::array<float, 1920> samples{};
    for (size_t i = 0; i < 960; ++i) {
        samples[i * 2] = float(0.2 * std::sin(2 * std::numbers::pi * frequency * i / 48000));
        samples[i * 2 + 1] = float(0.1 * std::sin(2 * std::numbers::pi * frequency * 2 * i / 48000));
    }
    return encoder.encode(samples, 48000).first();
}
QByteArray capacityVoicePacket(int channels) {
    const auto application = channels == 1 ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO;
    int error = OPUS_OK;
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder(
        opus_encoder_create(48000, channels, application, &error), opus_encoder_destroy);
    if (!encoder) throw std::runtime_error(opus_strerror(error));
    if (opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(channels == 1 ? 32000 : 128000)) != OPUS_OK)
        throw std::runtime_error("Capacity fixture Opus encoder could not be configured");
    std::array<float, 1920> samples{};
    for (size_t frame = 0; frame < 960; ++frame) {
        samples[frame * size_t(channels)] = float(0.1 * std::sin(2 * std::numbers::pi * 440 * frame / 48000));
        if (channels == 2)
            samples[frame * 2 + 1] = float(0.1 * std::sin(2 * std::numbers::pi * 660 * frame / 48000));
    }
    QByteArray packet(4000, '\0');
    const auto bytes = opus_encode_float(encoder.get(), samples.data(), 960,
        reinterpret_cast<unsigned char*>(packet.data()), int(packet.size()));
    if (bytes < 0) throw std::runtime_error(opus_strerror(bytes));
    packet.resize(bytes);
    return packet;
}
QByteArray wireFrame(const QJsonObject& object) {
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray frame(4, '\0');
    qToBigEndian<quint32>(quint32(data.size()), frame.data());
    return frame + data;
}
QNetworkInterface multicastInterface() {
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || !flags.testFlag(QNetworkInterface::CanMulticast) || flags.testFlag(QNetworkInterface::IsLoopBack)) continue;
        for (const auto& entry : interface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) return interface;
    }
    return {};
}
// Delay actual encrypted host-to-client bytes, preserving TLS stream order.
class DelayedLink final : public QObject {
    struct Link final { QPointer<QTcpSocket> downstream, upstream; bool delayed = false; };
    QTcpServer server_;
    QUdpSocket udp_;
    QHash<quint16, QUdpSocket*> udpRoutes_;
    QElapsedTimer clock_;
    QTimer timer_;
    std::deque<std::tuple<qint64, QPointer<QTcpSocket>, QByteArray>> pending_;
    QPointer<QTcpSocket> primary_;
    static void forward(QTcpSocket* target, const QByteArray& bytes) {
        if (!target || target->state() != QAbstractSocket::ConnectedState) return;
        target->write(bytes);
        // A transparent proxy must not add a second event-loop write delay
        // to either leg when its configured impairment is zero.
        target->flush();
    }
public:
    int delay = 0, udpDelay = -1, audioDatagrams = 0, videoDatagrams = 0, audioDropEvery = 0;
    bool blockUdp = false, holdReliable = false;
    bool dropLargeDatagrams = false;
    bool videoOnly = false;
    explicit DelayedLink(quint16 port, int connectDelay = 0) : server_(this) {
        clock_.start(); timer_.setInterval(5);
        connect(&server_, &QTcpServer::newConnection, this, [this, port, connectDelay] {
            auto link = std::make_shared<Link>();
            link->downstream = server_.nextPendingConnection();
            link->upstream = new QTcpSocket(this);
            link->downstream->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            link->delayed = !videoOnly || primary_;
            if (videoOnly && !primary_) primary_ = link->downstream;
            connect(link->downstream, &QTcpSocket::readyRead, this, [link] {
                if (link->upstream && link->upstream->state() == QAbstractSocket::ConnectedState)
                    forward(link->upstream, link->downstream->readAll());
            });
            connect(link->upstream, &QTcpSocket::connected, this, [link] {
                link->upstream->setSocketOption(QAbstractSocket::LowDelayOption, 1);
                if (link->downstream) forward(link->upstream, link->downstream->readAll());
            });
            connect(link->upstream, &QTcpSocket::readyRead, this, [this, link] {
                const bool waiting = std::any_of(pending_.begin(), pending_.end(), [link](const auto& item) {
                    return std::get<1>(item) == link->downstream;
                });
                if (!link->delayed || (delay == 0 && !waiting && !holdReliable)) {
                    forward(link->downstream, link->upstream->readAll());
                    return;
                }
                const auto at = clock_.elapsed() + (link->delayed ? delay : 0);
                pending_.emplace_back(at, link->downstream, link->upstream->readAll());
            });
            connect(link->downstream, &QTcpSocket::disconnected, this, [this, link] {
                if (primary_ == link->downstream) primary_.clear();
                if (link->upstream) link->upstream->abort();
            });
            connect(link->upstream, &QTcpSocket::disconnected, this, [link] { if (link->downstream) link->downstream->abort(); });
            const auto connectUpstream = [link, port] {
                if (link->downstream && link->downstream->state() == QAbstractSocket::ConnectedState)
                    link->upstream->connectToHost(QHostAddress::LocalHost, port);
            };
            if (connectDelay) QTimer::singleShot(connectDelay, link->upstream, connectUpstream);
            else connectUpstream();
        });
        connect(&timer_, &QTimer::timeout, this, [this] {
            // Preserve order per TLS connection, without delaying another link.
            QSet<QTcpSocket*> held;
            for (auto it = pending_.begin(); it != pending_.end();) {
                auto& [at, target, bytes] = *it;
                if (!target || target->state() != QAbstractSocket::ConnectedState) it = pending_.erase(it);
                else if (!holdReliable && at <= clock_.elapsed() && !held.contains(target)) { forward(target, bytes); it = pending_.erase(it); }
                else { held.insert(target); ++it; }
            }
        });
        // Reserve UDP first: a TCP ephemeral port may be unavailable to UDP.
        for (int attempt = 0; attempt < 32 && !server_.isListening(); ++attempt) {
            udp_.close();
            if (!udp_.bind(QHostAddress::LocalHost, 0, QUdpSocket::DontShareAddress))
                throw std::runtime_error(QString("Delayed UDP link cannot bind: %1").arg(udp_.errorString()).toStdString());
            server_.listen(QHostAddress::LocalHost, udp_.localPort());
        }
        if (!server_.isListening())
            throw std::runtime_error(QString("Delayed TLS link cannot bind port %1: %2")
                .arg(udp_.localPort()).arg(server_.errorString()).toStdString());
        // Match the media library's bounded receive window. OS defaults must
        // not add packet loss to a link configured without impairment.
        udp_.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 1024 * 1024);
        connect(&udp_, &QUdpSocket::readyRead, this, [this, port] {
            while (udp_.hasPendingDatagrams()) {
                const auto request = udp_.receiveDatagram();
                if (blockUdp) continue;
                const auto clientPort = quint16(request.senderPort());
                if (!udpRoutes_.contains(clientPort)) {
                    auto* route = new QUdpSocket(this);
                    if (!route->bind(QHostAddress::LocalHost)) throw std::runtime_error("UDP fixture route cannot bind.");
                    route->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 1024 * 1024);
                    udpRoutes_.insert(clientPort, route);
                    connect(route, &QUdpSocket::readyRead, this, [this, route, port, clientPort] {
                        while (route->hasPendingDatagrams()) {
                            const auto response = route->receiveDatagram();
                            if (response.senderPort() != port || blockUdp) continue;
                            const auto bytes = response.data();
                            const bool videoData = !bytes.isEmpty() && quint8(bytes[0]) == 23;
                            if (bytes.size() > 12 && (quint8(bytes[0]) >> 6) == 2 && (quint8(bytes[1]) & 127) == 111) {
                                ++audioDatagrams;
                                if (audioDropEvery && audioDatagrams % audioDropEvery == 0) continue;
                            }
                            if (videoData) ++videoDatagrams;
                            if (dropLargeDatagrams && videoData && bytes.size() > 512) continue;
                            const int hold = !videoOnly || videoData ? (udpDelay < 0 ? delay : udpDelay) : 0;
                            if (hold) QTimer::singleShot(hold, this, [this, bytes, clientPort] { udp_.writeDatagram(bytes, QHostAddress::LocalHost, clientPort); });
                            else udp_.writeDatagram(bytes, QHostAddress::LocalHost, clientPort);
                        }
                    });
                }
                udpRoutes_.value(clientPort)->writeDatagram(request.data(), QHostAddress::LocalHost, port);
            }
        });
        timer_.start();
    }
    ~DelayedLink() override {
        timer_.stop();
        const auto sockets = findChildren<QTcpSocket*>();
        for (auto* socket : sockets) socket->disconnect(this);
        for (auto* socket : sockets) socket->abort();
    }
    quint16 port() const { return server_.serverPort(); }
    bool disconnectPrimary() {
        if (!primary_) return false;
        primary_->abort();
        return true;
    }
};
}

class ChannelTests final : public QObject {
    Q_OBJECT
private slots:
#if QT_CONFIG(process)
    void failedTestStillDrainsCleanupEvents() {
        if (qEnvironmentVariableIsSet("SQUAD_TEST_CLEANUP_FAILURE")) {
            const auto cleanup = qScopeGuard([] {
                QTimer signal;
                signal.setSingleShot(true);
                signal.start(0);
                const bool signalled = waitForEvents(&signal, &QTimer::timeout,
                    [&] { return !signal.isActive(); });
                bool completed = false;
                QTimer::singleShot(0, &signal, [&] { completed = true; });
                const bool polled = waitForEvents([&] { return completed; });
                QElapsedTimer elapsed; elapsed.start();
                const bool expired = !waitForEvents([] { return false; }, 10)
                    && elapsed.elapsed() >= 10;
                qInfo("cleanup signalled=%d polled=%d expired=%d", signalled, polled, expired);
            });
            QFAIL("Intentional failure before asynchronous cleanup");
        }
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("SQUAD_TEST_CLEANUP_FAILURE", "1");
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {"failedTestStillDrainsCleanupEvents"});
        const auto stop = qScopeGuard([&] {
            if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(5000); }
        });
        QVERIFY(child.waitForFinished(15000));
        const auto output = child.readAll();
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 1);
        QVERIFY2(output.contains("Intentional failure before asynchronous cleanup"), output.constData());
        QVERIFY2(output.contains("cleanup signalled=1 polled=1 expired=1"), output.constData());
    }
#endif
#if defined(Q_OS_IOS)
    void simulatorIdentitySurvivesReload() {
        const auto slot = "simulator-contract-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto cleanup = qScopeGuard([&] {
            QKeychain::DeletePasswordJob job("SquadSpeak");
            job.setAutoDelete(false); job.setKey(slot); job.setInsecureFallback(false);
            QSignalSpy finished(&job, &QKeychain::Job::finished); job.start();
            QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 5000);
            QVERIFY(job.error() == QKeychain::NoError || job.error() == QKeychain::EntryNotFound);
        });
        QString previous;
        for (int attempt = 0; attempt < 2; ++attempt) {
            QObject context;
            bool completed = false;
            QString id, error;
            QVERIFY(TlsIdentity::load(slot, &context, [&](std::optional<TlsIdentity> identity, QString diagnostic) {
                if (identity) id = identity->id();
                error = diagnostic; completed = true;
            }));
            QTRY_VERIFY_WITH_TIMEOUT(completed, 10000);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(id.size(), 64);
            if (attempt) QCOMPARE(id, previous);
            previous = id;
        }
    }
#endif
    void permissionStoreRetainsLargeApprovalLists() {
        QTemporaryDir dir;
        const auto path = dir.filePath("host");
        { Device initial(path, "Host"); QVERIFY(initial.channel.setChannelName("Large host")); }
        QFile store(path + ".channel"); QVERIFY(store.open(QIODevice::ReadOnly));
        auto object = QJsonDocument::fromJson(store.readAll()).object(); store.close();
        QJsonArray approved;
        for (int i = 1; i <= 20000; ++i) approved.append(QString::number(i, 16).rightJustified(64, '0'));
        object.insert("approved", approved);
        QVERIFY(store.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        QVERIFY(bytes.size() > 1024 * 1024); QCOMPARE(store.write(bytes), bytes.size()); store.close();
        try {
            Device loaded(path, "Host");
            QVERIFY(loaded.channel.setChannelName("Renamed large host"));
        } catch (const std::exception& error) { QFAIL(error.what()); }
        QVERIFY(store.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(store.readAll()).object();
        QCOMPARE(saved.value("approved").toArray().size(), approved.size());
        QCOMPARE(saved.value("channelName").toString(), QString("Renamed large host"));
        store.close();
        QVERIFY(store.open(QIODevice::WriteOnly | QIODevice::Append));
        const QByteArray padding(16 * 1024 * 1024 + 1 - store.size(), ' ');
        QCOMPARE(store.write(padding), padding.size()); store.close();
        QVERIFY_EXCEPTION_THROWN(Device(path, "Host"), std::runtime_error);
    }

    void privateContactsRecoverAfterAdmissionStorageFailure() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"),
            a(dir.filePath("a"), "Alice"), b(dir.filePath("b"), "Bea");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        for (auto* client : {&a, &b}) QVERIFY(host.channel.decide(client->channel.ownId(), true));
        const auto store = dir.filePath("host.channel");
        QVERIFY(QFile::rename(store, store + ".saved"));
        QVERIFY(QDir().mkdir(store));
        QVERIFY(a.join(host.channel));
        QTRY_VERIFY(a.channel.joined());
        QTRY_VERIFY(host.channel.status().contains("could not be saved"));
        QVERIFY(a.session.setUserName("Alice updated"));
        QTRY_VERIFY(([&] { const auto participants = host.channel.hostParticipants(); return std::any_of(participants.cbegin(), participants.cend(),
            [](const QVariant& value) { return value.toMap().value("name") == "Alice updated"; }); }()));
        QVERIFY(QDir().rmdir(store));
        QVERIFY(QFile::rename(store + ".saved", store));
        bool disconnected = false;
        QObject observation;
        connect(&a.channel, &LocalChannel::stateChanged, &observation, [&] { disconnected |= !a.channel.joined(); });
        QVERIFY(b.join(host.channel));
        QTRY_VERIFY(b.channel.joined());
        QTRY_VERIFY(b.channel.canPrivateChat(host.channel.ownId(), a.channel.ownId()));
        QTRY_VERIFY(a.channel.canPrivateChat(host.channel.ownId(), b.channel.ownId()));
        QVERIFY(b.channel.openPrivateChat(host.channel.ownId(), a.channel.ownId()));
        QVERIFY(a.channel.openPrivateChat(host.channel.ownId(), b.channel.ownId()));
        QVERIFY(b.channel.sendChat("Storage recovered"));
        QTRY_COMPARE(a.channel.messages().size(), 1);
        QCOMPARE(a.channel.messages().first().toMap().value("text").toString(), QString("Storage recovered"));
        QVERIFY(!disconnected);
        // Loading the persisted permission file also checks reciprocal contact integrity.
        Device restored(dir.filePath("host"), "Host", TlsIdentity::create());
        QVERIFY(restored.channel.ready());
    }

    void privateChatPersistenceFailureAndPagingRecover() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        Device host(dir.filePath("host"), "Host", TlsIdentity::create(), clock),
            a(dir.filePath("a"), "Alice", TlsIdentity::create(), clock), b(dir.filePath("b"), "Bea", TlsIdentity::create(), clock);
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        for (auto* client : {&a, &b}) {
            QVERIFY(host.channel.decide(client->channel.ownId(), true)); QVERIFY(client->join(host.channel));
            QTRY_VERIFY(client->channel.chatReady());
        }
        QTRY_VERIFY(a.channel.canPrivateChat(host.channel.ownId(), b.channel.ownId()));
        QVERIFY(a.channel.openPrivateChat(host.channel.ownId(), b.channel.ownId()));
        QVERIFY(b.channel.openPrivateChat(host.channel.ownId(), a.channel.ownId()));
        const auto conversation = a.channel.chatHostId();
        QVERIFY(a.channel.sendChat("First receipt")); QTRY_VERIFY(!a.channel.chatPending());
        QTRY_COMPARE(b.channel.messages().size(), 1);
        const auto connection = QUuid::createUuid().toString();
        {
            auto locked = QSqlDatabase::addDatabase("QSQLITE", connection);
            locked.setDatabaseName(dir.filePath("host.channel.private/") + conversation + ".sqlite");
            QVERIFY(locked.open());
            QSqlQuery query(locked); QVERIFY(query.exec("BEGIN IMMEDIATE"));
            QSignalSpy sent(&a.channel, &LocalChannel::chatSent);
            QVERIFY(a.channel.sendChat("Retry after storage failure"));
            QTRY_VERIFY(!a.channel.chatPending());
            QVERIFY(!a.channel.chatError().isEmpty()); QCOMPARE(sent.size(), 0);
            QCOMPARE(b.channel.messages().size(), 1);
            QVERIFY(query.exec("ROLLBACK")); locked.close();
        }
        QSqlDatabase::removeDatabase(connection);
        QVERIFY(a.channel.sendChat("Retry after storage failure")); QTRY_VERIFY(!a.channel.chatPending());
        QTRY_COMPARE(b.channel.messages().size(), 2);
        for (int i = 2; i < 45; ++i) {
            if (i % 8 == 0) now += 10000;
            QVERIFY(a.channel.sendChat(QString("Message %1").arg(i)));
            QTRY_VERIFY(!a.channel.chatPending());
            QVERIFY2(a.channel.chatError().isEmpty(), qPrintable(a.channel.chatError()));
        }
        QTRY_COMPARE(b.channel.messages().size(), 45);
        QVERIFY(b.channel.openSavedChat(host.channel.ownId()));
        QVERIFY(b.channel.openPrivateChat(host.channel.ownId(), a.channel.ownId()));
        QTRY_VERIFY(!b.channel.historyLoading());
        QVERIFY(b.channel.messages().size() > 0 && b.channel.messages().size() <= ChatHistory::maximumPageMessages);
        QVERIFY(b.channel.hasOlderMessages());
        while (b.channel.hasOlderMessages()) {
            const auto loaded = b.channel.messages().size();
            QVERIFY(b.channel.loadOlderMessages()); QTRY_VERIFY(!b.channel.historyLoading());
            QVERIFY(b.channel.messages().size() > loaded && b.channel.messages().size() <= 45);
        }
        QCOMPARE(b.channel.messages().size(), 45); QVERIFY(!b.channel.hasOlderMessages());
        QCOMPARE(b.channel.messages().first().toMap().value("text").toString(), QString("First receipt"));
        QVERIFY(a.session.setUserName("Alice renamed"));
        QTRY_COMPARE(b.channel.privateChats().first().toMap().value("name").toString(), QString("Alice renamed"));
        QVERIFY(b.channel.refreshChat()); QTRY_VERIFY(!b.channel.historyLoading());
        QCOMPARE(b.channel.messages().last().toMap().value("name").toString(), QString("Alice renamed"));
    }

    void privateChatRejectsMalformedPeerContentWithoutLosingVoice_data() {
        QTest::addColumn<bool>("badCiphertext");
        QTest::newRow("invalid-pixels") << false;
        QTest::newRow("invalid-ciphertext") << true;
    }
    void privateChatRejectsMalformedPeerContentWithoutLosingVoice() {
        QFETCH(bool, badCiphertext);
        QTemporaryDir dir;
        const auto sender = TlsIdentity::create(), receiver = TlsIdentity::create();
        Device host(dir.filePath("host"), "Host"), b(dir.filePath("b"), "Bea", receiver);
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(sender.id(), true));
        QVERIFY(host.channel.decide(receiver.id(), true));
        QVERIFY(b.join(host.channel));
        QTRY_VERIFY(b.channel.chatReady());
        QSslSocket socket; socket.setSslConfiguration(sender.configuration());
        connect(&socket, &QSslSocket::sslErrors, &socket, [&socket](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
        QByteArray incoming; QList<QJsonObject> responses;
        connect(&socket, &QSslSocket::readyRead, &socket, [&] {
            incoming += socket.readAll();
            while (incoming.size() >= 4) {
                const auto size = qFromBigEndian<quint32>(incoming.constData());
                if (incoming.size() < size + 4) break;
                responses.append(QJsonDocument::fromJson(incoming.mid(4, size)).object());
                incoming.remove(0, size + 4);
            }
        });
        socket.connectToHostEncrypted("127.0.0.1", host.channel.port());
        QTRY_VERIFY(socket.isEncrypted());
        socket.write(wireFrame({{"type", "hello"}, {"version", 1}, {"name", "Sender"}, {"available", true},
            {"muted", true}, {"capabilities", QJsonArray{"private-chat"}}}));
        QTRY_VERIFY(b.channel.canPrivateChat(host.channel.ownId(), sender.id()));
        QVERIFY(b.channel.openPrivateChat(host.channel.ownId(), sender.id()));
        QTRY_VERIFY(!b.channel.historyLoading());
        const auto id = b.channel.chatHostId();
        const auto send = [&](QJsonObject payload) {
            payload.insert("peer", receiver.id()); payload.insert("conversation", id);
            socket.write(wireFrame({{"type", "privateChat"}, {"payload", payload}}));
        };
        const auto hashOf = [](const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
        QImage image(2, 2, QImage::Format_ARGB32); image.fill(Qt::red);
        QByteArray source; QBuffer buffer(&source); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        auto png = ChatContent::sanitizeImage(source);
        // Keep the allowed chunk structure but invalidate the compressed pixels.
        const auto dataOffset = png.indexOf("IDAT") + 4;
        QVERIFY(dataOffset > 4); if (!badCiphertext) png[dataOffset] = char(png[dataOffset] ^ 0x7f);
        QCOMPARE(ChatContent::sanitizedImageSize(png), image.size());
        const auto request = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto binding = ("SquadSpeak/private/v1/" + host.channel.ownId() + '/' + id + '/' + request + '/' + sender.id()).toUtf8();
        const auto key = TlsIdentity::newKey();
        auto encrypted = TlsIdentity::seal(png, key, binding + "/image");
        if (badCiphertext) encrypted.back() = char(encrypted.back() ^ 0x7f);
        const auto hash = hashOf(png), wireHash = hashOf(encrypted);
        const QJsonObject content{{"text", "Modified sender"}, {"image", QJsonObject{{"hash", hash}, {"size", png.size()}, {"width", 2}, {"height", 2},
            {"key", QString::fromLatin1(key.toBase64())}, {"context", QString::fromLatin1(binding.toBase64())}, {"wireHash", wireHash}, {"wireSize", encrypted.size()}}}};
        const auto sealed = sender.sealFor(QCborValue::fromJsonValue(content).toCbor(), receiver.configuration().localCertificate(), binding);
        send({{"kind", "imageStart"}, {"request", request}, {"issued", QDateTime::currentMSecsSinceEpoch()},
            {"sealed", QString::fromLatin1(sealed.toBase64())}, {"hash", wireHash}, {"size", encrypted.size()}});
        QTRY_VERIFY(std::any_of(responses.begin(), responses.end(), [](const auto& r) { return r.value("payload").toObject().value("kind") == "imageOffset"; }));
        send({{"kind", "imageChunk"}, {"request", request}, {"offset", 0}, {"data", QString::fromLatin1(encrypted.toBase64())}});
        QTRY_COMPARE(b.channel.messages().size(), 1);
        QVERIFY(b.channel.requestImage(hash));
        QTRY_VERIFY(!b.channel.chatError().isEmpty() || !b.channel.imageSource(hash).isEmpty());
        QVERIFY2(b.channel.imageSource(hash).isEmpty(), "A structurally valid PNG with invalid pixel data must never reach the renderer.");
        QVERIFY(b.channel.joined());
        QVERIFY(b.channel.sendChat("Valid image after invalid input", source)); QTRY_VERIFY(!b.channel.chatPending());
        const auto validHash = b.channel.messages().last().toMap().value("image").toMap().value("hash").toString();
        QVERIFY(b.channel.requestImage(validHash)); QTRY_VERIFY(!b.channel.imageSource(validHash).isEmpty());
        // A replay returns the same durable receipt, without creating a second message.
        const auto receipts = [&] {
            QJsonArray result;
            for (const auto& reply : responses) {
                const auto record = reply.value("payload").toObject().value("record").toObject();
                if (record.value("request") == request) result.append(record);
            }
            return result;
        };
        QTRY_COMPARE(receipts().size(), 1);
        send({{"kind", "imageStart"}, {"request", request}, {"issued", QDateTime::currentMSecsSinceEpoch()},
            {"sealed", QString::fromLatin1(sealed.toBase64())}, {"hash", wireHash}, {"size", encrypted.size()}});
        send({{"kind", "imageChunk"}, {"request", request}, {"offset", 0}, {"data", QString::fromLatin1(encrypted.toBase64())}});
        QTRY_COMPARE(receipts().size(), 2);
        QCOMPARE(receipts().first(), receipts().last()); QCOMPARE(b.channel.messages().size(), 2);
        send({{"kind", "send"}, {"request", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"issued", QDateTime::currentMSecsSinceEpoch()}, {"sealed", QString::fromLatin1(QByteArray("invalid CMS").toBase64())}});
        QTRY_VERIFY(!b.channel.chatError().isEmpty());
        QVERIFY(b.channel.sendChat("Still connected"));
        QTRY_VERIFY(!b.channel.chatPending());
        QVERIFY(b.channel.joined());
        QVERIFY(b.channel.openSavedChat(host.channel.ownId()));
        QVERIFY(b.channel.openPrivateChat(host.channel.ownId(), sender.id()));
        QTRY_VERIFY(!b.channel.historyLoading());
        const auto retained = b.channel.messages();
        QVERIFY(std::any_of(retained.begin(), retained.end(), [](const auto& value) {
            return value.toMap().value("text") == "Still connected";
        }));
        QVERIFY(b.channel.joined());
        send({{"kind", "futureMessageKind"}});
        socket.write(wireFrame({{"type", "ping"}}));
        QTRY_VERIFY(std::any_of(responses.begin(), responses.end(), [](const auto& r) { return r.value("type") == "pong"; }));
        // Route forgery uses a valid but unrelated recipient, never its private content.
        socket.write(wireFrame({{"type", "privateChat"}, {"payload", QJsonObject{{"kind", "history"}, {"peer", TlsIdentity::create().id()}, {"conversation", QString(64, 'b')}}}}));
        QTRY_COMPARE(socket.state(), QAbstractSocket::UnconnectedState);
        QVERIFY(b.channel.joined());
    }

    void privateChatScopeAndRemoteControlUseTheSameConversation() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), otherHost(dir.filePath("other-host"), "Other host"),
            a(dir.filePath("a"), "Alice"), b(dir.filePath("b"), "Bea"), controller(dir.filePath("controller"), "Control");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(otherHost.channel.listen(QHostAddress::LocalHost));
        QString extra;
#if !SQUADSPEAK_STORE_BUILD
        QVERIFY(host.session.setSupporterEnabled(true));
        extra = host.channel.addOwnedChannel("Second");
        QVERIFY(!extra.isEmpty());
#endif
        for (auto* client : {&a, &b}) {
            QVERIFY(host.channel.decide(client->channel.ownId(), true));
            QVERIFY(otherHost.channel.decide(client->channel.ownId(), true));
            if (!extra.isEmpty()) QVERIFY(host.channel.ownChannel(extra)->decide(client->channel.ownId(), true));
            QVERIFY(client->join(host.channel));
            QTRY_VERIFY(client->channel.chatReady());
        }
        QTRY_VERIFY(a.channel.canPrivateChat(host.channel.ownId(), b.channel.ownId()));
        QVERIFY(a.channel.openPrivateChat(host.channel.ownId(), b.channel.ownId()));
        const auto conversation = a.channel.chatHostId();
        QVERIFY(a.channel.sendChat("Shared host conversation"));
        QTRY_VERIFY(!a.channel.chatPending());
        if (!extra.isEmpty()) {
            QVERIFY(a.channel.joinAddress("127.0.0.1:" + QString::number(host.channel.port())));
            QTRY_VERIFY(a.channel.availableHosts().size() >= 1);
            QVERIFY(a.channel.join(extra, "127.0.0.1", host.channel.port()));
            QTRY_VERIFY(a.channel.joined());
            QCOMPARE(a.channel.joinedHostId(), extra);
            QTRY_VERIFY(a.channel.canPrivateChat(extra, b.channel.ownId()));
            QVERIFY(a.channel.openPrivateChat(extra, b.channel.ownId()));
            QCOMPARE(a.channel.chatHostId(), conversation);
            QTRY_COMPARE(a.channel.messages().size(), 1);
        }
        for (auto* client : {&a, &b}) {
            QVERIFY(client->join(otherHost.channel));
            QTRY_VERIFY(client->channel.chatReady());
        }
        QTRY_VERIFY(a.channel.canPrivateChat(otherHost.channel.ownId(), b.channel.ownId()));
        QVERIFY(a.channel.openPrivateChat(otherHost.channel.ownId(), b.channel.ownId()));
        QVERIFY(a.channel.chatHostId() != conversation);
        QTRY_VERIFY(!a.channel.historyLoading());
        QVERIFY(a.channel.messages().isEmpty());
        QVERIFY(a.channel.openSavedChat(conversation));
        QTRY_COMPARE(a.channel.messages().size(), 1);
        QVERIFY(a.channel.listen(QHostAddress::LocalHost));
        QVERIFY(controller.channel.pairAddress("127.0.0.1:" + QString::number(a.channel.port())));
        QTRY_COMPARE(a.channel.controlRequests().size(), 1);
        QVERIFY(a.channel.decideControl(controller.channel.ownId(), true));
        QTRY_VERIFY(controller.channel.controlConnected());
        QVERIFY(a.channel.setRemotePermission(controller.channel.ownId(), true));
        QTRY_VERIFY(controller.channel.remoteAllowed());
        QVERIFY(controller.channel.setRemoteMode(true));
        QTRY_COMPARE(controller.channel.remoteView().value("chatHostId").toString(), conversation);
        QTRY_COMPARE(controller.channel.remoteView().value("messages").toList().size(), 1);
        QVERIFY(!controller.channel.joined());
        QVERIFY(controller.channel.remoteAction("chat", {{"hostId", conversation}, {"text", "Sent by remote control"}}));
        QTRY_COMPARE(a.channel.messages().size(), 2);
        QCOMPARE(a.channel.messages().last().toMap().value("sender").toString(), a.channel.ownId());
        QImage image(3, 4, QImage::Format_ARGB32); image.fill(QColor(10, 80, 140, 90));
        QByteArray png; QBuffer buffer(&png); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        QVERIFY(a.channel.sendChat("Remote image", png)); QTRY_VERIFY(!a.channel.chatPending());
        QTRY_COMPARE(controller.channel.remoteView().value("messages").toList().size(), 3);
        const auto imageHash = controller.channel.remoteView().value("messages").toList().last().toMap().value("image").toMap().value("hash").toString();
        QVERIFY(controller.channel.requestImage(imageHash)); QTRY_VERIFY(!controller.channel.imageSource(imageHash).isEmpty());
        QCOMPARE(QImage::fromData(QByteArray::fromBase64(controller.channel.imageSource(imageHash).section(',', 1).toLatin1())).pixelColor(0, 0), QImage::fromData(ChatContent::sanitizeImage(png)).pixelColor(0, 0));
        QVERIFY(controller.channel.setPrivateBlocked(conversation, true));
        QTRY_VERIFY(controller.channel.remoteView().value("privateBlockedByMe").toBool());
        QVERIFY(controller.channel.setPrivateBlocked(conversation, false));
        QTRY_VERIFY(!controller.channel.remoteView().value("privateBlocked").toBool());
        QVERIFY(a.channel.setRemotePermission(controller.channel.ownId(), false));
        QTRY_VERIFY(controller.channel.remoteView().isEmpty());
        QVERIFY(!controller.channel.remoteAction("chat", {{"hostId", conversation}, {"text", "revoked"}}));
        QCOMPARE(a.channel.messages().size(), 3);
        QVERIFY(b.channel.openPrivateChat(host.channel.ownId(), a.channel.ownId()));
        const auto aliceOnline = [&] {
            for (const auto& value : b.channel.privateChats()) if (value.toMap().value("id") == conversation)
                return value.toMap().value("peerOnline").toBool();
            return false;
        };
        QVERIFY(aliceOnline());
        QVERIFY(host.channel.kick(a.channel.ownId()));
        if (!extra.isEmpty()) QVERIFY(host.channel.ownChannel(extra)->kick(a.channel.ownId()));
        QTRY_VERIFY(!aliceOnline());
    }

    void privateChatImagesAndOfflineHistorySurviveRestart() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        const auto relayIdentity = TlsIdentity::create();
        const auto aliceIdentity = TlsIdentity::create();
        const auto beaIdentity = TlsIdentity::create();
        auto relay = std::make_unique<Device>(dir.filePath("relay"), "Relay", relayIdentity, clock);
        Device alice(dir.filePath("alice"), "Alice", aliceIdentity, clock);
        auto bea = std::make_unique<Device>(dir.filePath("bea"), "Bea", beaIdentity, clock);
        QVERIFY(relay->channel.listen(QHostAddress::LocalHost));
        QVERIFY(relay->channel.setMessageLifetimeDays(30));
        for (auto* client : {&alice, bea.get()}) {
            QVERIFY(relay->channel.decide(client->channel.ownId(), true));
            QVERIFY(client->join(relay->channel));
            QTRY_VERIFY(client->channel.chatReady());
        }
        QTRY_VERIFY(alice.channel.canPrivateChat(relay->channel.ownId(), bea->channel.ownId()));
        QVERIFY(alice.channel.openPrivateChat(relay->channel.ownId(), bea->channel.ownId()));
        const auto conversation = alice.channel.chatHostId();
        QTRY_COMPARE(alice.channel.chatLifetimeDays(), 1);
        QVERIFY(relay->channel.setPrivateLifetimeDays(7));
        QTRY_COMPARE(alice.channel.chatLifetimeDays(), 7);
        // The 16 KiB text boundary includes control characters expanded by JSON.
        const QString text = "Private " + QString(16000, QChar('\t'));
        QVERIFY(alice.channel.sendChat(text));
        QTRY_VERIFY(!alice.channel.chatPending());
        QVERIFY2(alice.channel.chatError().isEmpty(), qPrintable(alice.channel.chatError()));
        QVERIFY(bea->channel.openPrivateChat(relay->channel.ownId(), alice.channel.ownId()));
        QTRY_COMPARE(bea->channel.messages().size(), 1);
        QCOMPARE(bea->channel.messages().first().toMap().value("text").toString(), text);
        const auto firstExpiry = bea->channel.messages().first().toMap().value("expires").toLongLong();
        QCOMPARE(firstExpiry, now + 7 * ChatHistory::lifetime);
        {
            ChatHistory stored(dir.filePath("relay.channel.private/") + conversation, relayIdentity, now);
            const auto record = stored.page().value("records").toArray().first().toObject();
            QVERIFY(record.value("text").toString().isEmpty());
            QVERIFY(record.contains("sealed"));
            const auto context = ("SquadSpeak/private/v1/" + relayIdentity.id() + '/' + conversation + '/'
                + record.value("request").toString() + '/' + aliceIdentity.id()).toUtf8();
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, static_cast<void>(relayIdentity.openFrom(
                QByteArray::fromBase64(record.value("sealed").toString().toLatin1()), aliceIdentity.configuration().localCertificate(), context)));
        }
        QImage original(17, 23, QImage::Format_ARGB32);
        original.fill(QColor(30, 140, 210, 120)); original.setText("Private metadata", "must not be uploaded");
        QByteArray source; QBuffer imageBuffer(&source); QVERIFY(imageBuffer.open(QIODevice::WriteOnly));
        QVERIFY(original.save(&imageBuffer, "PNG"));
        QVERIFY(alice.channel.sendChat("Image", source));
        QTRY_VERIFY_WITH_TIMEOUT(!alice.channel.chatPending(), 10000);
        QVERIFY2(alice.channel.chatError().isEmpty(), qPrintable(alice.channel.chatError()));
        QTRY_COMPARE(bea->channel.messages().size(), 2);
        const auto attachment = bea->channel.messages().last().toMap().value("image").toMap();
        const auto hash = attachment.value("hash").toString();
        QVERIFY(bea->channel.requestImage(hash));
        QTRY_VERIFY(!bea->channel.imageSource(hash).isEmpty());
        const auto png = QByteArray::fromBase64(bea->channel.imageSource(hash).section(',', 1).toLatin1());
        const auto image = QImage::fromData(png);
        QCOMPARE(image.size(), original.size());
        QCOMPARE(image.pixelColor(0, 0), original.pixelColor(0, 0));
        QVERIFY(image.textKeys().isEmpty());
        QVERIFY(alice.channel.setPrivateBlocked(conversation, true));
        QTRY_VERIFY(bea->channel.privateBlocked());
        QVERIFY(bea->channel.setPrivateBlocked(conversation, true));
        QTRY_VERIFY(bea->channel.privateBlockedByMe());
        QVERIFY(alice.channel.setPrivateBlocked(conversation, false));
        QTRY_VERIFY(!alice.channel.privateBlockedByMe());
        QVERIFY(alice.channel.privateBlocked());
        QVERIFY(!alice.channel.sendChat("still blocked by Bea"));
        bea.reset();
        relay.reset();
        relay = std::make_unique<Device>(dir.filePath("relay"), "Relay", relayIdentity, clock);
        QCOMPARE(relay->channel.privateLifetimeDays(), 7);
        QVERIFY(relay->channel.listen(QHostAddress::LocalHost));
        QVERIFY(alice.join(relay->channel));
        QTRY_VERIFY(alice.channel.chatReady());
        QVERIFY(alice.channel.openPrivateChat(relayIdentity.id(), beaIdentity.id()));
        QTRY_VERIFY(alice.channel.privateBlocked());
        QVERIFY(!alice.channel.sendChat("block survived"));
        bea = std::make_unique<Device>(dir.filePath("bea"), "Bea renamed", beaIdentity, clock);
        QVERIFY(bea->join(relay->channel));
        QTRY_VERIFY(bea->channel.canPrivateChat(relayIdentity.id(), aliceIdentity.id()));
        QVERIFY(bea->channel.openPrivateChat(relayIdentity.id(), aliceIdentity.id()));
        QTRY_COMPARE(bea->channel.messages().size(), 2);
        QCOMPARE(bea->channel.messages().first().toMap().value("expires").toLongLong(), firstExpiry);
        QVERIFY(bea->channel.setPrivateBlocked(conversation, false));
        QTRY_VERIFY(!alice.channel.privateBlocked());
        bea.reset();
        QVERIFY(!relay->channel.setPrivateLifetimeDays(90));
#if !SQUADSPEAK_STORE_BUILD
        QVERIFY(relay->session.setSupporterEnabled(true));
        QVERIFY(relay->channel.setPrivateLifetimeDays(90));
        QTRY_COMPARE(alice.channel.chatLifetimeDays(), 90);
        QVERIFY(relay->session.setSupporterEnabled(false));
        QTRY_COMPARE(alice.channel.chatLifetimeDays(), 30);
#endif
        QVERIFY(relay->channel.setPrivateLifetimeDays(1));
        QTRY_COMPARE(alice.channel.chatLifetimeDays(), 1);
        QVERIFY(alice.channel.sendChat("Queued while offline"));
        QTRY_VERIFY(!alice.channel.chatPending());
        now += 2 * ChatHistory::lifetime;
        bea = std::make_unique<Device>(dir.filePath("bea"), "Bea", beaIdentity, clock);
        QVERIFY(bea->join(relay->channel));
        QTRY_VERIFY(bea->channel.canPrivateChat(relayIdentity.id(), aliceIdentity.id()));
        QVERIFY(bea->channel.openPrivateChat(relayIdentity.id(), aliceIdentity.id()));
        QTRY_COMPARE(bea->channel.messages().size(), 2);
        QCOMPARE(bea->channel.messages().first().toMap().value("expires").toLongLong(), firstExpiry);
        QVERIFY(!alice.channel.sendChat("Corrupt", QByteArray("not an image")) || alice.channel.chatPending());
        QTRY_VERIFY(!alice.channel.chatPending());
        QVERIFY(!alice.channel.chatError().isEmpty());
        QCOMPARE(bea->channel.messages().size(), 2);
    }

    void privateChatsReuseHistoryWithoutExposingContentToHost() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto hostIdentity = TlsIdentity::create();
        Device host(dir.filePath("host"), "Host", hostIdentity, [&] { return now; });
        Device a(dir.filePath("a"), "Alice", TlsIdentity::create(), [&] { return now; });
        Device b(dir.filePath("b"), "Bea", TlsIdentity::create(), [&] { return now; });
        Device unrelated(dir.filePath("other"), "Other", TlsIdentity::create(), [&] { return now; });
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        DelayedLink senderLink(host.channel.port());
        for (auto* client : {&a, &b, &unrelated}) {
            QVERIFY(client->channel.join(host.channel.ownId(), "127.0.0.1",
                client == &a ? senderLink.port() : host.channel.port()));
            QTRY_VERIFY(!host.channel.requests().isEmpty());
            QVERIFY(host.channel.decide(client->channel.ownId(), true));
            QTRY_VERIFY(client->channel.chatReady());
        }
        QTRY_VERIFY(a.channel.canPrivateChat(host.channel.ownId(), b.channel.ownId()));
        QVERIFY(a.channel.openPrivateChat(host.channel.ownId(), b.channel.ownId()));
        QTRY_VERIFY(a.channel.chatReady());
        const auto conversation = a.channel.chatHostId();
        QVERIFY(conversation != host.channel.ownId());
        QVERIFY(a.channel.sendChat("A secret only Alice and Bea can read"));
        QTRY_VERIFY(!a.channel.chatPending());
        QTRY_VERIFY(b.channel.privateChats().size() >= 1);
        QVERIFY(b.channel.openPrivateChat(host.channel.ownId(), a.channel.ownId()));
        QTRY_COMPARE(b.channel.messages().size(), 1);
        QCOMPARE(b.channel.messages().first().toMap().value("text").toString(), QString("A secret only Alice and Bea can read"));
        QCOMPARE(b.channel.chatHostId(), conversation);
        QVERIFY(!unrelated.channel.openSavedChat(conversation));
        for (const auto& message : unrelated.channel.messages())
            QVERIFY(!message.toMap().value("text").toString().contains("A secret"));
        QVERIFY(b.channel.setPrivateBlocked(conversation, true));
        QTRY_VERIFY(a.channel.privateBlocked());
        QVERIFY(!a.channel.sendChat("blocked"));
        QVERIFY(!b.channel.sendChat("also blocked"));
        QVERIFY(b.channel.setPrivateBlocked(conversation, false));
        QTRY_VERIFY(!a.channel.privateBlocked());
        // Receiving a message does not confirm delivery back to its sender.
        senderLink.holdReliable = true;
        QVERIFY(a.channel.sendChat("allowed again"));
        QTRY_COMPARE(b.channel.messages().size(), 2);
        QVERIFY(a.channel.chatPending());
        QVERIFY(!a.channel.sendChat("Wait for the receipt"));
        senderLink.holdReliable = false;
        QTRY_VERIFY(!a.channel.chatPending());
        QVERIFY(b.channel.closeChat(host.channel.ownId()));
        QVERIFY(a.channel.sendChat("Stored while Bea is offline"));
        QTRY_VERIFY(!a.channel.chatPending());
        QVERIFY(b.join(host.channel));
        QTRY_VERIFY(b.channel.chatReady());
        QVERIFY(b.channel.openPrivateChat(host.channel.ownId(), a.channel.ownId()));
        QTRY_COMPARE(b.channel.messages().size(), 3);
        now += ChatHistory::lifetime;
        QVERIFY(b.channel.refreshChat());
        QTRY_VERIFY(b.channel.messages().isEmpty());
        const auto stillListed = [&] {
            const auto entries = b.channel.savedChannels();
            return std::any_of(entries.begin(), entries.end(), [&](const auto& row) { return row.toMap().value("id") == conversation; });
        };
        QTRY_VERIFY(!stillListed());
    }
    void delayedLinkPreservesBytesBeforeUpstreamConnects() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        DelayedLink link(server.serverPort(), 200);
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, link.port());
        QTRY_COMPARE(client.state(), QAbstractSocket::ConnectedState);
        const QByteArray request = "early handshake bytes";
        QCOMPARE(client.write(request), request.size());
        client.flush();
        QTRY_VERIFY(server.hasPendingConnections());
        auto* upstream = server.nextPendingConnection();
        QTRY_COMPARE(upstream->bytesAvailable(), request.size());
        QCOMPARE(upstream->readAll(), request);
        const QByteArray response = "server reply";
        QCOMPARE(upstream->write(response), response.size());
        upstream->flush();
        QTRY_COMPARE(client.bytesAvailable(), response.size());
        QCOMPARE(client.readAll(), response);
        client.abort();
        QTRY_COMPARE(upstream->state(), QAbstractSocket::UnconnectedState);
    }
    void approvalSurvivesClientSourceAddressChangeButNotIdentityReplacement() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("host"), "Owner");
        QVERIFY(host.channel.listen(QHostAddress::Any));
        QHostAddress firstSource;
        for (int attempt = 0; attempt < 3; ++attempt) {
            QSslSocket socket;
            socket.setSslConfiguration((attempt == 2 ? TlsIdentity::create() : identity).configuration());
            connect(&socket, &QSslSocket::sslErrors, &socket,
                [&socket](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
            const auto source = attempt == 0 ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(QHostAddress::LocalHostIPv6);
            QVERIFY2(socket.bind(source, 0), qPrintable(socket.errorString()));
            socket.connectToHostEncrypted(source.toString(), host.channel.port());
            QTRY_VERIFY_WITH_TIMEOUT(socket.isEncrypted(), 3000);
            if (attempt == 0) firstSource = socket.localAddress();
            else QVERIFY(socket.localAddress() != firstSource);
            const auto hello = wireFrame({{"type", "hello"}, {"version", 1}, {"name", "Travelling member"},
                {"available", true}, {"muted", true}, {"futureField", QJsonObject{{"camera", true}}}});
            QCOMPARE(socket.write(hello), hello.size());
            if (attempt == 0) {
                QTRY_COMPARE(host.channel.requests().size(), 1);
                QVERIFY(host.channel.decide(identity.id(), true));
            }
            if (attempt < 2) {
                QTRY_COMPARE(host.channel.hostClients().size(), 1);
                QVERIFY(host.channel.requests().isEmpty());
            } else {
                QTRY_COMPARE(host.channel.requests().size(), 1);
                QVERIFY(host.channel.hostClients().isEmpty());
            }
            socket.abort();
            QTRY_VERIFY(host.channel.hostClients().isEmpty() && host.channel.requests().isEmpty());
        }
    }
    void inactiveContactsExpirePermissionsAndSavedConnections() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        const auto hostId = TlsIdentity::create(), visitorId = TlsIdentity::create(), bannedId = TlsIdentity::create();
        {
            Device host(dir.filePath("host"), "Host", hostId, clock), visitor(dir.filePath("visitor"), "Visitor", visitorId, clock);
            QVERIFY(host.channel.decide(visitorId.id(), true));
            QVERIFY(host.channel.setBlocked(bannedId.id(), true));
            QVERIFY(host.channel.listen(QHostAddress::LocalHost));
            QVERIFY(visitor.join(host.channel));
            QTRY_VERIFY(visitor.channel.joined());
            QVERIFY(!visitor.channel.savedChannels().isEmpty());
            QVERIFY(host.channel.setRemotePermission(visitorId.id(), true));
            QCOMPARE(host.channel.controllers().size(), 1);
            QVERIFY(visitor.channel.leave());
        }
        now += 365LL * 86400000 - 1;
        {
            QFile file(dir.filePath("host.channel")); QVERIFY(file.open(QIODevice::ReadOnly));
            auto object = QJsonDocument::fromJson(file.readAll()).object(); file.close();
            object.insert("futureOption", "preserved");
            auto security = object.value("security").toObject(); security.insert("futureSecurity", true);
            object.insert("security", security);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QJsonDocument(object).toJson());
        }
        {
            Device host(dir.filePath("host"), "Host", hostId, clock), visitor(dir.filePath("visitor"), "Visitor", visitorId, clock);
            QCOMPARE(host.channel.blockedClients().size(), 1);
            QCOMPARE(host.channel.controllers().size(), 1);
            QVERIFY(!visitor.channel.savedChannels().isEmpty());
        }
        ++now;
        Device host(dir.filePath("host"), "Host", hostId, clock), visitor(dir.filePath("visitor"), "Visitor", visitorId, clock);
        QVERIFY(host.channel.blockedClients().isEmpty());
        QVERIFY(host.channel.controllers().isEmpty());
        QVERIFY(visitor.channel.savedChannels().isEmpty());
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(visitor.join(host.channel));
        QTRY_COMPARE(host.channel.requests().size(), 1);
        QVERIFY(!visitor.channel.joined());
        QVERIFY(host.channel.decide(visitorId.id(), true));
        QTRY_VERIFY(visitor.channel.joined());
        QFile file(dir.filePath("host.channel")); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto stored = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(stored.value("futureOption").toString(), QString("preserved"));
        QVERIFY(stored.value("security").toObject().value("futureSecurity").toBool());
    }
    void connectedContactsRenewPermissionsAcrossTheYearBoundary() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("host"), "Host", identity, clock), visitor(dir.filePath("visitor"), "Visitor", TlsIdentity::create(), clock);
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(visitor.channel.ownId(), true));
        QVERIFY(visitor.join(host.channel)); QTRY_VERIFY(visitor.channel.joined());
        QSignalSpy observed(&host.channel, &LocalChannel::contactsObserved);
        QSignalSpy visitorObserved(&visitor.channel, &LocalChannel::contactsObserved);
        now += 365LL * 86400000;
        QTRY_VERIFY(!observed.isEmpty() && !visitorObserved.isEmpty());
        QVERIFY(observed.last().first().toStringList().contains(visitor.channel.ownId()));
        QVERIFY(visitorObserved.last().first().toStringList().contains(host.channel.ownId()));
        QVERIFY(visitor.channel.leave());
        QVERIFY(visitor.channel.closeChat(host.channel.ownId()));
        QTRY_VERIFY(host.channel.hostClients().isEmpty());
        QVERIFY(visitor.join(host.channel)); QTRY_VERIFY(visitor.channel.joined());
        QVERIFY(host.channel.requests().isEmpty());
    }
    void contactCleanupWaitsForTheProtectedIdentity() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        const auto identity = TlsIdentity::create();
        const auto path = dir.filePath("host");
        {
            Device host(path, "Host", identity, clock);
            QVERIFY(host.channel.decide(identity.id(), true));
        }
        now += 365LL * 86400000;
        VoiceSession session(path + ".session");
        LocalChannel waiting(session, path + ".channel", std::nullopt, clock);
        QVERIFY(!waiting.ready());
        QFile file(path + ".channel"); QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(QJsonDocument::fromJson(file.readAll()).object().value("approved").toArray().contains(identity.id()));
    }
    void briefContactRenewsPermissionsBetweenMaintenancePasses() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        Device host(dir.filePath("host"), "Host", TlsIdentity::create(), clock),
            visitor(dir.filePath("visitor"), "Visitor", TlsIdentity::create(), clock);
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(visitor.channel.ownId(), true));
        QSignalSpy observed(&host.channel, &LocalChannel::contactsObserved);
        now += 365LL * 86400000 - 60000;
        QTRY_VERIFY(!observed.isEmpty()); observed.clear();
        now += 1000;
        QVERIFY(visitor.join(host.channel)); QTRY_VERIFY(visitor.channel.joined());
        QTRY_COMPARE(host.channel.hostClients().size(), 1);
        QVERIFY(visitor.channel.leave()); QVERIFY(visitor.channel.closeChat(host.channel.ownId()));
        QTRY_VERIFY(host.channel.hostClients().isEmpty());
        now += 60000;
        QTRY_VERIFY(!observed.isEmpty());
        QVERIFY(visitor.join(host.channel)); QTRY_VERIFY(visitor.channel.joined());
        QVERIFY(host.channel.requests().isEmpty());
    }
    void olderPermissionsGetOneYearFromFirstLoad_data() {
        QTest::addColumn<bool>("emptyTimestamps");
        QTest::newRow("missing") << false;
        QTest::newRow("empty") << true;
    }
    void olderPermissionsGetOneYearFromFirstLoad() {
        QFETCH(bool, emptyTimestamps);
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        const auto identity = TlsIdentity::create();
        const auto path = dir.filePath("host");
        {
            Device host(path, "Host", identity, clock);
            QVERIFY(host.channel.setBlocked(TlsIdentity::create().id(), true));
        }
        QFile file(path + ".channel"); QVERIFY(file.open(QIODevice::ReadOnly));
        auto saved = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        auto security = saved.value("security").toObject(); security.remove("lastSeen");
        if (emptyTimestamps) security.insert("lastSeen", QJsonObject{});
        saved.insert("security", security);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(QJsonDocument(saved).toJson()) > 0); file.close();
        now += 365LL * 86400000;
        { Device loaded(path, "Host", identity, clock); QCOMPARE(loaded.channel.blockedClients().size(), 1); }
        now += 365LL * 86400000 - 1;
        { Device loaded(path, "Host", identity, clock); QCOMPARE(loaded.channel.blockedClients().size(), 1); }
        ++now;
        Device expired(path, "Host", identity, clock);
        QVERIFY(expired.channel.blockedClients().isEmpty());
    }
    void failedContactCleanupRetainsPermissionsAndRetries() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto path = dir.filePath("host");
        Device host(path, "Host", TlsIdentity::create(), [&] { return now; });
        QVERIFY(host.channel.setBlocked(TlsIdentity::create().id(), true));
        QVERIFY(QFile::rename(path + ".channel", path + ".saved"));
        QVERIFY(QDir().mkdir(path + ".channel"));
        now += 365LL * 86400000;
        QTRY_VERIFY(host.channel.status().contains("could not be saved"));
        QCOMPARE(host.channel.blockedClients().size(), 1);
        QVERIFY(QDir().rmdir(path + ".channel"));
        QVERIFY(QFile::rename(path + ".saved", path + ".channel"));
        now += 60000;
        QTRY_VERIFY(host.channel.blockedClients().isEmpty());
    }
    void expiredPrivateContactsRemoveConversationFilesAndBlocks() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        const auto hostId = TlsIdentity::create();
        QString conversation;
        {
            Device host(dir.filePath("host"), "Host", hostId, clock),
                alice(dir.filePath("alice"), "Alice", TlsIdentity::create(), clock),
                bea(dir.filePath("bea"), "Bea", TlsIdentity::create(), clock);
            QVERIFY(host.channel.listen(QHostAddress::LocalHost));
            for (auto* client : {&alice, &bea}) {
                QVERIFY(host.channel.decide(client->channel.ownId(), true));
                QVERIFY(client->join(host.channel)); QTRY_VERIFY(client->channel.chatReady());
            }
            QTRY_VERIFY(alice.channel.canPrivateChat(host.channel.ownId(), bea.channel.ownId()));
            QVERIFY(alice.channel.openPrivateChat(host.channel.ownId(), bea.channel.ownId()));
            conversation = alice.channel.chatHostId();
            QVERIFY(alice.channel.sendChat("Expires")); QTRY_VERIFY(!alice.channel.chatPending());
            QVERIFY(alice.channel.setPrivateBlocked(conversation, true));
            QTRY_VERIFY(alice.channel.privateBlockedByMe());
        }
        const auto file = dir.filePath("host.channel.private/") + conversation + ".sqlite";
        QVERIFY(QFile::exists(file));
        now += 365LL * 86400000;
        Device host(dir.filePath("host"), "Host", hostId, clock);
        QVERIFY(!QFile::exists(file));
        QFile store(dir.filePath("host.channel")); QVERIFY(store.open(QIODevice::ReadOnly));
        const auto security = QJsonDocument::fromJson(store.readAll()).object().value("security").toObject();
        QVERIFY(security.value("privatePeers").toObject().isEmpty());
        QVERIFY(security.value("privateStates").toObject().isEmpty());
        QVERIFY(security.value("lastSeen").toObject().isEmpty());
    }
    void tlsWithoutDeviceCertificateCannotReachAdmission() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        const auto messagesBefore = host.channel.messages().size();
        const auto statusBefore = host.channel.status();

        QSslSocket socket;
        auto configuration = QSslConfiguration::defaultConfiguration();
        configuration.setLocalCertificate(QSslCertificate{});
        configuration.setPrivateKey(QSslKey{});
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        configuration.setProtocol(QSsl::TlsV1_2OrLater);
        configuration.setAllowedNextProtocols({"squadspeak/1"});
        socket.setSslConfiguration(configuration);
        connect(&socket, &QSslSocket::sslErrors, &socket,
            [&socket](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
        QSignalSpy connected(&socket, &QSslSocket::connected);
        QSignalSpy disconnected(&socket, &QSslSocket::disconnected);
        socket.connectToHostEncrypted("127.0.0.1", host.channel.port());
        QTRY_VERIFY_WITH_TIMEOUT(!connected.isEmpty(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!disconnected.isEmpty(), 5000);
        QVERIFY(!socket.isEncrypted());
        QVERIFY(host.channel.hostClients().isEmpty());
        QVERIFY(host.channel.requests().isEmpty());
        QCOMPARE(host.channel.messages().size(), messagesBefore);
        QCOMPARE(host.channel.status(), statusBefore);
    }
    void cancelledIncomingConnectionPreservesHostStatus() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), client(dir.filePath("client"), "Member");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        const auto statusBefore = host.channel.status();
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, host.channel.port());
        QVERIFY(waitForEvents(&probe, &QTcpSocket::connected, [&] { return probe.state() == QAbstractSocket::ConnectedState; }));
        probe.disconnectFromHost();
        QVERIFY(waitForEvents(&probe, &QTcpSocket::disconnected, [&] { return probe.state() == QAbstractSocket::UnconnectedState; }));
        QVERIFY(client.join(host.channel));
        QTRY_VERIFY(client.channel.joined());
        QVERIFY(host.channel.hosting());
        QCOMPARE(host.channel.status(), statusBefore);
    }
    void extensionsPreserveAdmissionAndKnownMessageValidation_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto* scenario : {"admitted", "pending", "malformed-capabilities", "malformed-presence", "malformed-type",
                                    "chat-extension", "chat-malformed-kind", "chat-malformed-history",
                                    "valid-language", "future-language", "malformed-language", "oversized-language", "malformed-language-presence"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void extensionsPreserveAdmissionAndKnownMessageValidation() {
        QFETCH(QString, scenario);
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("host"), "Owner");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        if (scenario != "pending") QVERIFY(host.channel.decide(identity.id(), true));
        QSslSocket socket;
        socket.setSslConfiguration(identity.configuration());
        connect(&socket, &QSslSocket::sslErrors, &socket,
            [&socket](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
        QByteArray incoming;
        QList<QJsonObject> messages;
        connect(&socket, &QSslSocket::readyRead, &socket, [&] {
            incoming += socket.readAll();
            while (incoming.size() >= 4) {
                const auto size = qFromBigEndian<quint32>(incoming.constData());
                if (incoming.size() < size + 4) break;
                messages.append(QJsonDocument::fromJson(incoming.mid(4, size)).object());
                incoming.remove(0, size + 4);
            }
        });
        socket.connectToHostEncrypted("127.0.0.1", host.channel.port());
        QTRY_VERIFY(socket.isEncrypted());
        QJsonObject hello{{"type", "hello"}, {"version", 1}, {"name", "Future client"}, {"available", true},
            {"muted", true}, {"adaptiveAudio", true}, {"remoteOffers", true},
            {"capabilities", QJsonArray{"camera.v2"}}, {"futureField", QJsonObject{{"nested", true}}}};
        if (scenario == "malformed-capabilities") hello.insert("capabilities", QJsonArray{42});
        if (scenario == "valid-language") hello.insert("language", "de-DE");
        if (scenario == "future-language") hello.insert("language", "qaa");
        if (scenario == "malformed-language") hello.insert("language", 42);
        if (scenario == "oversized-language") hello.insert("language", QString(1024, 'a'));
        socket.write(wireFrame(hello));
        if (scenario == "malformed-capabilities" || scenario == "malformed-language" || scenario == "oversized-language") {
            QTRY_COMPARE(socket.state(), QAbstractSocket::UnconnectedState);
            QVERIFY(host.channel.hostClients().isEmpty()); return;
        }
        QTRY_VERIFY(!messages.isEmpty());
        QJsonObject extension{{"type", "cameraOffer"}, {"futurePayload", QJsonArray{1, "unknown"}}};
        if (scenario == "malformed-presence") extension = {{"type", "presence"}, {"name", "Missing required fields"}};
        if (scenario == "malformed-type") extension.insert("type", 12);
        if (scenario == "malformed-language-presence")
            extension = {{"type", "presence"}, {"name", "Member"}, {"available", true}, {"muted", true}, {"language", "../../de"}};
        if (scenario.startsWith("chat-")) {
            const auto chatKey = [&] {
                for (const auto& message : messages) if (message.value("type") == "chatKey") return message;
                return QJsonObject{};
            };
            QTRY_VERIFY(!chatKey().isEmpty());
            const auto key = chatKey();
            const auto epoch = key.value("epoch").toString();
            QJsonObject payload{{"kind", "reaction.v2"}, {"future", true}};
            if (scenario == "chat-malformed-kind") payload.insert("kind", 42);
            if (scenario == "chat-malformed-history") payload.insert("kind", "history");
            const auto context = "SquadSpeak/chat/v1/" + host.channel.ownId().toUtf8() + '/' + epoch.toUtf8() + '/' + identity.id().toUtf8();
            const auto data = TlsIdentity::seal(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                QByteArray::fromBase64(key.value("key").toString().toLatin1()), context);
            extension = {{"type", "chat"}, {"epoch", epoch}, {"data", QString::fromLatin1(data.toBase64())}};
        }
        socket.write(wireFrame(extension));
        socket.write(wireFrame({{"type", "ping"}}));
        if (scenario != "admitted" && scenario != "chat-extension" && scenario != "valid-language" && scenario != "future-language") {
            QTRY_COMPARE(socket.state(), QAbstractSocket::UnconnectedState); return;
        }
        QTRY_VERIFY(std::any_of(messages.begin(), messages.end(), [](const auto& message) { return message.value("type") == "pong"; }));
        QCOMPARE(socket.state(), QAbstractSocket::ConnectedState);
        const auto joined = std::find_if(messages.begin(), messages.end(), [](const auto& message) { return message.value("type") == "join"; });
        QVERIFY(joined != messages.end()); QCOMPARE(joined->value("result").toString(), QString("accepted"));
        QCOMPARE(joined->value("adaptiveAudio").toBool(), false);
        if (scenario == "valid-language" || scenario == "future-language")
            QCOMPARE(host.channel.hostClients().first().toMap().value("language").toString(), scenario == "valid-language" ? "de-DE" : "qaa");
        else QVERIFY(host.channel.hostClients().first().toMap().value("language").toString().isEmpty());
        QVERIFY(std::none_of(messages.begin(), messages.end(), [](const auto& message) { return message.value("type") == "remoteOffer"; }));
    }
    void futureChatResponsesPreserveTheClientConnection_data() {
        QTest::addColumn<QJsonObject>("payload"); QTest::addColumn<bool>("accepted");
        QTest::addColumn<QString>("language");
        QTest::newRow("unknown") << QJsonObject{{"kind", "reaction.v2"}, {"future", true}} << true << QString("en");
        VoiceSession languages;
        for (const auto& entry : languages.languages()) {
            const auto language = entry.toMap().value("code").toString();
            for (const auto* kind : {"error", "imageError"}) {
                for (const auto* text : {"Invalid chat timestamp.", "Peer extension failure: camera.v3"}) {
                    QTest::newRow(qPrintable(language + '/' + kind + '/' + text))
                        << QJsonObject{{"kind", kind}, {"now", 1000}, {"error", text}} << true << language;
                }
            }
        }
        for (const auto* kind : {"error", "imageError"})
            QTest::newRow(qPrintable(QString(kind) + "-embedded-null"))
                << QJsonObject{{"kind", kind}, {"now", 1000},
                    {"error", QString("Invalid chat timestamp.") + QChar::Null + "future"}} << true << QString("de");
        const QJsonObject announcement{{"sequence", 1}, {"created", 1000}, {"expires", 1000 + ChatHistory::lifetime},
            {"sender", QString(64, 'a')}, {"name", "Channel news"}, {"avatarId", "system"},
            {"request", "b3244c2f-ae65-4d02-a355-a20539808cb1"}, {"text", "Still compatible"},
            {"event", QJsonObject{{"kind", "announcement"}, {"futureMetadata", QJsonObject{{"priority", 2}}}}}};
        QTest::newRow("announcement-extension") << QJsonObject{{"kind", "message"}, {"now", 1000}, {"record", announcement}} << true << QString("en");
        auto future = announcement;
        future.insert("event", QJsonObject{{"kind", "game.invitation.v2"}, {"future", QJsonObject{{"format", 2}}}});
        QTest::newRow("future-system-event") << QJsonObject{{"kind", "message"}, {"now", 1000}, {"record", future}} << true << QString("en");
        for (const QJsonValue& kind : {QJsonValue{}, QJsonValue(42), QJsonValue(""), QJsonValue(" "),
            QJsonValue(QString(65, 'x')), QJsonValue("future\n"), QJsonValue("joined")}) {
            auto malformed = announcement;
            malformed.insert("event", QJsonObject{{"kind", kind}});
            QTest::newRow(qPrintable("invalid-event-" + QJsonDocument(QJsonArray{kind}).toJson(QJsonDocument::Compact)))
                << QJsonObject{{"kind", "message"}, {"now", 1000}, {"record", malformed}} << false << QString("en");
        }
        auto invalid = announcement;
        invalid.insert("avatarId", "mossling");
        QTest::newRow("invalid-announcement") << QJsonObject{{"kind", "message"}, {"now", 1000}, {"record", invalid}} << false << QString("en");
        QTest::newRow("missing-kind") << QJsonObject{} << false << QString("en");
        QTest::newRow("malformed-kind") << QJsonObject{{"kind", 42}} << false << QString("en");
        QTest::newRow("malformed-message") << QJsonObject{{"kind", "message"}, {"now", 1000}} << false << QString("en");
    }
    void futureChatResponsesPreserveTheClientConnection() {
        QFETCH(QJsonObject, payload); QFETCH(bool, accepted); QFETCH(QString, language);
        QTranslator catalog;
        if (language != "en") {
            QVERIFY(catalog.load(":/i18n/squadspeak_" + language + ".qm"));
            QVERIFY(QCoreApplication::installTranslator(&catalog));
        }
        const auto remove = qScopeGuard([&] { QCoreApplication::removeTranslator(&catalog); });
        // Every row keeps its own profile, sockets and keys. Reuse only the
        // immutable device certificates, whose generation is tested separately.
        static const auto identity = TlsIdentity::create();
        static const auto clientIdentity = TlsIdentity::create();
        QTemporaryDir dir; Device client(dir.filePath("client"), "Member", clientIdentity);
        auto configuration = identity.configuration();
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslServer server; server.setSslConfiguration(configuration);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QVERIFY(waitForEvents(&server, &QTcpServer::pendingConnectionAvailable, [&] { return server.hasPendingConnections(); }));
        auto* socket = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(socket);
        QVERIFY(waitForEvents(socket, &QSslSocket::readyRead, [&] { return socket->bytesAvailable() > 4; }));
        socket->readAll();
        const auto key = TlsIdentity::newKey(); const auto epoch = QUuid::createUuid().toString(QUuid::WithoutBraces);
        socket->write(wireFrame({{"type", "join"}, {"result", "accepted"}}));
        socket->write(wireFrame({{"type", "chatKey"}, {"key", QString::fromLatin1(key.toBase64())}, {"epoch", epoch}, {"now", 1000}}));
        QVERIFY(waitForEvents(&client.channel, &LocalChannel::chatChanged, [&] { return client.channel.chatReady(); }));
        QSignalSpy notifications(&client.channel, &LocalChannel::chatNotification);
        const auto data = TlsIdentity::seal(QJsonDocument(payload).toJson(QJsonDocument::Compact), key,
            "SquadSpeak/chat/v1/" + identity.id().toUtf8() + '/' + epoch.toUtf8() + "/host");
        socket->write(wireFrame({{"type", "chat"}, {"epoch", epoch}, {"data", QString::fromLatin1(data.toBase64())}}));
        if (!accepted) {
            QVERIFY(waitForEvents(socket, &QSslSocket::disconnected, [&] { return socket->state() == QAbstractSocket::UnconnectedState; }));
            QVERIFY(!client.channel.chatReady()); return;
        }
        // A supported roster after the extension proves both frames were parsed.
        socket->write(wireFrame({{"type", "roster"}, {"host", identity.id()}, {"members", QJsonArray{
            QJsonObject{{"id", identity.id()}, {"name", "Future host"}, {"available", true}, {"muted", false}}}}}));
        QVERIFY(waitForEvents(&client.channel, &LocalChannel::participantsChanged, [&] { return client.channel.participants().size() == 1; }));
        QCOMPARE(participant(client.channel, identity.id()).value("name").toString(), QString("Future host"));
        QVERIFY(client.channel.chatReady());
        if (payload.value("kind") == "error" || payload.value("kind") == "imageError") {
            const auto source = payload.value("error").toString();
            const auto expected = source.contains(QChar::Null) ? source : LocalChannel::tr(source.toUtf8().constData());
            if (language != "en" && source == "Invalid chat timestamp.") QVERIFY(expected != source);
            else if (source.startsWith("Peer extension")) QCOMPARE(expected, source);
            QCOMPARE(client.channel.chatError(), expected);
            socket->write(wireFrame({{"type", "chatKey"}, {"key", QString::fromLatin1(key.toBase64())}, {"epoch", epoch}, {"now", 1000}}));
            QVERIFY(waitForEvents(&client.channel, &LocalChannel::chatChanged, [&] { return client.channel.chatError().isEmpty(); }));
            QVERIFY(client.channel.chatReady());
        } else QVERIFY(client.channel.chatError().isEmpty());
        if (payload.value("kind") == "message") {
            QCOMPARE(client.channel.messages().size(), 1);
            QCOMPARE(client.channel.messages().first().toMap().value("text").toString(), QString("Still compatible"));
            QCOMPARE(notifications.size(), payload.value("record").toObject().value("event").toObject().value("kind") == "announcement" ? 1 : 0);
        }
    }
    void liveChatKeepsSequenceOrderAndRejectsConflictingReplay() {
        QTemporaryDir dir; Device client(dir.filePath("client"), "Reader");
        const auto identity = TlsIdentity::create();
        auto configuration = identity.configuration(); configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslServer server; server.setSslConfiguration(configuration);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.openChat(identity.id(), "127.0.0.1", server.serverPort()));
        QTRY_VERIFY(server.hasPendingConnections());
        auto* socket = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(socket);
        QTRY_VERIFY(socket->bytesAvailable() > 4); socket->readAll();
        const auto key = TlsIdentity::newKey();
        const auto epoch = QUuid::createUuid().toString(QUuid::WithoutBraces);
        socket->write(wireFrame({{"type", "join"}, {"result", "accepted"}}));
        socket->write(wireFrame({{"type", "chatKey"}, {"key", QString::fromLatin1(key.toBase64())}, {"epoch", epoch}, {"now", 1000}}));
        QTRY_VERIFY(client.channel.chatReady());
        const auto send = [&](const QJsonObject& record) {
            const auto data = TlsIdentity::seal(QJsonDocument(QJsonObject{{"kind", "message"}, {"record", record}, {"now", 1000}})
                .toJson(QJsonDocument::Compact), key, "SquadSpeak/chat/v1/" + identity.id().toUtf8() + '/' + epoch.toUtf8() + "/host");
            return socket->write(wireFrame({{"type", "chat"}, {"epoch", epoch}, {"data", QString::fromLatin1(data.toBase64())}})) > 0;
        };
        QJsonObject replay;
        for (const auto sequence : {3, 1, 2}) {
            replay = {{"sender", identity.id()}, {"name", "Writer"}, {"request", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"text", QString::number(sequence)}, {"created", 1000}, {"expires", 1000 + ChatHistory::lifetime}, {"sequence", sequence}};
            QVERIFY(send(replay));
        }
        QTRY_COMPARE(client.channel.messages().size(), 3);
        for (int i = 0; i < 3; ++i) QCOMPARE(client.channel.messages().at(i).toMap().value("sequence").toInt(), i + 1);
        replay.insert("name", "Renamed writer"); QVERIFY(send(replay));
        QTRY_COMPARE(client.channel.messages().at(1).toMap().value("name").toString(), QString("Renamed writer"));
        QCOMPARE(client.channel.messages().size(), 3);
        replay.insert("text", "Changed content with the same sequence"); QVERIFY(send(replay));
        QTRY_COMPARE(socket->state(), QAbstractSocket::UnconnectedState);
        QVERIFY(!client.channel.chatReady());
        QVERIFY(client.channel.chatError().contains("Conflicting chat order"));
    }
    void screenAudioFollowsVoiceOrExplicitViewerAndNeverPreview() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Screen owner"), other(dir.filePath("other"), "Other host"),
            voice(dir.filePath("voice"), "Listener"), viewer(dir.filePath("viewer"), "Viewer"),
            preview(dir.filePath("preview"), "Preview"), pending(dir.filePath("pending"), "Pending");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(other.channel.listen(QHostAddress::LocalHost));
        QVERIFY(!host.channel.setScreenAudio(true));
        QVERIFY(host.channel.setScreenSharing(true));
        QVERIFY(other.channel.decide(viewer.channel.ownId(), true));
        QVERIFY(viewer.join(other.channel)); QTRY_VERIFY(viewer.channel.joined());
        for (auto* device : {&voice, &viewer, &preview}) {
            QVERIFY(host.channel.decide(device->channel.ownId(), true));
            QVERIFY(device->channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
            QTRY_VERIFY(device->channel.chatReady());
            QTRY_VERIFY(device->channel.screenInfo(host.channel.ownId()).value("available").toBool());
        }
        QVERIFY(voice.join(host.channel)); QTRY_VERIFY(voice.channel.joined());
        // The host can reconnect to its own TLS endpoint with its own device
        // identity. It must not receive its own system-audio relay.
        QVERIFY(host.join(host.channel)); QTRY_VERIFY(host.channel.joined());
        QVERIFY(pending.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_COMPARE(host.channel.requests().size(), 1);
        QVERIFY(!pending.channel.watchScreen(host.channel.ownId(), true, 0, true));
        QVERIFY(viewer.channel.watchScreen(host.channel.ownId(), true, 0, true));
        QVERIFY(preview.channel.watchScreen(host.channel.ownId(), true, 3, false));
        QTRY_COMPARE(viewer.channel.screenInfo(host.channel.ownId()).value("tier").toInt(), 1);
        QTRY_COMPARE(preview.channel.screenInfo(host.channel.ownId()).value("tier").toInt(), 3);
        squad::VoiceMixer encoder, decoder;
        std::array<float, 960> samples{};
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = float(0.1 * std::sin(2 * std::numbers::pi * 440 * i / 48000));
        const auto packet = encoder.encode(samples, 48000).first();
        QVERIFY(!host.channel.sendScreenAudio(packet));
        QVERIFY(host.channel.setScreenAudio(true));
        const auto hasScreenAudio = [&](const Device& device) {
            const auto sources = device.channel.audioSources();
            return std::any_of(sources.cbegin(), sources.cend(), [&](const auto& source) {
                const auto member = source.toMap();
                return member.value("id").toString() == host.channel.musicId() && member.value("musicActive").toBool();
            });
        };
        QTRY_VERIFY(hasScreenAudio(voice)); QTRY_VERIFY(hasScreenAudio(viewer));
        QVERIFY(!hasScreenAudio(preview));
        QSignalSpy hostPackets(&host.channel, &LocalChannel::audioReceived),
            voicePackets(&voice.channel, &LocalChannel::audioReceived), viewerPackets(&viewer.channel, &LocalChannel::audioReceived),
            previewPackets(&preview.channel, &LocalChannel::audioReceived), pendingPackets(&pending.channel, &LocalChannel::audioReceived);
        QVERIFY(host.channel.sendScreenAudio(packet));
        QTRY_COMPARE(voicePackets.size(), 1); QTRY_COMPARE(viewerPackets.size(), 1);
        QTest::qWait(50); QCOMPARE(hostPackets.size(), 0);
        QCOMPARE(viewerPackets.first().at(0).toString(), host.channel.musicId());
        QVERIFY(decoder.receive(host.channel.musicId(), viewerPackets.first().at(1).toByteArray(), 0));
        QCOMPARE(previewPackets.size(), 0); QCOMPARE(pendingPackets.size(), 0);
        // Closing the separate window keeps its preview, but stops audio from
        // that channel while the viewer still speaks in the other channel.
        QVERIFY(viewer.channel.watchScreen(host.channel.ownId(), true, 3, false));
        QTRY_COMPARE(viewer.channel.screenInfo(host.channel.ownId()).value("tier").toInt(), 3);
        QVERIFY(!hasScreenAudio(viewer));
        QVERIFY(host.channel.sendScreenAudio(packet)); QTRY_COMPARE(voicePackets.size(), 2);
        QTest::qWait(50); QCOMPARE(viewerPackets.size(), 1);
        QCOMPARE(viewer.channel.joinedHostId(), other.channel.ownId());
        // A preview is audible once its own channel becomes the voice channel.
        QVERIFY(preview.join(host.channel)); QTRY_VERIFY(preview.channel.joined());
        QTRY_VERIFY(hasScreenAudio(preview));
        QVERIFY(host.channel.sendScreenAudio(packet)); QTRY_COMPARE(previewPackets.size(), 1);
        QVERIFY(!host.channel.sendScreenAudio({}));
        QVERIFY(!host.channel.sendScreenAudio(QByteArray(4001, 'x')));
        QVERIFY(host.channel.setScreenAudio(false));
        QVERIFY(!host.channel.sendScreenAudio(packet));
        QVERIFY(host.channel.screenSharing());
        QTRY_VERIFY(!hasScreenAudio(preview));
        QVERIFY(host.channel.setScreenSharing(false));
        QVERIFY(!host.channel.setScreenAudio(true));
    }
    void screenOnlyViewerReceivesUdpAudioWithoutPersonalParticipation_data() {
        QTest::addColumn<bool>("reducedQuality");
        QTest::addColumn<bool>("lostFirstFrame");
        QTest::newRow("normal") << false << false;
        QTest::newRow("reduced-quality") << true << false;
        QTest::newRow("lost-first-frame") << false << true;
    }
    void screenOnlyViewerReceivesUdpAudioWithoutPersonalParticipation() {
        QFETCH(bool, reducedQuality);
        QFETCH(bool, lostFirstFrame);
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), viewer(dir.filePath("viewer"), "Viewer");
        QVERIFY(host.listenWithUdp());
        QVERIFY(host.channel.setScreenSharing(true)); QVERIFY(host.channel.setScreenAudio(true));
        DelayedLink link(host.channel.port()); link.udpDelay = 0;
        QVERIFY(host.channel.decide(viewer.channel.ownId(), true));
        QVERIFY(viewer.channel.openChat(host.channel.ownId(), "127.0.0.1", link.port()));
        QTRY_VERIFY(viewer.channel.screenInfo(host.channel.ownId()).value("available").toBool());
        QVERIFY(viewer.channel.watchScreen(host.channel.ownId(), true, 0, true));
        QTRY_COMPARE(viewer.channel.screenInfo(host.channel.ownId()).value("tier").toInt(), 1);
        squad::VoiceMixer encoder;
        std::array<float, 960> samples{};
        QSet<QByteArray> originals;
        const auto encode = [&] {
            const auto packet = encoder.encode(samples, 48000).first();
            originals.insert(packet);
            return packet;
        };
        int received = 0;
        bool transcoded = false;
        int error = 0;
        const std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
            opus_decoder_create(48000, 1, &error), opus_decoder_destroy);
        QVERIFY(decoder && error == OPUS_OK);
        QObject reception;
        connect(&viewer.channel, &LocalChannel::audioReceived, &reception, [&](const QString& id, const QByteArray& packet) {
            if (id != host.channel.musicId()) return;
            if (!originals.contains(packet)) transcoded = true;
            std::array<float, 960> samples{};
            QCOMPARE(opus_decode_float(decoder.get(), reinterpret_cast<const unsigned char*>(packet.constData()),
                int(packet.size()), samples.data(), int(samples.size()), 0), 960);
            // Adaptation may re-encode Opus. Check audible content rather than
            // requiring the original compressed bytes to survive the relay.
            const auto energy = std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
            if (energy > 0.1) ++received;
        });
        QTimer producer; producer.setInterval(20);
        connect(&producer, &QTimer::timeout, this, [&] { QVERIFY(host.channel.sendScreenAudio(encode())); });
        producer.start();
        if (reducedQuality) {
            link.audioDropEvery = 2;
            QVERIFY(waitForEvents(&viewer.channel, &LocalChannel::audioReceived, [&] { return transcoded; }, 10000));
            link.audioDropEvery = 0;
        }
        const int warmupDatagrams = link.audioDatagrams;
        QTRY_VERIFY_WITH_TIMEOUT(link.audioDatagrams > warmupDatagrams + 5, 5000); producer.stop();
        // Keep reliable traffic held until audio arrives. Prove UDP delivery
        // directly, without racing scheduler jitter against a second timeout.
        link.holdReliable = true;
        // Keep encoder history continuous across silence and the audible marker.
        // Splicing a fresh encoder here creates an artificial codec discontinuity.
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = float(0.2 * std::sin(2 * std::numbers::pi * 880 * i / 48000));
        const auto marker = encode();
        const auto beforeMarker = link.audioDatagrams;
        if (lostFirstFrame) link.audioDropEvery = 1;
        QVERIFY(host.channel.sendScreenAudio(marker));
        producer.start();
        if (lostFirstFrame) {
            QTRY_VERIFY(link.audioDatagrams > beforeMarker);
            link.audioDropEvery = 0;
        }
        // Audio is a stream over a lossy transport. A missing first frame must
        // not make the routing contract depend on one guaranteed datagram.
        QVERIFY(waitForEvents(&viewer.channel, &LocalChannel::audioReceived, [&] { return received >= 3; }));
        producer.stop();
        QVERIFY(!viewer.channel.joined()); QVERIFY(!viewer.channel.sendAudio(marker));
        QVERIFY(viewer.channel.chatReady());
        link.holdReliable = false;
        QVERIFY(viewer.channel.watchScreen(host.channel.ownId(), false));
        const auto beforeClosed = received;
        QVERIFY(host.channel.sendScreenAudio(marker)); QTest::qWait(100); QCOMPARE(received, beforeClosed);
        QVERIFY(viewer.channel.watchScreen(host.channel.ownId(), true, 0, true));
        QTRY_COMPARE(viewer.channel.screenInfo(host.channel.ownId()).value("tier").toInt(), 1);
        producer.start();
        QVERIFY(waitForEvents(&viewer.channel, &LocalChannel::audioReceived, [&] { return received >= beforeClosed + 3; }));
        producer.stop();
        QVERIFY(!viewer.channel.joined());
    }
    void screenAudioRequiresNegotiatedAndValidViewerRequests_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto* scenario : {"enabled", "preview", "legacy-control", "legacy-viewer", "no-capabilities",
                                    "invalid-hello", "invalid-limit"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }
    void screenAudioRequiresNegotiatedAndValidViewerRequests() {
        QFETCH(QString, scenario);
        QTemporaryDir dir; Device host(dir.filePath("host"), "Owner");
        const auto identity = TlsIdentity::create();
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.setScreenSharing(true));
        QVERIFY(host.channel.setScreenAudio(true));
        QVERIFY(host.channel.decide(identity.id(), true));
        QByteArray incoming;
        QSslSocket control, media;
        for (auto* socket : {&control, &media}) {
            socket->setSslConfiguration(identity.configuration());
            connect(socket, &QSslSocket::sslErrors, socket,
                [socket](const QList<QSslError>& errors) { socket->ignoreSslErrors(errors); });
        }
        connect(&control, &QSslSocket::readyRead, &control, [&] { incoming += control.readAll(); });
        control.connectToHostEncrypted("127.0.0.1", host.channel.port());
        QTRY_VERIFY(control.isEncrypted());
        QJsonArray capabilities{"screen-share", "screen-audio"};
        QJsonObject hello{{"type", "hello"}, {"version", 1}, {"name", "Viewer"},
            {"available", true}, {"muted", true}, {"observer", true}, {"capabilities", capabilities}};
        if (scenario == "legacy-control") hello.insert("capabilities", QJsonArray{"screen-share"});
        if (scenario == "no-capabilities") hello.remove("capabilities");
        control.write(wireFrame(hello));
        QTRY_VERIFY(incoming.contains("\"accepted\""));
        media.connectToHostEncrypted("127.0.0.1", host.channel.port());
        QTRY_VERIFY(media.isEncrypted());
        QJsonObject request{{"type", "screenHello"}, {"version", 1}, {"minimumTier", 3},
            {"audio", scenario != "preview"}, {"capabilities", capabilities}};
        if (scenario == "legacy-viewer") request.insert("capabilities", QJsonArray{"screen-share"});
        if (scenario == "no-capabilities") request.remove("capabilities");
        if (scenario == "invalid-hello") request.insert("audio", "true");
        media.write(wireFrame(request));
        if (scenario == "invalid-hello") QTRY_COMPARE(media.state(), QAbstractSocket::UnconnectedState);
        else {
            QTRY_COMPARE(host.channel.screenTiers(), QSet<int>{3});
            if (scenario == "invalid-limit") {
                media.write(wireFrame({{"type", "screenLimit"}, {"minimumTier", 3}, {"audio", 1}}));
                QTRY_COMPARE(media.state(), QAbstractSocket::UnconnectedState);
            }
        }
        squad::VoiceMixer encoder;
        const std::array<float, 960> samples{};
        const auto packet = encoder.encode(samples, 48000).first();
        QVERIFY(host.channel.sendScreenAudio(packet));
        control.write(wireFrame({{"type", "ping"}}));
        // A later reply on the same TLS stream proves all prior audio was read.
        QTRY_VERIFY(incoming.contains("\"type\":\"pong\""));
        QCOMPARE(incoming.contains("\"type\":\"screenAudio\""), scenario == "enabled");
        QCOMPARE(control.state(), QAbstractSocket::ConnectedState);
        if (scenario == "enabled") {
            media.abort(); QTRY_VERIFY(host.channel.screenTiers().isEmpty());
            incoming.clear();
            QVERIFY(host.channel.sendScreenAudio(packet));
            control.write(wireFrame({{"type", "ping"}}));
            QTRY_VERIFY(incoming.contains("\"type\":\"pong\""));
            QVERIFY(!incoming.contains("\"type\":\"screenAudio\""));
        }
    }
    void mediaAdmissionExceeds64AcrossOwnedChannels() {
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.session.setSupporterEnabled(true));
        const auto secondId = host.channel.addOwnedChannel("Second");
        auto* second = host.channel.ownChannel(secondId); QVERIFY(second);
        QVERIFY(host.channel.setScreenSharing(true));
        QVERIFY(!second->setScreenSharing(true));
        QVERIFY(host.channel.setScreenSharing(false));
        QVERIFY(second->setScreenSharing(true));
        QVERIFY(!host.channel.setScreenSharing(true));
        std::vector<std::unique_ptr<Device>> clients;
        for (int n = 0; n < 72; ++n) {
            auto device = std::make_unique<Device>(dir.filePath(QString::number(n)), QString("Member %1").arg(n));
            QVERIFY(host.channel.decide(device->channel.ownId(), true));
            QVERIFY(second->decide(device->channel.ownId(), true));
            clients.push_back(std::move(device));
        }
        const auto address = QString("localhost:%1").arg(host.channel.port());
        // Resolve channel-to-device identities before the simultaneous join.
        // Discovery overload has a separate pending-admission contract.
        for (const auto& client : clients) {
            QVERIFY(client->channel.openAddress(address));
            QTRY_VERIFY_WITH_TIMEOUT(!client->channel.directBusy(), 15000);
            const auto hosts = client->channel.hosts();
            QVERIFY(std::any_of(hosts.begin(), hosts.end(), [&](const auto& value) {
                const auto entry = value.toMap();
                return entry.value("id") == secondId && entry.value("deviceId") == host.channel.ownId();
            }));
        }
        for (int n = 0; n < 64; ++n)
            QVERIFY(clients[size_t(n)]->channel.join(n % 2 ? secondId : host.channel.ownId(), "localhost", host.channel.port()));
        QTRY_COMPARE_WITH_TIMEOUT(host.channel.hostParticipants().size(), 32, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(second->hostParticipants().size(), 32, 15000);
        for (int n = 0; n < 64; ++n) QTRY_VERIFY(clients[size_t(n)]->channel.joined());
        auto& overflow = clients[64]->channel;
        QVERIFY(overflow.openChat(secondId, "localhost", host.channel.port()));
        QTRY_VERIFY(overflow.chatReady());
        QTRY_VERIFY(overflow.screenInfo(secondId).value("available").toBool());
        QVERIFY(overflow.watchScreen(secondId, true));
        QTRY_COMPARE(overflow.screenInfo(secondId).value("tier").toInt(), 1);
        QVERIFY(overflow.chatReady());
        QVERIFY(overflow.join(host.channel.ownId(), "localhost", host.channel.port()));
        QTRY_VERIFY(overflow.joined());
        QCOMPARE(host.channel.hostParticipants().size(), 33);
        QCOMPARE(second->hostParticipants().size(), 32);
        const auto longName = QString::fromUcs4(U"🌊").repeated(64);
        for (int n = 0; n < 65; ++n) {
            const auto& client = clients[size_t(n)];
            QVERIFY(client->session.setUserName(longName));
            if (client->channel.joinedHostId() != host.channel.ownId()) {
                QVERIFY(client->channel.join(host.channel.ownId(), "localhost", host.channel.port()));
                QTRY_VERIFY(client->channel.joined());
            }
        }
        for (int n = 65; n < 72; ++n) {
            const auto& client = clients[size_t(n)];
            QVERIFY(client->session.setUserName(longName));
            QVERIFY(client->join(host.channel)); QTRY_VERIFY(client->channel.joined());
        }
        QTRY_COMPARE_WITH_TIMEOUT(clients.back()->channel.participants().size(), 72, 15000);
        QVERIFY(clients.front()->channel.joined());
        QTRY_VERIFY(clients.back()->channel.chatReady());
        QVERIFY(clients.back()->channel.sendChat("Large roster still has working chat"));
        QTRY_VERIFY(!clients.back()->channel.chatPending());
        // Move one listener back to verify isolation between owned channels.
        QVERIFY(clients[1]->channel.join(secondId, "localhost", host.channel.port()));
        QTRY_VERIFY(clients[1]->channel.joined());
        QSignalSpy same(&clients[4]->channel, &LocalChannel::audioReceived), other(&clients[1]->channel, &LocalChannel::audioReceived);
        QVERIFY(clients[2]->session.setMuted(false));
        QTRY_VERIFY(!participant(clients[4]->channel, clients[2]->channel.ownId()).value("muted", true).toBool());
        squad::VoiceMixer encoder;
        std::array<float, 960> samples{};
        for (size_t n = 0; n < samples.size(); ++n) samples[n] = float(0.1 * std::sin(2 * std::numbers::pi * 440 * n / 48000));
        const auto packet = encoder.encode(samples, 48000).first();
        QVERIFY(clients[2]->channel.sendAudio(packet));
        QTRY_COMPARE(same.size(), 1); QCOMPARE(other.size(), 0);
        QVERIFY(host.session.setSupporterEnabled(false));
        QVERIFY(!second->hosting()); QVERIFY(!second->screenSharing());
        QVERIFY(host.channel.hosting()); QVERIFY(clients[2]->channel.joined());
#endif
    }
    void encoderOverloadProtectsMediaAcrossOwnedChannels() {
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), guest(dir.filePath("guest"), "Guest");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.session.setSupporterEnabled(true));
        const auto secondId = host.channel.addOwnedChannel("Second");
        auto* second = host.channel.ownChannel(secondId); QVERIFY(second);
        QVERIFY(host.channel.joinSaved(host.channel.ownId())); QTRY_VERIFY(host.channel.joined());
        QVERIFY(second->setScreenSharing(true));
        QVERIFY(host.channel.decide(guest.channel.ownId(), true));
        QVERIFY(second->decide(guest.channel.ownId(), true));
        QVERIFY(guest.channel.openAddress(QString("localhost:%1").arg(host.channel.port())));
        QTRY_VERIFY(!guest.channel.directBusy());
        QVERIFY(guest.channel.openChat(secondId, "localhost", host.channel.port())); QTRY_VERIFY(guest.channel.chatReady());
        QTRY_VERIFY(guest.channel.screenInfo(secondId).value("available").toBool());
        QVERIFY(second->reportScreenEncodeTime(1200));
        QVERIFY(guest.channel.watchScreen(secondId, true));
        QTRY_COMPARE(guest.channel.screenInfo(secondId).value("status").toString(), QString("busy"));
        QVERIFY(guest.channel.sendChat("Text remains available")); QTRY_VERIFY(!guest.channel.chatPending());
        QVERIFY(guest.join(host.channel));
        QTRY_VERIFY(guest.channel.status().contains("busy"));
        QVERIFY(!guest.channel.joined()); QVERIFY(host.channel.joined());
        QTimer retry;
        connect(&retry, &QTimer::timeout, &guest.channel, [&] { if (!guest.channel.joined()) guest.join(host.channel); });
        retry.start(250); QTRY_VERIFY_WITH_TIMEOUT(guest.channel.joined(), 10000); retry.stop();
        QVERIFY(guest.channel.watchScreen(secondId, true));
        QTRY_COMPARE(guest.channel.screenInfo(secondId).value("tier").toInt(), 1);
        QVERIFY(host.channel.joined());
#endif
    }

    void ownedChannelsCanJoinWithoutPreviouslyOpeningChat() {
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.session.setSupporterEnabled(true));
        const auto id = host.channel.addOwnedChannel("Second");
        QVERIFY(!id.isEmpty());
        QVERIFY(host.channel.joinSaved(id));
        QTRY_VERIFY(host.channel.joined());
        QCOMPARE(host.channel.joinedHostId(), id);
        QVERIFY(host.channel.ownChannel(id)->hostClients().size() == 1);
#endif
    }
    void secondaryChannelGrantsDeviceControlWithoutDuplicateOffers() {
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), client(dir.filePath("client"), "Controller");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.session.setSupporterEnabled(true));
        const auto id = host.channel.addOwnedChannel("Second");
        auto* second = host.channel.ownChannel(id); QVERIFY(second);
        QVERIFY(client.channel.openAddress(QString("localhost:%1").arg(host.channel.port())));
        QTRY_VERIFY(!client.channel.directBusy());
        QVERIFY(second->decide(client.channel.ownId(), true));
        QVERIFY(client.channel.openChat(id, "localhost", host.channel.port()));
        QTRY_VERIFY(client.channel.chatReady());
        QVERIFY(second->setRemotePermission(client.channel.ownId(), true));
        QTRY_COMPARE(client.channel.remoteOffers().size(), 1);
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.channel.openChat(host.channel.ownId(), "localhost", host.channel.port()));
        QTRY_VERIFY(client.channel.chatReady());
        QCOMPARE(client.channel.remoteOffers().size(), 1);
        QVERIFY(client.channel.chooseRemoteOffer(id));
        QTRY_VERIFY(client.channel.controlConnected());
        QVERIFY(client.channel.setRemoteMode(true));
        QTRY_VERIFY(client.channel.remoteAllowed());
        QVERIFY(client.channel.remoteAction("mute", {{"value", false}}));
        QTRY_VERIFY(!host.session.muted());
        QVERIFY(client.channel.remoteAction("join", {{"hostId", id}}));
        QTRY_COMPARE(host.channel.joinedHostId(), id);
        QTRY_VERIFY(host.channel.joined());
        QVERIFY(client.channel.remoteAction("openChat", {{"hostId", id}}));
        QTRY_COMPARE(client.channel.remoteView().value("chatHostId").toString(), id);
        QVERIFY(client.channel.removeChannel(host.channel.ownId()));
        QVERIFY(host.channel.setRemotePermission(client.channel.ownId(), false));
        QTRY_VERIFY(client.channel.remoteOffers().isEmpty());
        QTRY_VERIFY(!client.channel.controlConnected());
#endif
    }
    void ownedChannelsSharePasswordWorkAndReleaseRemovedJobs() {
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.session.setSupporterEnabled(true));
        const auto first = host.channel.addOwnedChannel("First");
        const auto second = host.channel.addOwnedChannel("Second");
        QVERIFY(!first.isEmpty()); QVERIFY(!second.isEmpty());
        auto* a = host.channel.ownChannel(first);
        auto* b = host.channel.ownChannel(second);
        QVERIFY(a->setHostPassword("first-password"));
        QVERIFY(b->setHostPassword("second-password"));
        QVERIFY(!host.channel.setHostPassword("busy-password"));
        QVERIFY(host.channel.removeOwnedChannel(first));
        QTRY_VERIFY(!b->passwordBusy());
        QVERIFY(host.channel.setHostPassword("after-removal"));
        QTRY_VERIFY(!host.channel.passwordBusy());
        QVERIFY(host.channel.passwordProtected());
#endif
    }
    void ownedChannelsShareImageUploadBudget_data() {
        QTest::addColumn<bool>("secondary");
        QTest::addColumn<bool>("invalidImage");
        QTest::newRow("main-after-disconnect") << false << false;
        QTest::newRow("secondary-after-disconnect") << true << false;
        QTest::newRow("main-after-decoder-error") << false << true;
        QTest::newRow("secondary-after-decoder-error") << true << true;
    }
    void ownedChannelsShareImageUploadBudget() {
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QFETCH(bool, secondary);
        QFETCH(bool, invalidImage);
        QTemporaryDir dir;
        constexpr qint64 now = 1700000000000;
        Device host(dir.filePath("host"), "Owner", TlsIdentity::create(), [] { return now; });
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.session.setSupporterEnabled(true));
        const auto childId = host.channel.addOwnedChannel("Second");
        QVERIFY(!childId.isEmpty());
        auto* child = host.channel.ownChannel(childId);
        std::array<LocalChannel*, 5> channels{&host.channel, child, &host.channel, child,
            secondary ? child : &host.channel};
        std::array<TlsIdentity, 5> identities{TlsIdentity::create(), TlsIdentity::create(),
            TlsIdentity::create(), TlsIdentity::create(), TlsIdentity::create()};
        std::array<QByteArray, 5> incoming;
        std::array<QJsonObject, 5> keys;
        std::array<QList<QJsonObject>, 5> replies;
        std::array<QSslSocket, 5> sockets;
        for (size_t i = 0; i < sockets.size(); ++i) {
            QVERIFY(channels[i]->decide(identities[i].id(), true));
            auto& socket = sockets[i];
            socket.setSslConfiguration(identities[i].configuration());
            connect(&socket, &QSslSocket::sslErrors, &socket,
                [&socket](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
            connect(&socket, &QSslSocket::readyRead, &socket, [&, i] {
                auto& buffer = incoming[i];
                buffer += sockets[i].readAll();
                while (buffer.size() >= 4) {
                    const auto size = qFromBigEndian<quint32>(buffer.constData());
                    if (buffer.size() < size + 4) break;
                    const auto message = QJsonDocument::fromJson(buffer.mid(4, size)).object();
                    buffer.remove(0, size + 4);
                    if (message.value("type") == "chatKey") keys[i] = message;
                    if (message.value("type") != "chat") continue;
                    const auto context = "SquadSpeak/chat/v1/" + channels[i]->channelId().toUtf8()
                        + '/' + keys[i].value("epoch").toString().toUtf8() + "/host";
                    const auto plain = TlsIdentity::open(QByteArray::fromBase64(message.value("data").toString().toLatin1()),
                        QByteArray::fromBase64(keys[i].value("key").toString().toLatin1()), context);
                    replies[i].append(QJsonDocument::fromJson(plain).object());
                }
            });
            socket.connectToHostEncrypted("127.0.0.1", host.channel.port());
            QTRY_VERIFY(socket.isEncrypted());
            socket.write(wireFrame({{"type", "hello"}, {"version", 1}, {"targetChannel", channels[i]->channelId()},
                {"name", "Uploader"}, {"available", true}, {"muted", true}, {"observer", true}}));
            QTRY_VERIFY(!keys[i].isEmpty());
        }
        const auto send = [&](size_t i, const QJsonObject& payload) {
            replies[i].clear();
            const auto epoch = keys[i].value("epoch").toString();
            const auto context = "SquadSpeak/chat/v1/" + channels[i]->channelId().toUtf8()
                + '/' + epoch.toUtf8() + '/' + identities[i].id().toUtf8();
            const auto sealed = TlsIdentity::seal(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                QByteArray::fromBase64(keys[i].value("key").toString().toLatin1()), context);
            const auto frame = wireFrame({{"type", "chat"}, {"epoch", epoch}, {"data", QString::fromLatin1(sealed.toBase64())}});
            return sockets[i].write(frame) == frame.size();
        };
        const QByteArray invalid = "not an image";
        QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::blue);
        QByteArray png; QBuffer imageBuffer(&png);
        QVERIFY(imageBuffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&imageBuffer, "PNG"));
        const auto start = [&](const QByteArray& source) {
            return QJsonObject{{"kind", "imageStart"}, {"request", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"issued", now}, {"size", source.size()},
                {"hash", QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex())}};
        };
        auto first = start(invalid);
        for (size_t i = 0; i < 4; ++i) {
            QVERIFY(send(i, i == 0 ? first : start(png)));
            QTRY_VERIFY(!replies[i].isEmpty());
            QCOMPARE(replies[i].last().value("kind").toString(), QString("imageOffset"));
        }
        // Replacing an unfinished upload keeps its existing slot.
        first = start(invalid);
        QVERIFY(send(0, first));
        QTRY_VERIFY(!replies[0].isEmpty());
        QCOMPARE(replies[0].last().value("kind").toString(), QString("imageOffset"));
        const auto fifth = start(png);
        QVERIFY(send(4, fifth));
        QTRY_VERIFY(!replies[4].isEmpty());
        QCOMPARE(replies[4].last().value("kind").toString(), QString("error"));
        QVERIFY(replies[4].last().value("error").toString().contains("Four images"));
        if (invalidImage) {
            QVERIFY(send(0, {{"kind", "imageChunk"}, {"request", first.value("request")},
                {"offset", 0}, {"data", QString::fromLatin1(invalid.toBase64())}}));
            QTRY_VERIFY(!replies[0].isEmpty());
            QCOMPARE(replies[0].last().value("kind").toString(), QString("error"));
        } else {
            const auto epoch = keys[4].value("epoch");
            sockets[0].abort();
            QTRY_COMPARE(host.channel.hostClients().size(), secondary ? 1 : 2);
            if (!secondary) QTRY_VERIFY(keys[4].value("epoch") != epoch);
        }
        QVERIFY(send(4, fifth));
        QTRY_VERIFY(!replies[4].isEmpty());
        QCOMPARE(replies[4].last().value("kind").toString(), QString("imageOffset"));
        QCOMPARE(replies[4].last().value("offset").toInt(), 0);
        QVERIFY(send(4, {{"kind", "imageChunk"}, {"request", fifth.value("request")},
            {"offset", 0}, {"data", QString::fromLatin1(png.toBase64())}}));
        QTRY_VERIFY(!replies[4].isEmpty());
        QCOMPARE(replies[4].last().value("kind").toString(), QString("message"));
        QCOMPARE(replies[4].last().value("record").toObject().value("image").toObject().value("width").toInt(), 8);
        QCOMPARE(sockets[4].state(), QAbstractSocket::ConnectedState);
#endif
    }
    void ownedChannelsShareOneEndpointButKeepAccessAndHistorySeparate() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), client(dir.filePath("client"), "Member");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.addOwnedChannel("Second").isEmpty());
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QVERIFY(host.session.setSupporterEnabled(true));
        const auto id = host.channel.addOwnedChannel("Second");
        QVERIFY(!id.isEmpty()); QVERIFY(id != host.channel.ownId());
        auto* second = host.channel.ownChannel(id);
        QVERIFY(second); QCOMPARE(second->ownId(), host.channel.ownId());
        QCOMPARE(second->port(), host.channel.port());
        QCOMPARE(host.channel.ownedChannels().size(), 2);
        QVERIFY(client.channel.openAddress(QString("localhost:%1").arg(host.channel.port())));
        QTRY_VERIFY(!client.channel.directBusy());
        QCOMPARE(client.channel.availableHosts().size(), 2);
        QVERIFY(!client.channel.chatReady());
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.channel.openChat(host.channel.ownId(), "localhost", host.channel.port()));
        QTRY_VERIFY(client.channel.chatReady());
        QVERIFY(client.channel.sendChat("Only in the main channel"));
        QTRY_COMPARE(memberMessages(client.channel).size(), 1);
        QCOMPARE(client.channel.availableHosts().size(), 1);
        QSignalSpy events(&host.channel, &LocalChannel::channelEvent);
        QVERIFY(second->setHostPassword("separate-password"));
        QTRY_VERIFY(!second->passwordBusy()); QVERIFY(second->passwordProtected());
        QVERIFY(!host.channel.passwordProtected());
        QVERIFY(client.channel.openChat(id, "localhost", host.channel.port()));
        QTRY_VERIFY(client.channel.passwordRequired());
        QVERIFY(client.channel.submitPassword("separate-password", true));
        QTRY_COMPARE(second->requests().size(), 1);
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().first().toString(), QString("request"));
        QCOMPARE(host.channel.requests().first().toMap().value("hostId").toString(), id);
        QVERIFY(!client.channel.chatReady());
        QVERIFY(second->decide(client.channel.ownId(), true));
        QTRY_VERIFY(client.channel.chatReady());
        QCOMPARE(memberMessages(client.channel).size(), 0);
        QVERIFY(client.channel.sendChat("Only in the second channel"));
        QTRY_COMPARE(memberMessages(client.channel).size(), 1);
        QVERIFY(client.channel.openSavedChat(host.channel.ownId()));
        QTRY_COMPARE(memberMessages(client.channel).size(), 1);
        QCOMPARE(memberMessages(client.channel).first().toMap().value("text").toString(), QString("Only in the main channel"));
        QVERIFY(second->setBlocked(client.channel.ownId(), true));
        QVERIFY(client.channel.chatReady());
        QTRY_COMPARE(second->hostClients().size(), 0);
        QCOMPARE(host.channel.hostClients().size(), 1);
#endif
    }
    void ownedChannelLimitExpiryRestartAndConfirmedRemoval() {
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        QString first;
        {
            Device host(dir.filePath("host"), "Owner", identity, clock);
            QVERIFY(host.channel.listen(QHostAddress::LocalHost));
            QVERIFY(host.session.setSupporterEnabled(true));
            for (int n = 0; n < 9; ++n) {
                const auto id = host.channel.addOwnedChannel(QString("Channel %1").arg(n));
                QVERIFY(!id.isEmpty()); if (n == 0) first = id;
            }
            QCOMPARE(host.channel.ownedChannels().size(), 10);
            QVERIFY(host.channel.addOwnedChannel("One too many").isEmpty());
            QVERIFY(host.channel.ownChannel(first)->sendSystemMessage("Saved across expiry"));
            QVERIFY(host.channel.ownChannel(first)->setMessageLifetimeDays(7));
            QVERIFY(host.session.setSupporterEnabled(false));
            QVERIFY(host.channel.hosting());
            for (const auto& item : host.channel.ownedChannels()) if (item.toMap().value("id") != host.channel.ownId())
                QVERIFY(!item.toMap().value("hosting").toBool());
            QVERIFY(QFile::exists(dir.filePath("host.channel.hosts/") + first + "/channel.json.chat.sqlite"));
        }
        now += 365LL * 86400000;
        Device restored(dir.filePath("host"), "Owner", identity, clock);
        QVERIFY(restored.channel.listen(QHostAddress::LocalHost));
        QCOMPARE(restored.channel.ownedChannels().size(), 10);
        QVERIFY(!restored.channel.ownChannel(first)->hosting());
        QVERIFY(restored.session.setSupporterEnabled(true));
        QVERIFY(restored.channel.ownChannel(first)->hosting());
        QCOMPARE(restored.channel.ownChannel(first)->messageLifetimeDays(), 7);
        QVERIFY(!restored.channel.removeOwnedChannel(restored.channel.ownId()));
        QVERIFY(restored.channel.removeOwnedChannel(first));
        QCOMPARE(restored.channel.ownedChannels().size(), 9);
        QVERIFY(!restored.channel.ownChannel(first));
        QVERIFY(!QFile::exists(dir.filePath("host.channel.hosts/") + first));
        QVERIFY(!QFile::exists(dir.filePath("host.channel.hosts/") + first + ".deleted"));
        QVERIFY(!restored.channel.removeOwnedChannel(first));
#endif
    }
    void ownedChannelRemovalFailureKeepsHistoryAvailable() {
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), reader(dir.filePath("reader"), "Reader");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.session.setSupporterEnabled(true));
        const auto id = host.channel.addOwnedChannel("Retained history");
        auto* child = host.channel.ownChannel(id); QVERIFY(child);
        QVERIFY(reader.channel.openAddress(QString("localhost:%1").arg(host.channel.port())));
        QTRY_VERIFY(!reader.channel.directBusy());
        QVERIFY(child->decide(reader.channel.ownId(), true));
        QVERIFY(reader.channel.openChat(id, "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(reader.channel.chatReady());
        QVERIFY(child->sendSystemMessage("Before removal"));
        QTRY_COMPARE(reader.channel.messages().size(), 1);
        const auto directory = dir.filePath("host.channel.hosts/") + id;
        QVERIFY(QDir().mkpath(directory + ".deleted/occupied"));
        QVERIFY(!host.channel.removeOwnedChannel(id));
        QVERIFY(child->hosting()); QVERIFY(reader.channel.chatReady());
        QVERIFY(child->sendSystemMessage("After failed preparation"));
        QTRY_COMPARE(reader.channel.messages().size(), 2);
        QVERIFY(QDir(directory + ".deleted").removeRecursively());
        const auto config = dir.filePath("host.channel");
        QVERIFY(QFile::rename(config, config + ".backup"));
        QVERIFY(QDir().mkpath(config));
        QVERIFY(!host.channel.removeOwnedChannel(id));
        QVERIFY(child->hosting()); QVERIFY(QFile::exists(directory + "/channel.json.chat.sqlite"));
        QVERIFY(child->sendSystemMessage("After failed settings save"));
        QTRY_COMPARE(reader.channel.messages().size(), 3);
        QVERIFY(QDir().rmdir(config)); QVERIFY(QFile::rename(config + ".backup", config));
        QVERIFY(host.channel.removeOwnedChannel(id));
        QVERIFY(!QFile::exists(directory)); QVERIFY(!QFile::exists(directory + ".deleted"));
        QVERIFY(host.channel.hosting());
#endif
    }
    void screenSocketHandoffPreservesBufferedMessages_data() {
        QTest::addColumn<bool>("fragmented");
        QTest::newRow("coalesced") << false;
        QTest::newRow("fragmented") << true;
    }
    void screenSocketHandoffPreservesBufferedMessages() {
        QFETCH(bool, fragmented);
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("host"), "Owner"), client(dir.filePath("client"), "Viewer", identity);
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.setScreenSharing(true));
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(client.channel.chatReady());
        QSslSocket socket;
        socket.setSslConfiguration(identity.configuration());
        connect(&socket, &QSslSocket::sslErrors, &socket, [&socket](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
        socket.connectToHostEncrypted("127.0.0.1", host.channel.port());
        QTRY_VERIFY(socket.isEncrypted());
        const auto frame = [](const QJsonObject& object) {
            const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
            QByteArray result(4, '\0'); qToBigEndian<quint32>(quint32(data.size()), result.data());
            return result + data;
        };
        const auto hello = frame({{"type", "screenHello"}, {"version", 1}, {"capabilities", QJsonArray{"screen-share", "camera.v2"}}, {"futureField", QJsonObject{{"x", 1}}}});
        const auto limit = frame({{"type", "screenLimit"}, {"minimumTier", 3}});
        const auto first = hello + (fragmented ? limit.first(7) : limit);
        QCOMPARE(socket.write(first), first.size());
        if (fragmented) {
            QTRY_VERIFY(socket.bytesAvailable() > 0);
            const auto rest = limit.mid(7);
            QCOMPARE(socket.write(rest), rest.size());
        }
        QTRY_COMPARE(host.channel.screenTiers(), QSet<int>{3});
        const auto extension = frame({{"type", "cameraOffer"}, {"unknown", QJsonObject{{"future", true}}}})
            + frame({{"type", "screenLimit"}, {"minimumTier", 2}});
        QCOMPARE(socket.write(extension), extension.size());
        QTRY_COMPARE(host.channel.screenTiers(), QSet<int>{2});
        QCOMPARE(socket.state(), QAbstractSocket::ConnectedState);
        QCOMPARE(host.channel.hostClients().size(), 1);
        QVERIFY(client.channel.chatReady());
    }
    void futureScreenResponsesPreserveTheClientConnection_data() {
        QTest::addColumn<bool>("qualityFirst");
        QTest::addColumn<QJsonObject>("extension");
        QTest::addColumn<bool>("accepted");
        const QJsonObject future{{"type", "cameraOffer"}, {"futureField", true}};
        QTest::newRow("before-quality") << false << future << true;
        QTest::newRow("after-quality") << true << future << true;
        QTest::newRow("malformed-envelope") << false << QJsonObject{{"type", 42}} << false;
        QTest::newRow("invalid-known-message") << false << QJsonObject{{"type", "screenQuality"}, {"tier", 20}} << false;
    }
    void futureScreenResponsesPreserveTheClientConnection() {
        QFETCH(bool, qualityFirst); QFETCH(QJsonObject, extension); QFETCH(bool, accepted);
        QTemporaryDir dir; Device client(dir.filePath("client"), "Viewer");
        const auto identity = TlsIdentity::create();
        auto configuration = identity.configuration();
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslServer server; server.setSslConfiguration(configuration);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QTRY_VERIFY(server.hasPendingConnections());
        auto* control = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(control);
        QTRY_VERIFY(control->bytesAvailable() > 4); control->readAll();
        control->write(wireFrame({{"type", "join"}, {"result", "accepted"}, {"capabilities", QJsonArray{"screen-share"}}})
            + wireFrame({{"type", "roster"}, {"host", identity.id()}, {"screen", true}, {"members", QJsonArray{}}}));
        QTRY_VERIFY(client.channel.screenView().value("available").toBool());
        QVERIFY(client.channel.watchScreen(identity.id(), true));
        QTRY_VERIFY(server.hasPendingConnections());
        auto* media = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(media);
        QTRY_VERIFY(media->bytesAvailable() > 4); media->readAll();
        if (qualityFirst) {
            media->write(wireFrame({{"type", "screenQuality"}, {"tier", 1}}));
            QTRY_COMPARE(client.channel.screenView().value("tier").toInt(), 1);
        }
        QSignalSpy frames(&client.channel, &LocalChannel::screenFrameReceived);
        media->write(wireFrame(extension));
        if (!accepted) {
            QTRY_COMPARE(media->state(), QAbstractSocket::UnconnectedState);
            QCOMPARE(frames.size(), 0);
        } else {
            const QByteArray packet("next supported frame");
            const QJsonObject format{{"codec", "h264"}, {"width", 640}, {"height", 360}, {"extra", ""}};
            media->write(wireFrame({{"type", "screenQuality"}, {"tier", 2}})
                + wireFrame({{"type", "screenFrame"}, {"serial", 1}, {"size", packet.size()}, {"format", format}})
                + wireFrame({{"type", "screenChunk"}, {"serial", 1}, {"offset", 0}, {"data", QString::fromLatin1(packet.toBase64())}}));
            QTRY_COMPARE(frames.size(), 1);
            QCOMPARE(frames.first().at(3).toByteArray(), packet);
            QCOMPARE(client.channel.screenView().value("tier").toInt(), 2);
            QVERIFY(client.channel.acknowledgeScreen(identity.id(), 1, 1));
            QTRY_VERIFY(media->bytesAvailable() > 4);
            QCOMPARE(media->state(), QAbstractSocket::ConnectedState);
        }
        // Media extensions or invalid media must not close the control connection.
        control->write(wireFrame({{"type", "roster"}, {"host", identity.id()}, {"screen", false},
            {"members", QJsonArray{QJsonObject{{"id", identity.id()}, {"name", "Still connected"}, {"available", true}, {"muted", false}}}}}));
        QTRY_COMPARE(client.channel.participants().size(), 1);
        QCOMPARE(control->state(), QAbstractSocket::ConnectedState);
    }
    void screenDatagramValidationAndReassembly_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto* value : {"reorder-duplicate", "replace-incomplete", "unknown", "short-header", "oversize",
            "unaligned", "invalid-json", "invalid-format", "empty-metadata", "oversize-metadata", "missing-payload",
            "oversize-payload", "changing-size", "invalid-serial"})
            QTest::newRow(value) << QString(value);
    }
    void screenDatagramValidationAndReassembly() {
        QFETCH(QString, scenario);
        QTemporaryDir dir; Device client(dir.filePath("client"), "Viewer");
        const auto identity = TlsIdentity::create(); auto configuration = identity.configuration();
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslServer server; server.setSslConfiguration(configuration);
        // TCP's ephemeral allocation does not reserve the matching UDP port.
        // Hold both until the real media transport takes over the UDP socket.
        QUdpSocket reservation;
        for (int attempt = 0; attempt < 32 && !server.isListening(); ++attempt) {
            reservation.close();
            QVERIFY2(reservation.bind(QHostAddress::Any, 0, QUdpSocket::DontShareAddress), qPrintable(reservation.errorString()));
            server.listen(QHostAddress::LocalHost, reservation.localPort());
        }
        QVERIFY2(server.isListening(), qPrintable(server.errorString()));
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QTRY_VERIFY(server.hasPendingConnections());
        auto* control = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(control);
        QTRY_VERIFY(control->bytesAvailable() > 4); control->readAll();
        control->write(wireFrame({{"type", "join"}, {"result", "accepted"}, {"capabilities", QJsonArray{"screen-share", "udp-screen"}}})
            + wireFrame({{"type", "roster"}, {"host", identity.id()}, {"screen", true}, {"members", QJsonArray{}}}));
        QTRY_VERIFY(client.channel.screenView().value("available").toBool());
        QVERIFY(client.channel.watchScreen(identity.id(), true)); QTRY_VERIFY(server.hasPendingConnections());
        auto* socket = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(socket);
        QTRY_VERIFY(socket->bytesAvailable() > 4); socket->readAll();
        reservation.close();
        squad::MediaTransport transport(true, server.serverPort(), QHostAddress::LocalHost, 0, squad::MediaTransport::Medium::Data);
        connect(&transport, &squad::MediaTransport::signaling, socket, [socket](QJsonObject message) {
            message.insert("type", "screenMedia"); socket->write(wireFrame(message));
        });
        QByteArray signaling;
        connect(socket, &QSslSocket::readyRead, &transport, [&] {
            signaling += socket->readAll();
            while (signaling.size() >= 4) {
                const auto size = qFromBigEndian<quint32>(signaling.constData());
                if (signaling.size() < size + 4) return;
                const auto message = QJsonDocument::fromJson(signaling.mid(4, size)).object(); signaling.remove(0, size + 4);
                if (message.value("type") == "screenMedia") QVERIFY(transport.receive(message));
            }
        });
        socket->write(wireFrame({{"type", "screenQuality"}, {"tier", 1}, {"udp", true}}));
        QVERIFY(waitForEvents(&transport, &squad::MediaTransport::writable,
            [&] { return transport.udpActive(); }, 5000));
        QSignalSpy frames(&client.channel, &LocalChannel::screenFrameReceived);
        const auto sendDatagram = [&](const QByteArray& bytes) {
            bool sent = false;
            return waitForEvents(&transport, &squad::MediaTransport::writable,
                [&] { return sent || (sent = transport.sendDatagram(bytes)); }, 2000);
        };
        QJsonObject format{{"codec", "mpeg4"}, {"width", 640}, {"height", 360}, {"extra", ""}};
        if (scenario == "invalid-format") format.insert("width", 99999);
        const auto metadata = scenario == "invalid-json" ? QByteArray("not JSON") : QJsonDocument(format).toJson(QJsonDocument::Compact);
        const QByteArray payload(scenario == "oversize-payload" ? 2 * 1024 * 1024 + 1 : 20000, 'p');
        QByteArray envelope(4, '\0'); qToBigEndian<quint32>(quint32(metadata.size()), envelope.data()); envelope += metadata; envelope += payload;
        const auto fragment = [&](quint64 serial, quint32 offset) {
            QByteArray bytes(20, '\0'); bytes.replace(0, 4, "SQVF");
            qToBigEndian(serial, bytes.data() + 4); qToBigEndian<quint32>(quint32(envelope.size()), bytes.data() + 12);
            qToBigEndian(offset, bytes.data() + 16); bytes += envelope.mid(offset, 16364); return bytes;
        };
        const bool accepted = scenario == "reorder-duplicate" || scenario == "replace-incomplete" || scenario == "unknown";
        auto first = fragment(1, 0);
        if (scenario == "short-header") first = "SQVF";
        if (scenario == "oversize") qToBigEndian<quint32>(3000000, first.data() + 12);
        if (scenario == "unaligned") qToBigEndian<quint32>(1, first.data() + 16);
        if (scenario == "invalid-serial") qToBigEndian<quint64>(0, first.data() + 4);
        if (scenario == "empty-metadata") qToBigEndian<quint32>(0, first.data() + 20);
        if (scenario == "oversize-metadata") qToBigEndian<quint32>(32769, first.data() + 20);
        if (scenario == "missing-payload") qToBigEndian<quint32>(quint32(envelope.size() - 4), first.data() + 20);
        if (scenario == "unknown") QVERIFY(sendDatagram("camera.future"));
        if (scenario == "reorder-duplicate") {
            QVERIFY(sendDatagram(fragment(1, 16364))); QVERIFY(sendDatagram(fragment(1, 16364)));
        }
        QVERIFY(sendDatagram(first));
        if (scenario == "changing-size") {
            auto changed = fragment(1, 16364); qToBigEndian<quint32>(quint32(envelope.size() + 1), changed.data() + 12); changed += 'x';
            QVERIFY(sendDatagram(changed));
        } else if (scenario == "replace-incomplete") {
            QVERIFY(sendDatagram(fragment(2, 0))); QVERIFY(sendDatagram(fragment(1, 16364)));
            QVERIFY(sendDatagram(fragment(2, 16364)));
        } else if (!QStringList{"short-header", "oversize", "oversize-payload", "unaligned", "invalid-serial",
            "empty-metadata", "oversize-metadata", "missing-payload"}.contains(scenario)) {
            QVERIFY(sendDatagram(fragment(1, 16364)));
        }
        if (accepted) {
            QVERIFY(waitForEvents([&] { return frames.size() == 1; }));
            QCOMPARE(frames.first().at(3).toByteArray(), payload);
            QVERIFY(client.channel.acknowledgeScreen(identity.id(), frames.first().at(1).toLongLong(), 1));
            QVERIFY(sendDatagram(fragment(1, 0)));
            QVERIFY(sendDatagram(fragment(3, 0))); QVERIFY(sendDatagram(fragment(3, 16364)));
            QVERIFY(waitForEvents([&] { return frames.size() == 2; }));
            QCOMPARE(frames.last().at(1).toLongLong(), qint64(3));
            QCOMPARE(frames.last().at(3).toByteArray(), payload);
            QCOMPARE(socket->state(), QAbstractSocket::ConnectedState);
        } else {
            QVERIFY(waitForEvents(socket, &QSslSocket::disconnected,
                [&] { return socket->state() == QAbstractSocket::UnconnectedState; }));
            QCOMPARE(frames.size(), 0);
        }
        QCOMPARE(control->state(), QAbstractSocket::ConnectedState);
    }
    void screenSharingStartsOffAndHasNoPublicDiscoveryMetadata() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.property("screenSharing").isValid());
        QCOMPARE(host.channel.property("screenSharing").toBool(), false);
        for (const auto& value : host.channel.hosts()) QVERIFY(!value.toMap().contains("screen"));
    }
    void screenTransportUsesAdmissionAndDoesNotBlockChat() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), first(dir.filePath("first"), "First"), second(dir.filePath("second"), "Second");
        QVERIFY(!host.channel.setScreenSharing(true));
        QVERIFY(host.listenWithUdp());
        // This contract requires delivery of each exact frame to inspect ACK
        // backpressure. Fence off UDP, where a dropped frame is valid and the
        // capture source supplies another keyframe. The adjacent UDP contract
        // verifies that loss and recovery path explicitly.
        DelayedLink link(host.channel.port()); link.blockUdp = true;
        QVERIFY(!host.session.supporterEnabled());
        QVERIFY(host.channel.setScreenSharing(true));
        for (auto* client : {&first, &second}) {
            QVERIFY(!client->channel.watchScreen(host.channel.ownId(), true));
            QVERIFY(host.channel.decide(client->channel.ownId(), true));
            QVERIFY(client->channel.openChat(host.channel.ownId(), "127.0.0.1", link.port()));
            QTRY_VERIFY(client->channel.chatReady());
            QTRY_VERIFY(client->channel.screenView().value("available").toBool());
            QVERIFY(client->channel.watchScreen(host.channel.ownId(), true));
            QTRY_COMPARE(client->channel.screenView().value("tier").toInt(), 1);
        }
        QCOMPARE(host.channel.hostParticipants().size(), 0);
        QCOMPARE(host.channel.hostClients().size(), 2);
        QSignalSpy one(&first.channel, &LocalChannel::screenFrameReceived), two(&second.channel, &LocalChannel::screenFrameReceived);
        const QJsonObject format{{"codec", "h264"}, {"width", 1920}, {"height", 1080}, {"extra", ""}};
        const QByteArray large(512 * 1024, 'x');
        QVERIFY(!host.channel.sendScreenFrame(1, format, QByteArray(2 * 1024 * 1024 + 1, 'x'), true));
        QTRY_COMPARE(host.channel.screenTiers(), QSet<int>{1});
        QVERIFY(host.channel.sendScreenFrame(1, format, large, true));
        QTRY_COMPARE(one.size(), 1); QTRY_COMPARE(two.size(), 1);
        QVERIFY(host.channel.screenTiers().isEmpty());
        QCOMPARE(one.first().at(3).toByteArray(), large);
        QCOMPARE(two.first().at(3).toByteArray(), large);
        QVERIFY(second.channel.acknowledgeScreen(host.channel.ownId(), two.first().at(1).toLongLong(), 1));
        QVERIFY(first.channel.sendChat("Chat works while a video receiver holds its acknowledgement."));
        QTRY_COMPARE(memberMessages(second.channel).size(), 1);
        QTRY_COMPARE(host.channel.screenTiers(), QSet<int>{1});
        QVERIFY(host.channel.sendScreenFrame(1, format, QByteArray("next"), true));
        QTRY_COMPARE(two.size(), 2); QCOMPARE(one.size(), 1);
        QVERIFY(host.channel.screenTiers().isEmpty());
        QVERIFY(host.channel.setBlocked(first.channel.ownId(), true));
        QTRY_VERIFY(!first.channel.chatReady());
        QVERIFY(host.channel.screenSharing());
        QVERIFY(!first.channel.acknowledgeScreen(host.channel.ownId(), one.first().at(1).toLongLong(), 1));
#if !SQUADSPEAK_STORE_BUILD
        QVERIFY(host.session.setSupporterEnabled(true));
#endif
        QVERIFY(host.session.setMuted(false));
        QVERIFY(host.session.setSupporterEnabled(false));
        QVERIFY(host.channel.screenSharing());
        QVERIFY(host.channel.setScreenSharing(false));
        QTRY_VERIFY(!host.channel.screenSharing());
        QTRY_VERIFY(!second.channel.screenView().value("available").toBool());
        QVERIFY(second.channel.chatReady());
    }
    void screenUsesUdpWhileReliableTrafficIsDelayed_data() {
        QTest::addColumn<bool>("loseFirstLargeFrame");
        QTest::addColumn<int>("decodeMs");
        QTest::addColumn<bool>("slowStart");
        QTest::newRow("unimpaired") << false << 1 << false;
        QTest::newRow("lost-keyframe") << true << 1 << false;
        QTest::newRow("lost-keyframe-slow-decoder") << true << 100 << false;
        QTest::newRow("adapted-before-stream") << false << 1 << true;
    }
    void screenUsesUdpWhileReliableTrafficIsDelayed() {
        QFETCH(bool, loseFirstLargeFrame);
        QFETCH(int, decodeMs);
        QFETCH(bool, slowStart);
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), viewer(dir.filePath("viewer"), "Viewer");
        QVERIFY(host.listenWithUdp());
        QVERIFY(host.channel.setScreenSharing(true));
        DelayedLink link(host.channel.port()); link.udpDelay = 0;
        QVERIFY(host.channel.decide(viewer.channel.ownId(), true));
        QVERIFY(viewer.channel.openChat(host.channel.ownId(), "127.0.0.1", link.port()));
        QVERIFY(waitForEvents([&] { return viewer.channel.screenView().value("available").toBool(); }));
        QVERIFY(viewer.channel.watchScreen(host.channel.ownId(), true));
        QVERIFY(waitForEvents([&] { return viewer.channel.screenView().value("tier").toInt() == 1; }));
        if (slowStart) {
            QVERIFY(host.channel.reportScreenEncodeTime(1000));
            QCOMPARE(host.channel.screenTiers(), QSet<int>{2});
        }
        QSignalSpy frames(&viewer.channel, &LocalChannel::screenFrameReceived);
        int acknowledgmentMs = 1;
        connect(&viewer.channel, &LocalChannel::screenFrameReceived, &viewer.channel, [&](const QString& id, qint64 serial, const QJsonObject&, const QByteArray&) {
            QVERIFY(viewer.channel.acknowledgeScreen(id, serial, acknowledgmentMs));
        });
        const QJsonObject format{{"codec", "mpeg4"}, {"width", 640}, {"height", 360}, {"extra", ""}};
        const QByteArray payload(120000, 'v');
        QTimer source;
        source.setInterval(34);
        const auto warmup = connect(&source, &QTimer::timeout, &viewer.channel, [&] {
            for (int tier : host.channel.screenTiers())
                QVERIFY(host.channel.sendScreenFrame(tier, format, QByteArray(120000, 'w'), true));
        });
        source.start();
        QVERIFY2(waitForEvents([&] {
            return link.videoDatagrams > 20 && !frames.isEmpty() && !host.channel.screenTiers().isEmpty();
        }, 5000), qPrintable(QString("UDP warmup: %1 datagrams, %2 frames, viewer tier %3")
            .arg(link.videoDatagrams).arg(frames.size()).arg(viewer.channel.screenView().value("tier").toInt())));
        source.stop(); disconnect(warmup); frames.clear();
        // Quality may already have adapted during setup. Readiness means a
        // frame can be sent, not that its initial quality tier stayed fixed.
        const int initialTier = *host.channel.screenTiers().begin();
        acknowledgmentMs = decodeMs;
        // Hold TLS until this frame and its ACK arrive. Heartbeat counts do
        // not establish readiness, and timer races can measure runner stalls.
        link.holdReliable = true; const auto previous = link.videoDatagrams;
        QElapsedTimer latency; latency.start();
        QVERIFY(host.channel.sendScreenFrame(initialTier, format, payload, true));
        QVERIFY(waitForEvents([&] { return frames.size() == 1; }, 5000));
        QCOMPARE(frames.first().at(3).toByteArray(), payload);
        QVERIFY(link.videoDatagrams > previous);
        QVERIFY(waitForEvents([&] { return !host.channel.screenTiers().isEmpty(); }, 5000));
        qInfo() << "Screen frame and decode acknowledgment bypass delayed TLS in" << latency.elapsed() << "ms";
        link.holdReliable = false; link.delay = 2000;
        const QByteArray largest(2 * 1024 * 1024, 'L');
        // UDP may discard a frame. Keep supplying keyframes as the real capture
        // source does, but never queue one behind a frame awaiting its ACK.
        int largeFramesSent = 0;
        const auto largeSource = connect(&source, &QTimer::timeout, &viewer.channel, [&] {
            if (frames.size() != 1) return;
            const auto tiers = host.channel.screenTiers();
            if (tiers.isEmpty()) return;
            link.dropLargeDatagrams = loseFirstLargeFrame && largeFramesSent == 0;
            QVERIFY(host.channel.sendScreenFrame(*tiers.begin(), format, largest, true));
            ++largeFramesSent;
        });
        // Loss detection takes up to two maintenance ticks. The replacement
        // can use TLS, which this link deliberately delays, before delivery.
        const int recoveryTimeout = 2000 + link.delay + 2000;
        latency.restart(); source.start();
        QVERIFY(waitForEvents([&] { return frames.size() == 2; }, recoveryTimeout));
        source.stop(); disconnect(largeSource);
        if (loseFirstLargeFrame) {
            QVERIFY(largeFramesSent > 1);
            QVERIFY(frames.last().at(1).toLongLong() > frames.first().at(1).toLongLong() + 1);
        }
        qInfo() << "Maximum-size screen frame received after" << largeFramesSent << "source frames in" << latency.elapsed() << "ms";
        QCOMPARE(frames.last().at(3).toByteArray(), largest);
        QVERIFY(waitForEvents([&] { return !host.channel.screenTiers().isEmpty(); }));
        const int fallbackTier = *host.channel.screenTiers().begin();
        QVERIFY(fallbackTier >= initialTier && fallbackTier <= std::min(3, initialTier + 1));
        if (decodeMs == 100) QCOMPARE(fallbackTier, std::min(3, initialTier + 1));
        link.blockUdp = true; QTestEventLoop().enterLoopMSecs(450);
        latency.restart(); QVERIFY(host.channel.sendScreenFrame(fallbackTier, format, payload, true));
        QVERIFY(waitForEvents([&] { return frames.size() == 3; }, 5000));
        QVERIFY(latency.elapsed() >= 2000);
        QVERIFY(waitForEvents([&] { return !host.channel.screenTiers().isEmpty(); }));
        const int recoveredTier = *host.channel.screenTiers().begin();
        QVERIFY(recoveredTier >= fallbackTier && recoveredTier <= fallbackTier + 1);
        link.blockUdp = false;
        // Datagram counts can include old retransmissions. Require a fresh
        // frame and its ACK to bypass delayed TLS before dropping UDP again.
        bool udpRestored = false;
        connect(&source, &QTimer::timeout, &viewer.channel, [&] {
            const auto tiers = host.channel.screenTiers();
            if (tiers.isEmpty() || udpRestored) return;
            if (frames.size() > 3 && latency.elapsed() < 1000) { udpRestored = true; return; }
            latency.restart();
            QVERIFY(host.channel.sendScreenFrame(*tiers.begin(), format, "UDP restored", true));
        });
        source.start();
        QVERIFY(waitForEvents([&] { return udpRestored; }, recoveryTimeout + link.delay));
        source.stop(); link.delay = 0;
        const int stableTier = *host.channel.screenTiers().begin();
        const auto received = frames.size();
        // Lose a whole in-flight UDP frame. It must expire without closing chat,
        // and the next frame must be a fresh keyframe, not a growing backlog.
        link.blockUdp = true;
        QVERIFY(host.channel.sendScreenFrame(stableTier, format, payload, true));
        QVERIFY(waitForEvents([&] { return !host.channel.screenTiers().isEmpty(); }, 3000));
        const int nextTier = *host.channel.screenTiers().begin();
        QVERIFY(nextTier >= stableTier && nextTier <= stableTier + 1);
        QVERIFY(host.channel.screenNeedsKeyFrame(nextTier));
        QCOMPARE(frames.size(), received);
        QVERIFY(host.channel.sendScreenFrame(nextTier, format, payload, false));
        QTestEventLoop().enterLoopMSecs(80); QCOMPARE(frames.size(), received);
        QVERIFY(host.channel.sendScreenFrame(nextTier, format, payload, true));
        QVERIFY(waitForEvents([&] { return frames.size() == received + 1; }));
        QVERIFY(viewer.channel.chatReady());
    }
    void hostingClientsKeepVoiceAndScreenDatagramsIndependent() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), client(dir.filePath("client"), "Viewer"), speaker(dir.filePath("speaker"), "Speaker");
        QVERIFY(host.listenWithUdp()); QVERIFY(client.listenWithUdp());
        QVERIFY(host.channel.setScreenSharing(true));
        QVERIFY(host.channel.decide(client.channel.ownId(), true)); QVERIFY(host.channel.decide(speaker.channel.ownId(), true));
        DelayedLink link(host.channel.port()); link.udpDelay = 0;
        QVERIFY(client.channel.join(host.channel.ownId(), "127.0.0.1", link.port())); QVERIFY(speaker.join(host.channel));
        QTRY_COMPARE(client.channel.participants().size(), 2);
        QVERIFY(client.channel.watchScreen(host.channel.ownId(), true));
        QTRY_COMPARE(client.channel.screenInfo(host.channel.ownId()).value("tier").toInt(), 1);
        QVERIFY(speaker.session.setMuted(false));
        QTRY_VERIFY(!participant(client.channel, speaker.channel.ownId()).value("muted").toBool());
        connect(&client.channel, &LocalChannel::screenFrameReceived, &client.channel, [&](const QString& id, qint64 serial, const QJsonObject&, const QByteArray&) {
            QVERIFY(client.channel.acknowledgeScreen(id, serial, 1));
        });
        auto voice = voicePacket(0);
        QByteArray image = "warmup";
        const QJsonObject format{{"codec", "mpeg4"}, {"width", 640}, {"height", 360}, {"extra", ""}};
        QTimer producer; producer.setInterval(34);
        connect(&producer, &QTimer::timeout, this, [&] {
            QVERIFY(speaker.channel.sendAudio(voice));
            for (int tier : host.channel.screenTiers()) QVERIFY(host.channel.sendScreenFrame(tier, format, image, true));
        });
        producer.start();
        QVERIFY(waitForEvents([&] { return link.audioDatagrams >= 10 && link.videoDatagrams >= 10; }, 8000));
        // Hold TLS until fresh audio and video arrive. A timing race against
        // delayed TLS measures runner scheduling rather than transport isolation.
        link.holdReliable = true;
        voice = voicePacket(880); image = "UDP marker";
        int receivedAudio = 0, receivedVideo = 0;
        const std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
            opus_decoder_create(48000, 2, nullptr), opus_decoder_destroy);
        QVERIFY(decoder);
        QObject reception;
        connect(&client.channel, &LocalChannel::audioReceived, &reception, [&](const QString&, const QByteArray& packet) {
            std::array<float, 1920> samples{};
            QCOMPARE(opus_decode_float(decoder.get(), reinterpret_cast<const unsigned char*>(packet.constData()),
                int(packet.size()), samples.data(), 960, 0), 960);
            if (std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0) > 0.1) ++receivedAudio;
        });
        connect(&client.channel, &LocalChannel::screenFrameReceived, &reception,
            [&](const QString&, qint64, const QJsonObject&, const QByteArray& frame) { if (frame == image) ++receivedVideo; });
        QVERIFY(waitForEvents([&] { return receivedAudio >= 5 && receivedVideo >= 5; }));
        link.holdReliable = false;
        producer.stop(); QVERIFY(client.channel.hosting()); QVERIFY(client.channel.joined());
    }
    void screenQualityPrioritizesVoiceInAnotherOwnedChannel() {
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), viewer(dir.filePath("viewer"), "Viewer");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.session.setSupporterEnabled(true));
        const auto id = host.channel.addOwnedChannel("Video room");
        auto* video = host.channel.ownChannel(id); QVERIFY(video);
        QVERIFY(viewer.channel.openAddress(QString("localhost:%1").arg(host.channel.port())));
        QTRY_VERIFY(!viewer.channel.directBusy());
        QVERIFY(host.channel.decide(viewer.channel.ownId(), true));
        QVERIFY(video->decide(viewer.channel.ownId(), true));
        DelayedLink audioLink(host.channel.port());
        QVERIFY(viewer.channel.join(host.channel.ownId(), "127.0.0.1", audioLink.port()));
        QTRY_VERIFY(viewer.channel.joined());
        audioLink.delay = 400;
        QTRY_VERIFY_WITH_TIMEOUT(viewer.channel.receiveAudioBitrate() < 128, 7000);
        QVERIFY(video->setScreenSharing(true));
        QVERIFY(viewer.channel.openChat(id, "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(viewer.channel.screenView().value("available").toBool());
        QVERIFY(viewer.channel.watchScreen(id, true, 2));
        QTRY_COMPARE(viewer.channel.screenView().value("tier").toInt(), 2);
        QSignalSpy frames(&viewer.channel, &LocalChannel::screenFrameReceived);
        const QJsonObject format{{"codec", "h264"}, {"width", 640}, {"height", 360}, {"extra", ""}};
        for (int n = 0; n < 3; ++n) {
            QVERIFY(video->sendScreenFrame(2, format, QByteArray("frame"), true));
            QTRY_COMPARE(frames.size(), n + 1);
            QVERIFY(viewer.channel.acknowledgeScreen(id, frames.last().at(1).toLongLong(), 1));
            QTRY_COMPARE(video->screenTiers(), QSet<int>{n < 2 ? 2 : 3});
        }
        QTRY_COMPARE(viewer.channel.screenView().value("tier").toInt(), 3);
        QVERIFY(viewer.channel.joined());
        QCOMPARE(viewer.channel.joinedHostId(), host.channel.ownId());
#endif
    }
    void screenQualityAdaptsForOnlyTheSlowReceiver() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), slow(dir.filePath("slow"), "Slow"), fast(dir.filePath("fast"), "Fast");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.setScreenSharing(true));
        for (auto* client : {&slow, &fast}) {
            QVERIFY(host.channel.decide(client->channel.ownId(), true));
            QVERIFY(client->channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
            QVERIFY(waitForEvents(&client->channel, &LocalChannel::screenChanged,
                [&] { return client->channel.screenView().value("available").toBool(); }));
            QVERIFY(client->channel.watchScreen(host.channel.ownId(), true, client == &fast ? 1 : 0));
            QVERIFY(waitForEvents(&client->channel, &LocalChannel::screenChanged,
                [&] { return client->channel.screenView().value("tier").toInt() == 1; }));
        }
        QSignalSpy frames(&slow.channel, &LocalChannel::screenFrameReceived), fastFrames(&fast.channel, &LocalChannel::screenFrameReceived);
        connect(&fast.channel, &LocalChannel::screenFrameReceived, &fast.channel, [&](const QString& id, qint64 serial, const QJsonObject&, const QByteArray&) {
            QVERIFY(fast.channel.acknowledgeScreen(id, serial, 1));
        });
        const QJsonObject format{{"codec", "h264"}, {"width", 640}, {"height", 360}, {"extra", ""}};
        QTimer capture;
        capture.setInterval(34);
        capture.setTimerType(Qt::PreciseTimer);
        connect(&capture, &QTimer::timeout, &host.channel, [&] {
            // Keep both viewers receiving throughout degradation and recovery.
            for (int tier = 1; tier <= 3; ++tier)
                QVERIFY(host.channel.sendScreenFrame(tier, format, QByteArray("frame"), true));
        });
        capture.start();
        qsizetype acknowledged = 0;
        for (int tier = 1; tier <= 3; ++tier) {
            for (int sample = 0; sample < 3; ++sample) {
                QVERIFY(waitForEvents(&slow.channel, &LocalChannel::screenFrameReceived,
                    [&] { return frames.size() == acknowledged + 1; }));
                QVERIFY(slow.channel.acknowledgeScreen(host.channel.ownId(), frames.at(acknowledged++).at(1).toLongLong(), 500));
            }
            QVERIFY(waitForEvents(&slow.channel, &LocalChannel::screenChanged,
                [&] { return slow.channel.screenView().value("tier").toInt() == tier + 1; }));
            QCOMPARE(fast.channel.screenView().value("tier").toInt(), 1);
        }
        QVERIFY(host.channel.screenSharing());
        QVERIFY(fastFrames.size() >= 3);
        const auto beforeRecovery = fastFrames.size();
        const auto acknowledgement = connect(&slow.channel, &LocalChannel::screenFrameReceived, &slow.channel,
            [&](const QString& id, qint64 serial, const QJsonObject&, const QByteArray&) {
            // Capture continues while an earlier frame is still being decoded.
            QTimer::singleShot(50, &slow.channel, [&, id, serial] {
                QVERIFY(slow.channel.acknowledgeScreen(id, serial, 1));
            });
        });
        // The state-change signal ends the wait; the timeout only bounds failure.
        QVERIFY(waitForEvents(&slow.channel, &LocalChannel::screenChanged,
            [&] { return slow.channel.screenView().value("tier").toInt() == 2; }, 25000));
        capture.stop(); disconnect(acknowledgement);
        QCOMPARE(slow.channel.screenView().value("tier").toInt(), 2);
        QCOMPARE(fast.channel.screenView().value("tier").toInt(), 1);
        QVERIFY(fastFrames.size() > beforeRecovery);
        QVERIFY(slow.channel.chatReady()); QVERIFY(fast.channel.chatReady());
    }
    void sixtyFourVideoViewersKeepIndependentQualityAndVoice() {
        QElapsedTimer elapsed; elapsed.start();
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.setScreenSharing(true));
        std::vector<std::unique_ptr<Device>> viewers;
        viewers.reserve(64);
        for (int i = 0; i < 64; ++i) {
            viewers.push_back(std::make_unique<Device>(dir.filePath(QString("viewer%1").arg(i)), QString("Viewer %1").arg(i)));
            QVERIFY(host.channel.decide(viewers.back()->channel.ownId(), true));
        }
        qInfo() << "64-viewer identities prepared in ms" << elapsed.elapsed();
        std::array<QPair<QJsonObject, QByteArray>, 6> screenFrames;
        for (size_t i = 0; i < screenFrames.size(); ++i) {
            const int tier = i < 3 ? 1 : 3;
            QImage source(i < 3 ? QSize(1920, 1080) : QSize(640, 360), QImage::Format_RGB32);
            // Native encoders may buffer input. Independent keyframes keep
            // each packet's expected color unambiguous during warm-up.
            VideoCodec screenEncoder;
            source.fill(QColor(30, 80 + int(i % 3) * 40, 180));
            for (int attempt = 0; attempt < 12 && screenFrames[i].second.isEmpty(); ++attempt)
                screenFrames[i] = screenEncoder.encode(source, tier, true);
            QVERIFY(!screenFrames[i].second.isEmpty());
            QVERIFY(screenFrames[i].first.value("key").toBool());
        }
        DelayedLink slowLink(host.channel.port()); slowLink.videoOnly = true;
        QVERIFY(viewers.front()->channel.openChat(host.channel.ownId(), "127.0.0.1", slowLink.port()));
        QTRY_VERIFY(viewers.front()->channel.chatReady());
        QVERIFY(slowLink.disconnectPrimary());
        QTRY_VERIFY(!viewers.front()->channel.chatReady());
        QTRY_VERIFY_WITH_TIMEOUT(viewers.front()->channel.chatReady(), 5000);
        // Two viewers also speak; the other 62 receive video through text sessions.
        // The separate 64-voice test covers audio fanout without placing 64
        // client-side audio-probe timers in this video's single event loop.
        for (size_t i = 1; i < viewers.size(); ++i) {
            if (i <= 2) QVERIFY(viewers[i]->join(host.channel));
            else QVERIFY(viewers[i]->channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        }
        slowLink.delay = 500;
        qInfo() << "64-viewer joins started in ms" << elapsed.elapsed();
        for (const auto& viewer : viewers) QTRY_VERIFY_WITH_TIMEOUT(viewer->channel.chatReady(), 15000);
        for (size_t i = 0; i < viewers.size(); ++i) {
            const auto& viewer = viewers[i];
            QTRY_VERIFY_WITH_TIMEOUT(viewer->channel.screenInfo(host.channel.ownId()).value("available").toBool(), 15000);
            QVERIFY(viewer->channel.watchScreen(host.channel.ownId(), true, 1));
            QTRY_COMPARE_WITH_TIMEOUT(viewer->channel.screenView().value("tier").toInt(), 1, 15000);
        }
        qInfo() << "64-viewer screen links ready in ms" << elapsed.elapsed();
        // Let both voice links recover from TLS admission before measuring
        // video, which deliberately yields to impaired speech connections.
        const auto audioQualities = [&] {
            QStringList values;
            for (size_t i : {1, 2}) values.append(QString::number(viewers[i]->channel.receiveAudioBitrate()));
            return values.join(',');
        };
        QTRY_VERIFY2_WITH_TIMEOUT(viewers[1]->channel.receiveAudioBitrate() == 128
            && viewers[2]->channel.receiveAudioBitrate() == 128, qPrintable(audioQualities()), 45000);
        std::vector<std::unique_ptr<QSignalSpy>> frames;
        frames.reserve(viewers.size());
        std::array<VideoCodec, 2> decoders;
        std::array<int, 2> decoded{};
        QString decodeError;
        QObject reception;
        QThreadPool decoding;
        decoding.setMaxThreadCount(2);
        for (size_t i = 0; i < viewers.size(); ++i) {
            auto* viewer = viewers[i].get();
            frames.push_back(std::make_unique<QSignalSpy>(&viewer->channel, &LocalChannel::screenFrameReceived));
            connect(&viewer->channel, &LocalChannel::screenFrameReceived, &reception,
                [&, viewer, i](const QString& id, qint64 serial, const QJsonObject& format, const QByteArray& packet) {
                    if (i >= decoders.size()) {
                        if (!viewer->channel.acknowledgeScreen(id, serial, 0)) decodeError = "Video acknowledgement failed";
                        return;
                    }
                    // As in ScreenShare, decoding must not block the host and
                    // every other receiver sharing this test's event loop.
                    QElapsedTimer began; began.start();
                    auto* watcher = new QFutureWatcher<QImage>(&reception);
                    connect(watcher, &QFutureWatcher<QImage>::finished, &reception, [&, watcher, viewer, i, id, serial, began] {
                        watcher->deleteLater();
                        try {
                            const auto image = watcher->result();
                            const auto color = image.isNull() ? QColor{} : image.pixelColor(100, 100);
                            const auto expectedSize = decoded[i] < 3 ? QSize(1920, 1080) : QSize(640, 360);
                            const QColor expectedColor(30, 80 + (decoded[i] % 3) * 40, 180);
                            if (image.size() != expectedSize || std::abs(color.red() - expectedColor.red()) > 12
                                || std::abs(color.green() - expectedColor.green()) > 12 || std::abs(color.blue() - expectedColor.blue()) > 12)
                                decodeError = QString("Viewer %1 frame %2 decoded %3x%4 color %5; expected %6x%7 color %8")
                                    .arg(i).arg(decoded[i]).arg(image.width()).arg(image.height()).arg(color.name())
                                    .arg(expectedSize.width()).arg(expectedSize.height()).arg(expectedColor.name());
                            ++decoded[i];
                        } catch (const std::exception& error) { decodeError = QString::fromUtf8(error.what()); }
                        if (!viewer->channel.acknowledgeScreen(id, serial, int(began.elapsed())))
                            decodeError = "Video acknowledgement failed";
                    });
                    QPromise<QImage> promise; watcher->setFuture(promise.future());
                    decoding.start([&, i, format, packet, promise = std::move(promise)]() mutable {
                        promise.start();
                        try { promise.addResult(decoders[i].decode(format, packet)); }
                        catch (...) { promise.setException(std::current_exception()); }
                        promise.finish();
                    });
                });
        }
        squad::VoiceMixer encoder, playback;
        std::array<float, 960> samples{};
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = float(0.1 * std::sin(2 * std::numbers::pi * 440 * i / 48000));
        const auto packet = encoder.encode(samples, 48000).first();
        int audioPackets = 0, audibleFrames = 0;
        bool validAudio = true;
        QElapsedTimer playout; playout.start();
        QObject audioReception;
        connect(&viewers[2]->channel, &LocalChannel::audioReceived, &audioReception,
            [&](const QString& id, const QByteArray& bytes) {
                ++audioPackets;
                validAudio &= playback.receive(id, bytes, playout.elapsed());
            });
        QVERIFY(viewers[1]->session.setMuted(false));
        QTimer audio;
        audio.setTimerType(Qt::PreciseTimer);
        connect(&audio, &QTimer::timeout, &audioReception, [&] {
            validAudio &= viewers[1]->channel.sendAudio(packet);
            const auto output = playback.render(48000, playout.elapsed());
            if (std::any_of(output.begin(), output.end(), [](float value) { return std::abs(value) > 0.005f; }))
                ++audibleFrames;
        });
        audio.start(20);
        for (int frame = 0; frame < int(screenFrames.size()); ++frame) {
            if (frame == 3) {
                // Decode/network pressure may legitimately reduce 1080p on a
                // shared runner. At the lowest tier, isolate the impaired link
                // without requiring 64 receivers to share a 30-fps CPU budget.
                for (const auto& viewer : viewers) QVERIFY(viewer->channel.watchScreen(host.channel.ownId(), true, 3));
                for (const auto& viewer : viewers) QTRY_COMPARE(viewer->channel.screenView().value("tier").toInt(), 3);
            }
            const auto& encoded = screenFrames[size_t(frame)];
            QVERIFY(host.channel.sendScreenFrame(frame < 3 ? 1 : 3, encoded.first, encoded.second, true));
            QVERIFY(waitForEvents([&] {
                return !decodeError.isEmpty() || (decoded[0] == frame + 1 && decoded[1] == frame + 1
                    && std::all_of(frames.cbegin(), frames.cend(), [&](const auto& spy) { return spy->size() == frame + 1; }));
            }));
            QVERIFY2(decodeError.isEmpty(), qPrintable(decodeError));
            QTestEventLoop settling; settling.enterLoopMSecs(40);
        }
        audio.stop();
        QVERIFY(validAudio);
        QVERIFY(audioPackets >= 20); QVERIFY(audibleFrames >= 20);
        QCOMPARE(decoded[0], 6); QCOMPARE(decoded[1], 6);
        qInfo() << "64 viewers received three 1080p and three 360p frames; two decoders verified all frames; voice packets"
                << audioPackets << "non-silent playout frames" << audibleFrames;
        QVERIFY(waitForEvents([&] { return viewers.front()->channel.screenView().value("tier").toInt() == 4; }));
        for (size_t i = 1; i < viewers.size(); ++i) {
            const int tier = viewers[i]->channel.screenView().value("tier").toInt();
            QVERIFY2(tier == 3, qPrintable(QString("Unaffected viewer %1 changed to tier %2").arg(i).arg(tier)));
        }
        for (const auto& viewer : viewers) QVERIFY(viewer->channel.closeChat(host.channel.ownId()));
        QVERIFY(host.channel.setScreenSharing(false));
    }
    void aDelayedVideoConnectionLeavesOtherReceiversAndChatResponsive() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), slow(dir.filePath("slow"), "Slow"), fast(dir.filePath("fast"), "Fast");
        QVERIFY(host.listenWithUdp());
        QVERIFY(host.channel.setScreenSharing(true));
        DelayedLink link(host.channel.port()); link.videoOnly = true;
        QSignalSpy slowFrames(&slow.channel, &LocalChannel::screenFrameReceived);
        QSignalSpy fastFrames(&fast.channel, &LocalChannel::screenFrameReceived);
        for (auto* client : {&slow, &fast}) {
            QVERIFY(host.channel.decide(client->channel.ownId(), true));
            QVERIFY(client->channel.openChat(host.channel.ownId(), "127.0.0.1", client == &slow ? link.port() : host.channel.port()));
            QTRY_VERIFY(client->channel.screenView().value("available").toBool());
            QVERIFY(client->channel.watchScreen(host.channel.ownId(), true));
            QTRY_COMPARE(client->channel.screenView().value("tier").toInt(), 1);
            connect(&client->channel, &LocalChannel::screenFrameReceived, &client->channel,
                [client](const QString& id, qint64 serial, const QJsonObject&, const QByteArray&) {
                    QVERIFY(client->channel.acknowledgeScreen(id, serial, 1));
                });
        }
        const QJsonObject format{{"codec", "h264"}, {"width", 640}, {"height", 360}, {"extra", ""}};
        QTestEventLoop delivery;
        QTimer capture; capture.setInterval(34); capture.setTimerType(Qt::PreciseTimer);
        connect(&capture, &QTimer::timeout, &host.channel, [&] {
            QVERIFY(host.channel.sendScreenFrame(1, format, QByteArray(50000, 'x'), true));
            const bool ready = link.delay ? slow.channel.screenView().value("tier").toInt() >= 2
                : slowFrames.size() >= 10 && fastFrames.size() >= 10 && link.videoDatagrams >= 10;
            if (ready) delivery.exitLoop();
        });
        capture.start(); delivery.enterLoopMSecs(5000);
        QVERIFY(slowFrames.size() >= 10 && fastFrames.size() >= 10 && link.videoDatagrams >= 10);
        QCOMPARE(slow.channel.screenView().value("tier").toInt(), 1);
        QCOMPARE(fast.channel.screenView().value("tier").toInt(), 1);
        link.delay = 180;
        // Three lost frames can each expire on the second one-second
        // maintenance tick; the quality notice then crosses the delayed TLS
        // link. Six seconds races that final notice instead of testing it.
        delivery.enterLoopMSecs(7000);
        capture.stop();
        QVERIFY2(slow.channel.screenView().value("tier").toInt() >= 2,
            qPrintable(QString("Slow tier %1, fast tier %2, slow/fast frames %3/%4, proxied video datagrams %5, deadline %6")
                .arg(slow.channel.screenView().value("tier").toInt()).arg(fast.channel.screenView().value("tier").toInt())
                .arg(slowFrames.size()).arg(fastFrames.size()).arg(link.videoDatagrams).arg(delivery.timeout())));
        QCOMPARE(fast.channel.screenView().value("tier").toInt(), 1);
        QVERIFY(slow.channel.sendChat("The control connection is independent of video."));
        QTRY_COMPARE_WITH_TIMEOUT(memberMessages(fast.channel).size(), 1, 1000);
        QVERIFY(slow.channel.chatReady()); QVERIFY(fast.channel.chatReady());
        for (int sample = 0; sample < 9; ++sample) QVERIFY(host.channel.reportScreenEncodeTime(400));
        QVERIFY(host.channel.screenSharing());
    }
    void firstAvatarUsesIdentityAndPersistsWithoutReplacingSelections() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        const auto hash = QCryptographicHash::hash(identity.id().toUtf8(), QCryptographicHash::Sha256);
        const auto expected = VoiceSession::avatars().at(qFromBigEndian<quint32>(hash.constData()) % 10);
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto path = dir.filePath(QString::number(attempt));
            VoiceSession session(path + ".session");
            // Settings may be edited before the asynchronous identity load finishes.
            QVERIFY(session.setUserName("New member"));
            {
                LocalChannel channel(session, path + ".channel", identity);
                QVERIFY(channel.startService(QHostAddress::LocalHost, 0));
                QVERIFY(channel.initialize());
                QCOMPARE(session.avatar(), expected);
            }
            VoiceSession restored(path + ".session");
            QCOMPARE(restored.avatar(), expected);
            QFile saved(path + ".session");
            QVERIFY(saved.open(QIODevice::ReadOnly));
            QCOMPARE(QJsonDocument::fromJson(saved.readAll()).object().value("avatar").toString(), expected);
            saved.close();
            QVERIFY(restored.setAvatar("mossling"));
            VoiceSession selected(path + ".session");
            LocalChannel reopened(selected, path + ".channel", identity);
            QVERIFY(reopened.startService(QHostAddress::LocalHost, 0));
            QVERIFY(reopened.initialize());
            QCOMPARE(selected.avatar(), QString("mossling"));
        }
    }

    void firstAvatarSaveFailureCanBeRetriedBeforeHosting() {
        QTemporaryDir dir;
        const auto path = dir.filePath("missing/session");
        VoiceSession session(path);
        LocalChannel channel(session, dir.filePath("channel"), TlsIdentity::create());
        QVERIFY(channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(!channel.initialize());
        QVERIFY(!channel.hosting());
        QVERIFY(!session.error().isEmpty());
        QCOMPARE(channel.status(), session.error());
        QVERIFY(QDir(dir.path()).mkdir("missing"));
        QVERIFY(channel.initialize());
        QVERIFY(channel.hosting());
        VoiceSession restored(path);
        QCOMPARE(restored.avatar(), session.avatar());
    }

    void firstChannelNameIsGeneratedOnceWithoutCreatingExtraChannels() {
        QTemporaryDir dir;
        VoiceSession session;
        const auto path = dir.filePath("channel.json");
        const auto identity = TlsIdentity::create();
        LocalChannel first(session, path, identity);
        const auto name = first.channelName();
        QVERIFY(name != "My channel");
        QVERIFY(!QFile::exists(path));
        QVERIFY(first.listen(QHostAddress::LocalHost));
        QCOMPARE(VoiceSession::validatedName(name), name);
        QCOMPARE(first.ownedChannels().size(), 1);
        LocalChannel restored(session, path, identity);
        QCOMPARE(restored.channelName(), name);
        QVERIFY(restored.setChannelName("My channel"));
        LocalChannel existing(session, path, identity);
        QCOMPARE(existing.channelName(), QString("My channel"));
    }

    void channelNameBelongsToHostStorageAndMigratesTheOldProfile_data() {
        QTest::addColumn<bool>("existingPermissions");
        QTest::newRow("new-channel-store") << false;
        QTest::newRow("existing-channel-store") << true;
    }
    void channelNameBelongsToHostStorageAndMigratesTheOldProfile() {
        QFETCH(bool, existingPermissions);
        QTemporaryDir dir;
        const auto profilePath = dir.filePath("session.json");
        QFile profile(profilePath);
        QVERIFY(profile.open(QIODevice::WriteOnly));
        profile.write(R"({"version":1,"userName":"Person","channelName":"Existing channel","muted":true})");
        profile.close();
        if (existingPermissions) {
            QFile permissions(dir.filePath("channel.json"));
            QVERIFY(permissions.open(QIODevice::WriteOnly));
            const QJsonObject settings{{"version", 1}, {"approved", QJsonArray{QString(64, 'a')}},
                {"attempts", QJsonObject{}}, {"requestsAllowed", false}, {"servicePort", 48765}};
            QVERIFY(permissions.write(QJsonDocument(settings).toJson()) > 0);
        }
        VoiceSession session(profilePath);
        LocalChannel channel(session, dir.filePath("channel.json"), TlsIdentity::create());
        QCOMPARE(channel.property("channelName").toString(), QString("Existing channel"));
        if (existingPermissions) {
            QCOMPARE(channel.configuredPort(), 48765);
            QVERIFY(!channel.requestsAllowed());
            QFile permissions(dir.filePath("channel.json"));
            QVERIFY(permissions.open(QIODevice::ReadOnly));
            QCOMPARE(QJsonDocument::fromJson(permissions.readAll()).object().value("approved").toArray(),
                     QJsonArray{QString(64, 'a')});
        }
        bool renamed = false;
        QVERIFY(QMetaObject::invokeMethod(&channel, "setChannelName", Q_RETURN_ARG(bool, renamed), Q_ARG(QString, "Server name")));
        QVERIFY(renamed);
        QVERIFY(session.setUserName("Another person"));
        VoiceSession noPersonalProfile(QString{});
        LocalChannel restored(noPersonalProfile, dir.filePath("channel.json"), TlsIdentity::create());
        QCOMPARE(restored.property("channelName").toString(), QString("Server name"));
    }
    void hostPresetsAreAtomicAndCannotUndoRuntimeBans() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Person");
        const auto clientId = TlsIdentity::create().id();
        QVERIFY(host.channel.configureHost({{"channelName", "Server"}, {"approvedClients", QJsonArray{clientId}}, {"messageLifetimeDays", 7}}));
        QVERIFY(host.channel.setBlocked(clientId, true));
        QVERIFY(host.channel.configureHost({{"approvedClients", QJsonArray{clientId}}}));
        QCOMPARE(host.channel.blockedClients().size(), 1);
        const auto expected = host.channel.hostConfiguration();
        QVERIFY(!host.channel.configureHost({{"channelName", "Changed"}, {"port", 0}}));
        QCOMPARE(host.channel.hostConfiguration(), expected);
        QVERIFY(!host.channel.setChannelName("Line\nTwo"));
        QCOMPARE(host.channel.hostConfiguration(), expected);
        QVERIFY(!host.channel.configureHost({{"channelName", "Changed"}, {"blockedClients", QJsonArray{QString(64, 'f'), "bad"}}}));
        QCOMPARE(host.channel.hostConfiguration(), expected);
        Device restored(dir.filePath("host"), "Person");
        QCOMPARE(restored.channel.hostConfiguration(), expected);
        QVERIFY(restored.channel.setBlocked(clientId, false));
        QVERIFY(restored.channel.blockedClients().isEmpty());
        VoiceSession transient;
        LocalChannel unwritable(transient, dir.filePath("missing/channel.json"), TlsIdentity::create());
        const auto before = unwritable.hostConfiguration();
        QVERIFY(!unwritable.configureHost({{"hostName", "Changed host"}, {"channelName", "Changed"}, {"port", 54321}, {"messageLifetimeDays", 30}}));
        QCOMPARE(unwritable.hostConfiguration(), before);
    }
    void addChannelCandidatesExcludeSavedHostsAndDuplicateEndpoints() {
        QTemporaryDir dir;
        const auto fixedClock = [] { return qint64(1700000000000LL); };
        QHostAddress discoveryDestination;
        const auto discoveryInterface = multicastInterface();
        for (const auto& entry : discoveryInterface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) { discoveryDestination = entry.ip(); break; }
        QVERIFY(!discoveryDestination.isNull());
        Device observer(dir.filePath("observer"), "Observer", TlsIdentity::create(), fixedClock);
        Device host(dir.filePath("host"), "Host", TlsIdentity::create(), fixedClock);
        QVERIFY(observer.channel.listen(discoveryDestination));
        QVERIFY(host.channel.listen(discoveryDestination));
        QVERIFY(observer.channel.startDiscovery());
        QUdpSocket announcements;
        QVERIFY(announcements.bind(discoveryDestination, 0));
        announcements.setMulticastInterface(discoveryInterface);
        announcements.setSocketOption(QAbstractSocket::MulticastTtlOption, 0);
        announcements.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
        QTcpServer anotherEndpoint;
        QVERIFY(anotherEndpoint.listen(QHostAddress::LocalHost));
        const auto advertise = [&](QString id, QString name, int port) {
            const auto data = QJsonDocument(QJsonObject{{"protocol", "squadspeak/1"},
                {"id", id}, {"name", name}, {"port", port}}).toJson(QJsonDocument::Compact);
            return announcements.writeDatagram(data, QHostAddress("239.255.85.73"), 48762) == data.size();
        };
        const auto duplicate = TlsIdentity::create().id();
        const auto another = TlsIdentity::create().id();
        QVERIFY(advertise(host.channel.ownId(), "Host", host.channel.port()));
        QVERIFY(advertise(duplicate, "Duplicate endpoint", host.channel.port()));
        const auto hasHost = [&](const QString& id) {
            const auto hosts = observer.channel.hosts();
            return std::any_of(hosts.cbegin(), hosts.cend(),
                [&](const auto& value) { return value.toMap().value("id").toString() == id; });
        };
        QTRY_VERIFY(hasHost(host.channel.ownId()));
        QTRY_VERIFY(hasHost(duplicate));
        auto candidates = [&] {
            QVariantList deliberate;
            for (const auto& value : observer.channel.property("availableHosts").toList()) {
                const auto id = value.toMap().value("id").toString();
                if (id == host.channel.ownId() || id == duplicate || id == another)
                    deliberate.append(value);
            }
            return deliberate;
        };
        QTRY_COMPARE(candidates().size(), 1);
        QVERIFY(host.channel.decide(observer.channel.ownId(), true));
        QVERIFY(observer.channel.openChat(host.channel.ownId(), discoveryDestination.toString(), host.channel.port()));
        QTRY_VERIFY(observer.channel.chatReady());
        QTRY_VERIFY(hasHost(host.channel.ownId()));
        QTRY_VERIFY(hasHost(duplicate));
        QTRY_VERIFY(candidates().isEmpty());
        QVERIFY(advertise(host.channel.ownId(), "Renamed host", host.channel.port()));
        QTest::qWait(50);
        QVERIFY(candidates().isEmpty());
        QVERIFY(observer.channel.removeChannel(host.channel.ownId()));
        const auto anotherPort = anotherEndpoint.serverPort();
        QTRY_VERIFY([&] {
            advertise(host.channel.ownId(), "Host", host.channel.port());
            advertise(duplicate, "Duplicate endpoint", host.channel.port());
            return candidates().size() == 1;
        }());
        QTRY_VERIFY([&] {
            advertise(host.channel.ownId(), "Host", host.channel.port());
            advertise(duplicate, "Duplicate endpoint", host.channel.port());
            advertise(another, "Another port", anotherPort);
            return candidates().size() == 2;
        }());
    }

    void absoluteDnsNamesKeepTheirTrailingDot() {
        QTemporaryDir dir;
        Device client(dir.filePath("client"), "Client");
        const auto targetId = TlsIdentity::create().id();
        for (const auto& name : {QString("Voice.Example."), QString("LOCALHOST."), QString("Voice.LOCALHOST.")}) {
            QVERIFY(client.channel.openChat(targetId, name, 48763));
            QCOMPARE(client.channel.savedChannels().first().toMap().value("address").toString(), name.toLower());
            QVERIFY(client.channel.closeChat(targetId));
        }
    }
    void dnsNamesReachIpv6OnlyChatAndControlTargets_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("localhost") << QString("localhost");
        QTest::newRow("localhost-subdomain") << QString("device.localhost");
        QTest::newRow("absolute-localhost") << QString("device.localhost.");
    }
    void dnsNamesReachIpv6OnlyChatAndControlTargets() {
        QFETCH(QString, name);
        QTemporaryDir dir;
        Device target(dir.filePath("target"), "Target"), client(dir.filePath("client"), "Client");
        QVERIFY(target.channel.listen(QHostAddress::LocalHostIPv6));
        QVERIFY(target.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.channel.openAddress(name + ":" + QString::number(target.channel.port())));
        // Address discovery and chat admission are separate TLS handshakes.
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.chatReady(), 10000);
        QCOMPARE(client.channel.savedChannels().first().toMap().value("address").toString(), name);
        QVERIFY(target.channel.setRemotePermission(client.channel.ownId(), true));
        QTRY_COMPARE(client.channel.remoteOffers().size(), 1);
        QVERIFY(client.channel.chooseRemoteOffer(target.channel.ownId()));
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.controlConnected(), 10000);
    }

    void dnsReconnectFollowsAddressFamilyChange() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        auto host = std::make_unique<Device>(dir.filePath("host"), "Host", identity);
        Device client(dir.filePath("client"), "Client");
        QVERIFY(host->channel.listen(QHostAddress::LocalHostIPv6));
        const auto port = host->channel.port();
        QVERIFY(host->channel.decide(client.channel.ownId(), true));
        QVERIFY(client.channel.joinAddress("localhost:" + QString::number(port)));
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.joined(), 10000);
        host.reset();
        QTRY_VERIFY(!client.channel.joined());
        host = std::make_unique<Device>(dir.filePath("host"), "Host", identity);
        QVERIFY(host->channel.listen(QHostAddress::LocalHost, port));
        // Reconnect waits for the closed socket, its retry delay, and a fresh TLS handshake.
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.joined(), 30000);
        QCOMPARE(client.channel.savedChannels().first().toMap().value("address").toString(), QString("localhost"));
        QVERIFY(client.channel.sendChat("After address change"));
        QTRY_COMPARE(memberMessages(client.channel).size(), 1);
    }

    void dnsIdentityFailureDoesNotTryAnotherAddressFamily() {
        QTemporaryDir dir;
        Device target(dir.filePath("target"), "Target"), client(dir.filePath("client"), "Client");
        QVERIFY(target.channel.listen(QHostAddress::LocalHostIPv6));
        const auto port = target.channel.port();
        QVERIFY(target.channel.decide(client.channel.ownId(), true));
        const auto address = "localhost:" + QString::number(port);
        QVERIFY(client.channel.openAddress(address));
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.chatReady(), 10000);
        QVERIFY(client.channel.closeChat(target.channel.ownId()));
        Device replacement(dir.filePath("replacement"), "Replacement");
        QVERIFY(replacement.channel.listen(QHostAddress::LocalHost, port));
        QVERIFY(client.channel.openAddress(address));
        QTRY_VERIFY_WITH_TIMEOUT(!client.channel.directBusy(), 10000);
        QVERIFY2(client.channel.status().contains("different device identity"), qPrintable(client.channel.status()));
        QVERIFY(!client.channel.chatReady());
        QVERIFY(replacement.channel.requests().isEmpty());
    }

    void headlessTemporarilySuspendsRemoteControlWithoutErasingGrants() {
        QTemporaryDir dir;
        const auto targetIdentity = TlsIdentity::create(), controllerIdentity = TlsIdentity::create();
        auto target = std::make_unique<Device>(dir.filePath("target"), "Target", targetIdentity);
        auto controller = std::make_unique<Device>(dir.filePath("controller"), "Controller", controllerIdentity);
        QVERIFY(target->channel.listen(QHostAddress::LocalHost));
        const auto port = target->channel.port();
        QVERIFY(target->channel.decide(controllerIdentity.id(), true));
        QVERIFY(controller->join(target->channel)); QTRY_VERIFY(controller->channel.chatReady());
        QVERIFY(target->channel.setRemotePermission(controllerIdentity.id(), true));
        QTRY_COMPARE(controller->channel.remoteOffers().size(), 1);
        QVERIFY(controller->channel.chooseRemoteOffer(targetIdentity.id()));
        QTRY_VERIFY(controller->channel.controlConnected());
        target.reset();
        target = std::make_unique<Device>(dir.filePath("target"), "Target", targetIdentity);
        QVERIFY(target->channel.listen(QHostAddress::LocalHost, port));
        QVERIFY(target->channel.initialize(true));
        QTRY_VERIFY_WITH_TIMEOUT(controller->channel.controlStatus().contains("unavailable"), 6000);
        QVERIFY(controller->channel.remoteMode());
        QVERIFY(!controller->channel.controlConnected());
        target.reset();
        target = std::make_unique<Device>(dir.filePath("target"), "Target", targetIdentity);
        QVERIFY(target->channel.listen(QHostAddress::LocalHost, port));
        QTRY_VERIFY_WITH_TIMEOUT(controller->channel.controlConnected(), 6000);
        controller.reset();
        {
            Device server(dir.filePath("controller"), "Controller", controllerIdentity);
            QVERIFY(server.channel.startService(QHostAddress::LocalHost, 0));
            QVERIFY(server.channel.initialize(true));
            QTest::qWait(150);
            QVERIFY(!server.channel.remoteMode()); QVERIFY(!server.channel.controlConnected());
            QVERIFY(!server.channel.chooseRemoteOffer(targetIdentity.id()));
            QVERIFY(!server.channel.openChat(server.channel.ownId(), "127.0.0.1", server.channel.port()));
            QVERIFY(!server.channel.chatReady());
        }
        controller = std::make_unique<Device>(dir.filePath("controller"), "Controller", controllerIdentity);
        QVERIFY(controller->channel.remoteMode());
        QTRY_VERIFY_WITH_TIMEOUT(controller->channel.controlConnected(), 6000);
    }

    void remoteAvailabilityFollowsTargetModeWithoutRevokingPermissions() {
        QTemporaryDir dir;
        Device target(dir.filePath("target"), "Target"), destination(dir.filePath("destination"), "Destination"),
            viewer(dir.filePath("viewer"), "Viewer"), controller(dir.filePath("controller"), "Controller");
        QVERIFY(target.channel.listen(QHostAddress::LocalHost));
        QVERIFY(destination.channel.listen(QHostAddress::LocalHost));
        for (auto* client : {&viewer, &controller}) {
            QVERIFY(target.channel.decide(client->channel.ownId(), true));
            QVERIFY(client->channel.openChat(target.channel.ownId(), "127.0.0.1", target.channel.port()));
            QTRY_VERIFY(client->channel.chatReady());
            QVERIFY(target.channel.setRemotePermission(client->channel.ownId(), true));
            QTRY_COMPARE(client->channel.remoteOffers().size(), 1);
        }
        QVERIFY(controller.channel.chooseRemoteOffer(target.channel.ownId()));
        QTRY_VERIFY(controller.channel.controlConnected());
        QVERIFY(destination.channel.decide(target.channel.ownId(), true));
        QVERIFY(target.channel.openChat(destination.channel.ownId(), "127.0.0.1", destination.channel.port()));
        QTRY_VERIFY(target.channel.chatReady());
        QVERIFY(destination.channel.setRemotePermission(target.channel.ownId(), true));
        QTRY_COMPARE(target.channel.remoteOffers().size(), 1);
        QVERIFY(target.channel.chooseRemoteOffer(destination.channel.ownId()));
        QTRY_VERIFY(target.channel.controlConnected());
        QTRY_VERIFY(viewer.channel.remoteOffers().isEmpty());
        QVERIFY(!viewer.channel.chooseRemoteOffer(target.channel.ownId()));
        QTRY_VERIFY(!controller.channel.controlConnected());
        QVERIFY(controller.channel.remoteMode());
        QCOMPARE(target.channel.controllers().size(), 2);
        QVERIFY(viewer.channel.chatReady());
        QTRY_VERIFY(target.channel.setRemoteMode(false));
        QTRY_COMPARE(viewer.channel.remoteOffers().size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(controller.channel.controlConnected(), 6000);
        QCOMPARE(target.channel.controllers().size(), 2);
        QVERIFY(target.channel.initialize(true));
        QTRY_VERIFY(viewer.channel.remoteOffers().isEmpty());
        QTRY_VERIFY(!controller.channel.controlConnected());
        QVERIFY(controller.channel.remoteMode());
        QCOMPARE(target.channel.controllers().size(), 2);
        QVERIFY(viewer.channel.chatReady());
    }

    void savedDnsNameSurvivesDiscoveryAndExplicitRejoins() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), client(dir.filePath("client"), "Client");
        QVERIFY(host.channel.listen(QHostAddress::AnyIPv4));
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.channel.openAddress("localhost:" + QString::number(host.channel.port())));
        QTRY_VERIFY(client.channel.chatReady());
        QVERIFY(client.channel.startDiscovery());
        QUdpSocket publisher;
        const auto interface = multicastInterface();
        QVERIFY(interface.isValid());
        QVERIFY(publisher.bind(QHostAddress(QHostAddress::AnyIPv4), 0));
        publisher.setMulticastInterface(interface);
        publisher.setSocketOption(QAbstractSocket::MulticastTtlOption, 0);
        publisher.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
        const auto announcement = QJsonDocument(QJsonObject{{"protocol", "squadspeak/1"}, {"id", host.channel.ownId()},
            {"name", "Host"}, {"port", host.channel.port()}}).toJson(QJsonDocument::Compact);
        QVERIFY(publisher.writeDatagram(announcement, QHostAddress("239.255.85.73"), 48762) > 0);
        const auto discovered = [&] {
            for (const auto& value : client.channel.hosts()) {
                const auto entry = value.toMap();
                if (entry.value("id").toString() == host.channel.ownId())
                    return !QHostAddress(entry.value("address").toString()).isNull();
            }
            return false;
        };
        QTRY_VERIFY(discovered());
        QVERIFY(client.channel.joinSaved(host.channel.ownId()));
        QTRY_VERIFY(client.channel.joined());
        QCOMPARE(client.channel.savedChannels().first().toMap().value("address").toString(), QString("localhost"));
        QVERIFY(client.channel.closeChat(host.channel.ownId()));
        QVERIFY(client.channel.openSavedChat(host.channel.ownId()));
        QTRY_VERIFY(client.channel.chatReady());
        QCOMPARE(client.channel.savedChannels().first().toMap().value("address").toString(), QString("localhost"));
    }

    void hostOnlyIgnoresSavedVoiceAndRemoteIntent() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        Device target(dir.filePath("target"), "Target");
        QVERIFY(target.channel.listen(QHostAddress::LocalHost));
        QVERIFY(target.channel.decide(identity.id(), true));
        {
            Device client(dir.filePath("server"), "Server", identity);
            QVERIFY(client.join(target.channel)); QTRY_VERIFY(client.channel.chatReady());
            QVERIFY(client.channel.setAutoJoin(target.channel.ownId(), true));
        }
        Device server(dir.filePath("server"), "Server", identity);
        QVERIFY(server.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(server.channel.initialize(true));
        QTest::qWait(150);
        QVERIFY(server.channel.hosting()); QVERIFY(!server.channel.joined());
        QVERIFY(!server.channel.chatReady());
        QVERIFY(!server.channel.joinSaved(target.channel.ownId()));
        QVERIFY(!server.channel.openSavedChat(target.channel.ownId()));
        QVERIFY(!server.channel.join(server.channel.ownId(), "127.0.0.1", server.channel.port()));
        QVERIFY(!server.channel.openChat(server.channel.ownId(), "127.0.0.1", server.channel.port()));
        QVERIFY(!server.channel.chatReady());
        QVERIFY(!server.channel.setRemoteMode(true));
        QVERIFY(!server.channel.remoteMode());
        QVERIFY(!server.channel.savedChannels().isEmpty());
    }

    void headlessAutomaticallyAdmitsUnapprovedClientsExceptBans() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Headless host");
        Device client(dir.filePath("client"), "Client");
        QVERIFY(host.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(host.channel.initialize(true));
        QVERIFY(client.join(host.channel));
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.joined(), 6000);
        QVERIFY(host.channel.requests().isEmpty());

        Device passwordHost(dir.filePath("password-host"), "Password host");
        Device passwordClient(dir.filePath("password-client"), "Password client");
        QVERIFY(passwordHost.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(passwordHost.channel.initialize(true));
        QVERIFY(passwordHost.channel.setHostPassword("headless-secret"));
        QTRY_VERIFY(!passwordHost.channel.passwordBusy());
        QVERIFY(passwordClient.join(passwordHost.channel));
        QTRY_VERIFY_WITH_TIMEOUT(passwordClient.channel.passwordRequired(), 6000);
        QVERIFY(!passwordClient.channel.joined());
        QVERIFY(passwordClient.channel.submitPassword("headless-secret", false));
        QTRY_VERIFY_WITH_TIMEOUT(passwordClient.channel.joined(), 6000);

        Device wrongPasswordClient(dir.filePath("wrong-password-client"), "Wrong password");
        QVERIFY(wrongPasswordClient.join(passwordHost.channel));
        QTRY_VERIFY(wrongPasswordClient.channel.passwordRequired());
        QVERIFY(wrongPasswordClient.channel.submitPassword("wrong-secret", false));
        QTRY_VERIFY(wrongPasswordClient.channel.passwordRetrySeconds() > 0);
        QVERIFY(!wrongPasswordClient.channel.joined());
        QVERIFY(!wrongPasswordClient.channel.chatReady());
        QVERIFY(passwordHost.channel.requests().isEmpty());

        Device bannedHost(dir.filePath("banned-host"), "Banned host");
        Device bannedClient(dir.filePath("banned-client"), "Banned client");
        QVERIFY(bannedHost.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(bannedHost.channel.setBlocked(bannedClient.channel.ownId(), true));
        QVERIFY(bannedHost.channel.setHostPassword("headless-secret"));
        QTRY_VERIFY(!bannedHost.channel.passwordBusy());
        QVERIFY(bannedHost.channel.initialize(true));
        QVERIFY(bannedClient.join(bannedHost.channel));
        QTRY_VERIFY_WITH_TIMEOUT(bannedClient.channel.status().contains("blocked"), 6000);
        QVERIFY(!bannedClient.channel.joined());
        QVERIFY(!bannedClient.channel.passwordRequired());
        QVERIFY(bannedHost.channel.requests().isEmpty());
    }

    void accessRequestsExposeAuthenticatedPeerServiceMetadata() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host");
        Device client(dir.filePath("client"), "Client");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.setChannelName("Client service"));
        QVERIFY(client.join(host.channel));
        QTRY_COMPARE(host.channel.requests().size(), 1);
        const auto request = host.channel.requests().first().toMap();
        QCOMPARE(request.value("id").toString(), client.channel.ownId());
        QCOMPARE(request.value("name").toString(), QString("Client"));
        QCOMPARE(request.value("avatarId").toString(), QString("mossling"));
        QCOMPARE(request.value("address").toString(), QString("127.0.0.1"));
        QCOMPARE(request.value("channel").toString(), QString("Client service"));
        QCOMPARE(request.value("port").toInt(), int(client.channel.port()));
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QTRY_VERIFY(client.channel.joined());
    }

    void headlessAdmissionIsRuntimeAndDesktopRestartRequiresApproval() {
        QTemporaryDir dir;
        const auto hostIdentity = TlsIdentity::create(), clientIdentity = TlsIdentity::create();
        {
            Device headless(dir.filePath("host"), "Headless host", hostIdentity);
            Device client(dir.filePath("client"), "Client", clientIdentity);
            QVERIFY(headless.channel.configureHost({{"requestsAllowed", false}}));
            QVERIFY(headless.channel.startService(QHostAddress::LocalHost, 0));
            QVERIFY(headless.channel.initialize(true));
            const auto generated = headless.channel.hostConfiguration().value("hostName").toString();
            QVERIFY(!generated.isEmpty());
            QCOMPARE(VoiceSession::validatedName(generated), generated);
            {
                Device restored(dir.filePath("host"), "Unused profile", hostIdentity);
                QCOMPARE(restored.channel.hostConfiguration().value("hostName").toString(), generated);
            }
            QVERIFY(headless.channel.configureHost({{"hostName", "Willow Station"}}));
            QVERIFY(!headless.channel.configureHost({{"hostName", "Changed"}, {"port", 0}}));
            QCOMPARE(headless.channel.hostConfiguration().value("hostName").toString(), QString("Willow Station"));
            QVERIFY(headless.channel.requestsAllowed());
            QVERIFY(client.join(headless.channel));
            QTRY_VERIFY_WITH_TIMEOUT(client.channel.joined(), 6000);
            QTRY_COMPARE(client.channel.savedChannels().first().toMap().value("ownerName").toString(), QString("Willow Station"));
            QVERIFY(headless.channel.hostParticipants().size() == 1);
            QVERIFY(!headless.channel.joined());
            QVERIFY(headless.channel.requests().isEmpty());
        }
        {
            Device desktop(dir.filePath("host"), "Desktop host", hostIdentity);
            QCOMPARE(desktop.channel.hostConfiguration().value("hostName").toString(), QString("Willow Station"));
            Device client(dir.filePath("client-restarted"), "Client restarted", clientIdentity);
            QVERIFY(!desktop.channel.requestsAllowed());
            QVERIFY(!desktop.channel.hostConfiguration().value("approvedClients").toArray().contains(clientIdentity.id()));
            QVERIFY(desktop.channel.setRequestsAllowed(true));
            QVERIFY(desktop.channel.listen(QHostAddress::LocalHost));
            QVERIFY(client.join(desktop.channel));
            QTRY_COMPARE_WITH_TIMEOUT(desktop.channel.requests().size(), 1, 6000);
            QVERIFY(!client.channel.joined());
            QVERIFY(desktop.channel.decide(client.channel.ownId(), true));
            QTRY_VERIFY_WITH_TIMEOUT(client.channel.joined(), 6000);
        }
    }

    void repeatedVoiceJoinKeepsConnectionAndHistory() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), member(dir.filePath("member"), "Member");
        Device observer(dir.filePath("observer"), "Observer");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(member.channel.ownId(), true));
        QVERIFY(host.channel.decide(observer.channel.ownId(), true));
        QVERIFY(observer.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(observer.channel.chatReady());
        QVERIFY(member.join(host.channel));
        QTRY_VERIFY(member.channel.joined());
        QTRY_COMPARE(observer.channel.messages().size(), 1);
        QSignalSpy events(&member.channel, &LocalChannel::channelEvent);
        for (int repeat = 0; repeat < 3; ++repeat) {
            QVERIFY(member.join(host.channel));
            QVERIFY(member.channel.joined());
            QVERIFY(member.channel.joinSaved(host.channel.ownId()));
            QVERIFY(member.channel.joined());
        }
        QVERIFY(member.channel.sendChat("Still connected"));
        QTRY_COMPARE(observer.channel.messages().last().toMap().value("text").toString(), QString("Still connected"));
        QCOMPARE(observer.channel.messages().size(), 2);
        QCOMPARE(events.size(), 0);
        QCOMPARE(host.channel.hostParticipants().size(), 1);
    }

    void systemMessagesTrackVoiceOnlyAndKeepHostLifetime() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("events"), "Host", identity, [&] { return now; });
        Device observer(dir.filePath("observer"), "Observer"), member(dir.filePath("member"), "Member");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.setMessageLifetimeDays(7));
        QVERIFY(host.channel.decide(observer.channel.ownId(), true));
        QVERIFY(host.channel.decide(member.channel.ownId(), true));
        QVERIFY(observer.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QVERIFY(member.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(observer.channel.chatReady() && member.channel.chatReady());
        QTest::qWait(100); QVERIFY(observer.channel.messages().isEmpty());
        QVERIFY(member.join(host.channel)); QTRY_VERIFY(member.channel.joined());
        QTRY_COMPARE(observer.channel.messages().size(), 1);
        QVERIFY(member.channel.leave()); QTRY_VERIFY(!member.channel.joined());
        QTRY_COMPARE(observer.channel.messages().size(), 2);
        QVERIFY(member.join(host.channel)); QTRY_VERIFY(member.channel.joined());
        QTRY_COMPARE(observer.channel.messages().size(), 3);
        QVERIFY(host.channel.kick(member.channel.ownId())); QTRY_VERIFY(!member.channel.joined());
        QTRY_COMPARE(observer.channel.messages().size(), 4);
        QVERIFY(member.join(host.channel)); QTRY_VERIFY(member.channel.joined());
        QTRY_COMPARE(observer.channel.messages().size(), 5);
        QVERIFY(host.channel.setBlocked(member.channel.ownId(), true)); QTRY_VERIFY(!member.channel.joined());
        QTRY_COMPARE(observer.channel.messages().size(), 6);
        const QStringList kinds{"joined", "left", "joined", "kicked", "joined", "banned"};
        const QStringList texts{"Member joined.", "Member left.", "Member joined.",
                                "Member was kicked.", "Member joined.", "Member was banned from the channel."};
        for (int i = 0; i < kinds.size(); ++i) {
            const auto record = observer.channel.messages().at(i).toMap();
            QCOMPARE(record.value("text").toString(), texts.at(i));
            const auto event = record.value("event").toMap();
            QCOMPARE(event.value("kind").toString(), kinds.at(i));
            QCOMPARE(event.value("member").toString(), member.channel.ownId());
            QCOMPARE(event.value("name").toString(), QString("Member"));
            QCOMPARE(record.value("name").toString(), QString("System"));
            QCOMPARE(record.value("avatarId").toString(), QString("system"));
            QVERIFY(record.value("sender").toString() != host.channel.ownId());
            QCOMPARE(record.value("expires").toLongLong() - record.value("created").toLongLong(), 7 * ChatHistory::lifetime);
        }
        QVERIFY(observer.channel.sendChat("System-looking user text is still a member message."));
        QTRY_COMPARE(observer.channel.messages().size(), 7);
        QVERIFY(!observer.channel.messages().last().toMap().contains("event"));
        host.channel.stopHost();
        ChatHistory saved(dir.filePath("events.channel.chat"), identity, now);
        QCOMPARE(saved.page().value("records").toArray().size(), 7);
        QVERIFY(ChatHistory::validMessage(saved.page().value("records").toArray().first().toObject()));
        auto malformed = saved.page().value("records").toArray().first().toObject();
        malformed.insert("event", QJsonObject{{"kind", "joined"}, {"member", "invalid-device"}, {"name", "Member"}});
        QVERIFY(!ChatHistory::validMessage(malformed));
        QVERIFY(saved.expire(now + 7 * ChatHistory::lifetime)); QVERIFY(saved.page().value("records").toArray().isEmpty());
    }

    void futureSystemEventSurvivesEncryptedHistoryRestart() {
        QTemporaryDir directory;
        const auto identity = TlsIdentity::create();
        const qint64 now = 1700000000000;
        const auto path = directory.filePath("history");
        QJsonObject receipt;
        {
            ChatHistory history(path, identity, now);
            receipt = history.append(identity.id(), "System", QUuid::createUuid().toString(QUuid::WithoutBraces),
                "A feature this client does not know.", now, {}, "system", ChatHistory::lifetime,
                {{"kind", "future.notice.v2"}, {"future", QJsonObject{{"value", 2}}}});
            QCOMPARE(history.page().value("records").toArray().size(), 1);
        }
        ChatHistory restored(path, identity, now + 1);
        QCOMPARE(restored.page().value("records").toArray().size(), 1);
        QCOMPARE(restored.page().value("records").toArray().first().toObject(), receipt);
        QVERIFY(restored.expire(now + ChatHistory::lifetime));
        QVERIFY(restored.page().value("records").toArray().isEmpty());
    }

    void rosterDepartureExtensionsPreserveBaseline_data() {
        QTest::addColumn<QJsonValue>("departures"); QTest::addColumn<QString>("expected");
        const QString id(64, 'f');
        QTest::newRow("missing") << QJsonValue{} << QString("memberLeave");
        QTest::newRow("future-kind") << QJsonValue(QJsonObject{{id, "moved-v2"}}) << QString("memberLeave");
        QTest::newRow("future-value") << QJsonValue(QJsonObject{{id, QJsonObject{{"v2", true}}}}) << QString("memberLeave");
        QTest::newRow("future-envelope") << QJsonValue(QJsonArray{id}) << QString("memberLeave");
        QTest::newRow("kick") << QJsonValue(QJsonObject{{id, "kicked"}}) << QString("kick");
        QTest::newRow("ban") << QJsonValue(QJsonObject{{id, "banned"}}) << QString("ban");
    }
    void rosterDepartureExtensionsPreserveBaseline() {
        QFETCH(QJsonValue, departures); QFETCH(QString, expected);
        QTemporaryDir dir; Device client(dir.filePath("client"), "Listener");
        const auto identity = TlsIdentity::create();
        auto configuration = identity.configuration(); configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslServer server; server.setSslConfiguration(configuration);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QTRY_VERIFY(server.hasPendingConnections());
        auto* socket = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(socket);
        QTRY_VERIFY(socket->bytesAvailable() > 4); socket->readAll();
        socket->write(wireFrame({{"type", "join"}, {"result", "accepted"}}));
        const QJsonObject listener{{"id", client.channel.ownId()}, {"name", "Listener"}, {"available", true}, {"muted", false}};
        const QJsonObject departing{{"id", QString(64, 'f')}, {"name", "Departing"}, {"available", true}, {"muted", false}};
        QJsonObject roster{{"type", "roster"}, {"host", identity.id()}, {"members", QJsonArray{listener, departing}}};
        socket->write(wireFrame(roster)); QTRY_COMPARE(client.channel.participants().size(), 2);
        QSignalSpy events(&client.channel, &LocalChannel::channelEvent);
        roster.insert("members", QJsonArray{listener}); roster.insert("departures", departures);
        socket->write(wireFrame(roster)); QTRY_COMPARE(client.channel.participants().size(), 1);
        QCOMPARE(events.count(), 1); QCOMPARE(events.first().first().toString(), expected);
        auto updated = listener; updated.insert("name", "Barrier");
        roster.insert("members", QJsonArray{updated});
        socket->write(wireFrame(roster));
        QTRY_COMPARE(participant(client.channel, client.channel.ownId()).value("name").toString(), QString("Barrier"));
        QCOMPARE(events.count(), 1); QVERIFY(client.channel.joined());
    }

    void rosterSnapshotsAreAtomicAndBounded_data() {
        QTest::addColumn<QString>("failure");
        for (const auto* value : {"", "unaccepted", "oversize", "fractional-size", "invalid-id", "nested-start",
            "wrong-id", "wrong-offset", "fractional-offset", "bad-base64", "empty-chunk", "large-chunk", "overrun",
            "early-end", "wrong-end", "invalid-json", "wrong-type", "interleaved-roster", "repeat-chunk"})
            QTest::newRow(*value ? value : "complete-and-repeat") << QString::fromLatin1(value);
    }
    void rosterSnapshotsAreAtomicAndBounded() {
        QFETCH(QString, failure);
        QTemporaryDir dir;
        static const auto identity = TlsIdentity::create();
        static const auto clientIdentity = TlsIdentity::create();
        Device client(dir.filePath("client"), "Reader", clientIdentity);
        auto configuration = identity.configuration(); configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslServer server; server.setSslConfiguration(configuration);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QVERIFY(waitForEvents(&server, &QTcpServer::pendingConnectionAvailable, [&] { return server.hasPendingConnections(); }));
        auto* socket = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(socket);
        QVERIFY(waitForEvents(socket, &QSslSocket::readyRead, [&] { return socket->bytesAvailable() > 4; })); socket->readAll();
        QByteArray incoming;
        const auto receivedAck = [&](qint64 offset) {
            bool received = false;
            return waitForEvents(socket, &QSslSocket::readyRead, [&] {
                if (received) return true;
                incoming.append(socket->readAll());
                while (incoming.size() >= 4) {
                    const auto size = qFromBigEndian<quint32>(incoming.constData());
                    if (incoming.size() < size + 4) break;
                    const auto message = QJsonDocument::fromJson(incoming.mid(4, size)).object();
                    incoming.remove(0, size + 4);
                    if (message.value("type") == "rosterAck" && message.value("offset").toInteger(-1) == offset) return received = true;
                }
                return false;
            });
        };
        QJsonObject roster{{"type", "roster"}, {"host", identity.id()}, {"members", QJsonArray{
            QJsonObject{{"id", client.channel.ownId()}, {"name", "Before"}, {"available", true}, {"muted", true}},
            QJsonObject{{"id", identity.id()}, {"name", "Speaker"}, {"available", true}, {"muted", false}}}}};
        if (failure != "unaccepted") {
            socket->write(wireFrame({{"type", "join"}, {"result", "accepted"}}) + wireFrame(roster));
            QTRY_COMPARE(participant(client.channel, client.channel.ownId()).value("name").toString(), QString("Before"));
        }
        auto members = roster.value("members").toArray(); auto member = members.first().toObject();
        member.insert("name", "After"); members[0] = member; roster.insert("members", members);
        auto bytes = QJsonDocument(roster).toJson(QJsonDocument::Compact);
        if (failure == "invalid-json") bytes = "not JSON";
        if (failure == "wrong-type") bytes = "{\"type\":\"rosterStart\"}";
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject start{{"type", "rosterStart"}, {"id", id}, {"size", bytes.size()}};
        if (failure == "oversize") start.insert("size", 2 * 1024 * 1024 + 1);
        if (failure == "fractional-size") start.insert("size", 2.5);
        if (failure == "invalid-id") start.insert("id", "invalid");
        socket->write(wireFrame(start));
        if (failure == "nested-start") socket->write(wireFrame(start));
        if (failure == "interleaved-roster") socket->write(wireFrame(roster));
        QJsonObject chunk{{"type", "rosterChunk"}, {"id", id}, {"offset", 0}, {"data", QString::fromLatin1(bytes.first(1).toBase64())}};
        if (failure == "wrong-id") chunk.insert("id", QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (failure == "wrong-offset") chunk.insert("offset", 1);
        if (failure == "fractional-offset") chunk.insert("offset", 0.5);
        if (failure == "bad-base64") chunk.insert("data", "%%%%");
        if (failure == "empty-chunk") chunk.insert("data", "");
        if (failure == "large-chunk") chunk.insert("data", QString::fromLatin1(QByteArray(16385, 'x').toBase64()));
        if (failure == "overrun") chunk.insert("data", QString::fromLatin1((bytes + 'x').toBase64()));
        socket->write(wireFrame(chunk));
        const bool validChunk = failure.isEmpty() || failure == "early-end" || failure == "wrong-end"
            || failure == "invalid-json" || failure == "wrong-type" || failure == "repeat-chunk";
        if (validChunk) {
            QVERIFY(receivedAck(0));
            QCOMPARE(participant(client.channel, client.channel.ownId()).value("name").toString(), QString("Before"));
            if (failure.isEmpty()) {
                squad::VoiceMixer encoder;
                const std::array<float, 960> samples{};
                const auto audio = encoder.encode(samples, 48000).first();
                QSignalSpy heard(&client.channel, &LocalChannel::audioReceived);
                socket->write(wireFrame({{"type", "audio"}, {"sender", identity.id()}, {"data", QString::fromLatin1(audio.toBase64())}}));
                QVERIFY(waitForEvents(&client.channel, &LocalChannel::audioReceived, [&] { return heard.size() == 1; }));
                QCOMPARE(heard.first().at(1).toByteArray(), audio);
                QCOMPARE(participant(client.channel, client.channel.ownId()).value("name").toString(), QString("Before"));
            }
            if (failure == "repeat-chunk") socket->write(wireFrame(chunk));
            else if (failure != "early-end") {
                chunk.insert("offset", 1); chunk.insert("data", QString::fromLatin1(bytes.mid(1).toBase64()));
                socket->write(wireFrame(chunk));
                QVERIFY(receivedAck(1));
            }
            socket->write(wireFrame({{"type", "rosterEnd"}, {"id", failure == "wrong-end" ? "wrong" : id}}));
        }
        if (!failure.isEmpty()) {
            QVERIFY(waitForEvents(socket, &QSslSocket::disconnected, [&] { return socket->state() == QAbstractSocket::UnconnectedState; }));
            QVERIFY(!client.channel.joined()); return;
        }
        QTRY_COMPARE(participant(client.channel, client.channel.ownId()).value("name").toString(), QString("After"));
        // A completed snapshot releases its buffer and allows the next update.
        member.insert("name", "Again"); members[0] = member; roster.insert("members", members);
        bytes = QJsonDocument(roster).toJson(QJsonDocument::Compact); start.insert("size", bytes.size());
        socket->write(wireFrame(start) + wireFrame({{"type", "rosterChunk"}, {"id", id}, {"offset", 0},
            {"data", QString::fromLatin1(bytes.toBase64())}}));
        QVERIFY(receivedAck(0));
        socket->write(wireFrame({{"type", "rosterEnd"}, {"id", id}}));
        QTRY_COMPARE(participant(client.channel, client.channel.ownId()).value("name").toString(), QString("Again"));
        QVERIFY(client.channel.joined());
    }

    void rosterOwnerNameValidation_data() {
        QTest::addColumn<QJsonValue>("ownerName");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("older-peer") << QJsonValue(QJsonValue::Undefined) << true;
        QTest::newRow("headless") << QJsonValue("") << true;
        QTest::newRow("profile") << QJsonValue("Moonlit Fox") << true;
        QTest::newRow("null") << QJsonValue(QJsonValue::Null) << false;
        QTest::newRow("number") << QJsonValue(42) << false;
        QTest::newRow("object") << QJsonValue(QJsonObject{}) << false;
        QTest::newRow("line-break") << QJsonValue("One\nTwo") << false;
        QTest::newRow("too-long") << QJsonValue(QString(81, 'x')) << false;
    }

    void rosterOwnerNameValidation() {
        QFETCH(QJsonValue, ownerName); QFETCH(bool, accepted);
        QTemporaryDir dir;
        Device client(dir.filePath("client"), "Reader");
        const auto identity = TlsIdentity::create();
        QSslServer server; server.setSslConfiguration(identity.configuration());
        connect(&server, &QSslServer::sslErrors, &server,
            [](QSslSocket* socket, const QList<QSslError>& errors) { socket->ignoreSslErrors(errors); });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QTRY_VERIFY(server.hasPendingConnections());
        auto* socket = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(socket);
        QTRY_VERIFY(socket->bytesAvailable() > 4); socket->readAll();
        QSignalSpy disconnected(socket, &QSslSocket::disconnected);
        socket->write(wireFrame({{"type", "join"}, {"result", "accepted"}}));
        QTRY_VERIFY(client.channel.joined());
        QJsonObject roster{{"type", "roster"}, {"host", identity.id()}, {"name", "Room"},
            {"members", QJsonArray{QJsonObject{{"id", client.channel.ownId()}, {"name", "Received roster"},
                {"available", true}, {"muted", true}}}}};
        if (!ownerName.isUndefined()) roster.insert("ownerName", ownerName);
        socket->write(wireFrame(roster));
        if (!accepted) {
            QTRY_COMPARE(disconnected.size(), 1);
            QVERIFY(!client.channel.savedChannels().first().toMap().contains("ownerName"));
            return;
        }
        QTRY_COMPARE(participant(client.channel, client.channel.ownId()).value("name").toString(), QString("Received roster"));
        QCOMPARE(client.channel.savedChannels().first().toMap().value("ownerName").toString(), ownerName.toString());
        QVERIFY(client.channel.joined());
    }

    void rosterDepartureReasonsReachRemainingVoiceListeners() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host");
        Device listener(dir.filePath("listener"), "Listener");
        Device listenerTwo(dir.filePath("listener-two"), "Listener Two");
        Device observer(dir.filePath("observer"), "Observer");
        Device first(dir.filePath("first"), "First");
        Device second(dir.filePath("second"), "Second");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        for (const auto* client : {&listener, &listenerTwo, &observer, &first, &second})
            QVERIFY(host.channel.decide(client->channel.ownId(), true));
        QVERIFY(observer.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(observer.channel.chatReady());
        QVERIFY(listener.join(host.channel));
        QVERIFY(listenerTwo.join(host.channel));
        QVERIFY(first.join(host.channel));
        QVERIFY(second.join(host.channel));
        QTRY_VERIFY(listener.channel.joined() && listenerTwo.channel.joined() && first.channel.joined() && second.channel.joined());
        QSignalSpy listenerEvents(&listener.channel, &LocalChannel::channelEvent);
        QSignalSpy listenerTwoEvents(&listenerTwo.channel, &LocalChannel::channelEvent);
        QSignalSpy observerEvents(&observer.channel, &LocalChannel::channelEvent);
        QSignalSpy firstEvents(&first.channel, &LocalChannel::channelEvent);
        QSignalSpy secondEvents(&second.channel, &LocalChannel::channelEvent);

        QTRY_COMPARE(listener.channel.participants().size(), 4);
        QTRY_COMPARE(listenerTwo.channel.participants().size(), 4);
        listenerEvents.clear();
        listenerTwoEvents.clear();

        QVERIFY(first.channel.leave());
        QTRY_COMPARE(listenerEvents.count(), 1);
        QCOMPARE(listenerEvents.first().first().toString(), QString("memberLeave"));
        QTRY_COMPARE(listenerTwoEvents.count(), 1);
        QCOMPARE(listenerTwoEvents.first().first().toString(), QString("memberLeave"));
        listenerEvents.clear();
        listenerTwoEvents.clear();
        QVERIFY(first.join(host.channel));
        QTRY_VERIFY(first.channel.joined());
        QTRY_COMPARE(listener.channel.participants().size(), 4);
        QTRY_COMPARE(listenerTwo.channel.participants().size(), 4);
        listenerEvents.clear();
        listenerTwoEvents.clear();
        firstEvents.clear();
        secondEvents.clear();

        QVERIFY(host.channel.kick(first.channel.ownId()));
        QVERIFY(host.channel.setBlocked(second.channel.ownId(), true));
        QTRY_COMPARE(firstEvents.count(), 1);
        QTRY_COMPARE(secondEvents.count(), 1);
        QCOMPARE(firstEvents.first().first().toString(), QString("kick"));
        QCOMPARE(secondEvents.first().first().toString(), QString("ban"));
        QTRY_COMPARE(listenerEvents.count(), 2);
        QStringList kinds;
        for (const auto& event : listenerEvents) kinds.append(event.first().toString());
        QVERIFY(kinds.contains("kick"));
        QVERIFY(kinds.contains("ban"));
        QVERIFY(!kinds.contains("memberLeave"));
        QTRY_COMPARE(listenerTwoEvents.count(), 2);
        QStringList secondKinds;
        for (const auto& event : listenerTwoEvents) secondKinds.append(event.first().toString());
        QVERIFY(secondKinds.contains("kick"));
        QVERIFY(secondKinds.contains("ban"));
        QCOMPARE(observerEvents.count(), 0);
        QVERIFY(listenerTwo.session.setUserName("Updated listener"));
        QTRY_COMPARE(participant(listener.channel, listenerTwo.channel.ownId()).value("name").toString(), QString("Updated listener"));
        QCOMPARE(listenerEvents.count(), 2);
        QCOMPARE(listenerTwoEvents.count(), 2);
        QTRY_VERIFY(!first.channel.joined() && !second.channel.joined());
    }

    void hostSettingsPersistAndTtlChangesOnlyNewMessages() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("host-settings"), "Host", identity, [&] { return now; });
        Device client(dir.filePath("client-settings"), "Client", TlsIdentity::create(), [&] { return now; });
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        const auto activePort = host.channel.port();
        QVERIFY(!host.channel.setConfiguredPort(0)); QVERIFY(!host.channel.setConfiguredPort(65536));
        QVERIFY(host.channel.setConfiguredPort(49152)); QCOMPARE(host.channel.port(), activePort);
        QVERIFY(!host.channel.setMessageLifetimeDays(2));
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.join(host.channel)); QTRY_VERIFY(client.channel.chatReady());
        const auto send = [&](const QString& text, int count) {
            if (!client.channel.sendChat(text)) return false;
            QElapsedTimer timer; timer.start();
            while (memberMessages(client.channel).size() < count && timer.elapsed() < 3000) QTest::qWait(10);
            return memberMessages(client.channel).size() == count;
        };
        QCOMPARE(client.channel.chatLifetimeDays(), 1);
        QVERIFY(send("one day", 1));
        QVERIFY(host.channel.setMessageLifetimeDays(7));
        QTRY_COMPARE(client.channel.chatLifetimeDays(), 7);
        QVERIFY(send("one week", 2));
        QVERIFY(host.channel.setMessageLifetimeDays(30));
        QTRY_COMPARE(client.channel.chatLifetimeDays(), 30);
        QVERIFY(send("thirty days", 3));
        const auto messages = memberMessages(client.channel);
        for (int i = 0; i < 3; ++i) {
            const auto record = messages[i].toMap();
            QCOMPARE(record.value("expires").toLongLong() - record.value("created").toLongLong(),
                     (ChatHistory::lifetime * std::array<int, 3>{1, 7, 30}[i]));
        }
        Device restored(dir.filePath("host-settings"), "Host", identity, [&] { return now; });
        QCOMPARE(restored.channel.configuredPort(), 49152); QCOMPARE(restored.channel.messageLifetimeDays(), 30);
        ChatHistory history(dir.filePath("host-settings.channel.chat"), identity, now + ChatHistory::lifetime);
        QCOMPARE(history.page().value("records").toArray().size(), 2);
        QVERIFY(history.expire(now + 7 * ChatHistory::lifetime)); QCOMPARE(history.page().value("records").toArray().size(), 1);
        QVERIFY(history.expire(now + 30 * ChatHistory::lifetime)); QVERIFY(history.page().value("records").toArray().isEmpty());
    }

    void hostsOfferControlWithoutClientPairingAndRevokeIt() {
        QTemporaryDir dir;
        Device first(dir.filePath("first"), "First"), second(dir.filePath("second"), "Second"), client(dir.filePath("client"), "Client");
        QVERIFY(first.channel.listen(QHostAddress::LocalHost));
        QVERIFY(second.channel.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.listen(QHostAddress::LocalHost));
        QVERIFY(!client.channel.chooseRemoteOffer(first.channel.ownId()));
        for (auto* host : {&first, &second}) {
            QVERIFY(host->channel.decide(client.channel.ownId(), true));
            QVERIFY(client.channel.openChat(host->channel.ownId(), "127.0.0.1", host->channel.port()));
            QTRY_VERIFY(client.channel.chatReady());
            QVERIFY(host->channel.setRemotePermission(client.channel.ownId(), true));
        }
        QTRY_COMPARE(client.channel.remoteOffers().size(), 2);
        QVERIFY(!client.channel.remoteMode());
        QVERIFY(first.channel.controlRequests().isEmpty());
        QVERIFY(second.channel.controlRequests().isEmpty());
        QVERIFY(first.join(first.channel)); QTRY_VERIFY(first.channel.joined());
        QVERIFY(client.join(client.channel)); QTRY_VERIFY(client.channel.joined());
        QVERIFY(client.channel.sendChat("Personal history")); QTRY_VERIFY(!client.channel.chatPending());
        QVERIFY(!client.channel.messages().isEmpty());
        QVERIFY(client.channel.chooseRemoteOffer(first.channel.ownId()));
        QTRY_VERIFY(client.channel.remoteAllowed());
        QVERIFY(!client.channel.joined());
        QVERIFY(!client.channel.chatReady());
        QVERIFY(client.channel.chatHostId().isEmpty());
        QVERIFY(client.channel.messages().isEmpty());
        QVERIFY(client.channel.participants().isEmpty());
        QVERIFY(client.channel.hosting());
        const auto hasController = [&](const LocalChannel& host) {
            for (const auto& value : host.hostClients())
                if (value.toMap().value("id").toString() == client.channel.ownId()) return true;
            return false;
        };
        QTRY_VERIFY(!hasController(first.channel) && !hasController(second.channel) && !hasController(client.channel));
        QVERIFY(!client.channel.openChat(second.channel.ownId(), "127.0.0.1", second.channel.port()));
        QVERIFY(!client.channel.sendAudio(QByteArray("controller microphone")));
        QVERIFY(!client.channel.sendChat("controller identity"));
        QTRY_COMPARE(client.channel.remoteView().value("joinedHostId").toString(), first.channel.ownId());
        QVERIFY(client.channel.remoteAction("chat", {{"text", "Sent through the target"}}));
        QTRY_COMPARE(memberMessages(first.channel).size(), 1);
        QCOMPARE(memberMessages(first.channel).first().toMap().value("sender").toString(), first.channel.ownId());
        QVERIFY(client.channel.setRemoteMode(false));
        QVERIFY(client.channel.chooseRemoteOffer(second.channel.ownId()));
        QTRY_VERIFY(client.channel.remoteAllowed());
        QTRY_COMPARE(client.channel.remoteView().value("ownId").toString(), second.channel.ownId());
        QVERIFY(second.channel.setRemotePermission(client.channel.ownId(), false));
        QTRY_VERIFY(!client.channel.remoteMode());
        QVERIFY(!client.channel.remoteAllowed()); QVERIFY(client.channel.remoteView().isEmpty());
        QCOMPARE(client.channel.remoteOffers().size(), 1);
        QVERIFY(!client.channel.chooseRemoteOffer(second.channel.ownId()));
        QVERIFY(second.channel.controlRequests().isEmpty());
        // The grant survives closing the ordinary chat, and is re-announced after reconnect.
        QVERIFY(client.channel.openSavedChat(first.channel.ownId())); QTRY_VERIFY(client.channel.chatReady());
        QVERIFY(first.channel.setRemotePermission(client.channel.ownId(), false));
        QTRY_VERIFY(client.channel.remoteOffers().isEmpty());
        QVERIFY(first.channel.setRemotePermission(client.channel.ownId(), true));
        QTRY_COMPARE(client.channel.remoteOffers().size(), 1);
        QVERIFY(client.channel.closeChat(first.channel.ownId()));
        QVERIFY(client.channel.openSavedChat(first.channel.ownId())); QTRY_VERIFY(client.channel.chatReady());
        QTRY_COMPARE(client.channel.remoteOffers().size(), 1);
        LocalChannel restored(client.session, dir.filePath("client.channel"), TlsIdentity::create());
        QCOMPARE(restored.remoteOffers().size(), 1);
    }

    void initializationHostsExactlyOneChannelAndOpensItsChatWithoutVoice() {
        QTemporaryDir dir;
        Device device(dir.filePath("device"), "Member");
        QVERIFY(device.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(device.channel.initialize());
        QTRY_VERIFY(device.channel.chatReady());
        QVERIFY(device.channel.hosting()); QVERIFY(!device.channel.joined());
        QCOMPARE(device.channel.chatHostId(), device.channel.ownId());
        QCOMPARE(device.channel.savedChannels().size(), 1);
        QVERIFY(device.channel.initialize());
        QCOMPARE(device.channel.savedChannels().size(), 1);
        QVERIFY(device.channel.setChannelName("One channel"));
        QTRY_COMPARE(device.channel.savedChannels().first().toMap().value("name").toString(), QString("One channel"));
    }

    void multicastDiscoveryFindsIndependentHostsAndJoinsDiscoveredEndpoint() {
        QTemporaryDir dir;
        QUdpSocket probeFirst, probeSecond;
        const auto interface = multicastInterface();
        QVERIFY(interface.isValid());
        const QHostAddress group(QStringLiteral("239.255.85.73"));
        const auto probeOptions = QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint;
        if (!probeFirst.bind(QHostAddress::AnyIPv4, 0, probeOptions))
            QSKIP("The test host cannot bind a multicast probe socket.");
        if (!probeSecond.bind(QHostAddress::AnyIPv4, probeFirst.localPort(), probeOptions))
            QSKIP("The test host cannot share a multicast probe port between local sockets.");
        if (!probeFirst.joinMulticastGroup(group, interface) || !probeSecond.joinMulticastGroup(group, interface))
            QSKIP("The test host cannot join the multicast probe group.");
        probeFirst.setMulticastInterface(interface);
        probeFirst.setSocketOption(QAbstractSocket::MulticastTtlOption, 0);
        probeFirst.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
        probeFirst.writeDatagram("probe", group, probeFirst.localPort());
        QTest::qWait(500);
        if (!probeFirst.hasPendingDatagrams() || !probeSecond.hasPendingDatagrams())
            QSKIP("The test host does not replicate multicast datagrams to both local sockets.");
        probeFirst.receiveDatagram(); probeSecond.receiveDatagram();

        Device first(dir.filePath("discovery-first"), "First host");
        Device second(dir.filePath("discovery-second"), "Second host");
        QVERIFY(first.channel.listen(QHostAddress::AnyIPv4, 0));
        QVERIFY(second.channel.listen(QHostAddress::AnyIPv4, 0));
        QVERIFY(first.channel.startDiscovery());
        QVERIFY(second.channel.startDiscovery());

        QVariantMap discovered;
        LocalChannel* host = nullptr;
        LocalChannel* client = nullptr;
        bool firstSawSecond = false;
        bool secondSawFirst = false;
        const auto findDiscoveredHosts = [&] {
            for (const auto& value : first.channel.hosts()) {
                const auto entry = value.toMap();
                if (entry.value("id").toString() == second.channel.ownId()) {
                    discovered = entry;
                    host = &second.channel; client = &first.channel;
                    firstSawSecond = true;
                }
            }
            for (const auto& value : second.channel.hosts()) {
                const auto entry = value.toMap();
                if (entry.value("id").toString() == first.channel.ownId()) {
                    secondSawFirst = true;
                    if (!host) { discovered = entry; host = &first.channel; client = &second.channel; }
                }
            }
            return firstSawSecond && secondSawFirst && host != nullptr && client != nullptr;
        };
        QTest::qWait(3000);
        QVERIFY(findDiscoveredHosts());
        QVERIFY(!discovered.value("address").toString().isEmpty());
        QVERIFY(discovered.value("port").toInt() > 0);
        QVERIFY(host && client);
        QVERIFY(host->decide(client->ownId(), true));
        QVERIFY(client->join(discovered.value("id").toString(), discovered.value("address").toString(), discovered.value("port").toInt()));
        QTRY_VERIFY_WITH_TIMEOUT(client->joined(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(client->participants().size(), 1, 10000);
        QCOMPARE(client->participants().first().toMap().value("id").toString(), client->ownId());
        QVERIFY(client->sendChat("Discovered over multicast."));
        QTRY_COMPARE_WITH_TIMEOUT(memberMessages(*client).size(), 1, 10000);
        QCOMPARE(memberMessages(*client).first().toMap().value("text").toString(), QString("Discovered over multicast."));
    }

    void boundedDiscoverySearchFindsLoopbackHostWithoutSideEffects() {
        QTemporaryDir dir;
        QTcpServer reservation;
        quint16 port = 0;
        for (quint16 candidate = 48763; candidate <= 48783; ++candidate) {
            if (reservation.listen(QHostAddress::LocalHost, candidate)) {
                port = candidate; reservation.close(); break;
            }
        }
        QVERIFY2(port != 0, "No bounded discovery port is available.");
        Device host(dir.filePath("scan-host"), "Scanned host");
        Device scanner(dir.filePath("scan-client"), "Scanner");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost, port));
        QVERIFY(scanner.channel.startService(QHostAddress::LocalHost, 0));
        const auto hostStore = dir.filePath("scan-host.channel");
        QFile beforeHostStore(hostStore);
        const auto hostExisted = beforeHostStore.exists();
        QByteArray beforeHost;
        if (hostExisted) {
            QVERIFY(beforeHostStore.open(QIODevice::ReadOnly));
            beforeHost = beforeHostStore.readAll();
            beforeHostStore.close();
        }
        const auto scannerStore = dir.filePath("scan-client.channel");
        QFile beforeStore(scannerStore);
        const auto existed = beforeStore.exists();
        QByteArray before;
        if (existed) {
            QVERIFY(beforeStore.open(QIODevice::ReadOnly));
            before = beforeStore.readAll();
            beforeStore.close();
        }
        QVERIFY(scanner.channel.setDiscoverySearch(true));
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const auto& value : scanner.channel.hosts())
                if (value.toMap().value("id").toString() == host.channel.ownId()) return true;
            return false;
        }(), 10000);
        QVERIFY(!scanner.channel.joined());
        QVERIFY(scanner.channel.chatHostId().isEmpty());
        QVERIFY(scanner.channel.savedChannels().isEmpty());
        QVERIFY(scanner.channel.requests().isEmpty());
        QVERIFY(host.channel.requests().isEmpty());
        QFile afterStore(scannerStore);
        QCOMPARE(afterStore.exists(), existed);
        if (existed) { QVERIFY(afterStore.open(QIODevice::ReadOnly)); QCOMPARE(afterStore.readAll(), before); afterStore.close(); }
        QFile afterHostStore(hostStore);
        QCOMPARE(afterHostStore.exists(), hostExisted);
        if (hostExisted) { QVERIFY(afterHostStore.open(QIODevice::ReadOnly)); QCOMPARE(afterHostStore.readAll(), beforeHost); afterHostStore.close(); }
        int foundCount = 0;
        for (const auto& value : scanner.channel.hosts())
            if (value.toMap().value("id").toString() == host.channel.ownId()) ++foundCount;
        QCOMPARE(foundCount, 1);
        QVERIFY(host.channel.decide(scanner.channel.ownId(), true));
        QVERIFY(scanner.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY_WITH_TIMEOUT(scanner.channel.chatReady(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const auto saved = scanner.channel.savedChannels();
            return std::any_of(saved.cbegin(), saved.cend(),
                [&](const QVariant& value) { return value.toMap().value("id").toString() == host.channel.ownId(); });
        }(), 10000);
        QVERIFY(scanner.channel.setDiscoverySearch(false));
        QVERIFY(scanner.channel.setDiscoverySearch(true));
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const auto& value : scanner.channel.hosts())
                if (value.toMap().value("id").toString() == host.channel.ownId()) return true;
            return false;
        }(), 10000);
        int repeatedCount = 0;
        for (const auto& value : scanner.channel.hosts())
            if (value.toMap().value("id").toString() == host.channel.ownId()) ++repeatedCount;
        QCOMPARE(repeatedCount, 1);
        bool offered = false;
        for (const auto& value : scanner.channel.availableHosts())
            offered |= value.toMap().value("id").toString() == host.channel.ownId();
        QVERIFY(!offered);
        QVERIFY(scanner.channel.setDiscoverySearch(false));
    }

    void repeatedDiscoveryDoesNotRebuildUnchangedChannels() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host");
        Device scanner(dir.filePath("scanner"), "Scanner");
        quint16 port = 48763;
        while (port <= 48783 && !host.channel.listen(QHostAddress::LocalHost, port)) ++port;
        QVERIFY(port <= 48783);
        QVERIFY(scanner.channel.startService(QHostAddress::LocalHost, 0));
        QSignalSpy changes(&scanner.channel, &LocalChannel::hostsChanged);
        QVERIFY(scanner.channel.setDiscoverySearch(true));
        QTRY_VERIFY(!scanner.channel.discoverySearching());
        QCOMPARE(scanner.channel.hosts().size(), 1);
        QCOMPARE(changes.size(), 1);
        const auto original = scanner.channel.hosts();
        changes.clear();
        for (int repeat = 0; repeat < 3; ++repeat) {
            QVERIFY(scanner.channel.setDiscoverySearch(true));
            QTRY_VERIFY(!scanner.channel.discoverySearching());
            QCOMPARE(scanner.channel.hosts(), original);
            QCOMPARE(changes.size(), 0);
        }
        QVERIFY(host.channel.setChannelName("Renamed"));
        QVERIFY(scanner.channel.setDiscoverySearch(true));
        QTRY_VERIFY(!scanner.channel.discoverySearching());
        QCOMPARE(changes.size(), 1);
        QCOMPARE(scanner.channel.hosts().first().toMap().value("name").toString(), QString("Renamed"));
    }

    void discoveryRefreshesLivenessWithoutRepeatingVisibleUpdates() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        Device observer(dir.filePath("observer"), "Observer", TlsIdentity::create(), [&] { return now; });
        QVERIFY(observer.channel.startDiscovery());
        QUdpSocket source;
        QVERIFY(source.bind(QHostAddress::LocalHost, 0));
        const QString device(64, 'a'), second(64, 'b');
        QJsonObject message{{"protocol", "squadspeak/1"}, {"id", device}, {"name", "Host"}, {"port", 50100},
            {"channels", QJsonArray{QJsonObject{{"id", device}, {"name", "First"}},
                                   QJsonObject{{"id", second}, {"name", "Second"}}}}};
        const auto send = [&] {
            const auto data = QJsonDocument(message).toJson(QJsonDocument::Compact);
            return source.writeDatagram(data, QHostAddress::LocalHost, 48762) == data.size();
        };
        QSignalSpy changes(&observer.channel, &LocalChannel::hostsChanged);
        QVERIFY(send());
        QTRY_COMPARE(observer.channel.hosts().size(), 2);
        QCOMPARE(changes.size(), 1);
        changes.clear();
        now += 5000;
        for (int repeat = 0; repeat < 5; ++repeat) QVERIFY(send());
        // A changed datagram is the completion barrier for preceding repeats.
        message.insert("port", 50101);
        QVERIFY(send());
        QTRY_COMPARE(observer.channel.hosts().first().toMap().value("port").toInt(), 50101);
        QCOMPARE(changes.size(), 1);
        changes.clear();
        now += 3000;
        QTest::qWait(1700); // Original announcement expired, refreshed hosts must remain.
        QCOMPARE(observer.channel.hosts().size(), 2);
        QCOMPARE(changes.size(), 0);
        message.insert("channels", QJsonArray{QJsonObject{{"id", device}, {"name", "Renamed"}}});
        QVERIFY(send());
        QTRY_COMPARE(observer.channel.hosts().size(), 1);
        QCOMPARE(observer.channel.hosts().first().toMap().value("name").toString(), QString("Renamed"));
        QCOMPARE(changes.size(), 1);
        now += 6001;
        QTRY_VERIFY_WITH_TIMEOUT(observer.channel.hosts().isEmpty(), 3000);
        QCOMPARE(changes.size(), 2);
    }

    void unchangedDiscoveryHeartbeatDoesNotRebuildChannels() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost, 0));
        QVERIFY(host.channel.startDiscovery());
        QSignalSpy changes(&host.channel, &LocalChannel::hostsChanged);
        const auto original = host.channel.hosts();
        QTest::qWait(3200); // Observe two scheduled discovery announcements.
        QCOMPARE(host.channel.hosts(), original);
        QCOMPARE(changes.size(), 0);
        QVERIFY(host.channel.setChannelName("Renamed"));
        QCOMPARE(changes.size(), 1);
        QCOMPARE(host.channel.hosts().first().toMap().value("name").toString(), QString("Renamed"));
    }

    void boundedDiscoveryResultsSurviveUntilTheDialogCloses_data() {
        QTest::addColumn<bool>("selected");
        QTest::newRow("unselected") << false;
        QTest::newRow("pending-request") << true;
    }
    void boundedDiscoveryResultsSurviveUntilTheDialogCloses() {
        QFETCH(bool, selected);
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        Device host(dir.filePath("host"), "TCP only");
        QVERIFY(host.channel.setChannelName("Bea - local test"));
        quint16 port = 48763;
        while (port <= 48783 && !host.channel.listen(QHostAddress::LocalHost, port)) ++port;
        QVERIFY(port <= 48783);
        Device scanner(dir.filePath("scanner"), "Scanner", TlsIdentity::create(), [&] { return now; });
        QVERIFY(scanner.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(scanner.channel.startDiscovery());
        const auto found = [&] {
            for (const auto& value : scanner.channel.availableHosts())
                if (value.toMap().value("id").toString() == host.channel.ownId()) return true;
            return false;
        };
        QVERIFY(scanner.channel.setDiscoverySearch(true));
        QTRY_VERIFY(found());
        QTRY_VERIFY(!scanner.channel.discoverySearching());
        now += 10000;
        QTest::qWait(1800); // Cross the public discovery heartbeat without refreshing the TCP-only host.
        QVERIFY(found());
        if (selected) {
            QVERIFY(scanner.channel.openSavedChat(host.channel.ownId()));
            QTRY_COMPARE(host.channel.requests().size(), 1);
            QTRY_COMPARE(scanner.channel.savedChannels().first().toMap().value("access").toString(), QString("pending"));
        }
        QVERIFY(scanner.channel.setDiscoverySearch(false));
        QTRY_VERIFY(!found());
        if (selected) {
            const auto saved = scanner.channel.savedChannels();
            QCOMPARE(saved.size(), 1);
            QCOMPARE(saved.first().toMap().value("name").toString(), host.channel.channelName());
            QCOMPARE(saved.first().toMap().value("access").toString(), QString("pending"));
            QVERIFY(!scanner.channel.chatReady());
            QVERIFY(scanner.channel.participants().isEmpty());
            QVERIFY(host.channel.decide(scanner.channel.ownId(), true));
            QTRY_VERIFY(scanner.channel.chatReady());
            QCOMPARE(scanner.channel.savedChannels().first().toMap().value("name").toString(), host.channel.channelName());
        }
    }

    void boundedDiscoverySearchCanBeCancelledAndReopened() {
        QTemporaryDir dir;
        Device scanner(dir.filePath("scan-cancel"), "Scanner");
        QVERIFY(scanner.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(scanner.channel.setDiscoverySearch(true));
        QVERIFY(scanner.channel.discoverySearching());
        QVERIFY(scanner.channel.setDiscoverySearch(false));
        QVERIFY(!scanner.channel.discoverySearching());
        QVERIFY(scanner.channel.setDiscoverySearch(true));
        QVERIFY(scanner.channel.setDiscoverySearch(false));
        QVERIFY(!scanner.channel.discoverySearching());
    }

    void destroyingDiscoveryClosesOutstandingConnections() {
        QTemporaryDir dir;
        QTcpServer stalled;
        for (quint16 port = 48763; port <= 48783 && !stalled.isListening(); ++port)
            stalled.listen(QHostAddress::LocalHost, port);
        QVERIFY2(stalled.isListening(), "No bounded discovery port is available.");
        for (int cycle = 0; cycle < 3; ++cycle) {
            auto scanner = std::make_unique<Device>(dir.filePath(QString::number(cycle)), "Scanner");
            QVERIFY(scanner->channel.startService(QHostAddress::LocalHost, 0));
            QTcpSocket* connection = nullptr;
            bool connectedBeforeDestruction = false;
            const auto accepted = connect(&stalled, &QTcpServer::newConnection, &stalled, [&] {
                connection = stalled.nextPendingConnection();
                connectedBeforeDestruction = connection && connection->state() == QAbstractSocket::ConnectedState;
                scanner.reset();
            });
            QVERIFY(scanner->channel.setDiscoverySearch(true));
            QTRY_VERIFY(connection);
            disconnect(accepted);
            QVERIFY(connectedBeforeDestruction);
            QVERIFY(!scanner);
            QTRY_COMPARE(connection->state(), QAbstractSocket::UnconnectedState);
            connection->deleteLater();
        }
    }

    void boundedDiscoverySearchIgnoresNonHostingEndpoint() {
        QTemporaryDir dir;
        QTcpServer reservation;
        quint16 port = 0;
        for (quint16 candidate = 48763; candidate <= 48783; ++candidate) {
            if (reservation.listen(QHostAddress::LocalHost, candidate)) {
                port = candidate; reservation.close(); break;
            }
        }
        QVERIFY2(port != 0, "No bounded discovery port is available.");
        Device endpoint(dir.filePath("scan-endpoint"), "Closed endpoint");
        Device scanner(dir.filePath("scan-nonhost"), "Scanner");
        QVERIFY(endpoint.channel.startService(QHostAddress::LocalHost, port));
        QVERIFY(scanner.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(scanner.channel.setDiscoverySearch(true));
        QTest::qWait(2000);
        bool found = false;
        for (const auto& value : scanner.channel.hosts()) found |= value.toMap().value("id") == endpoint.channel.ownId();
        QVERIFY(!found);
        QVERIFY(scanner.channel.setDiscoverySearch(false));
    }

    void boundedDiscoverySearchIgnoresOutsidePortRange() {
        QTemporaryDir dir;
        QTcpServer reservation;
        quint16 port = 48800;
        while (port < 48820 && !reservation.listen(QHostAddress::LocalHost, port)) ++port;
        QVERIFY2(port < 48820, "No outside-range test port is available.");
        reservation.close();
        Device endpoint(dir.filePath("scan-outside"), "Outside range");
        Device scanner(dir.filePath("scan-outside-client"), "Scanner");
        QVERIFY(endpoint.channel.listen(QHostAddress::LocalHost, port));
        QVERIFY(scanner.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(scanner.channel.setDiscoverySearch(true));
        QTest::qWait(2000);
        bool found = false;
        for (const auto& value : scanner.channel.hosts()) found |= value.toMap().value("id") == endpoint.channel.ownId();
        QVERIFY(!found);
        QVERIFY(scanner.channel.setDiscoverySearch(false));
    }

    void boundedDiscoverySearchRejectsMalformedResponse() {
        QTemporaryDir dir;
        QTcpServer reservation;
        quint16 port = 0;
        for (quint16 candidate = 48763; candidate <= 48783; ++candidate) {
            if (reservation.listen(QHostAddress::LocalHost, candidate)) {
                port = candidate; reservation.close(); break;
            }
        }
        QVERIFY2(port != 0, "No bounded discovery port is available.");
        const auto identity = TlsIdentity::create();
        QSslServer malformed;
        malformed.setSslConfiguration(identity.configuration());
        QObject::connect(&malformed, &QSslServer::sslErrors, &malformed,
            [](QSslSocket* socket, const QList<QSslError>& errors) { socket->ignoreSslErrors(errors); });
        QObject::connect(&malformed, &QSslServer::pendingConnectionAvailable, &malformed, [&] {
            auto* socket = qobject_cast<QSslSocket*>(malformed.nextPendingConnection());
            QObject::connect(socket, &QSslSocket::readyRead, &malformed, [socket] {
                socket->readAll();
                QByteArray frame(4, '\0'); qToBigEndian<quint32>(2 * 1024 * 1024, frame.data()); socket->write(frame);
            });
        });
        QVERIFY(malformed.listen(QHostAddress::LocalHost, port));
        Device scanner(dir.filePath("scan-malformed"), "Scanner");
        QVERIFY(scanner.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(scanner.channel.setDiscoverySearch(true));
        QTest::qWait(2000);
        bool found = false;
        for (const auto& value : scanner.channel.hosts()) found |= value.toMap().value("id") == identity.id();
        QVERIFY(!found);
        QVERIFY(scanner.channel.setDiscoverySearch(false));
    }

    void voiceUsesUdpWhileReliableTrafficIsDelayedThenFallsBackAndRecovers() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), source(dir.filePath("source"), "Source"), listener(dir.filePath("listener"), "Listener");
        QVERIFY(host.listenWithUdp());
        DelayedLink link(host.channel.port()); link.udpDelay = 0;
        QVERIFY(host.channel.decide(source.channel.ownId(), true)); QVERIFY(host.channel.decide(listener.channel.ownId(), true));
        QVERIFY(source.join(host.channel)); QVERIFY(listener.channel.join(host.channel.ownId(), "127.0.0.1", link.port()));
        QVERIFY(waitForEvents([&] { return source.channel.participants().size() == 2; })); QVERIFY(waitForEvents([&] { return listener.channel.participants().size() == 2; }));
        QVERIFY(source.session.setMuted(false));
        QVERIFY(waitForEvents([&] { return !participant(listener.channel, source.channel.ownId()).value("muted").toBool(); }));
        const auto warmup = voicePacket(), packet = voicePacket(880);
        QVERIFY(warmup != packet);
        int received = 0;
        connect(&listener.channel, &LocalChannel::audioReceived, this, [&](const QString& id, const QByteArray& bytes) {
            if (id == source.channel.ownId() && bytes == packet) ++received;
        });
        QTimer sender; sender.setInterval(20);
        connect(&sender, &QTimer::timeout, this, [&] { QVERIFY(source.channel.sendAudio(warmup)); });
        sender.start(); QVERIFY(waitForEvents([&] { return link.audioDatagrams > 5; }, 5000)); sender.stop();
        QTestEventLoop().enterLoopMSecs(150); received = 0;
        link.delay = 500; const auto before = link.audioDatagrams;
        QElapsedTimer latency; latency.start(); QVERIFY(source.channel.sendAudio(packet));
        QVERIFY(waitForEvents([&] { return received == 1; }, 250));
        QVERIFY(latency.elapsed() < 250); QVERIFY(link.audioDatagrams > before);
        const auto udpMs = latency.elapsed();
        link.blockUdp = true; QTestEventLoop().enterLoopMSecs(450); received = 0;
        latency.restart(); QVERIFY(source.channel.sendAudio(packet)); QVERIFY(waitForEvents([&] { return received == 1; }, 2000));
        QVERIFY(latency.elapsed() >= 500);
        qInfo() << "Voice during 500 ms TLS delay: UDP" << udpMs << "ms; fallback" << latency.elapsed() << "ms";
        link.blockUdp = false; link.delay = 0; const auto recovering = link.audioDatagrams;
        sender.start(); QVERIFY(waitForEvents([&] { return link.audioDatagrams > recovering + 5; }, 5000)); sender.stop();
        QVERIFY(listener.channel.joined());
        QVERIFY(host.channel.setBlocked(source.channel.ownId(), true)); QVERIFY(waitForEvents([&] { return !source.channel.joined(); }));
        QVERIFY(!source.channel.sendAudio(packet));
    }

    void legacyHostKeepsItsQualityLabelsAndDecodableAudio_data() {
        QTest::addColumn<bool>("adaptive");
        QTest::newRow("baseline") << false;
        QTest::newRow("adaptive-32") << true;
    }
    void legacyHostKeepsItsQualityLabelsAndDecodableAudio() {
        QFETCH(bool, adaptive);
        QTemporaryDir dir; Device client(dir.filePath("client"), "Listener");
        const auto identity = TlsIdentity::create();
        auto configuration = identity.configuration(); configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslServer server; server.setSslConfiguration(configuration);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QTRY_VERIFY(server.hasPendingConnections());
        auto* control = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(control);
        QTRY_VERIFY(control->bytesAvailable() > 4); control->readAll();
        QJsonObject accepted{{"type", "join"}, {"result", "accepted"}, {"adaptiveAudio", adaptive}};
        if (adaptive) accepted.insert("capabilities", QJsonArray{"adaptive-audio"});
        const QJsonObject member{{"id", identity.id()}, {"name", "Speaker"}, {"muted", false}, {"available", true}};
        control->write(wireFrame(accepted) + wireFrame({{"type", "roster"}, {"host", identity.id()}, {"members", QJsonArray{member}}}));
        QTRY_COMPARE(client.channel.participants().size(), 1);
        QCOMPARE(client.channel.receiveAudioBitrate(), 32);
        const auto packet = voicePacket();
        QSignalSpy received(&client.channel, &LocalChannel::audioReceived);
        // An older host forwards the source unchanged under its original tier label.
        control->write(wireFrame({{"type", "audio"}, {"sender", identity.id()}, {"bitrate", 32},
            {"data", QString::fromLatin1(packet.toBase64())}}));
        QTRY_COMPARE(received.size(), 1); QCOMPARE(received.first().at(1).toByteArray(), packet);
        squad::VoiceMixer playback;
        QVERIFY(playback.receive(identity.id(), received.first().at(1).toByteArray(), 0));
        const auto samples = playback.render(48000, 0);
        QVERIFY(std::any_of(samples.begin(), samples.end(), [](float sample) { return std::abs(sample) > 0.005f; }));
        QCOMPARE(client.channel.receiveAudioBitrate(), 32);
        QVERIFY(client.channel.joined());
        if (adaptive) {
            for (const int bitrate : {20, 12, 8, 12, 20, 32}) {
                control->write(wireFrame({{"type", "audioQuality"}, {"bitrate", bitrate}}));
                QTRY_COMPARE(client.channel.receiveAudioBitrate(), bitrate);
            }
        }
        QVERIFY(client.channel.joined());
    }

    void receiverQualityHonorsNegotiatedCapabilities_data() {
        QTest::addColumn<int>("generation");
        QTest::newRow("baseline") << 0;
        QTest::newRow("adaptive-32") << 1;
        QTest::newRow("adaptive-64") << 2;
        QTest::newRow("adaptive-stereo") << 3;
    }
    void receiverQualityHonorsNegotiatedCapabilities() {
        QFETCH(int, generation);
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), source(dir.filePath("source"), "Source");
        const auto identity = TlsIdentity::create();
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(source.channel.ownId(), true));
        QVERIFY(host.channel.decide(identity.id(), true));
        QVERIFY(source.join(host.channel)); QTRY_VERIFY(source.channel.joined());
        QVERIFY(source.session.setMuted(false));
        QSslSocket socket; socket.setSslConfiguration(identity.configuration());
        connect(&socket, &QSslSocket::sslErrors, &socket,
            [&socket](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
        QByteArray buffer;
        QList<QJsonObject> packets;
        connect(&socket, &QSslSocket::readyRead, &socket, [&] {
            buffer += socket.readAll();
            while (buffer.size() >= 4) {
                const auto size = qFromBigEndian<quint32>(buffer.constData());
                if (buffer.size() < size + 4) break;
                const auto message = QJsonDocument::fromJson(buffer.mid(4, size)).object();
                buffer.remove(0, size + 4);
                if (message.value("type") == "audioProbe")
                    socket.write(wireFrame({{"type", "audioAck"}, {"serial", message.value("serial")}}));
                if (message.value("type") == "audio") packets.append(message);
                if (message.value("type") == "audioQuality" && generation < 2)
                    QVERIFY(message.value("bitrate").toInt() <= 32);
            }
        });
        socket.connectToHostEncrypted("127.0.0.1", host.channel.port()); QTRY_VERIFY(socket.isEncrypted());
        QJsonObject hello{{"type", "hello"}, {"version", 1}, {"name", "Receiver"}, {"muted", false}, {"available", true}};
        if (generation) {
            hello.insert("adaptiveAudio", true);
            hello.insert("capabilities", generation == 1 ? QJsonArray{"adaptive-audio"} : generation == 2 ? QJsonArray{"adaptive-audio", "audio-64"}
                : QJsonArray{"adaptive-audio", "audio-64", "audio-stereo"});
        }
        socket.write(wireFrame(hello));
        QTRY_COMPARE(source.channel.participants().size(), 2);
        const auto original = voicePacket();
        QVERIFY(source.channel.sendAudio(original)); QTRY_COMPARE(packets.size(), 1);
        const auto received = QByteArray::fromBase64(packets.first().value("data").toString().toLatin1());
        QCOMPARE(packets.first().value("bitrate").toInt(), generation == 3 ? 128 : generation == 2 ? 64 : 32);
        if (generation == 3) QCOMPARE(received, original);
        else { QCOMPARE(received.size(), generation == 2 ? 160 : 80); QVERIFY(received != original); }
        squad::VoiceMixer decoder;
        decoder.setAutomatic(false);
        QVERIFY(decoder.receive("source", received, 0));
        const auto audible = decoder.render(48000, 0);
        QVERIFY(std::any_of(audible.begin(), audible.end(), [](float sample) { return std::abs(sample) > 0.005f; }));
        // A baseline peer can also send its lower-rate packet back to the new peer.
        QSignalSpy incoming(&source.channel, &LocalChannel::audioReceived);
        socket.write(wireFrame({{"type", "audio"}, {"data", QString::fromLatin1(received.toBase64())}}));
        QTRY_COMPARE(incoming.size(), 1);
        QCOMPARE(incoming.first().at(1).toByteArray(), received);
        QCOMPARE(socket.state(), QAbstractSocket::ConnectedState);
    }

    void receiverQualityAdaptsWithoutChangingOtherReceivers() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), source(dir.filePath("source"), "Source"),
            second(dir.filePath("second"), "Second"), healthy(dir.filePath("healthy"), "Healthy"),
            slow(dir.filePath("slow"), "Slow");
        QVERIFY(host.listenWithUdp());
        DelayedLink link(host.channel.port());
        for (auto* device : {&source, &second, &healthy, &slow}) QVERIFY(host.channel.decide(device->channel.ownId(), true));
        QVERIFY(source.join(host.channel)); QVERIFY(second.join(host.channel)); QVERIFY(healthy.join(host.channel));
        QVERIFY(slow.channel.join(host.channel.ownId(), "127.0.0.1", link.port()));
        QVERIFY(waitForEvents([&] { return slow.channel.participants().size() == 4; }));
        QVERIFY(source.session.setMuted(false)); QVERIFY(second.session.setMuted(false));
        QVERIFY(waitForEvents([&] { return !participant(healthy.channel, source.channel.ownId()).value("muted").toBool(); }));
        QSignalSpy goodAudio(&healthy.channel, &LocalChannel::audioReceived), slowAudio(&slow.channel, &LocalChannel::audioReceived);
        squad::VoiceMixer decoder(2);
        const auto packet = voicePacket();
        QCOMPARE(healthy.channel.receiveAudioBitrate(), 128); QCOMPARE(slow.channel.receiveAudioBitrate(), 128);
        // Impair an established media path, not its still-pending ICE/DTLS handshake.
        QTimer warmup; warmup.setInterval(20); int warmupFrames = 0;
        connect(&warmup, &QTimer::timeout, this, [&] { QVERIFY(source.channel.sendAudio(packet)); ++warmupFrames; });
        warmup.start(); QVERIFY(waitForEvents([&] { return link.audioDatagrams > 5; }, 5000)); warmup.stop();
        QVERIFY(waitForEvents([&] { return goodAudio.size() == warmupFrames; })); QVERIFY(waitForEvents([&] { return slowAudio.size() == warmupFrames; }));
        goodAudio.clear(); slowAudio.clear();
        qint64 playbackTime = 0;
        QVERIFY(source.channel.sendAudio(packet));
        QVERIFY(waitForEvents([&] { return goodAudio.size() == 1; })); QVERIFY(waitForEvents([&] { return slowAudio.size() == 1; }));
        QCOMPARE(goodAudio.first().at(1).toByteArray(), packet); QCOMPARE(slowAudio.first().at(1).toByteArray(), packet);
        // A single brief delay cannot lower the tier.
        link.delay = 400; QTestEventLoop().enterLoopMSecs(450); link.delay = 0; QTestEventLoop().enterLoopMSecs(500);
        QCOMPARE(slow.channel.receiveAudioBitrate(), 128);
        for (const auto bitrate : {64, 32, 20, 12, 8}) {
            link.delay = 400;
            QVERIFY(waitForEvents([&] { return slow.channel.receiveAudioBitrate() == bitrate; }, 7000));
            // Stop degrading while checking this tier's real Opus packets.
            // The ordered TLS queue drains with the packets received below.
            link.delay = 0;
            const auto goodBefore = goodAudio.size(), slowBefore = slowAudio.size();
            for (auto* speaker : {&source, &second}) QVERIFY(speaker->channel.sendAudio(packet));
            QVERIFY(waitForEvents([&] { return goodAudio.size() == goodBefore + 2; })); QVERIFY(waitForEvents([&] { return slowAudio.size() == slowBefore + 2; }));
            for (qsizetype i = goodBefore; i < goodAudio.size(); ++i) QCOMPARE(goodAudio.at(i).at(1).toByteArray(), packet);
            for (qsizetype i = slowBefore; i < slowAudio.size(); ++i) {
                const auto compressed = slowAudio.at(i).at(1).toByteArray();
                QCOMPARE(compressed.size(), bitrate * 1000 / 8 / 50);
                QVERIFY(compressed != packet);
                QCOMPARE(opus_packet_get_nb_channels(reinterpret_cast<const unsigned char*>(compressed.constData())), bitrate >= 64 ? 2 : 1);
                playbackTime += 20;
                QVERIFY(decoder.receive(slowAudio.at(i).at(0).toString(), compressed, playbackTime));
                const auto audible = decoder.render(48000, playbackTime);
                QVERIFY(std::any_of(audible.begin(), audible.end(), [](float sample) { return std::abs(sample) > 0.005f; }));
            }
            QCOMPARE(healthy.channel.receiveAudioBitrate(), 128);
        }
        QVERIFY(slow.session.setMuted(false));
        QVERIFY(waitForEvents([&] { return !participant(healthy.channel, slow.channel.ownId()).value("muted").toBool(); }));
        const auto beforeUplink = goodAudio.size();
        QVERIFY(slow.channel.sendAudio(packet));
        QVERIFY(waitForEvents([&] { return goodAudio.size() == beforeUplink + 1; }));
        QCOMPARE(goodAudio.last().at(1).toByteArray(), packet);
        link.delay = 0;
        QTestEventLoop().enterLoopMSecs(2500); QCOMPARE(slow.channel.receiveAudioBitrate(), 8);
        for (const auto bitrate : {12, 20, 32, 64, 128}) QVERIFY(waitForEvents([&] { return slow.channel.receiveAudioBitrate() == bitrate; }, 16000));
        const auto beforeRecovery = slowAudio.size();
        QVERIFY(source.channel.sendAudio(packet));
        QVERIFY(waitForEvents([&] { return slowAudio.size() == beforeRecovery + 1; })); QCOMPARE(slowAudio.last().at(1).toByteArray(), packet);
        QCOMPARE(healthy.channel.receiveAudioBitrate(), 128);
        QVERIFY(slow.channel.leave()); QCOMPARE(slow.channel.receiveAudioBitrate(), 0);
        QVERIFY(slow.channel.join(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QVERIFY(waitForEvents([&] { return slow.channel.joined(); })); QCOMPARE(slow.channel.receiveAudioBitrate(), 128);
    }
    void localPreferencesDoNotRepublishUnchangedPresence() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), speaker(dir.filePath("speaker"), "Speaker"), listener(dir.filePath("listener"), "Listener");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(speaker.channel.ownId(), true));
        QVERIFY(host.channel.decide(listener.channel.ownId(), true));
        QVERIFY(host.join(host.channel)); QVERIFY(speaker.join(host.channel)); QVERIFY(listener.join(host.channel));
        QTRY_COMPARE(listener.channel.participants().size(), 3);
        QVERIFY(speaker.session.setMuted(false));
        QTRY_COMPARE(participant(listener.channel, speaker.channel.ownId()).value("muted").toBool(), false);
        QSignalSpy changed(&listener.channel, &LocalChannel::participantsChanged);
        QVERIFY(host.session.setTheme("dark"));
        QVERIFY(speaker.session.setTheme("light"));
        QVERIFY(speaker.session.setPushToTalk(true));
        QVERIFY(speaker.session.setPttKeyHeld(true));
        QVERIFY(!changed.wait(150));
        QCOMPARE(changed.size(), 0);
        QVERIFY(speaker.session.transmissionAllowed());
        QVERIFY(speaker.session.setMuted(true));
        QTRY_COMPARE(participant(listener.channel, speaker.channel.ownId()).value("muted").toBool(), true);
        QVERIFY(speaker.session.setUserName("Renamed"));
        QTRY_COMPARE(participant(listener.channel, speaker.channel.ownId()).value("name").toString(), "Renamed");
        QVERIFY(speaker.session.setAvatar("mechanic"));
        QTRY_COMPARE(participant(listener.channel, speaker.channel.ownId()).value("avatarId").toString(), "mechanic");
        const auto port = host.channel.port();
        QVERIFY(host.channel.stopHost());
        QTRY_VERIFY(!listener.channel.joined());
        QVERIFY(host.channel.listen(QHostAddress::LocalHost, port));
        QTRY_COMPARE(listener.channel.participants().size(), 3);
        QCOMPARE(participant(listener.channel, speaker.channel.ownId()).value("name").toString(), "Renamed");
        QCOMPARE(participant(listener.channel, speaker.channel.ownId()).value("avatarId").toString(), "mechanic");
        QVERIFY(participant(listener.channel, speaker.channel.ownId()).value("muted").toBool());
    }

    void presenceWireSendsOnlyChangedStateAndResendsAfterReconnect() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        QSslServer server; server.setSslConfiguration(identity.configuration());
        connect(&server, &QSslServer::sslErrors, &server,
            [](QSslSocket* socket, const QList<QSslError>& errors) { socket->ignoreSslErrors(errors); });
        QList<QJsonObject> presence;
        connect(&server, &QSslServer::pendingConnectionAvailable, &server, [&] {
            auto* socket = qobject_cast<QSslSocket*>(server.nextPendingConnection());
            const auto buffer = std::make_shared<QByteArray>();
            const auto read = [socket, &presence, buffer] {
                *buffer += socket->readAll();
                while (buffer->size() >= 4) {
                    const auto size = qFromBigEndian<quint32>(buffer->constData());
                    if (buffer->size() < size + 4) return;
                    const auto message = QJsonDocument::fromJson(buffer->mid(4, size)).object();
                    buffer->remove(0, size + 4);
                    if (message.value("type") == "hello") socket->write(wireFrame({{"type", "join"}, {"result", "accepted"}}));
                    else if (message.value("type") == "presence") presence.append(message);
                }
            };
            connect(socket, &QSslSocket::readyRead, socket, read);
            read();
        });
        QVERIFY(server.listen(QHostAddress::LocalHost));
        Device client(dir.filePath("client"), "Speaker");
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QTRY_COMPARE(presence.size(), 1);
        QVERIFY(client.session.setTheme("dark"));
        QVERIFY(client.session.setPushToTalk(true));
        QVERIFY(client.session.setPttKeyHeld(true));
        QVERIFY(client.session.setUserName("Renamed"));
        QTRY_VERIFY(!presence.isEmpty() && presence.last().value("name") == "Renamed");
        QCOMPARE(presence.size(), 2);
        QVERIFY(client.channel.leave());
        QVERIFY(client.channel.closeChat(identity.id()));
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QTRY_COMPARE(presence.size(), 3);
        QCOMPARE(presence.last().value("name").toString(), "Renamed");
    }

    void profileLanguageReachesListenersAndSurvivesReconnect() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), speaker(dir.filePath("speaker"), "Speaker"), listener(dir.filePath("listener"), "Listener");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.session.setLanguage("fr"));
        QVERIFY(speaker.session.setLanguage("de"));
        QVERIFY(host.channel.decide(speaker.channel.ownId(), true));
        QVERIFY(host.channel.decide(listener.channel.ownId(), true));
        QVERIFY(host.join(host.channel));
        QVERIFY(speaker.join(host.channel));
        QVERIFY(listener.join(host.channel));
        QTRY_COMPARE(listener.channel.participants().size(), 3);
        QCOMPARE(participant(listener.channel, host.channel.ownId()).value("language").toString(), "fr");
        QCOMPARE(participant(listener.channel, speaker.channel.ownId()).value("language").toString(), "de");
        QVERIFY(host.session.setLanguage("es"));
        QVERIFY(speaker.session.setLanguage("en"));
        QTRY_COMPARE(participant(listener.channel, host.channel.ownId()).value("language").toString(), "es");
        QTRY_COMPARE(participant(listener.channel, speaker.channel.ownId()).value("language").toString(), "en");
        QVERIFY(speaker.channel.leave());
        QTRY_COMPARE(listener.channel.participants().size(), 2);
        QVERIFY(speaker.join(host.channel));
        QTRY_COMPARE(listener.channel.participants().size(), 3);
        QCOMPARE(participant(listener.channel, speaker.channel.ownId()).value("language").toString(), "en");
    }
    void profileChangesUpdateEarlierMessagesForTextClientsAndSurviveRestart() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        auto host = std::make_unique<Device>(dir.filePath("host"), "Host", identity);
        Device writer(dir.filePath("writer"), "Before"), reader(dir.filePath("reader"), "Reader");
        QVERIFY(host->channel.listen(QHostAddress::LocalHost));
        const auto port = host->channel.port();
        QVERIFY(host->channel.decide(writer.channel.ownId(), true));
        QVERIFY(host->channel.decide(reader.channel.ownId(), true));
        QVERIFY(writer.channel.openChat(host->channel.ownId(), "127.0.0.1", port));
        QVERIFY(reader.join(host->channel));
        QTRY_VERIFY(writer.channel.chatReady() && reader.channel.chatReady());
        QVERIFY(writer.channel.sendChat("The message stays the same."));
        QTRY_COMPARE(memberMessages(reader.channel).size(), 1);
        const auto before = memberMessages(reader.channel).first().toMap();
        QVERIFY(writer.session.setUserName("After"));
        QVERIFY(writer.session.setAvatar("mechanic"));
        QTRY_COMPARE(memberMessages(reader.channel).first().toMap().value("name").toString(), QString("After"));
        QTRY_COMPARE(memberMessages(reader.channel).first().toMap().value("avatarId").toString(), QString("mechanic"));
        const auto after = memberMessages(reader.channel).first().toMap();
        for (const auto* key : {"sender", "sequence", "request", "created", "expires", "text"}) QCOMPARE(after.value(key), before.value(key));
        QVERIFY(writer.channel.closeChat(host->channel.ownId()));
        host.reset(); host = std::make_unique<Device>(dir.filePath("host"), "Host", identity);
        QVERIFY(host->channel.listen(QHostAddress::LocalHost, port));
        QTRY_VERIFY_WITH_TIMEOUT(reader.channel.chatReady(), 5000);
        QTRY_COMPARE(memberMessages(reader.channel).size(), 1);
        QCOMPARE(memberMessages(reader.channel).first().toMap().value("name").toString(), QString("After"));
        QCOMPARE(memberMessages(reader.channel).first().toMap().value("avatarId").toString(), QString("mechanic"));
    }
    void initTestCase() { qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1"); qInfo() << QSslSocket::activeBackend() << QSslSocket::availableBackends(); QVERIFY2(QSslSocket::supportsSsl(), qPrintable(QSslSocket::sslLibraryVersionString())); }
    void futureAvatarUsesBaselineOnLegacyHelloBoundary() {
        QTemporaryDir dir;
        QFile profile(dir.filePath("writer.session")); QVERIFY(profile.open(QIODevice::WriteOnly));
        profile.write(R"({"version":1,"userName":"Writer","channelName":"Channel","muted":true,"avatar":"future-dragon-v2"})");
        profile.close();
        const auto identity = TlsIdentity::create();
        QSslServer legacy; legacy.setSslConfiguration(identity.configuration());
        QObject::connect(&legacy, &QSslServer::sslErrors, &legacy,
            [](QSslSocket* socket, const QList<QSslError>& errors) { socket->ignoreSslErrors(errors); });
        QByteArray buffer;
        QJsonObject hello;
        QObject::connect(&legacy, &QSslServer::pendingConnectionAvailable, &legacy, [&] {
            auto* socket = qobject_cast<QSslSocket*>(legacy.nextPendingConnection());
            const auto read = [&, socket] {
                buffer += socket->readAll();
                if (buffer.size() < 4) return;
                const auto length = qFromBigEndian<quint32>(buffer.constData());
                if (length > 65536 || buffer.size() < 4 + length) return;
                hello = QJsonDocument::fromJson(buffer.mid(4, length)).object();
            };
            QObject::connect(socket, &QSslSocket::readyRead, &legacy, read);
            read();
        });
        QVERIFY(legacy.listen(QHostAddress::LocalHost));
        Device writer(dir.filePath("writer"), "Writer");
        QVERIFY(writer.channel.openChat(identity.id(), "127.0.0.1", legacy.serverPort()));
        QTRY_VERIFY(!hello.isEmpty());
        QCOMPARE(hello.value("type").toString(), QString("hello"));
        QVERIFY(QStringList({"mossling", "courier", "mechanic"}).contains(hello.value("avatar").toString()));
        QCOMPARE(hello.value("avatarId").toString(), QString("future-dragon-v2"));
        QCOMPARE(hello.value("avatar").toString(), VoiceSession::avatarFallback("future-dragon-v2"));
    }
    void futureAvatarsAndOfflineSendersSurviveHostRestart() {
        QTemporaryDir dir;
        const auto hostIdentity = TlsIdentity::create();
        auto host = std::make_unique<Device>(dir.filePath("host"), "Host", hostIdentity);
        QFile profile(dir.filePath("writer.session")); QVERIFY(profile.open(QIODevice::WriteOnly));
        profile.write(R"({"version":1,"userName":"Writer","channelName":"Channel","muted":true,"avatar":"future-dragon-v2"})");
        profile.close();
        Device writer(dir.filePath("writer"), "Writer"), reader(dir.filePath("reader"), "Reader");
        QVERIFY(host->channel.listen(QHostAddress::LocalHost));
        const auto port = host->channel.port();
        QVERIFY(host->channel.decide(writer.channel.ownId(), true));
        QVERIFY(host->channel.decide(reader.channel.ownId(), true));
        QVERIFY(writer.join(host->channel)); QVERIFY(reader.join(host->channel));
        QTRY_VERIFY(writer.channel.chatReady() && reader.channel.chatReady());
        QTRY_VERIFY(reader.channel.chatPresenceKnown());
        QTRY_VERIFY(reader.channel.chatOnlineIds().contains(writer.channel.ownId()));
        QTRY_COMPARE(participant(reader.channel, writer.channel.ownId()).value("avatarId").toString(), QString("future-dragon-v2"));
        QVERIFY(writer.channel.sendChat("My portrait outlives this connection."));
        QTRY_COMPARE(memberMessages(reader.channel).size(), 1);
        QCOMPARE(memberMessages(reader.channel).first().toMap().value("avatarId").toString(), QString("future-dragon-v2"));
        QCOMPARE(memberMessages(reader.channel).first().toMap().value("avatar").toString(), VoiceSession::avatarFallback("future-dragon-v2"));
        QVERIFY(writer.channel.leave());
        QTRY_COMPARE(reader.channel.participants().size(), 1);
        QVERIFY(reader.channel.chatOnlineIds().contains(writer.channel.ownId())); // Text access is still online.
        QVERIFY(writer.channel.closeChat(host->channel.ownId()));
        QTRY_VERIFY(!reader.channel.chatOnlineIds().contains(writer.channel.ownId()));
        QCOMPARE(memberMessages(reader.channel).size(), 1);
        host.reset();
        host = std::make_unique<Device>(dir.filePath("host"), "Host", hostIdentity);
        QVERIFY(host->channel.listen(QHostAddress::LocalHost, port));
        QTRY_VERIFY_WITH_TIMEOUT(reader.channel.chatReady(), 5000);
        QTRY_COMPARE(memberMessages(reader.channel).size(), 1);
        QTRY_VERIFY(reader.channel.chatPresenceKnown());
        QVERIFY(!reader.channel.chatOnlineIds().contains(writer.channel.ownId()));
        QCOMPARE(memberMessages(reader.channel).first().toMap().value("avatarId").toString(), QString("future-dragon-v2"));
        QVERIFY(host->channel.setBlocked(reader.channel.ownId(), true));
        QTRY_VERIFY(!reader.channel.chatReady());
        QVERIFY(reader.channel.chatOnlineIds().isEmpty()); QVERIFY(!reader.channel.chatPresenceKnown());
    }
    void historyOpensAtTheLatestBoundedPage() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        const auto now = QDateTime::currentMSecsSinceEpoch();
        ChatHistory history(dir.filePath("host.channel.chat"), identity, now);
        for (int i = 1; i <= 200; ++i)
            history.append(identity.id(), "Earlier member", QUuid::createUuid().toString(QUuid::WithoutBraces),
                QString("Message %1").arg(i), now);
        Device host(dir.filePath("host"), "Host", identity), client(dir.filePath("client"), "Reader");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(!client.channel.messages().isEmpty());
        QTRY_COMPARE(client.channel.messages().last().toMap().value("sequence").toInt(), 200);
        QVERIFY2(client.channel.messages().size() <= 40, "Opening chat must not download all retained history.");
        while (client.channel.hasOlderMessages()) {
            QVERIFY(client.channel.loadOlderMessages()); QTRY_VERIFY(!client.channel.historyLoading());
            QVERIFY(client.channel.messages().size() <= 160);
        }
        QCOMPARE(client.channel.messages().first().toMap().value("sequence").toInt(), 1);
        QCOMPARE(client.channel.messages().size(), 160);
        QVERIFY(client.channel.hasNewerMessages());
    }
    void historyScrollsBothWaysWithinByteAndMessageLimits() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        qint64 now = 1800000000000;
        ChatHistory history(dir.filePath("host.channel.chat"), identity, now);
        for (int i = 1; i <= 180; ++i)
            history.append(identity.id(), "Writer", QUuid::createUuid().toString(QUuid::WithoutBraces),
                QString::number(i) + QString(4000, 'x'), now, {}, "courier");
        Device host(dir.filePath("host"), "Host", identity, [&] { return now; });
        Device reader(dir.filePath("reader"), "Reader");
        QVERIFY(reader.session.setAvatar("courier"));
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(reader.channel.ownId(), true));
        QVERIFY(reader.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(reader.channel.chatReady() && !reader.channel.historyLoading());
        QVERIFY(!reader.channel.joined()); QVERIFY(reader.channel.hasOlderMessages());
        QVERIFY(reader.channel.messages().size() <= 10);
        QSet<int> seen;
        auto inspect = [&] {
            const auto records = reader.channel.messages();
            if (records.size() > 160 || QJsonDocument(QJsonArray::fromVariantList(records)).toJson(QJsonDocument::Compact).size() > 512 * 1024) return false;
            int previous = 0;
            for (const auto& value : records) {
                const auto entry = value.toMap(); const auto sequence = entry.value("sequence").toInt();
                if (previous && sequence != previous + 1) return false;
                if (entry.value("avatar").toString() != "courier") return false;
                previous = sequence; seen.insert(sequence);
            }
            return true;
        };
        QVERIFY(inspect());
        while (reader.channel.hasOlderMessages()) {
            QVERIFY(reader.channel.loadOlderMessages());
            QVERIFY(!reader.channel.loadOlderMessages());
            QTRY_VERIFY(!reader.channel.historyLoading());
            QVERIFY(inspect());
        }
        QCOMPARE(seen.size(), 180); QVERIFY(reader.channel.hasNewerMessages());
        QCOMPARE(reader.channel.messages().first().toMap().value("sequence").toInt(), 1);
        const auto beforeLiveMessage = reader.channel.messages();
        QVERIFY(reader.channel.sendChat("A new message while reading old pages")); QTRY_VERIFY(!reader.channel.chatPending());
        QCOMPARE(reader.channel.messages(), beforeLiveMessage);
        while (reader.channel.hasNewerMessages()) {
            QVERIFY(reader.channel.loadNewerMessages()); QTRY_VERIFY(!reader.channel.historyLoading()); QVERIFY(inspect());
        }
        QCOMPARE(reader.channel.messages().last().toMap().value("sequence").toInt(), 181);
        now += ChatHistory::lifetime;
        QTRY_VERIFY(reader.channel.messages().isEmpty());
        QVERIFY(reader.channel.refreshChat()); QTRY_VERIFY(!reader.channel.historyLoading());
        QVERIFY(reader.channel.messages().isEmpty()); QVERIFY(!reader.channel.hasOlderMessages());
    }
    void textAccessToAnotherHostDoesNotMoveVoiceOrExposePendingMembers() {
        QTemporaryDir dir;
        Device a(dir.filePath("a"), "Host A"), b(dir.filePath("b"), "Host B");
        Device reader(dir.filePath("reader"), "Reader"), voiceA(dir.filePath("voice-a"), "Voice A"), voiceB(dir.filePath("voice-b"), "Voice B");
        QVERIFY(a.channel.listen(QHostAddress::LocalHost)); QVERIFY(b.channel.listen(QHostAddress::LocalHost));
        QVERIFY(a.channel.decide(reader.channel.ownId(), true)); QVERIFY(a.channel.decide(voiceA.channel.ownId(), true));
        QVERIFY(b.channel.decide(voiceB.channel.ownId(), true));
        QVERIFY(reader.join(a.channel)); QVERIFY(voiceA.join(a.channel)); QVERIFY(voiceB.join(b.channel));
        QTRY_VERIFY(reader.channel.chatReady() && voiceA.channel.chatReady() && voiceB.channel.chatReady());
        QVERIFY(voiceB.channel.sendChat("Visible only after approval")); QTRY_VERIFY(!voiceB.channel.chatPending());
        QVERIFY(reader.channel.openChat(b.channel.ownId(), "127.0.0.1", b.channel.port()));
        QTRY_COMPARE(b.channel.requests().size(), 1);
        QTRY_VERIFY(reader.channel.status().contains("Waiting for host approval"));
        QVERIFY(reader.channel.joined()); QCOMPARE(reader.channel.joinedHostId(), a.channel.ownId());
        QVERIFY(!reader.channel.chatReady()); QVERIFY(reader.channel.chatMembers().isEmpty()); QVERIFY(memberMessages(reader.channel).isEmpty());
        for (const auto& value : reader.channel.savedChannels()) {
            const auto entry = value.toMap();
            if (entry.value("id").toString() != b.channel.ownId()) continue;
            QCOMPARE(entry.value("access").toString(), QString("pending")); QVERIFY(entry.value("members").toList().isEmpty());
        }
        QVERIFY(b.channel.decide(reader.channel.ownId(), true));
        QTRY_VERIFY(reader.channel.chatReady() && !reader.channel.historyLoading());
        QTRY_COMPARE(reader.channel.chatMembers().size(), 1);
        QCOMPARE(b.channel.hostParticipants().size(), 1);
        QCOMPARE(b.channel.hostClients().size(), 2);
        QCOMPARE(memberMessages(reader.channel).size(), 1);
        QVERIFY(reader.channel.sendChat("Text in B, voice in A")); QTRY_VERIFY(!reader.channel.chatPending());
        QTRY_COMPARE(memberMessages(voiceB.channel).size(), 2); QVERIFY(memberMessages(voiceA.channel).isEmpty());
        QVERIFY(reader.session.setMuted(false)); QVERIFY(voiceA.session.setMuted(false)); QVERIFY(voiceB.session.setMuted(false));
        QTRY_VERIFY(!participant(voiceA.channel, reader.channel.ownId()).value("muted").toBool());
        QSignalSpy atA(&voiceA.channel, &LocalChannel::audioReceived), atB(&voiceB.channel, &LocalChannel::audioReceived), atReader(&reader.channel, &LocalChannel::audioReceived);
        QVERIFY(reader.channel.sendAudio(voicePacket())); QTRY_COMPARE(atA.size(), 1); QCOMPARE(atB.size(), 0);
        QCOMPARE(atReader.size(), 0);
        QVERIFY(voiceA.channel.sendAudio(voicePacket())); QTRY_COMPARE(atReader.size(), 1);
        QVERIFY(voiceB.channel.sendAudio(voicePacket())); QTest::qWait(100); QCOMPARE(atReader.size(), 1);
        QVERIFY(b.channel.setBlocked(reader.channel.ownId(), true));
        QTRY_VERIFY(!reader.channel.chatReady()); QVERIFY(memberMessages(reader.channel).isEmpty()); QVERIFY(reader.channel.chatMembers().isEmpty());
        QVERIFY(reader.channel.joined()); QCOMPARE(reader.channel.joinedHostId(), a.channel.ownId());
        QVERIFY(reader.channel.openSavedChat(a.channel.ownId())); QTRY_VERIFY(reader.channel.chatReady());
        QVERIFY(reader.channel.sendChat("Still here")); QTRY_COMPARE(memberMessages(voiceA.channel).size(), 1);
        QVERIFY(reader.channel.leave()); QVERIFY(!reader.channel.joined());
        QVERIFY(reader.channel.chatReady());
        QTRY_COMPARE(a.channel.hostParticipants().size(), 1);
        QVERIFY(!reader.channel.sendAudio("must not leave the device"));
        QVERIFY(reader.channel.sendChat("Chat stays open after leaving voice")); QTRY_COMPARE(memberMessages(voiceA.channel).size(), 2);
        QCOMPARE(reader.channel.chatHostId(), a.channel.ownId());
    }
    void textOnlyAccessUsesPasswordsAndReconnectsWithoutJoiningVoice() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), reader(dir.filePath("reader"), "Reader");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        const auto port = host.channel.port();
        QVERIFY(host.channel.decide(reader.channel.ownId(), true));
        QVERIFY(host.channel.setHostPassword("text access secret")); QTRY_VERIFY(!host.channel.passwordBusy());
        QVERIFY(reader.channel.openAddress("127.0.0.1:" + QString::number(port)));
        QTRY_VERIFY(reader.channel.passwordRequired());
        QVERIFY(memberMessages(reader.channel).isEmpty()); QVERIFY(reader.channel.chatMembers().isEmpty());
        QVERIFY(!reader.channel.joined());
        QVERIFY(reader.channel.submitPassword("text access secret", true)); QTRY_VERIFY(reader.channel.chatReady());
        QVERIFY(!reader.channel.joined()); QVERIFY(host.channel.hostParticipants().isEmpty());
        QVERIFY(reader.channel.sendChat("A text-only message")); QTRY_VERIFY(!reader.channel.chatPending());
        QVERIFY(host.channel.stopHost()); QTRY_VERIFY(!reader.channel.chatReady());
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QTRY_VERIFY_WITH_TIMEOUT(reader.channel.chatReady(), 8000);
        QTRY_VERIFY(!reader.channel.historyLoading());
        QCOMPARE(memberMessages(reader.channel).size(), 1); QVERIFY(!reader.channel.joined());
        QVERIFY(reader.channel.removeChannel(host.channel.ownId()));
        QVERIFY(reader.channel.savedChannels().isEmpty()); QVERIFY(!reader.channel.chatReady());
    }
    void finalHostDecisionClosesClientWithoutReconnect_data() {
        QTest::addColumn<QString>("result"); QTest::addColumn<QString>("status"); QTest::addColumn<int>("extensions");
        for (const int count : {4, 20}) {
            const auto suffix = count == 4 ? QByteArray{} : QByteArray("-buffered");
            QTest::newRow(("kick" + suffix).constData()) << QString("kicked") << QString("removed") << count;
            QTest::newRow(("ban" + suffix).constData()) << QString("blocked") << QString("blocked") << count;
            QTest::newRow(("replacement" + suffix).constData()) << QString("replaced") << QString("newer connection") << count;
            QTest::newRow(("capacity" + suffix).constData()) << QString("busy") << QString("busy") << count;
        }
    }
    void finalHostDecisionClosesClientWithoutReconnect() {
        QFETCH(QString, result); QFETCH(QString, status); QFETCH(int, extensions);
        QStringList trace;
        QTemporaryDir dir; Device client(dir.filePath("client"), "Member");
        const auto identity = TlsIdentity::create();
        auto configuration = identity.configuration(); configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslServer server; server.setSslConfiguration(configuration);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QTRY_VERIFY(server.hasPendingConnections());
        auto* socket = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(socket);
        QTRY_VERIFY(socket->bytesAvailable() > 4); socket->readAll();
        socket->write(wireFrame({{"type", "join"}, {"result", "accepted"}}));
        QTRY_VERIFY(client.channel.joined());
        // Keep socket-event evidence on failure without changing delivery or timing.
        const auto watch = [&trace](QSslSocket* peer, const QString& side) {
            const auto record = [&trace, peer, side](const QString& event) {
                trace.append(QString("%1 %2 state=%3 open=%4 encrypted=%5 available=%6 queued=%7/%8 error=%9")
                    .arg(side, event).arg(peer->state()).arg(peer->isOpen()).arg(peer->isEncrypted())
                    .arg(peer->bytesAvailable()).arg(peer->bytesToWrite()).arg(peer->encryptedBytesToWrite()).arg(peer->error()));
            };
            QObject::connect(peer, &QSslSocket::readyRead, peer, [record] { record("read"); });
            QObject::connect(peer, &QSslSocket::stateChanged, peer, [record] { record("state"); });
            QObject::connect(peer, &QSslSocket::errorOccurred, peer, [record] { record("error"); });
            QObject::connect(peer, &QSslSocket::disconnected, peer, [record] { record("disconnected"); });
            QObject::connect(peer, &QSslSocket::encryptedBytesWritten, peer, [record](qint64 bytes) { record(QString("wrote=%1").arg(bytes)); });
            record("watch");
        };
        watch(socket, "host");
        for (auto* peer : client.channel.findChildren<QSslSocket*>(QString{}, Qt::FindDirectChildrenOnly)) watch(peer, "client");
        // Several frames and TLS records precede the final decision and EOF.
        // Unknown additive extensions must not conceal the supported result.
        const auto extension = wireFrame({{"type", "futureExtension"}, {"data", QString(8192, 'x')}});
        QByteArray frames;
        for (int i = 0; i < extensions; ++i) frames += extension;
        frames += wireFrame({{"type", "join"}, {"result", result}});
        QCOMPARE(socket->write(frames), qint64(frames.size()));
        QTRY_VERIFY(!client.channel.joined());
        QVERIFY2(client.channel.status().contains(status), qPrintable(client.channel.status() + "\n" + trace.join('\n')));
        QTRY_COMPARE(socket->state(), QAbstractSocket::UnconnectedState);
        // The normal reconnect delay is two seconds; a final decision stops it.
        QTest::qWait(2500);
        QVERIFY(!server.hasPendingConnections()); QVERIFY(!client.channel.joined());
        QVERIFY2(client.channel.status().contains(status), qPrintable(client.channel.status()));
    }
    void finalHostDecisionRevokesImmediatelyAndBoundsLegacyConnections_data() {
        QTest::addColumn<bool>("control");
        QTest::newRow("kick") << false;
        QTest::newRow("unavailable-control") << true;
    }
    void finalHostDecisionRevokesImmediatelyAndBoundsLegacyConnections() {
        QFETCH(bool, control);
        QTemporaryDir dir; Device host(dir.filePath("host"), "Owner"), witness(dir.filePath("witness"), "Listener");
        const auto identity = TlsIdentity::create();
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        if (control) QVERIFY(host.channel.initialize(true));
        QVERIFY(host.channel.decide(identity.id(), true));
        QVERIFY(host.channel.decide(witness.channel.ownId(), true));
        QVERIFY(witness.join(host.channel)); QTRY_VERIFY(witness.channel.joined());
        QByteArray received;
        QSslSocket socket; socket.setSslConfiguration(identity.configuration());
        connect(&socket, &QSslSocket::sslErrors, &socket,
            [&socket](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
        connect(&socket, &QSslSocket::readyRead, &socket, [&] { received += socket.readAll(); });
        socket.connectToHostEncrypted("127.0.0.1", host.channel.port());
        QTRY_VERIFY(socket.isEncrypted());
        const auto hello = wireFrame({{"type", "hello"}, {"version", 1}, {"name", "Legacy member"},
            {"available", true}, {"muted", true}});
        socket.write(hello);
        QTRY_COMPARE(host.channel.hostParticipants().size(), 2);
        QTRY_COMPARE(witness.channel.participants().size(), 2);
        QSignalSpy events(&witness.channel, &LocalChannel::channelEvent);
        if (control) socket.write(wireFrame({{"type", "controlHello"}}));
        else QVERIFY(host.channel.kick(identity.id()));
        QTRY_COMPARE(host.channel.hostParticipants().size(), 1);
        QTRY_VERIFY(received.contains(control ? "\"result\":\"unavailable\"" : "\"result\":\"kicked\""));
        QTRY_COMPARE(witness.channel.participants().size(), 1);
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(events.first().first().toString(), control ? QString("memberLeave") : QString("kick"));
        QCOMPARE(socket.state(), QAbstractSocket::ConnectedState);
        // A legacy client can keep its socket open, but cannot regain access,
        // change the roster or prolong closure with traffic or a new approval.
        received.clear();
        QVERIFY(host.channel.decide(identity.id(), true));
        QTimer traffic; traffic.setInterval(100);
        connect(&traffic, &QTimer::timeout, &socket, [&] {
            if (socket.state() == QAbstractSocket::ConnectedState)
                socket.write(hello + wireFrame({{"type", "ping"}}));
        });
        traffic.start();
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::UnconnectedState, 10000);
        QCOMPARE(host.channel.hostParticipants().size(), 1);
        QCOMPARE(events.size(), 1);
        QVERIFY(received.isEmpty());
        // The retained device approval is usable by a fresh connection.
        Device next(dir.filePath("next"), "Member", identity);
        QVERIFY(next.join(host.channel)); QTRY_VERIFY(next.channel.joined());
    }
    void ownerCanKickBanAndUnblockWithoutJoining() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        auto host = std::make_unique<Device>(dir.filePath("host"), "Owner", identity);
        Device a(dir.filePath("a"), "Member A"), b(dir.filePath("b"), "Member B");
        QVERIFY(host->channel.listen(QHostAddress::LocalHost));
        QVERIFY(host->channel.decide(a.channel.ownId(), true));
        QVERIFY(host->channel.decide(b.channel.ownId(), true));
        QVERIFY(a.join(host->channel)); QVERIFY(b.join(host->channel));
        QTRY_VERIFY(a.channel.chatReady() && b.channel.chatReady());
        QTRY_COMPARE(host->channel.hostParticipants().size(), 2);
        QVERIFY(!host->channel.joined());
        QVERIFY(!host->channel.kick(host->channel.ownId()));
        QStringList states;
        QObject trace;
        connect(&a.channel, &LocalChannel::stateChanged, &trace, [&] { states.append(a.channel.status()); });
        for (int attempt = 0; attempt < 12; ++attempt) {
            states.clear();
            QVERIFY(host->channel.kick(a.channel.ownId()));
            QTRY_VERIFY(!a.channel.joined());
            QVERIFY2(a.channel.status().contains("removed"), qPrintable(states.join(" | ")));
            QTRY_COMPARE(host->channel.hostParticipants().size(), 1);
            QVERIFY(a.join(host->channel)); QTRY_VERIFY(a.channel.chatReady());
        }
        QVERIFY(host->channel.setBlocked(a.channel.ownId(), true));
        QTRY_VERIFY(!a.channel.joined());
        QCOMPARE(host->channel.blockedClients().size(), 1);
        QVERIFY(!host->channel.decide(a.channel.ownId(), true));
        QVERIFY(a.join(host->channel)); QTRY_VERIFY(a.channel.status().contains("blocked"));
        QVERIFY(!a.channel.joined()); QVERIFY(host->channel.requests().isEmpty());
        QVERIFY(b.channel.sendChat("still connected")); QTRY_VERIFY(!b.channel.chatPending());
        host.reset();
        host = std::make_unique<Device>(dir.filePath("host"), "Owner", identity);
        QVERIFY(host->channel.listen(QHostAddress::LocalHost));
        QCOMPARE(host->channel.blockedClients().size(), 1);
        QVERIFY(a.join(host->channel)); QTRY_VERIFY(a.channel.status().contains("blocked"));
        QVERIFY(host->channel.requests().isEmpty());
        QVERIFY(host->channel.setBlocked(a.channel.ownId(), false));
        QVERIFY(host->channel.blockedClients().isEmpty());
        QVERIFY(a.join(host->channel)); QTRY_VERIFY(a.channel.chatReady());
    }

    void failedBanPersistenceKeepsExistingAccess() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), client(dir.filePath("client"), "Member");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.join(host.channel)); QTRY_VERIFY(client.channel.chatReady());
        const auto path = dir.filePath("host.channel"), backup = path + ".backup";
        QVERIFY(QFile::rename(path, backup)); QVERIFY(QDir().mkdir(path));
        QVERIFY(!host.channel.setBlocked(client.channel.ownId(), true));
        QVERIFY(host.channel.blockedClients().isEmpty()); QVERIFY(client.channel.joined());
        QVERIFY(QDir().rmdir(path)); QVERIFY(QFile::rename(backup, path));
        QVERIFY(host.channel.setBlocked(client.channel.ownId(), true)); QTRY_VERIFY(!client.channel.joined());
        QVERIFY(QFile::rename(path, backup)); QVERIFY(QDir().mkdir(path));
        QVERIFY(!host.channel.setBlocked(client.channel.ownId(), false));
        QCOMPARE(host.channel.blockedClients().size(), 1);
        QVERIFY(QDir().rmdir(path)); QVERIFY(QFile::rename(backup, path));
        QVERIFY(!host.channel.setBlocked("invalid", true));
        QVERIFY(!host.channel.setBlocked(host.channel.ownId(), true));
        QVERIFY(!host.channel.kick(QString(64, 'a')));
    }

    void shareAddressesUseTheListeningInterfaceAndPort() {
        QTemporaryDir dir;
        Device loopback(dir.filePath("loopback"), "Local");
        QVERIFY(loopback.channel.listen(QHostAddress::LocalHost));
        QCOMPARE(loopback.channel.channelAddresses(loopback.channel.ownId()),
            QStringList{"127.0.0.1:" + QString::number(loopback.channel.port())});
        QVERIFY(loopback.channel.channelAddresses(QString(64, 'f')).isEmpty());

        Device host(dir.filePath("host"), "Host");
        QVERIFY(host.channel.listen(QHostAddress::AnyIPv4));
        const auto endpoints = host.channel.channelAddresses(host.channel.ownId());
        QSet<QString> unique;
        for (const auto& endpoint : endpoints) {
            QVERIFY(!unique.contains(endpoint)); unique.insert(endpoint);
            const auto separator = endpoint.lastIndexOf(':');
            const QHostAddress address(endpoint.first(separator));
            QVERIFY(!address.isNull());
            QVERIFY(!address.isLoopback()); QVERIFY(!address.isLinkLocal());
            QCOMPARE(address.protocol(), QAbstractSocket::IPv4Protocol);
            QCOMPARE(endpoint.mid(separator + 1).toUShort(), host.channel.port());
        }
        Device client(dir.filePath("client"), "Client");
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.channel.openChat(host.channel.ownId(), "localhost", host.channel.port()));
        QTRY_VERIFY(client.channel.chatReady());
        QCOMPARE(client.channel.channelAddresses(host.channel.ownId()),
            QStringList{"localhost:" + QString::number(host.channel.port())});
    }

    void hostOwnerNamesUpdateAcrossChannelsAndSurviveRestart() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("host"), "Willow Otter");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        const bool extraChannel = License::directDistribution();
        QString extra;
        LocalChannel* child = nullptr;
        if (extraChannel) {
            QVERIFY(host.session.setSupporterEnabled(true));
            extra = host.channel.addOwnedChannel("Quiet room");
            child = host.channel.ownChannel(extra); QVERIFY(child);
        }
        const auto expectedNames = [extraChannel](const QString& name) {
            return extraChannel ? QStringList{name, name} : QStringList{name};
        };
        const auto ownerNames = [](const LocalChannel& channel) {
            QStringList result;
            for (const auto& entry : channel.savedChannels()) result.append(entry.toMap().value("ownerName").toString());
            return result;
        };
        QCOMPARE(ownerNames(host.channel), expectedNames("Willow Otter"));
        auto client = std::make_unique<Device>(dir.filePath("client"), "Reader", identity);
        QVERIFY(client->channel.openAddress(QString("localhost:%1").arg(host.channel.port())));
        QTRY_VERIFY(!client->channel.directBusy());
        QCOMPARE(client->channel.hosts().size(), extraChannel ? 2 : 1);
        for (const auto& entry : client->channel.hosts()) QVERIFY(!entry.toMap().contains("ownerName"));
        QVERIFY(client->channel.openSavedChat(host.channel.ownId()));
        QTRY_COMPARE(host.channel.requests().size(), 1);
        QCOMPARE(ownerNames(client->channel), QStringList({""}));
        QVERIFY(host.channel.decide(identity.id(), true));
        QTRY_VERIFY(client->channel.chatReady());
        QTRY_COMPARE(ownerNames(client->channel), QStringList({"Willow Otter"}));
        if (extraChannel) {
            QVERIFY(child->decide(identity.id(), true));
            QVERIFY(client->channel.openSavedChat(extra));
            QTRY_VERIFY(client->channel.chatReady());
            QTRY_COMPARE(ownerNames(client->channel), expectedNames("Willow Otter"));
            QVERIFY(client->channel.closeChat(extra));
        }
        QVERIFY(host.session.setUserName("Moonlit Fox"));
        QTRY_COMPARE(ownerNames(client->channel), expectedNames("Moonlit Fox"));
        QCOMPARE(ownerNames(host.channel), expectedNames("Moonlit Fox"));
        client.reset();
        client = std::make_unique<Device>(dir.filePath("client"), "Reader", identity);
        QCOMPARE(ownerNames(client->channel), expectedNames("Moonlit Fox"));
        QVERIFY(!client->channel.chatReady());
        // A headless restart keeps the same identity but publishes no personal profile.
        QVERIFY(host.channel.initialize(true));
        QVERIFY(client->channel.openSavedChat(host.channel.ownId()));
        QTRY_VERIFY(client->channel.chatReady());
        const auto headlessName = host.channel.hostConfiguration().value("hostName").toString();
        QVERIFY(!headlessName.isEmpty());
        QTRY_COMPARE(ownerNames(client->channel), expectedNames(headlessName));
        if (extraChannel) {
            QVERIFY(client->channel.openSavedChat(extra));
            QTRY_VERIFY(client->channel.chatReady());
        }
        QVERIFY(host.channel.configureHost({{"hostName", "Night Owl Station"}}));
        QTRY_COMPARE(ownerNames(client->channel), expectedNames("Night Owl Station"));
        QVERIFY(host.session.setUserName("Unused personal name"));
        QCOMPARE(ownerNames(host.channel), expectedNames("Night Owl Station"));
        if (extraChannel) {
            QVERIFY(!child->configureHost({{"hostName", "Different host"}}));
            QCOMPARE(child->hostConfiguration().value("hostName").toString(), QString("Night Owl Station"));
        }
    }

    void savedChannelsSurviveRestartAndAutoJoinFallsBack() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        qint64 now = 1700000000000;
        Device a(dir.filePath("a"), "Host A"), b(dir.filePath("b"), "Host B");
        QVERIFY(a.channel.setChannelName("First")); QVERIFY(b.channel.setChannelName("Second"));
        QVERIFY(a.channel.listen(QHostAddress::LocalHost)); QVERIFY(b.channel.listen(QHostAddress::LocalHost));
        QVERIFY(a.channel.decide(identity.id(), true)); QVERIFY(b.channel.decide(identity.id(), true));
        auto client = std::make_unique<Device>(dir.filePath("client"), "Member", identity, [&] { return now; });
        QVERIFY(client->join(b.channel)); QTRY_VERIFY(client->channel.chatReady());
        QVERIFY(client->channel.setAutoJoin(b.channel.ownId(), true));
        now += 1000;
        QVERIFY(client->join(a.channel)); QTRY_VERIFY(client->channel.chatReady());
        QVERIFY(client->channel.setAutoJoin(a.channel.ownId(), true));
        QTRY_COMPARE(client->channel.savedChannels().first().toMap().value("name").toString(), QString("First"));
        QCOMPARE(client->channel.savedChannels().size(), 2);
        client.reset(); QVERIFY(a.channel.stopHost());
        now += 1000;
        client = std::make_unique<Device>(dir.filePath("client"), "Member", identity, [&] { return now; });
        QTRY_VERIFY(client->channel.chatReady()); QCOMPARE(client->channel.joinedHostId(), b.channel.ownId());
        QCOMPARE(client->channel.savedChannels().size(), 2);
        QVERIFY(client->channel.leave());
        QVERIFY(client->channel.startService(QHostAddress::LocalHost, 0));
        QTest::qWait(100); QVERIFY(!client->channel.joined());
        client.reset();
        client = std::make_unique<Device>(dir.filePath("client"), "Member", identity, [&] { return now; });
        QTRY_VERIFY(client->channel.chatReady()); QCOMPARE(client->channel.joinedHostId(), b.channel.ownId());
        QVERIFY(client->channel.removeChannel(b.channel.ownId()));
        QVERIFY(!client->channel.joined()); QCOMPARE(client->channel.savedChannels().size(), 1);
        QVERIFY(client->channel.setAutoJoin(a.channel.ownId(), false));
        client.reset();
        client = std::make_unique<Device>(dir.filePath("client"), "Member", identity, [&] { return now; });
        QVERIFY(client->channel.startService(QHostAddress::LocalHost, 0));
        QTest::qWait(100); QVERIFY(!client->channel.joined()); QVERIFY(client->channel.joinedHostId().isEmpty());
        QCOMPARE(client->channel.savedChannels().size(), 1);
        QVERIFY(!client->channel.setAutoJoin(b.channel.ownId(), true));
        QVERIFY(!client->channel.joinSaved(b.channel.ownId()));
    }

    void removingASavedChannelIsAtomicAndBansRemoveIt() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Owner"), client(dir.filePath("client"), "Member");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost)); QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.join(host.channel)); QTRY_VERIFY(client.channel.chatReady());
        QTRY_COMPARE(client.channel.savedChannels().size(), 1);
        QVERIFY(client.channel.setAutoJoin(host.channel.ownId(), true));
        const auto path = dir.filePath("client.channel"), backup = path + ".backup";
        QVERIFY(QFile::rename(path, backup)); QVERIFY(QDir().mkdir(path));
        QVERIFY(!client.channel.removeChannel(host.channel.ownId()));
        QCOMPARE(client.channel.savedChannels().size(), 1); QVERIFY(client.channel.joined());
        QVERIFY(QDir().rmdir(path)); QVERIFY(QFile::rename(backup, path));
        QVERIFY(host.channel.setBlocked(client.channel.ownId(), true));
        QTRY_VERIFY(!client.channel.joined()); QTRY_VERIFY(client.channel.savedChannels().isEmpty());
    }
    void passwordRetryNotifiesWhenCountdownExpires() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        Device client(dir.filePath("client"), "Member");
        auto configuration = identity.configuration();
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslServer server; server.setSslConfiguration(configuration);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.join(identity.id(), "127.0.0.1", server.serverPort()));
        QVERIFY(waitForEvents(&server, &QTcpServer::pendingConnectionAvailable, [&] { return server.hasPendingConnections(); }));
        auto* socket = qobject_cast<QSslSocket*>(server.nextPendingConnection()); QVERIFY(socket);
        QVERIFY(waitForEvents(socket, &QSslSocket::readyRead, [&] { return socket->bytesAvailable() > 4; }));
        socket->readAll();
        int displayedSeconds = -1;
        QObject observer;
        connect(&client.channel, &LocalChannel::stateChanged, &observer, [&] {
            displayedSeconds = client.channel.passwordRetrySeconds();
        });
        socket->write(wireFrame({{"type", "join"}, {"result", "password"}, {"retryMs", 1500}}));
        QTRY_VERIFY(client.channel.passwordRequired());
        QVERIFY(displayedSeconds > 0);
        QTRY_COMPARE_WITH_TIMEOUT(displayedSeconds, 0, 5000);
        QVERIFY(client.channel.submitPassword("retry after countdown", false));
    }

    void passwordsPreserveActiveMembersAndPersistPerHostSecrets() {
        QTemporaryDir dir;
        const auto hostIdentity = TlsIdentity::create();
        const auto clientIdentity = TlsIdentity::create();
        qint64 now = 1700000000000;
        auto host = std::make_unique<Device>(dir.filePath("host"), "Host", hostIdentity, [&] { return now; });
        auto client = std::make_unique<Device>(dir.filePath("client"), "Client", clientIdentity);
        QVERIFY(host->channel.listen(QHostAddress::LocalHost));
        QVERIFY(host->channel.decide(clientIdentity.id(), true));
        QVERIFY(host->channel.setHostPassword("first secret ü"));
        QTRY_VERIFY(!host->channel.passwordBusy()); QVERIFY(host->channel.passwordProtected());
        QVERIFY(client->join(host->channel)); QTRY_VERIFY(client->channel.passwordRequired());
        QVERIFY(!client->channel.joined()); QVERIFY(!client->channel.chatReady());
        QVERIFY(host->channel.requests().isEmpty());
        QVERIFY(client->channel.submitPassword("first secret ü", true));
        QTRY_VERIFY(client->channel.chatReady()); QVERIFY(client->channel.passwordSaved());
        for (const auto& file : {dir.filePath("host.channel"), dir.filePath("client.channel")}) {
            QFile data(file); QVERIFY(data.open(QIODevice::ReadOnly)); QVERIFY(!data.readAll().contains("first secret"));
        }
        client.reset();
        client = std::make_unique<Device>(dir.filePath("client"), "Client", clientIdentity);
        QVERIFY(client->join(host->channel)); QTRY_VERIFY(client->channel.chatReady());
        QVERIFY(host->channel.setHostPassword("changed password"));
        QTRY_VERIFY(!host->channel.passwordBusy());
        QVERIFY(client->channel.joined()); QVERIFY(client->channel.sendChat("Still here after password change"));
        QTRY_VERIFY(!client->channel.chatPending());
        QVERIFY(client->channel.leave()); QVERIFY(client->join(host->channel));
        QTRY_VERIFY(client->channel.passwordRequired() && client->channel.passwordRetrySeconds() > 0);
        QVERIFY(!client->channel.joined()); QVERIFY(!client->channel.submitPassword("changed password", true));
        QVERIFY(client->channel.forgetPassword()); QVERIFY(!client->channel.passwordSaved());
        QVERIFY(client->channel.leave()); now += 30001;
        QVERIFY(client->join(host->channel)); QTRY_VERIFY(client->channel.passwordRequired());
        QVERIFY(client->channel.submitPassword("changed password", true)); QTRY_VERIFY(client->channel.chatReady());
        client.reset(); host.reset();
        host = std::make_unique<Device>(dir.filePath("host"), "Host", hostIdentity, [&] { return now; });
        client = std::make_unique<Device>(dir.filePath("client"), "Client", clientIdentity);
        QVERIFY(host->channel.passwordProtected()); QVERIFY(host->channel.listen(QHostAddress::LocalHost));
        QVERIFY(client->join(host->channel)); QTRY_VERIFY(client->channel.chatReady());
        QVERIFY(client->channel.forgetPassword()); QVERIFY(client->channel.joined());
        QVERIFY(host->channel.setHostPassword("")); QVERIFY(!host->channel.passwordProtected());
        QVERIFY(client->channel.leave()); QVERIFY(client->join(host->channel)); QTRY_VERIFY(client->channel.chatReady());
    }

    void passwordDoesNotReplaceAdmissionOrLeakToAnotherHost() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), client(dir.filePath("client"), "Client"), other(dir.filePath("other"), "Other");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost)); QVERIFY(other.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.setHostPassword("host one")); QTRY_VERIFY(!host.channel.passwordBusy());
        QVERIFY(other.channel.setHostPassword("host two")); QTRY_VERIFY(!other.channel.passwordBusy());
        QVERIFY(client.join(host.channel)); QTRY_VERIFY(client.channel.passwordRequired());
        QVERIFY(client.channel.submitPassword("host one", true));
        QTRY_COMPARE(host.channel.requests().size(), 1); QVERIFY(!client.channel.joined());
        QVERIFY(host.channel.setHostPassword("new host one")); QTRY_VERIFY(!host.channel.passwordBusy());
        QTRY_VERIFY(client.channel.passwordRequired()); QVERIFY(host.channel.requests().isEmpty());
        QVERIFY(host.channel.decide(client.channel.ownId(), true)); QVERIFY(!client.channel.joined());
        QVERIFY(client.channel.submitPassword("new host one", true)); QTRY_VERIFY(client.channel.chatReady());
        QVERIFY(client.join(other.channel)); QTRY_VERIFY(client.channel.passwordRequired());
        QCOMPARE(client.channel.passwordRetrySeconds(), 0); QVERIFY(other.channel.requests().isEmpty());
        QVERIFY(!client.channel.passwordSaved());
        QVERIFY(other.channel.setRequestsAllowed(false));
        QVERIFY(client.channel.submitPassword("host two", false));
        QTRY_VERIFY(client.channel.status().contains("rejected")); QVERIFY(other.channel.requests().isEmpty());
        QVERIFY(other.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.join(other.channel)); QTRY_VERIFY(client.channel.passwordRequired());
        QVERIFY(client.channel.submitPassword("host two", false)); QTRY_VERIFY(client.channel.chatReady());
        QVERIFY(!client.channel.passwordSaved());
        QVERIFY(host.channel.join(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(host.channel.chatReady());
    }

    void concurrentPasswordChecksRetryCapacityWithoutRejectingCorrectSecrets() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.setHostPassword("shared phrase")); QTRY_VERIFY(!host.channel.passwordBusy());
        std::vector<std::unique_ptr<Device>> clients;
        for (int i = 0; i < 5; ++i) {
            auto client = std::make_unique<Device>(dir.filePath(QString::number(i)), "Client " + QString::number(i));
            QVERIFY(host.channel.decide(client->channel.ownId(), true));
            QVERIFY(client->join(host.channel)); clients.push_back(std::move(client));
        }
        QTRY_VERIFY(std::all_of(clients.begin(), clients.end(), [](const auto& client) { return client->channel.passwordRequired(); }));
        for (const auto& client : clients) QVERIFY(client->channel.submitPassword("shared phrase", false));
        QTRY_VERIFY_WITH_TIMEOUT(std::all_of(clients.begin(), clients.end(), [](const auto& client) { return client->channel.chatReady(); }), 8000);
        QVERIFY(host.channel.requests().isEmpty());
    }

    void storedPasswordsRejectTamperingAndIdentityChanges() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("host"), "Host");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.setHostPassword("private stored phrase")); QTRY_VERIFY(!host.channel.passwordBusy());
        QVERIFY(host.channel.decide(identity.id(), true));
        {
            Device client(dir.filePath("client"), "Client", identity);
            QVERIFY(client.join(host.channel)); QTRY_VERIFY(client.channel.passwordRequired());
            QVERIFY(client.channel.submitPassword("private stored phrase", true)); QTRY_VERIFY(client.channel.chatReady());
        }
        {
            Device wrongIdentity(dir.filePath("client"), "Client");
            QVERIFY(!wrongIdentity.join(host.channel)); QVERIFY(!wrongIdentity.channel.joined());
            QVERIFY(wrongIdentity.channel.status().contains("device identity"));
        }
        QFile file(dir.filePath("client.channel")); QVERIFY(file.open(QIODevice::ReadOnly));
        auto saved = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        auto security = saved.value("security").toObject(); auto passwords = security.value("saved").toObject();
        auto value = QByteArray::fromBase64(passwords.value(host.channel.ownId()).toString().toLatin1());
        value[15] = char(value[15] ^ 1);
        passwords.insert(host.channel.ownId(), QString::fromLatin1(value.toBase64())); security.insert("saved", passwords); saved.insert("security", security);
        QVERIFY(file.open(QIODevice::WriteOnly)); const auto serialized = QJsonDocument(saved).toJson();
        QCOMPARE(file.write(serialized), serialized.size()); file.close();
        Device altered(dir.filePath("client"), "Client", identity);
        QVERIFY(!altered.join(host.channel)); QVERIFY(!altered.channel.joined());
        QVERIFY(altered.channel.status().contains("modified"));
    }

    void passwordRejectsInvalidInputAndDoesNotApplyFailedPersistence() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host");
        QVERIFY(!host.channel.setHostPassword(QString(1025, 'x')));
        QVERIFY(!host.channel.setHostPassword("line\nbreak"));
        QVERIFY(!host.channel.submitPassword("no connection", true));
        QVERIFY(!host.channel.forgetPassword());
        QVERIFY(host.channel.setHostPassword("saved password"));
        QVERIFY(!host.channel.setHostPassword("overlapping change"));
        QTRY_VERIFY(!host.channel.passwordBusy()); QVERIFY(host.channel.passwordProtected());
        QVERIFY(QFile::remove(dir.filePath("host.channel"))); QVERIFY(QDir().mkdir(dir.filePath("host.channel")));
        QVERIFY(!host.channel.setHostPassword("")); QVERIFY(host.channel.passwordProtected());
        QVERIFY(host.channel.setHostPassword("cannot be saved")); QTRY_VERIFY(!host.channel.passwordBusy());
        QVERIFY(host.channel.passwordProtected()); QVERIFY(host.channel.status().contains("could not be saved"));
        {
            Device destroyedWhileHashing(dir.filePath("short"), "Short");
            QVERIFY(destroyedWhileHashing.channel.setHostPassword("cancel with owner"));
        }
        QCoreApplication::processEvents();
    }

    void silentNetworkLossReconnectsAndDeliversPendingChatOnce() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), client(dir.filePath("client"), "Client");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QTcpServer relay;
        bool dropping = false;
        connect(&relay, &QTcpServer::newConnection, &relay, [&] {
            auto* incoming = relay.nextPendingConnection();
            auto* outgoing = new QTcpSocket(incoming);
            const auto queued = std::make_shared<QByteArray>();
            connect(incoming, &QTcpSocket::readyRead, outgoing, [&, incoming, outgoing, queued] {
                const auto bytes = incoming->readAll();
                if (dropping) return;
                if (outgoing->state() == QAbstractSocket::ConnectedState) outgoing->write(bytes);
                else queued->append(bytes);
            });
            connect(outgoing, &QTcpSocket::connected, incoming, [outgoing, queued] { outgoing->write(*queued); queued->clear(); });
            connect(outgoing, &QTcpSocket::readyRead, incoming, [&, incoming, outgoing] {
                const auto bytes = outgoing->readAll();
                if (!dropping) incoming->write(bytes);
            });
            connect(incoming, &QTcpSocket::disconnected, incoming, &QObject::deleteLater);
            connect(outgoing, &QTcpSocket::disconnected, incoming, &QTcpSocket::disconnectFromHost);
            outgoing->connectToHost(QHostAddress::LocalHost, host.channel.port());
        });
        QVERIFY(relay.listen(QHostAddress::LocalHost));
        QVERIFY(client.channel.join(host.channel.ownId(), "127.0.0.1", relay.serverPort()));
        QTRY_VERIFY(client.channel.chatReady());
        bool disconnectedWhileIdle = false;
        const auto idleWatch = connect(&client.channel, &LocalChannel::stateChanged, &client.channel,
            [&] { disconnectedWhileIdle |= !client.channel.joined(); });
        QTest::qWait(9500);
        disconnect(idleWatch);
        QVERIFY(!disconnectedWhileIdle);
        QVERIFY(client.session.setPushToTalk(true));
        QVERIFY(client.session.setMuted(false));
        QVERIFY(client.session.setPttButtonHeld(true));
        QSignalSpy receipts(&client.channel, &LocalChannel::chatSent);
        dropping = true;
        QVERIFY(client.channel.sendChat("Nach dem Netzausfall genau einmal"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.channel.joined(), 12000);
        QVERIFY(client.channel.chatPending());
        QCOMPARE(receipts.size(), 0);
        QVERIFY(client.session.transmissionAllowed());
        dropping = false;
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.chatReady(), 12000);
        QTRY_VERIFY(!client.channel.chatPending());
        QCOMPARE(receipts.size(), 1);
        QCOMPARE(memberMessages(client.channel).size(), 1);
        QVERIFY(client.session.transmissionAllowed());
    }
    void extendedRetentionKeepsFreeAndLegacyReadersUsable_data() {
        QTest::addColumn<int>("capabilities");
        QTest::newRow("unadvertised") << 0;
        QTest::newRow("old-capabilities") << 1;
        QTest::newRow("extended-free-reader") << 2;
    }
    void extendedRetentionKeepsFreeAndLegacyReadersUsable() {
        QFETCH(int, capabilities);
        if (!License::directDistribution()) return;
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        Device host(dir.filePath("host"), "Host", TlsIdentity::create(), [&] { return now; });
        const auto identity = TlsIdentity::create();
        QVERIFY(host.session.setSupporterEnabled(true));
        QVERIFY(host.channel.setMessageLifetimeDays(360));
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(identity.id(), true));
        QSslSocket socket; socket.setSslConfiguration(identity.configuration());
        connect(&socket, &QSslSocket::sslErrors, &socket, [&socket](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
        QByteArray incoming;
        QJsonObject key;
        QList<QJsonObject> payloads;
        connect(&socket, &QSslSocket::readyRead, &socket, [&] {
            incoming += socket.readAll();
            while (incoming.size() >= 4) {
                const auto size = qFromBigEndian<quint32>(incoming.constData());
                if (incoming.size() < size + 4) break;
                const auto message = QJsonDocument::fromJson(incoming.mid(4, size)).object(); incoming.remove(0, size + 4);
                if (message.value("type") == "chatKey") key = message;
                if (message.value("type") == "chat") {
                    const auto context = "SquadSpeak/chat/v1/" + host.channel.ownId().toUtf8() + '/' + key.value("epoch").toString().toUtf8() + "/host";
                    payloads.append(QJsonDocument::fromJson(TlsIdentity::open(QByteArray::fromBase64(message.value("data").toString().toLatin1()),
                        QByteArray::fromBase64(key.value("key").toString().toLatin1()), context)).object());
                }
            }
        });
        socket.connectToHostEncrypted("127.0.0.1", host.channel.port()); QTRY_VERIFY(socket.isEncrypted());
        QJsonObject hello{{"type", "hello"}, {"version", 1}, {"name", "Reader"}, {"available", true}, {"muted", true}, {"observer", true}};
        if (capabilities) hello.insert("capabilities", capabilities == 2 ? QJsonArray{"extended-retention"} : QJsonArray{"adaptive-audio"});
        socket.write(wireFrame(hello)); QTRY_VERIFY(!key.isEmpty());
        QCOMPARE(key.value("lifetimeDays").toInt(), capabilities == 2 ? 360 : 30);
        QVERIFY(host.channel.sendSystemMessage("Retained announcement"));
        QTRY_VERIFY(!payloads.isEmpty());
        const auto receipt = payloads.last().value("record").toObject();
        QCOMPARE(receipt.value("expires").toInteger() - receipt.value("created").toInteger(),
            (capabilities == 2 ? 360 : 30) * ChatHistory::lifetime);
        const auto send = [&](QJsonObject payload) {
            const auto context = "SquadSpeak/chat/v1/" + host.channel.ownId().toUtf8() + '/' + key.value("epoch").toString().toUtf8() + '/' + identity.id().toUtf8();
            const auto sealed = TlsIdentity::seal(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                QByteArray::fromBase64(key.value("key").toString().toLatin1()), context);
            socket.write(wireFrame({{"type", "chat"}, {"epoch", key.value("epoch")}, {"data", QString::fromLatin1(sealed.toBase64())}}));
        };
        now += 40 * ChatHistory::lifetime;
        payloads.clear();
        send({{"kind", "history"}, {"request", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"cursor", 0}, {"direction", "latest"}, {"extended", true}});
        QTRY_VERIFY(std::any_of(payloads.begin(), payloads.end(), [](const auto& p) { return p.value("kind") == "history"; }));
        for (const auto& page : payloads) if (page.value("kind") == "history") QCOMPARE(page.value("records").toArray().size(), capabilities == 2 ? 1 : 0);
        QCOMPARE(host.channel.hostHistory().value("records").toArray().size(), 1);
        payloads.clear();
        send({{"kind", "send"}, {"request", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"text", "Still works"}, {"issued", now}});
        QTRY_VERIFY(std::any_of(payloads.begin(), payloads.end(), [](const auto& p) { return p.value("record").toObject().value("text") == "Still works"; }));
        QCOMPARE(socket.state(), QAbstractSocket::ConnectedState);
    }
    void storedLifetimeValidationDoesNotUseEntitlementFallback() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        {
            Device host(dir.filePath("host"), "Host", identity);
            QVERIFY(host.channel.setMessageLifetimeDays(7));
        }
        QFile file(dir.filePath("host.channel"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto settings = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        settings.insert("messageLifetimeDays", 31);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(settings).toJson()); file.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Device(dir.filePath("host"), "Host", identity));
    }
    void supporterExpiryChangesOnlyNewMessageLifetimes() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("host"), "Host", identity, [&] { return now; });
        QVERIFY(!host.channel.setMessageLifetimeDays(90));
        if (!License::directDistribution()) return;
        QVERIFY(host.session.setSupporterEnabled(true));
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        for (const auto days : {90, 180, 360}) {
            QVERIFY(host.channel.setMessageLifetimeDays(days));
            QVERIFY(host.channel.sendSystemMessage(QString::number(days)));
        }
        QVERIFY(host.session.setSupporterEnabled(false));
        QCOMPARE(host.channel.messageLifetimeDays(), 30);
        QVERIFY(host.channel.sendSystemMessage("After expiry"));
        const auto records = host.channel.hostHistory().value("records").toArray();
        for (int i = 0; i < 4; ++i) QCOMPARE(records[i].toObject().value("expires").toInteger() - records[i].toObject().value("created").toInteger(),
            (std::array<int, 4>{90, 180, 360, 30}[i] * ChatHistory::lifetime));
        Device restored(dir.filePath("host"), "Host", identity, [&] { return now; });
        QCOMPARE(restored.channel.messageLifetimeDays(), 30);
        ChatHistory stored(dir.filePath("host.channel.chat"), identity, now);
        QCOMPARE(stored.page().value("records").toArray(), records);
    }
    void chatHistoryAuthenticatesStorageAndPreservesExpiry() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        const auto path = dir.filePath("history");
        const QString request = "8f7d5509-93bd-4765-aee3-35c3b40adba4";
        const qint64 now = 1700000000000;
        ChatHistory history(path, identity, now);
        const auto receipt = history.append(identity.id(), "Alice", request, "secret chat text", now);
        QCOMPARE(history.append(identity.id(), "New name", request, "secret chat text", now + 5000), receipt);
        QCOMPARE(history.page().value("records").toArray().size(), 1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, history.append(identity.id(), "Alice", request, "different text", now));
        ChatHistory restarted(path, identity, now + ChatHistory::lifetime - 1);
        QCOMPARE(restarted.page().value("records").toArray(), QJsonArray{receipt});
        QVERIFY(restarted.expire(now + ChatHistory::lifetime));
        QVERIFY(restarted.page().value("records").toArray().isEmpty());
        ChatHistory rewound(path, identity, now);
        QVERIFY(rewound.page().value("records").toArray().isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ChatHistory(dir.filePath("missing/history"), identity, now));
        QVERIFY(!ChatHistory::validText(" \n\t")); QVERIFY(!ChatHistory::validText(QString(16385, 'a')));
        QVERIFY(!ChatHistory::validText(QString("a") + QChar(1))); QVERIFY(ChatHistory::validText("**Hallo**\n| a | b |"));
        const auto key = TlsIdentity::newKey();
        const auto box = TlsIdentity::seal("hello", key, "channel/a");
        QCOMPARE(TlsIdentity::open(box, key, "channel/a"), QByteArray("hello"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)TlsIdentity::open(box, key, "channel/b"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)TlsIdentity::open(box, TlsIdentity::newKey(), "channel/a"));
    }
    void imageChatStripsMetadataTransfersChunksAndExpires() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&now] { return now; };
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("host"), "Host", identity, clock);
        Device a(dir.filePath("a"), "Alice", TlsIdentity::create(), clock);
        Device b(dir.filePath("b"), "Bob", TlsIdentity::create(), clock);
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(a.channel.ownId(), true)); QVERIFY(host.channel.decide(b.channel.ownId(), true));
        QVERIFY(a.join(host.channel)); QVERIFY(b.join(host.channel)); QTRY_VERIFY(a.channel.chatReady() && b.channel.chatReady());
        QImage pixels(300, 300, QImage::Format_ARGB32);
        quint32 random = 1234567;
        for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x) {
            random = random * 1664525U + 1013904223U;
            pixels.setPixel(x, y, (random & 0xffffff) | 0x80000000U);
        }
        pixels.setText("Author", "Private test metadata");
        QByteArray original; QBuffer buffer(&original); buffer.open(QIODevice::WriteOnly); QVERIFY(pixels.save(&buffer, "PNG"));
        QByteArray prepared; QString failure; bool finished = false;
        QVERIFY(ChatContent::prepare(original, this, [&](QByteArray png, QString error) { prepared = png; failure = error; finished = true; }));
        QTRY_VERIFY(finished); QVERIFY2(failure.isEmpty(), qPrintable(failure)); QVERIFY(prepared.size() > 65536);
        QVERIFY(!prepared.contains("Private test metadata"));
        QVERIFY(a.session.setMuted(false));
        QSignalSpy audio(&b.channel, &LocalChannel::audioReceived);
        QVERIFY(a.channel.sendChat("Bild mit Transparenz", original));
        QVERIFY(a.channel.sendAudio(voicePacket()));
        QTRY_COMPARE(audio.size(), 1);
        QTRY_COMPARE(memberMessages(b.channel).size(), 1); QTRY_VERIFY(!a.channel.chatPending());
        const auto message = memberMessages(b.channel).first().toMap();
        const auto image = message.value("image").toMap();
        const auto hash = image.value("hash").toString();
        QVERIFY(message.value("text").toString().contains("attachment:" + hash));
        QCOMPARE(image.value("width").toInt(), 300); QCOMPARE(image.value("height").toInt(), 300);
        QVERIFY(b.channel.requestImage(hash)); QVERIFY(b.channel.requestImage(hash));
        QTRY_VERIFY(!b.channel.imageSource(hash).isEmpty());
        const auto received = QByteArray::fromBase64(b.channel.imageSource(hash).mid(22).toLatin1());
        const auto decoded = QImage::fromData(received, "PNG");
        QCOMPARE(decoded.size(), pixels.size()); QVERIFY(decoded.textKeys().isEmpty()); QCOMPARE(decoded.pixelColor(10, 10).alpha(), 128);
        QVERIFY(!b.channel.requestImage(QString(64, 'a')));
        const auto blob = dir.filePath("host.channel.chat.images/" + hash + ".enc");
        QVERIFY(QFile::exists(blob));
        ChatHistory restarted(dir.filePath("host.channel.chat"), identity, now);
        QCOMPARE(restarted.image(hash, now), prepared);
        QCOMPARE(received, prepared);
        QVERIFY(a.channel.sendChat("invalid source", "<svg/>"));
        QTRY_VERIFY(!a.channel.chatPending()); QVERIFY(!a.channel.chatError().isEmpty());
        QCOMPARE(memberMessages(b.channel).size(), 1);
        now += ChatHistory::lifetime;
        QTRY_VERIFY(memberMessages(b.channel).isEmpty()); QTRY_VERIFY(!QFile::exists(blob));
        QVERIFY(b.channel.imageSource(hash).isEmpty()); QVERIFY(!b.channel.requestImage(hash));
    }
    void chatNotificationsFollowVoiceMembershipRatherThanTheSelectedChat() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "First"), second(dir.filePath("second"), "Second");
        Device reader(dir.filePath("reader"), "Reader"), writer(dir.filePath("writer"), "Writer");
        QSignalSpy notifications(&reader.channel, SIGNAL(chatNotification(QString,QVariantMap)));
        QVERIFY(notifications.isValid());
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(second.channel.listen(QHostAddress::LocalHost));
        for (auto* server : {&host, &second}) {
            QVERIFY(server->channel.decide(reader.channel.ownId(), true));
            QVERIFY(server->channel.decide(writer.channel.ownId(), true));
        }
        QVERIFY(writer.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(writer.channel.chatReady());
        QVERIFY(writer.channel.sendChat("Before your arrival"));
        QTRY_VERIFY(!writer.channel.chatPending());
        QVERIFY(reader.join(host.channel));
        QTRY_VERIFY(reader.channel.chatReady());
        QTRY_COMPARE(memberMessages(reader.channel).size(), 1);
        QCOMPARE(notifications.size(), 0);
        QVERIFY(writer.channel.sendChat("Visible chat"));
        QTRY_COMPARE(notifications.size(), 1);
        QCOMPARE(notifications.last().at(0).toString(), host.channel.ownId());
        QCOMPARE(notifications.last().at(1).toMap().value("text").toString(), QString("Visible chat"));
        QVERIFY(reader.channel.sendChat("My own message"));
        QTRY_VERIFY(!reader.channel.chatPending());
        QVERIFY(reader.channel.refreshChat());
        QTRY_VERIFY(!reader.channel.historyLoading());
        QCOMPARE(notifications.size(), 1);
        QVERIFY(reader.channel.openChat(second.channel.ownId(), "127.0.0.1", second.channel.port()));
        QTRY_VERIFY(reader.channel.chatReady());
        QVERIFY(writer.channel.sendChat("Voice channel while another chat is selected"));
        QTRY_COMPARE(notifications.size(), 2);
        QVERIFY(writer.channel.openChat(second.channel.ownId(), "127.0.0.1", second.channel.port()));
        QTRY_VERIFY(writer.channel.chatReady());
        QVERIFY(writer.channel.sendChat("Text-only channel"));
        QTRY_COMPARE(memberMessages(reader.channel).size(), 1);
        QCOMPARE(notifications.size(), 2);
        QVERIFY(writer.join(host.channel));
        QTRY_COMPARE(reader.channel.participants().size(), 2);
        QCOMPARE(notifications.size(), 2); // Joining is already covered by its own event sound.
        QVERIFY(reader.join(second.channel));
        QTRY_VERIFY(reader.channel.joined() && reader.channel.joinedHostId() == second.channel.ownId());
        QVERIFY(writer.channel.sendChat("Previous voice channel"));
        QTRY_VERIFY(!writer.channel.chatPending());
        QVERIFY(second.channel.sendSystemMessage("New channel announcement"));
        QTRY_COMPARE(notifications.size(), 3);
        QCOMPARE(notifications.last().at(0).toString(), second.channel.ownId());
        QCOMPARE(notifications.last().at(1).toMap().value("event").toMap().value("kind").toString(), QString("announcement"));
        QVERIFY(reader.channel.leave());
        QVERIFY(second.channel.sendSystemMessage("After leaving voice"));
        QTRY_VERIFY([&] {
            const auto records = reader.channel.messages();
            return std::any_of(records.cbegin(), records.cend(), [](const auto& value) {
                return value.toMap().value("text").toString() == "After leaving voice";
            });
        }());
        QCOMPARE(notifications.size(), 3);
    }
    void botNotificationsContainOnlyFreshSystemMessagesInTheVoiceChannel() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), reader(dir.filePath("reader"), "Reader"), member(dir.filePath("member"), "Member");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(reader.channel.ownId(), true));
        QVERIFY(host.channel.decide(member.channel.ownId(), true));
        QVERIFY(host.channel.sendSystemMessage("Historical announcement"));
        QSignalSpy messages(&reader.channel, &LocalChannel::botNotification);
        QVERIFY(reader.join(host.channel));
        QTRY_VERIFY(reader.channel.chatReady() && !reader.channel.historyLoading() && !reader.channel.messages().isEmpty());
        QVERIFY(reader.channel.refreshChat()); QTRY_VERIFY(!reader.channel.historyLoading());
        // Our own join can arrive live or with the initial page. Neither page
        // may replay the earlier announcement. The refresh is a server fence.
        for (const auto& delivery : messages) {
            const auto event = delivery.at(0).toMap().value("event").toMap();
            QCOMPARE(event.value("kind").toString(), QString("joined"));
            QCOMPARE(event.value("member").toString(), reader.channel.ownId());
        }
        int count = int(messages.size());
        const auto check = [&](const QString& kind) {
            ++count;
            QTRY_COMPARE(messages.size(), count);
            QCOMPARE(messages.last().at(0).toMap().value("event").toMap().value("kind").toString(), kind);
        };
        QVERIFY(member.join(host.channel)); check("joined");
        QTRY_VERIFY(member.channel.chatReady() && !member.channel.historyLoading());
        QVERIFY(member.channel.sendChat("A human message")); QTRY_VERIFY(!member.channel.chatPending());
        QCOMPARE(messages.size(), count);
        QVERIFY(member.channel.leave()); check("left");
        QVERIFY(member.join(host.channel)); check("joined");
        QTRY_VERIFY(member.channel.chatReady() && !member.channel.historyLoading());
        QVERIFY(host.channel.kick(member.channel.ownId())); check("kicked");
        QTRY_VERIFY(!member.channel.joined());
        QVERIFY(member.join(host.channel)); check("joined");
        QTRY_VERIFY(member.channel.chatReady() && !member.channel.historyLoading());
        QVERIFY(host.channel.setBlocked(member.channel.ownId(), true)); check("banned");
        QVERIFY(host.channel.sendSystemMessage("A new announcement")); check("announcement");
        QVERIFY(reader.channel.refreshChat()); QTRY_VERIFY(!reader.channel.historyLoading());
        QCOMPARE(messages.size(), count);
        QVERIFY(reader.channel.leave());
        QVERIFY(host.channel.sendSystemMessage("After leaving"));
        QVERIFY(reader.channel.refreshChat()); QTRY_VERIFY(!reader.channel.historyLoading());
        QCOMPARE(messages.size(), count);
    }

    void unreadTracksLiveMessagesAcrossHistoryExpiryAndRemoteReading() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        Device host(dir.filePath("host"), "Host", TlsIdentity::create(), clock);
        Device reader(dir.filePath("reader"), "Reader", TlsIdentity::create(), clock);
        Device writer(dir.filePath("writer"), "Writer", TlsIdentity::create(), clock);
        Device control(dir.filePath("control"), "Control");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(reader.channel.ownId(), true));
        QVERIFY(host.channel.decide(writer.channel.ownId(), true));
        QVERIFY(writer.join(host.channel)); QTRY_VERIFY(writer.channel.chatReady());
        QVERIFY(writer.channel.sendChat("History")); QTRY_VERIFY(!writer.channel.chatPending());
        QVERIFY(reader.channel.openChat(host.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY(reader.channel.chatReady() && !reader.channel.historyLoading());
        const auto unread = [&] {
            for (const auto& value : reader.channel.savedChannels())
                if (value.toMap().value("id") == host.channel.ownId()) return value.toMap().value("unread").toBool();
            return false;
        };
        QVERIFY(!unread());
        QVERIFY(!reader.channel.joined());
        QVERIFY(host.channel.sendSystemMessage("Announcement"));
        QTRY_VERIFY(reader.channel.messages().last().toMap().contains("event"));
        QVERIFY(!unread());
        QVERIFY(host.channel.setMessageLifetimeDays(7));
        QVERIFY(writer.channel.sendChat("Long lived")); QTRY_VERIFY(!writer.channel.chatPending());
        QTRY_VERIFY(unread());
        QVERIFY(host.channel.setMessageLifetimeDays(1));
        QVERIFY(writer.channel.sendChat("Short lived")); QTRY_VERIFY(!writer.channel.chatPending());
        QTRY_COMPARE(memberMessages(reader.channel).size(), 3);
        const auto last = reader.channel.messages().last().toMap().value("sequence").toLongLong();
        QVERIFY(!reader.channel.markChatRead(host.channel.ownId(), last - 1));
        QVERIFY(!reader.channel.markChatRead(host.channel.ownId(), last + 1));
        QVERIFY(reader.channel.refreshChat()); QTRY_VERIFY(!reader.channel.historyLoading());
        QVERIFY(unread());
        // A later, shorter-lived message cannot erase an older unread message.
        now += 2 * ChatHistory::lifetime;
        QTRY_COMPARE(memberMessages(reader.channel).size(), 1);
        QVERIFY(unread());
        QVERIFY(reader.channel.markChatRead(host.channel.ownId(), reader.channel.messages().last().toMap().value("sequence").toLongLong()));
        QVERIFY(!unread());
        QVERIFY(writer.channel.sendChat("Read from remote")); QTRY_VERIFY(!writer.channel.chatPending());
        QTRY_VERIFY(unread());
        QVERIFY(reader.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(control.channel.pairAddress("127.0.0.1:" + QString::number(reader.channel.port())));
        QTRY_COMPARE(reader.channel.controlRequests().size(), 1);
        QVERIFY(reader.channel.decideControl(control.channel.ownId(), true));
        QTRY_VERIFY(control.channel.controlConnected());
        QVERIFY(reader.channel.setRemotePermission(control.channel.ownId(), true));
        QTRY_VERIFY(control.channel.remoteAllowed());
        QVERIFY(control.channel.setRemoteMode(true));
        QTRY_COMPARE(control.channel.remoteView().value("chatHostId").toString(), host.channel.ownId());
        QTRY_VERIFY(!control.channel.remoteView().value("messages").toList().isEmpty());
        const auto visible = control.channel.remoteView().value("messages").toList().last().toMap().value("sequence").toLongLong();
        QVERIFY(!control.channel.markChatRead(reader.channel.ownId(), visible));
        QVERIFY(!control.channel.remoteAction("markChatRead", {{"lastVisibleSequence", 0.5}}));
        QVERIFY(control.channel.markChatRead(host.channel.ownId(), visible));
        QTRY_VERIFY(!unread());
        QVERIFY(control.channel.setRemoteMode(false));
        QVERIFY(reader.channel.sendChat("Own reply")); QTRY_VERIFY(!reader.channel.chatPending());
        QVERIFY(!unread());
        QVERIFY(writer.channel.sendChat("Expires unread")); QTRY_VERIFY(!writer.channel.chatPending());
        QTRY_VERIFY(unread());
        now += 8 * ChatHistory::lifetime;
        QTRY_VERIFY(!unread());
    }

    void chatSurvivesHostRestartAndReplaysToNewApprovedParticipants() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto clock = [&now] { return now; };
        const auto identity = TlsIdentity::create();
        auto host = std::make_unique<Device>(dir.filePath("host"), "Host", identity, clock);
        Device a(dir.filePath("a"), "Alice", TlsIdentity::create(), clock);
        Device b(dir.filePath("b"), "Bob", TlsIdentity::create(), clock);
        Device outsider(dir.filePath("outside"), "Outside", TlsIdentity::create(), clock);
        QSignalSpy notifications(&b.channel, &LocalChannel::chatNotification);
        QSignalSpy arriving(&outsider.channel, &LocalChannel::chatNotification);
        QVERIFY(host->channel.listen(QHostAddress::LocalHost)); const auto port = host->channel.port();
        QVERIFY(host->channel.decide(a.channel.ownId(), true)); QVERIFY(host->channel.decide(b.channel.ownId(), true));
        QVERIFY(a.join(host->channel)); QVERIFY(b.join(host->channel)); QVERIFY(outsider.join(host->channel));
        QTRY_VERIFY(a.channel.chatReady() && b.channel.chatReady());
        QVERIFY(!host->channel.joined()); QVERIFY(!outsider.channel.sendChat("forbidden"));
        QVERIFY(a.channel.sendChat("**Hallo** https://example.org"));
        QTRY_COMPARE(memberMessages(b.channel).size(), 1); QTRY_VERIFY(!a.channel.chatPending());
        QCOMPARE(memberMessages(a.channel), memberMessages(b.channel));
        QCOMPARE(notifications.size(), 1);
        const auto first = memberMessages(a.channel).first().toMap();
        QCOMPARE(first.value("sender").toString(), a.channel.ownId()); QCOMPARE(first.value("name").toString(), QString("Alice"));
        QVERIFY(memberMessages(outsider.channel).isEmpty());
        host.reset(); QTRY_VERIFY(!a.channel.joined()); QTRY_VERIFY(!b.channel.joined());
        now += 10000;
        host = std::make_unique<Device>(dir.filePath("host"), "Host", identity, clock);
        QVERIFY(host->channel.listen(QHostAddress::LocalHost, port));
        QTRY_VERIFY(a.channel.chatReady() && b.channel.chatReady());
        QTRY_COMPARE(memberMessages(a.channel).size(), 1); QCOMPARE(memberMessages(a.channel).first().toMap(), first);
        QCOMPARE(notifications.size(), 1);
        QVERIFY(host->channel.decide(outsider.channel.ownId(), true));
        QVERIFY(outsider.join(host->channel)); QTRY_VERIFY(outsider.channel.chatReady());
        QTRY_COMPARE(memberMessages(outsider.channel), memberMessages(a.channel));
        QCOMPARE(arriving.size(), 0);
        QVERIFY(host->channel.decide(a.channel.ownId(), false)); QTRY_VERIFY(!a.channel.joined());
        QTRY_VERIFY(b.channel.chatReady()); QVERIFY(b.channel.sendChat("after revocation"));
        QTRY_COMPARE(memberMessages(outsider.channel).size(), 2); QTRY_VERIFY(!b.channel.chatPending());
        QCOMPARE(notifications.size(), 1);
        QCOMPARE(arriving.size(), 1);
        QVERIFY(memberMessages(a.channel).size() <= 1);
        now = first.value("expires").toLongLong();
        QTRY_COMPARE(memberMessages(b.channel).size(), 1);
        QCOMPARE(memberMessages(b.channel).first().toMap().value("text").toString(), QString("after revocation"));
        now += 10000;
        QTRY_VERIFY(memberMessages(b.channel).isEmpty()); QTRY_VERIFY(memberMessages(outsider.channel).isEmpty());
        QVERIFY(b.channel.leave()); QVERIFY(outsider.channel.leave()); host.reset();
        ChatHistory stored(dir.filePath("host.channel.chat"), identity, now);
        for (const auto value : stored.page().value("records").toArray()) {
            const auto record = value.toObject();
            QCOMPARE(record.value("event").toObject().value("kind").toString(), QString("left"));
            QCOMPARE(record.value("created").toInteger(), now);
        }
        stored.expire(now + ChatHistory::lifetime); QVERIFY(stored.page().value("records").toArray().isEmpty());
    }
    void pendingChatRetriesAfterDisconnectAndReleasesDraftAfterRevocation() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        auto host = std::make_unique<Device>(dir.filePath("host"), "Host", identity);
        Device client(dir.filePath("client"), "Client");
        QVERIFY(host->channel.listen(QHostAddress::LocalHost)); const auto port = host->channel.port();
        QVERIFY(host->channel.decide(client.channel.ownId(), true)); QVERIFY(client.join(host->channel));
        QTRY_VERIFY(client.channel.chatReady());
        QSignalSpy sent(&client.channel, &LocalChannel::chatSent);
        QVERIFY(client.channel.sendChat("keep this draft"));
        host.reset(); QTRY_VERIFY(!client.channel.joined()); QVERIFY(client.channel.chatPending());
        host = std::make_unique<Device>(dir.filePath("host"), "Host", identity);
        QVERIFY(host->channel.listen(QHostAddress::LocalHost, port));
        QTRY_COMPARE(sent.size(), 1); QTRY_VERIFY(!client.channel.chatPending());
        QCOMPARE(memberMessages(client.channel).size(), 1);
        QVERIFY(client.channel.sendChat("revoked draft"));
        QVERIFY(host->channel.decide(client.channel.ownId(), false));
        QTRY_VERIFY(!client.channel.joined());
        QTRY_VERIFY(!client.channel.chatPending());
        QCOMPARE(sent.size(), 1);
    }
    void grantingRemotePermissionPreservesNegotiatedCapabilities() {
        QTemporaryDir dir;
        Device target(dir.filePath("target"), "Target");
        const auto identity = TlsIdentity::create();
        QVERIFY(target.channel.startService(QHostAddress::LocalHost, 0));
        QSslSocket socket;
        socket.setSslConfiguration(identity.configuration());
        connect(&socket, &QSslSocket::sslErrors, &socket,
            [&socket](const QList<QSslError>& errors) { socket.ignoreSslErrors(errors); });
        QList<QJsonObject> replies;
        QByteArray input;
        connect(&socket, &QSslSocket::readyRead, &socket, [&] {
            input += socket.readAll();
            while (input.size() >= 4) {
                const auto size = qFromBigEndian<quint32>(input.constData());
                if (input.size() < 4 + size) return;
                replies.append(QJsonDocument::fromJson(input.mid(4, size)).object());
                input.remove(0, 4 + size);
            }
        });
        socket.connectToHostEncrypted("127.0.0.1", target.channel.port());
        QTRY_VERIFY(socket.isEncrypted());
        socket.write(wireFrame({{"type", "controlHello"}, {"version", 1}, {"name", "PTT only"},
            {"held", false}, {"revision", 1}, {"pairing", true}, {"capabilities", QJsonArray{"camera.v2"}}}));
        QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        QVERIFY(target.channel.decideControl(identity.id(), true));
        QTRY_VERIFY(std::any_of(replies.begin(), replies.end(), [](const auto& value) {
            return value.value("result") == "accepted";
        }));
        replies.clear();
        QVERIFY(target.channel.setRemotePermission(identity.id(), true));
        QTRY_VERIFY(!replies.isEmpty());
        QCOMPARE(replies.first().value("result").toString(), "accepted");
        QVERIFY(!replies.first().value("remoteAllowed").toBool());
        const auto further = wireFrame({{"type", "cameraOffer"}})
            + wireFrame({{"type", "pttState"}, {"held", true}, {"revision", 2}});
        socket.write(further);
        QTRY_VERIFY(target.session.pttHeld());
        QCOMPARE(socket.state(), QAbstractSocket::ConnectedState);
    }
    void remotePttNeedsSeparateApprovalAndCombinesControllers() {
        QTemporaryDir dir;
        Device target(dir.filePath("target"), "Target"), a(dir.filePath("a"), "A"), b(dir.filePath("b"), "B");
        QVERIFY(target.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(target.session.setPushToTalk(true)); QVERIFY(target.session.setMuted(false));
        QVERIFY(target.channel.decide(a.channel.ownId(), true));
        QVERIFY(target.channel.setRequestsAllowed(false));
        const auto endpoint = "127.0.0.1:" + QString::number(target.channel.port());
        QVERIFY(a.channel.pairAddress(endpoint));
        QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        QVERIFY(target.channel.requests().isEmpty()); QVERIFY(target.channel.controllers().isEmpty());
        QVERIFY(!a.channel.controlConnected()); QVERIFY(!target.session.transmissionAllowed());
        QSignalSpy requests(&target.channel, &LocalChannel::requestsChanged);
        QTest::qWait(6500);
        QCOMPARE(target.channel.controlRequests().size(), 1); QCOMPARE(requests.size(), 0);
        QVERIFY(target.channel.decideControl(a.channel.ownId(), true));
        QTRY_VERIFY(a.channel.controlConnected());
        QVERIFY(b.channel.pairAddress(endpoint));
        QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        QVERIFY(target.channel.decideControl(b.channel.ownId(), true));
        QTRY_VERIFY(b.channel.controlConnected());
        QCOMPARE(target.channel.controllers().size(), 2);
        QVERIFY(!target.channel.hosting()); QVERIFY(!a.channel.joined()); QVERIFY(!b.channel.joined());
        QVERIFY(a.session.setPushToTalk(true)); QVERIFY(a.session.setMuted(false));
        QVERIFY(a.session.setPttButtonHeld(true));
        QTRY_VERIFY(target.session.transmissionAllowed());
        QVERIFY(!a.session.transmissionAllowed());
        QVERIFY(b.session.setPttButtonHeld(true));
        QTRY_VERIFY(target.channel.controllers().at(1).toMap().value("held").toBool()
            && target.channel.controllers().at(0).toMap().value("held").toBool());
        QVERIFY(a.session.setPttButtonHeld(false));
        QTest::qWait(100); QVERIFY(target.session.transmissionAllowed());
        QVERIFY(target.session.setPttButtonHeld(true));
        QVERIFY(b.session.setPttButtonHeld(false));
        QTest::qWait(100); QVERIFY(target.session.transmissionAllowed());
        QVERIFY(target.session.setPttButtonHeld(false));
        QTRY_VERIFY(!target.session.transmissionAllowed());
        QVERIFY(a.session.setPttButtonHeld(true)); QTRY_VERIFY(target.session.transmissionAllowed());
        QVERIFY(target.session.setMuted(true)); QVERIFY(!target.session.transmissionAllowed());
        QVERIFY(target.session.setMuted(false)); QVERIFY(target.session.transmissionAllowed());
        QVERIFY(target.session.setAudioTestActive(true)); QVERIFY(!target.session.transmissionAllowed());
        QVERIFY(target.session.setAudioTestActive(false)); QVERIFY(target.session.transmissionAllowed());
        QVERIFY(target.channel.startHost()); QVERIFY(target.channel.stopHost());
        QVERIFY(a.channel.controlConnected()); QVERIFY(target.session.transmissionAllowed());
        QVERIFY(target.channel.decideControl(a.channel.ownId(), false));
        QTRY_VERIFY(!a.channel.controlConnected()); QTRY_VERIFY(!target.session.transmissionAllowed());
        QCOMPARE(target.channel.controllers().size(), 1);
        QVERIFY(a.session.pttInputHeld());
    }

    void remoteChannelModeRequiresExplicitPermissionAndPersists() {
        QTemporaryDir dir;
        auto target = std::make_unique<Device>(dir.filePath("target"), "Target");
        Device controller(dir.filePath("controller"), "Controller");
        QVERIFY(target->channel.startService(QHostAddress::LocalHost, 0));
        const auto endpoint = "localhost:" + QString::number(target->channel.port());
        QVERIFY(controller.channel.pairAddress(endpoint));
        QTRY_COMPARE(target->channel.controlRequests().size(), 1);
        QVERIFY(target->channel.decideControl(controller.channel.ownId(), true));
        QTRY_VERIFY(controller.channel.controlConnected());
        QVERIFY(!controller.channel.remoteAllowed());
        QVERIFY(controller.channel.setRemoteMode(true));
        QVERIFY(!controller.channel.remoteAction("chat", {{"text", "not yet allowed"}}));
        QVERIFY(target->channel.setRemotePermission(controller.channel.ownId(), true));
        QTRY_VERIFY(controller.channel.remoteAllowed());
        QVERIFY(controller.channel.setRemoteMode(true));
        QVERIFY(controller.channel.remoteMode());
        QVERIFY(!controller.channel.joined());
        QVERIFY(controller.session.setPttButtonHeld(true));
        QTRY_VERIFY(target->session.pttHeld());
        QVERIFY(!controller.channel.setRemoteMode(false));
        QVERIFY(controller.channel.remoteMode());
        QVERIFY(controller.session.setPttButtonHeld(false));
        QTRY_VERIFY(!target->session.pttHeld());
        QTRY_VERIFY(controller.channel.setRemoteMode(false));
        QCOMPARE(controller.channel.remoteView().size(), 0);
        target.reset();
        target = std::make_unique<Device>(dir.filePath("target"), "Target");
        QVERIFY(target->channel.startService(QHostAddress::LocalHost, 0));
        QCOMPARE(target->channel.controllers().size(), 1);
        QVERIFY(target->channel.controllers().first().toMap().value("remote").toBool());
    }

    void remoteViewUsesTargetJoinedChannelAndActionsAreAuthenticated() {
        QTemporaryDir dir;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        Device upstream(dir.filePath("upstream"), "Upstream", TlsIdentity::create(), [&] { return now; }), target(dir.filePath("target"), "Target"),
            controller(dir.filePath("controller"), "Controller");
        QVERIFY(upstream.channel.listen(QHostAddress::LocalHost));
        QVERIFY(upstream.channel.decide(target.channel.ownId(), true));
        QVERIFY(target.join(upstream.channel));
        QTRY_VERIFY(target.channel.chatReady());
        QVERIFY(target.channel.listen(QHostAddress::LocalHost));
        QVERIFY(target.join(target.channel)); QTRY_VERIFY(target.channel.chatReady());
        QVERIFY(target.join(upstream.channel)); QTRY_VERIFY(target.channel.chatReady());
        QVERIFY(controller.channel.pairAddress("127.0.0.1:" + QString::number(target.channel.port())));
        QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        QVERIFY(target.channel.decideControl(controller.channel.ownId(), true));
        QTRY_VERIFY(controller.channel.controlConnected());
        QVERIFY(target.channel.setRemotePermission(controller.channel.ownId(), true));
        QTRY_VERIFY(controller.channel.remoteAllowed());
        QVERIFY(controller.channel.setRemoteMode(true));

        const auto longText = QString(12000, QChar('x'));
        for (int i = 0; i < 30; ++i) {
            if (i % 10 == 0) now += 11000;
            QVERIFY(target.channel.sendChat(longText + QString::number(i)));
            QTRY_VERIFY(!target.channel.chatPending());
            QVERIFY2(target.channel.chatError().isEmpty(), qPrintable(target.channel.chatError()));
        }
        QTRY_VERIFY(controller.channel.remoteView().value("joined").toBool());
        QCOMPARE(controller.channel.remoteView().value("joinedHostId").toString(), upstream.channel.ownId());
        for (const auto& value : controller.channel.remoteView().value("channels").toList()) {
            const auto entry = value.toMap();
            QCOMPARE(entry.value("ownerName").toString(), entry.value("owned").toBool() ? QString("Target") : QString("Upstream"));
        }
        QTRY_VERIFY(controller.channel.remoteView().value("messages").toList().size() >= 30);
        QVERIFY(controller.channel.remoteView().value("messages").toList().size() * longText.size() > 300000);

        QVERIFY(target.channel.publishLevels({{target.channel.ownId(), 0.75}}));
        QTRY_COMPARE(controller.channel.remoteLevels().value(target.channel.ownId()).toDouble(), 0.75);
        QVERIFY(target.channel.publishLevels({})); QTRY_VERIFY(controller.channel.remoteLevels().isEmpty());
        QVERIFY(!target.channel.publishLevels({{target.channel.ownId(), 2.0}}));
        QImage pixels(300, 300, QImage::Format_ARGB32);
        quint32 random = 78416;
        for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x) {
            random = random * 1664525U + 1013904223U;
            pixels.setPixel(x, y, (random & 0xffffff) | 0x80000000U);
        }
        pixels.setText("Author", "Private test metadata");
        QByteArray original; QBuffer imageBuffer(&original); imageBuffer.open(QIODevice::WriteOnly); QVERIFY(pixels.save(&imageBuffer, "PNG"));
        now += 11000;
        QVERIFY(target.channel.sendChat("Remote image", original));
        QTRY_VERIFY(!target.channel.chatPending()); QVERIFY2(target.channel.chatError().isEmpty(), qPrintable(target.channel.chatError()));
        QTRY_COMPARE(controller.channel.remoteView().value("messages").toList(), target.channel.messages());
        const auto hash = memberMessages(target.channel).last().toMap().value("image").toMap().value("hash").toString();
        QVERIFY(controller.channel.requestImage(hash)); QVERIFY(controller.channel.requestImage(hash));
        QTRY_VERIFY(!controller.channel.imageSource(hash).isEmpty());
        QCOMPARE(controller.channel.imageSource(hash), target.channel.imageSource(hash));
        const auto png = QByteArray::fromBase64(controller.channel.imageSource(hash).mid(22).toLatin1());
        QVERIFY(png.size() > 65536); QVERIFY(!png.contains("Private test metadata"));
        QCOMPARE(QImage::fromData(png).pixelColor(10, 10).alpha(), 128);
        QVERIFY(!controller.channel.requestImage(QString(64, 'a')));

        now += 11000;
        QSignalSpy remoteSent(&controller.channel, &LocalChannel::remoteChatSent);
        QVERIFY(controller.channel.remoteAction("chat", {{"text", "sent as target"}}));
        QTRY_COMPARE(remoteSent.size(), 1);
        QCOMPARE(remoteSent.first().at(0).toString(), QString("sent as target"));
        QTRY_VERIFY(!memberMessages(target.channel).isEmpty()
            && memberMessages(target.channel).last().toMap().value("text").toString() == "sent as target");

        QCOMPARE(memberMessages(target.channel).last().toMap().value("sender").toString(), target.channel.ownId());
        QCOMPARE(memberMessages(target.channel).last().toMap().value("name").toString(), QString("Target"));
        QVERIFY(target.session.setMuted(false));
        QVERIFY(controller.channel.remoteAction("mute", {{"value", true}}));
        QTRY_VERIFY(target.session.muted());
        QVERIFY(controller.channel.remoteAction("deafen", {{"value", true}}));
        QTRY_VERIFY(target.session.deafened());
        QVERIFY(!controller.channel.remoteAction("ban", {{"id", upstream.channel.ownId()}}));
        QVERIFY(!controller.join(upstream.channel));
        QVERIFY(controller.channel.remoteAction("join", {{"hostId", target.channel.ownId()}}));
        QTRY_COMPARE(target.channel.joinedHostId(), target.channel.ownId());
        QTRY_VERIFY(target.channel.chatReady());
        QTRY_COMPARE(controller.channel.remoteView().value("joinedHostId").toString(), target.channel.ownId());
        QVERIFY(controller.channel.remoteAction("leave")); QTRY_VERIFY(!target.channel.joined());
        QVERIFY(target.channel.hosting());
        QTRY_VERIFY(!controller.channel.remoteView().value("joined").toBool());
        QVERIFY(controller.channel.remoteAction("join", {{"hostId", upstream.channel.ownId()}}));
        QTRY_VERIFY(target.channel.chatReady());
        QTRY_COMPARE(controller.channel.remoteView().value("joinedHostId").toString(), upstream.channel.ownId());
        QTRY_VERIFY(controller.channel.setRemoteMode(false));
        QVERIFY(upstream.channel.decide(controller.channel.ownId(), true));
        QVERIFY(controller.join(upstream.channel));
        QTRY_VERIFY(controller.channel.chatReady());
        QVERIFY(controller.channel.setRequestsAllowed(false));
        QTest::qWait(2200);
        QVERIFY(!controller.channel.controlConnected()); QVERIFY(controller.channel.joined());
        QVERIFY(controller.channel.setRemoteMode(true));
        QVERIFY(!controller.channel.joined());
        QTRY_VERIFY(controller.channel.controlConnected());
        QTRY_VERIFY(controller.channel.remoteAllowed());
        QTRY_VERIFY(!controller.channel.remoteView().value("historyLoading").toBool() && !controller.channel.remoteView().value("messages").toList().isEmpty());
        QVERIFY(controller.channel.remoteView().value("messages").toList().size() <= 40);
        QVERIFY(controller.channel.remoteView().value("hasOlderMessages").toBool());
        QVERIFY(controller.channel.requestImage(hash));
        QVERIFY(controller.channel.remoteAction("mute", {{"value", false}}));
        QVERIFY(target.channel.setRemotePermission(controller.channel.ownId(), false));
        QTRY_VERIFY(!controller.channel.remoteAllowed());
        QVERIFY(target.session.muted());
        QVERIFY(!controller.channel.remoteAction("mute", {{"value", false}}));
        QTRY_VERIFY(controller.channel.remoteView().isEmpty());
        QTest::qWait(100); QVERIFY(!controller.channel.controlConnected());
        QVERIFY(controller.channel.imageSource(hash).isEmpty());
        QVERIFY(!controller.channel.requestImage(hash));
        QVERIFY(controller.channel.remoteLevels().isEmpty());
    }

    void remoteViewRejectsOverflowingChatLifetimeBeforeMultiplication() {
        QTemporaryDir dir;
        Device controller(dir.filePath("controller"), "Controller");
        const auto targetIdentity = TlsIdentity::create();
        auto configuration = targetIdentity.configuration();
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        QSslServer server;
        server.setSslConfiguration(configuration);
        QVERIFY(server.listen(QHostAddress::LocalHost));

        QVERIFY(controller.channel.pairAddress("127.0.0.1:" + QString::number(server.serverPort())));
        QTRY_VERIFY(server.hasPendingConnections());
        auto* identitySocket = qobject_cast<QSslSocket*>(server.nextPendingConnection());
        QVERIFY(identitySocket);
        QTRY_VERIFY(identitySocket->bytesAvailable() > 4);
        identitySocket->readAll();
        identitySocket->write(wireFrame({{"type", "identity"}, {"version", 1}, {"name", "Target"},
            {"channel", "Target channel"}, {"hosting", true}}));

        QTRY_VERIFY(server.hasPendingConnections());
        auto* control = qobject_cast<QSslSocket*>(server.nextPendingConnection());
        QVERIFY(control);
        QTRY_VERIFY(control->bytesAvailable() > 4);
        control->readAll();
        control->write(wireFrame({{"type", "control"}, {"result", "accepted"}, {"remoteAllowed", true},
            {"capabilities", QJsonArray{"remote-control"}}}));
        QTRY_VERIFY(controller.channel.controlConnected());

        QJsonObject snapshot{
            {"ownId", controller.channel.ownId()}, {"name", "Target"}, {"avatar", "mossling"},
            {"channels", QJsonArray{}}, {"participants", QJsonArray{}}, {"chatMembers", QJsonArray{}},
            {"messages", QJsonArray{}}, {"joined", false}, {"chatReady", false}, {"chatPending", false},
            {"historyLoading", false}, {"hasOlderMessages", false}, {"hasNewerMessages", false},
            {"muted", false}, {"deafened", false}, {"available", true}, {"pushToTalk", false},
            {"joinedHostId", ""}, {"chatHostId", ""},
            {"chatLifetimeDays", QJsonValue(qint64(1) + (qint64(1) << 54))}
        };
        const auto bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        control->write(wireFrame({{"type", "remoteViewStart"}, {"id", id}, {"size", bytes.size()}})
            + wireFrame({{"type", "remoteViewChunk"}, {"id", id}, {"offset", 0},
                {"data", QString::fromLatin1(bytes.toBase64())}})
            + wireFrame({{"type", "remoteViewEnd"}, {"id", id}}));

        QTRY_VERIFY(!controller.channel.controlConnected());
        QVERIFY(controller.channel.remoteView().isEmpty());
    }

    void savedPrivateLifetimeRejectsOverflowBeforeMultiplication() {
        QTemporaryDir dir;
        const auto path = dir.filePath("channel.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QJsonObject settings{
            {"version", 1}, {"approved", QJsonArray{}}, {"attempts", QJsonObject{}},
            {"requestsAllowed", true}, {"security", QJsonObject{
                {"verifier", ""}, {"saved", QJsonObject{}},
                {"privateLifetimeDays", QJsonValue(qint64(1) + (qint64(1) << 54))}
            }}
        };
        QVERIFY(file.write(QJsonDocument(settings).toJson()) > 0);
        file.close();

        VoiceSession session;
        bool rejected = false;
        try {
            LocalChannel channel(session, path, TlsIdentity::create());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        QVERIFY(rejected);
    }

    void preapprovedConnectedParticipantGetsRemotePermissionOnlyAndModePersists() {
        QTemporaryDir dir;
        Device target(dir.filePath("target"), "Target"), controller(dir.filePath("controller"), "Controller");
        QVERIFY(target.channel.listen(QHostAddress::LocalHost));
        QVERIFY(target.channel.decide(controller.channel.ownId(), true));
        QVERIFY(controller.join(target.channel));
        QTRY_VERIFY(controller.channel.chatReady());
        QVERIFY(target.channel.setRemotePermission(controller.channel.ownId(), true));
        QVERIFY(controller.channel.pairAddress("127.0.0.1:" + QString::number(target.channel.port())));
        QTRY_VERIFY(controller.channel.controlConnected());
        QTRY_VERIFY(controller.channel.remoteAllowed());
        QVERIFY(!controller.channel.joined());
        QVERIFY(controller.channel.setRemoteMode(true));
        QVERIFY(!controller.channel.remoteAction("admin", {}));
        QTRY_VERIFY(controller.channel.setRemoteMode(false));
        QVERIFY(!controller.channel.remoteMode());
    }

    void remoteModeDoesNotCreatePhantomMembershipAndPresenceSurvivesControlledClock() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        Device target(dir.filePath("target"), "Target", TlsIdentity::create(), [&] { return now; });
        Device controller(dir.filePath("controller"), "Controller");
        QVERIFY(target.channel.listen(QHostAddress::LocalHost));
        QVERIFY(target.channel.decide(controller.channel.ownId(), true));
        QVERIFY(controller.channel.pairAddress("127.0.0.1:" + QString::number(target.channel.port())));
        QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        QVERIFY(target.channel.decideControl(controller.channel.ownId(), true));
        QTRY_VERIFY(controller.channel.controlConnected());
        QVERIFY(target.channel.setRemotePermission(controller.channel.ownId(), true));
        QTRY_VERIFY(controller.channel.remoteAllowed());
        QVERIFY(controller.channel.setRemoteMode(true));
        QVERIFY(!target.channel.joined());
        QVERIFY(target.channel.hosting());
        QTRY_VERIFY(!controller.channel.remoteView().isEmpty());
        QVERIFY(!controller.channel.remoteView().value("joined").toBool());
        QVERIFY(!controller.join(target.channel));
        QTRY_VERIFY(controller.channel.setRemoteMode(false));
        QVERIFY(controller.join(target.channel));
        QTRY_VERIFY(controller.channel.joined());
        QSignalSpy events(&controller.channel, &LocalChannel::channelEvent);
        QVERIFY(controller.session.setAvatar("mechanic"));
        QVERIFY(controller.session.setDeafened(true));
        QTRY_VERIFY(participant(controller.channel, controller.channel.ownId()).value("avatar").toString() == "mechanic");
        QTRY_VERIFY(participant(controller.channel, controller.channel.ownId()).value("deafened").toBool());
        now += 3600000;
        QTRY_VERIFY(participant(controller.channel, controller.channel.ownId()).value("sleeping").toBool());
        QVERIFY(controller.session.setMuted(false));
        squad::VoiceMixer encoder;
        std::array<float, 960> samples;
        for (size_t i = 0; i < samples.size(); ++i) samples[i] = float(0.1 * std::sin(2 * std::numbers::pi * 220 * i / 48000));
        const auto packet = encoder.encode(samples, 48000).first();
        QVERIFY(controller.channel.sendAudio(packet));
        QTRY_VERIFY(!participant(controller.channel, controller.channel.ownId()).value("sleeping").toBool());
        QVERIFY(target.channel.kick(controller.channel.ownId()));
        QTRY_VERIFY(events.count() > 0);
        QCOMPARE(events.last().first().toString(), QString("kick"));
    }
    void remotePttPersistsHeldStateAndNewOfflineReleaseWinsAfterReconnect() {
        QTemporaryDir dir;
        const auto targetIdentity = TlsIdentity::create(), controllerIdentity = TlsIdentity::create();
        auto target = std::make_unique<Device>(dir.filePath("target"), "Target", targetIdentity);
        auto controller = std::make_unique<Device>(dir.filePath("controller"), "Controller", controllerIdentity);
        QVERIFY(target->channel.startService(QHostAddress::LocalHost, 0));
        const auto port = target->channel.port();
        QVERIFY(target->session.setPushToTalk(true)); QVERIFY(target->session.setMuted(false));
        QVERIFY(controller->channel.pairAddress("127.0.0.1:" + QString::number(port)));
        QTRY_COMPARE(target->channel.controlRequests().size(), 1);
        QVERIFY(target->channel.decideControl(controller->channel.ownId(), true));
        QTRY_VERIFY(controller->channel.controlConnected());
        QVERIFY(controller->session.setPttButtonHeld(true)); QTRY_VERIFY(target->session.transmissionAllowed());
        controller.reset();
        QVERIFY(target->session.transmissionAllowed());
        controller = std::make_unique<Device>(dir.filePath("controller"), "Controller", controllerIdentity);
        QVERIFY(controller->session.pttInputHeld()); QTRY_VERIFY(controller->channel.controlConnected());
        QVERIFY(target->session.transmissionAllowed()); QVERIFY(target->channel.controlRequests().isEmpty());
        target.reset();
        QTRY_VERIFY(!controller->channel.controlConnected());
        QVERIFY(controller->session.setPttButtonHeld(false));
        QVERIFY(!controller->channel.clearControlTarget());
        Device other(dir.filePath("other"), "Other");
        QVERIFY(other.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(controller->channel.pairAddress("127.0.0.1:" + QString::number(other.channel.port())));
        QTRY_VERIFY(controller->channel.controlStatus().contains("target"));
        QCOMPARE(controller->channel.controlTargetName(), QString("Target"));
        QVERIFY(other.channel.controlRequests().isEmpty());
        target = std::make_unique<Device>(dir.filePath("target"), "Target", targetIdentity);
        QVERIFY(target->session.transmissionAllowed());
        QVERIFY(target->channel.startService(QHostAddress::LocalHost, port));
        QTRY_VERIFY_WITH_TIMEOUT(controller->channel.controlConnected(), 6000);
        QTRY_VERIFY(!target->session.transmissionAllowed());
        QVERIFY(target->channel.controlRequests().isEmpty());
        QTRY_VERIFY(controller->channel.clearControlTarget());
        QVERIFY(controller->channel.controlTargetName().isEmpty());
        QVERIFY(controller->session.setPushToTalk(true)); QVERIFY(controller->session.setMuted(false));
        QVERIFY(controller->session.setPttButtonHeld(true));
        QVERIFY(controller->session.transmissionAllowed()); QVERIFY(!target->session.transmissionAllowed());
    }
    void audioReadinessPresenceDoesNotChangeStoredMuteIntent() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), source(dir.filePath("source"), "Source"), observer(dir.filePath("observer"), "Observer");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(source.channel.ownId(), true));
        QVERIFY(host.channel.decide(observer.channel.ownId(), true));
        QVERIFY(source.join(host.channel));
        QVERIFY(observer.join(host.channel));
        QTRY_VERIFY(!participant(observer.channel, source.channel.ownId()).isEmpty());
        QVERIFY(source.session.setMuted(false));
        QVERIFY(source.session.setAudioReadiness(false, true));
        QTRY_VERIFY(participant(observer.channel, source.channel.ownId()).value("muted").toBool());
        QVERIFY(!source.session.muted());
        QVERIFY(source.session.setAudioReadiness(true, false));
        QTRY_VERIFY(participant(observer.channel, source.channel.ownId()).value("deafened").toBool());
        QVERIFY(!source.session.deafened());
    }
    void remotePttControlsVoiceInTheTargetsCurrentChannelOnly() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), target(dir.filePath("target"), "Microphone"),
            controller(dir.filePath("controller"), "Keyboard"), receiver(dir.filePath("receiver"), "Listener");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(target.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(host.channel.decide(target.channel.ownId(), true));
        QVERIFY(host.channel.decide(receiver.channel.ownId(), true));
        QVERIFY(target.join(host.channel)); QVERIFY(receiver.join(host.channel));
        QTRY_COMPARE(receiver.channel.participants().size(), 2);
        QVERIFY(target.session.setPushToTalk(true)); QVERIFY(target.session.setMuted(false));
        QVERIFY(controller.channel.pairAddress("127.0.0.1:" + QString::number(target.channel.port())));
        QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        QVERIFY(target.channel.decideControl(controller.channel.ownId(), true));
        QTRY_VERIFY(controller.channel.controlConnected());
        squad::VoiceMixer encoder, decoder;
        std::array<float, 960> signal;
        for (size_t i = 0; i < signal.size(); ++i) signal[i] = float(0.2 * std::sin(2 * std::numbers::pi * 440 * i / 48000));
        const auto packet = encoder.encode(signal, 48000).first();
        QSignalSpy heard(&receiver.channel, &LocalChannel::audioReceived), controlAudio(&controller.channel, &LocalChannel::audioReceived);
        QVERIFY(!target.channel.sendAudio(packet));
        QVERIFY(controller.session.setPttKeyHeld(true));
        QVERIFY(controller.session.pttInputHeld());
        QVERIFY(!controller.session.transmissionAllowed());
        QVERIFY(waitForEvents(&target.session, &VoiceSession::presenceChanged,
            [&] { return target.session.transmissionAllowed(); }));
        QTRY_VERIFY(!participant(receiver.channel, target.channel.ownId()).value("muted").toBool());
        QVERIFY(target.channel.sendAudio(packet)); QTRY_COMPARE(heard.size(), 1);
        QVERIFY(decoder.receive(target.channel.ownId(), heard.first().at(1).toByteArray(), 0));
        const auto decoded = decoder.render(48000, 0);
        QVERIFY(std::any_of(decoded.begin(), decoded.end(), [](auto sample) { return std::abs(sample) > 0.01; }));
        QVERIFY(!controller.channel.joined()); QVERIFY(!target.channel.hosting()); QCOMPARE(controlAudio.size(), 0);
        QVERIFY(target.session.setAudioTestActive(true)); QVERIFY(!target.channel.sendAudio(packet));
        QVERIFY(target.session.setAudioTestActive(false));
        QTRY_VERIFY(participant(receiver.channel, target.channel.ownId()).value("available").toBool());
        QVERIFY(target.channel.sendAudio(packet)); QTRY_COMPARE(heard.size(), 2);
        QVERIFY(controller.session.setPttKeyHeld(false)); QTRY_VERIFY(!target.session.transmissionAllowed());
        QVERIFY(!target.channel.sendAudio(packet)); QCOMPARE(heard.size(), 2);
    }
    void remotePttRejectsRolledBackStateAndRevocationSurvivesRestart_data() {
        QTest::addColumn<bool>("conflictingEqualRevision");
        QTest::newRow("older-revision") << false;
        QTest::newRow("same-revision-different-state") << true;
    }
    void remotePttRejectsRolledBackStateAndRevocationSurvivesRestart() {
        QFETCH(bool, conflictingEqualRevision);
        QTemporaryDir dir;
        qint64 now = 1000000;
        Device target(dir.filePath("target"), "Target", TlsIdentity::create(), [&] { return now; });
        const auto identity = TlsIdentity::create();
        auto controller = std::make_unique<Device>(dir.filePath("controller"), "Controller", identity);
        QVERIFY(target.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(target.session.setPushToTalk(true)); QVERIFY(target.session.setMuted(false));
        const auto endpoint = "127.0.0.1:" + QString::number(target.channel.port());
        QVERIFY(controller->channel.pairAddress(endpoint)); QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        QVERIFY(target.channel.decideControl(identity.id(), true)); QTRY_VERIFY(controller->channel.controlConnected());
        QFile settings(dir.filePath("controller.session")); QVERIFY(settings.open(QIODevice::ReadOnly));
        auto oldSettings = settings.readAll(); settings.close();
        QVERIFY(controller->session.setPttButtonHeld(true)); QTRY_VERIFY(target.session.transmissionAllowed());
        QVERIFY(controller->session.setPttButtonHeld(false)); QTRY_VERIFY(!target.session.transmissionAllowed());
        controller.reset();
        if (conflictingEqualRevision) {
            auto object = QJsonDocument::fromJson(oldSettings).object();
            object.insert("pttRevision", 2); object.insert("pttButtonHeld", true);
            oldSettings = QJsonDocument(object).toJson();
        }
        QVERIFY(settings.open(QIODevice::WriteOnly)); QCOMPARE(settings.write(oldSettings), oldSettings.size()); settings.close();
        controller = std::make_unique<Device>(dir.filePath("controller"), "Controller", identity);
        QTRY_VERIFY(controller->channel.controlStatus().contains("stale"));
        QVERIFY(!target.session.transmissionAllowed()); QVERIFY(target.channel.controlRequests().isEmpty());
        now += 60001;
        QVERIFY(controller->session.releasePttInput());
        QVERIFY(controller->channel.pairAddress(endpoint)); QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        QVERIFY(target.channel.decideControl(identity.id(), true)); QTRY_VERIFY(controller->channel.controlConnected());
        QVERIFY(controller->session.setPttButtonHeld(true)); QTRY_VERIFY(target.session.transmissionAllowed());
        QVERIFY(target.channel.decideControl(identity.id(), false));
        QTRY_VERIFY(controller->channel.controlStatus().contains("removed"));
        QVERIFY(!target.session.transmissionAllowed());
        controller.reset();
        controller = std::make_unique<Device>(dir.filePath("controller"), "Controller", identity);
        QVERIFY(controller->session.pttInputHeld()); QVERIFY(!controller->channel.clearControlTarget());
        QVERIFY(controller->session.releasePttInput()); QVERIFY(controller->channel.clearControlTarget());
        QVERIFY(target.channel.controllers().isEmpty()); QVERIFY(!target.session.transmissionAllowed());
    }
    void remotePttReconnectRejectsAChangedTargetIdentity() {
        QTemporaryDir dir;
        auto target = std::make_unique<Device>(dir.filePath("target"), "Target");
        Device controller(dir.filePath("controller"), "Controller");
        QVERIFY(target->channel.startService(QHostAddress::LocalHost, 0));
        const auto port = target->channel.port();
        QVERIFY(controller.channel.pairAddress("127.0.0.1:" + QString::number(port)));
        QTRY_COMPARE(target->channel.controlRequests().size(), 1);
        QVERIFY(target->channel.decideControl(controller.channel.ownId(), true));
        QTRY_VERIFY(controller.channel.controlConnected());
        target.reset(); QTRY_VERIFY(!controller.channel.controlConnected());
        Device replacement(dir.filePath("replacement"), "Target");
        QVERIFY(replacement.channel.startService(QHostAddress::LocalHost, port));
        QTRY_VERIFY_WITH_TIMEOUT(controller.channel.controlStatus().contains("different device identity"), 6000);
        QVERIFY(!controller.channel.controlConnected());
        QVERIFY(replacement.channel.controllers().isEmpty()); QVERIFY(replacement.channel.controlRequests().isEmpty());
        QVERIFY(!controller.channel.clearControlTarget());
    }
    void remotePttFailedPersistenceCannotApproveOrRevoke() {
        QTemporaryDir dir;
        Device target(dir.filePath("target"), "Target"), controller(dir.filePath("controller"), "Controller");
        QVERIFY(target.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(target.session.setPushToTalk(true)); QVERIFY(target.session.setMuted(false));
        QVERIFY(controller.channel.pairAddress("127.0.0.1:" + QString::number(target.channel.port())));
        QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        const auto path = dir.filePath("target.channel"), backup = path + ".backup";
        QVERIFY(QFile::rename(path, backup)); QVERIFY(QDir().mkdir(path));
        QVERIFY(!target.channel.decideControl(controller.channel.ownId(), true));
        QVERIFY(target.channel.controllers().isEmpty()); QVERIFY(!controller.channel.controlConnected());
        QVERIFY(QDir().rmdir(path)); QVERIFY(QFile::rename(backup, path));
        QVERIFY(target.channel.decideControl(controller.channel.ownId(), true)); QTRY_VERIFY(controller.channel.controlConnected());
        QVERIFY(controller.session.setPttButtonHeld(true)); QTRY_VERIFY(target.session.transmissionAllowed());
        QVERIFY(QFile::rename(path, backup)); QVERIFY(QDir().mkdir(path));
        QVERIFY(!target.channel.decideControl(controller.channel.ownId(), false));
        QVERIFY(!target.channel.setRemotePermission(controller.channel.ownId(), true));
        QVERIFY(!controller.channel.remoteAllowed());
        QCOMPARE(target.channel.controllers().size(), 1); QVERIFY(target.session.transmissionAllowed());
        QVERIFY(QDir().rmdir(path)); QVERIFY(QFile::rename(backup, path));
        QVERIFY(target.channel.decideControl(controller.channel.ownId(), false));
        QTRY_VERIFY(!controller.channel.controlConnected()); QVERIFY(!target.session.transmissionAllowed());
    }
    void remotePttRejectionBackoffAndChannelRightsAreIndependent() {
        QTemporaryDir dir;
        qint64 now = 1000000;
        Device target(dir.filePath("target"), "Target", TlsIdentity::create(), [&] { return now; });
        Device controller(dir.filePath("controller"), "Controller");
        QVERIFY(target.channel.listen(QHostAddress::LocalHost));
        QVERIFY(target.channel.setRequestsAllowed(false));
        const auto endpoint = "127.0.0.1:" + QString::number(target.channel.port());
        QVERIFY(controller.channel.pairAddress(endpoint)); QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        QVERIFY(target.channel.decideControl(controller.channel.ownId(), false));
        QTRY_VERIFY(controller.channel.controlStatus().contains("rejected"));
        QTRY_VERIFY(target.channel.controlRequests().isEmpty());
        QSignalSpy requests(&target.channel, &LocalChannel::requestsChanged);
        QVERIFY(controller.channel.pairAddress(endpoint));
        QTRY_VERIFY(controller.channel.controlStatus().contains("60 seconds"));
        QCOMPARE(requests.size(), 0); QVERIFY(target.channel.controlRequests().isEmpty());
        now += 60001;
        QVERIFY(controller.channel.pairAddress(endpoint)); QTRY_COMPARE(target.channel.controlRequests().size(), 1);
        QVERIFY(target.channel.decideControl(controller.channel.ownId(), true)); QTRY_VERIFY(controller.channel.controlConnected());
        QVERIFY(!controller.join(target.channel));
        QTRY_VERIFY(controller.channel.setRemoteMode(false));
        QVERIFY(controller.join(target.channel)); QTRY_VERIFY(controller.channel.status().contains("rejected"));
        QVERIFY(!controller.channel.joined()); QVERIFY(!controller.channel.controlConnected());
        QVERIFY(target.channel.decide(controller.channel.ownId(), true));
        QVERIFY(controller.join(target.channel)); QTRY_VERIFY(controller.channel.joined());
        QVERIFY(!controller.channel.controlConnected()); QTRY_COMPARE(controller.channel.participants().size(), 1);
        QVERIFY(target.channel.decide(controller.channel.ownId(), false));
        QTRY_VERIFY(!controller.channel.joined());
        QVERIFY(controller.channel.setRemoteMode(true)); QTRY_VERIFY(controller.channel.controlConnected());
        QVERIFY(target.channel.controlRequests().isEmpty());
    }
    void directPreapprovalReachesClientsWithoutAChannelAndSurvivesRestart() {
        QTemporaryDir dir;
        Device client(dir.filePath("client"), "Client"), stranger(dir.filePath("stranger"), "Client");
        QVERIFY(client.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(!client.channel.hosting());
        QVERIFY(client.channel.hosts().isEmpty());
        const auto identity = TlsIdentity::create();
        {
            Device host(dir.filePath("host"), "Host", identity);
            QVERIFY(host.channel.listen(QHostAddress::LocalHost));
            QVERIFY(host.channel.setRequestsAllowed(false));
            QVERIFY(host.channel.approveAddress("127.0.0.1:" + QString::number(client.channel.port())));
            QVERIFY(!host.channel.approveAddress("127.0.0.1:1"));
            QTRY_VERIFY(!host.channel.directBusy());
            QVERIFY2(host.channel.status().contains("permitted"), qPrintable(host.channel.status()));
            QVERIFY(host.channel.requests().isEmpty()); QVERIFY(client.channel.requests().isEmpty());
        }
        Device host(dir.filePath("host"), "Host", identity);
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        const auto endpoint = "127.0.0.1:" + QString::number(host.channel.port());
        QVERIFY(client.channel.joinAddress(endpoint));
        QTRY_VERIFY(client.channel.joined());
        QVERIFY(!client.channel.hosting());
        QCOMPARE(client.channel.joinedHostId(), host.channel.ownId());
        QVERIFY(host.channel.requests().isEmpty());
        QVERIFY(stranger.channel.joinAddress(endpoint));
        QTRY_VERIFY(stranger.channel.status().contains("rejected"));
        QVERIFY(!stranger.channel.joined());
        QVERIFY(client.channel.startHost());
        QVERIFY(client.channel.joined()); QVERIFY(client.channel.hosting());
        QVERIFY(client.channel.stopHost());
        QVERIFY(client.channel.port() > 0); QVERIFY(client.channel.joined());
    }
    void directJoinUsesDefaultPortAndTheSameAdmissionRules() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), client(dir.filePath("client"), "Client");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost, LocalChannel::defaultPort));
        QVERIFY(client.channel.joinAddress(" 127.0.0.1 "));
        QTRY_COMPARE(host.channel.requests().size(), 1);
        QCOMPARE(host.channel.requests().first().toMap().value("id").toString(), client.channel.ownId());
        QVERIFY(!client.channel.joined());
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QTRY_VERIFY(client.channel.joined());
        QVERIFY(host.channel.stopHost());
        QTRY_VERIFY(!client.channel.joined());
        QVERIFY(host.channel.startHost());
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.joined(), 5000);
        QVERIFY(client.channel.leave());
        QVERIFY(!client.channel.joined());
    }
    void directAddressAcceptsLocalhostHostnameAndRejectsMalformedName() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), client(dir.filePath("client"), "Client");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost, 0));
        QVERIFY(client.channel.joinAddress("LOCALHOST.:" + QString::number(host.channel.port())));
        QTRY_COMPARE(host.channel.requests().size(), 1);
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QTRY_VERIFY(client.channel.joined());
        const auto port = host.channel.port();
        QVERIFY(host.channel.stopHost());
        QTRY_VERIFY(!client.channel.joined());
        QVERIFY(host.channel.listen(QHostAddress::LocalHost, port));
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.joined(), 5000);
        QVERIFY(!client.channel.joinAddress("not a valid hostname:1234"));
        QVERIFY(!client.channel.joinAddress("999.999.999.999:1234"));
    }
    void directIpv6AndClosedChannelDoNotCreatePhantomMembership() {
        QTemporaryDir dir;
        Device target(dir.filePath("target"), "Target"), client(dir.filePath("client"), "Client");
        QVERIFY(target.channel.startService(QHostAddress::LocalHostIPv6, 0));
        const auto endpoint = "[::1]:" + QString::number(target.channel.port());
        QVERIFY(client.channel.joinAddress(endpoint));
        QTRY_VERIFY(!client.channel.directBusy());
        QVERIFY2(client.channel.status().contains("has not opened a channel"), qPrintable(client.channel.status()));
        QVERIFY(!client.channel.joined()); QVERIFY(target.channel.requests().isEmpty());
        QVERIFY(target.channel.startHost());
        QVERIFY(target.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.channel.joinAddress(endpoint));
        QTRY_VERIFY(client.channel.joined());
        QCOMPARE(client.channel.joinedHostId(), target.channel.ownId());
    }
    void directAddressPinRejectsReplacementAfterRestart() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        quint16 port = 0;
        {
            Device target(dir.filePath("target"), "Target"), owner(dir.filePath("owner"), "Owner", identity);
            QVERIFY(target.channel.startService(QHostAddress::LocalHost, 0)); port = target.channel.port();
            QVERIFY(owner.channel.approveAddress("127.0.0.1:" + QString::number(port)));
            QTRY_VERIFY(!owner.channel.directBusy());
            QVERIFY(owner.channel.status().contains("permitted"));
        }
        Device replacement(dir.filePath("replacement"), "Target"), owner(dir.filePath("owner"), "Owner", identity);
        QVERIFY(replacement.channel.startService(QHostAddress::LocalHost, port));
        QVERIFY(owner.channel.approveAddress("127.0.0.1:" + QString::number(port)));
        QTRY_VERIFY(!owner.channel.directBusy());
        QVERIFY2(owner.channel.status().contains("device identity"), qPrintable(owner.channel.status()));
        QVERIFY(owner.channel.listen(QHostAddress::LocalHost));
        QVERIFY(owner.channel.setRequestsAllowed(false));
        QVERIFY(replacement.join(owner.channel));
        QTRY_VERIFY(replacement.channel.status().contains("rejected"));
        QVERIFY(owner.channel.requests().isEmpty());
    }
    void directInvalidInputFailureAndCancellationLeaveNoApproval() {
        QTemporaryDir dir;
        Device device(dir.filePath("device"), "Device"), client(dir.filePath("client"), "Client");
        for (const auto& text : {"", "example..org", "127.0.0.1:", "127.0.0.1:0", "127.0.0.1:65536", "127.0.0.1:+80",
                "[::1", "[::1]x", "[127.0.0.1]:42", "0.0.0.0", "::", "255.255.255.255", "239.1.1.1", "https://127.0.0.1"}) {
            QVERIFY2(!device.channel.approveAddress(QString::fromLatin1(text)), text);
            QVERIFY(!device.channel.directBusy());
        }
        QTcpServer unused;
        QVERIFY(unused.listen(QHostAddress::LocalHost, 0)); const auto port = unused.serverPort(); unused.close();
        QVERIFY(device.channel.approveAddress("127.0.0.1:" + QString::number(port)));
        QTRY_VERIFY(!device.channel.directBusy());
        QVERIFY(device.channel.status().contains("failed"));
        QVERIFY(client.channel.startService(QHostAddress::LocalHost, 0));
        QVERIFY(device.channel.approveAddress("127.0.0.1:" + QString::number(client.channel.port())));
        QVERIFY(device.channel.leave()); QVERIFY(!device.channel.directBusy());
        QVERIFY(device.channel.listen(QHostAddress::LocalHost));
        QVERIFY(device.channel.setRequestsAllowed(false));
        QVERIFY(client.join(device.channel));
        QTRY_VERIFY(client.channel.status().contains("rejected"));
    }
    void directTimeoutAndFailedPersistenceDoNotApprove() {
        QTemporaryDir dir;
        Device client(dir.filePath("client"), "Client");
        QVERIFY(client.channel.startService(QHostAddress::LocalHost, 0));
        VoiceSession session(dir.filePath("owner.session"));
        LocalChannel owner(session, dir.filePath("missing/owner.channel"), TlsIdentity::create());
        QVERIFY(owner.approveAddress("127.0.0.1:" + QString::number(client.channel.port())));
        QTRY_VERIFY(!owner.directBusy());
        QVERIFY2(owner.status().contains("could not be saved"), qPrintable(owner.status()));
        QVERIFY(QDir().mkdir(dir.filePath("missing")));
        QVERIFY(owner.listen(QHostAddress::LocalHost));
        QVERIFY(owner.setRequestsAllowed(false));
        QVERIFY(client.join(owner));
        QTRY_VERIFY(client.channel.status().contains("rejected"));
        QVERIFY(owner.requests().isEmpty());
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
        QVERIFY(owner.approveAddress("127.0.0.1:" + QString::number(silent.serverPort())));
        QTRY_VERIFY_WITH_TIMEOUT(!owner.directBusy(), 6500);
        QVERIFY2(owner.status().contains("not reachable in time"), qPrintable(owner.status()));
        QVERIFY(owner.requests().isEmpty());
    }
    void localHistoryFailureUsesSelectedLanguage_data() {
        QTest::addColumn<QString>("language");
        VoiceSession available;
        for (const auto& entry : available.languages()) {
            const auto language = entry.toMap().value("code").toString();
            QTest::newRow(qPrintable(language)) << language;
        }
    }
    void localHistoryFailureUsesSelectedLanguage() {
        QFETCH(QString, language);
        QTranslator catalog;
        if (language != "en") {
            QVERIFY(catalog.load(":/i18n/squadspeak_" + language + ".qm"));
            QVERIFY(QCoreApplication::installTranslator(&catalog));
        }
        const auto remove = qScopeGuard([&] { QCoreApplication::removeTranslator(&catalog); });
        QTemporaryDir directory;
        const auto path = directory.filePath("host");
        const auto identity = TlsIdentity::create();
        Device host(path, "Host", identity);
        QFile history(path + ".channel.chat");
        QVERIFY(history.open(QIODevice::WriteOnly));
        const auto context = "SquadSpeak/chat-store/v1/" + host.channel.ownId().toUtf8();
        const auto encrypted = TlsIdentity::seal("{}", identity.deriveKey(context, "history encryption"), context);
        QCOMPARE(history.write(encrypted), encrypted.size()); history.close();
        QVERIFY(!host.channel.listen(QHostAddress::LocalHost));
        const auto expected = LocalChannel::tr("Invalid chat history.");
        if (language != "en") QVERIFY(expected != "Invalid chat history.");
        QCOMPARE(host.channel.status(), expected);
        QVERIFY(!host.channel.hosting());
        QVERIFY(history.remove());
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.hosting());
        QVERIFY(host.channel.sendSystemMessage("Recovered"));
    }
    void identityFailuresUseSelectedLanguage_data() {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<QString>("message");
        const std::array cases{
            std::pair{"large-key", "Invalid device identity size."},
            std::pair{"invalid-key", "The stored device identity is invalid or expired."},
            std::pair{"seal", "Encryption failed."},
            std::pair{"open", "Invalid encrypted data."},
            std::pair{"tampered", "Data was modified or belongs to another device identity."},
            std::pair{"password", "Invalid password size."},
#ifdef Q_OS_WIN
            std::pair{"file", "Identity files require Linux or macOS. Use the Windows credential store."}
#else
            std::pair{"path", "Invalid identity file path."},
            std::pair{"parent", "Identity file needs an existing private parent directory."},
            std::pair{"missing", "The original identity file is missing. Restore it or use a new server storage prefix."},
            std::pair{"permissions", "Identity storage must belong to this account: file mode 0600 or 0400, parent not writable by others."},
            std::pair{"empty-file", "Invalid device identity size."},
            std::pair{"link", "Identity file could not be opened; symbolic links are not allowed."}
#endif
        };
        VoiceSession available;
        for (const auto& entry : available.languages()) {
            const auto language = entry.toMap().value("code").toString();
            for (const auto& [scenario, message] : cases)
                QTest::newRow(qPrintable(language + '/' + scenario)) << language << QString(scenario) << QString(message);
        }
    }
    void identityFailuresUseSelectedLanguage() {
        QFETCH(QString, language); QFETCH(QString, scenario); QFETCH(QString, message);
        QTemporaryDir directory;
        VoiceSession profile(directory.filePath("profile"));
        QTranslator catalog;
        if (language != "en") {
            QVERIFY(catalog.load(":/i18n/squadspeak_" + language + ".qm"));
            QVERIFY(QCoreApplication::installTranslator(&catalog));
        }
        const auto remove = qScopeGuard([&] { QCoreApplication::removeTranslator(&catalog); });
        const auto expected = QCoreApplication::translate("TlsIdentity", message.toUtf8().constData());
        if (language != "en") QVERIFY(expected != message);
        const auto key = TlsIdentity::newKey();
        const auto path = directory.filePath("identity");
        if (scenario == "permissions" || scenario == "empty-file" || scenario == "link") {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            if (scenario != "empty-file") QCOMPARE(file.write("invalid"), 7);
            QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | (scenario == "permissions" ? QFileDevice::ReadGroup : QFileDevice::Permissions{})));
            file.close();
            if (scenario == "link") QVERIFY(QFile::link(path, directory.filePath("link")));
        }
        QString failure;
        try {
            if (scenario == "large-key") (void)TlsIdentity::fromPem(QByteArray(16385, 'x'));
            else if (scenario == "invalid-key") (void)TlsIdentity::fromPem("invalid");
            else if (scenario == "seal") (void)TlsIdentity::seal("test", {}, "context");
            else if (scenario == "open") (void)TlsIdentity::open("invalid", key, "context");
            else if (scenario == "tampered") (void)TlsIdentity::open(TlsIdentity::seal("test", key, "context"), key, "different context");
            else if (scenario == "password") (void)TlsIdentity::passwordHash({}, key);
            else if (scenario == "path") (void)TlsIdentity::loadFile({}, false);
            else if (scenario == "parent") (void)TlsIdentity::loadFile(directory.filePath("missing/identity"), false);
            else if (scenario == "link") (void)TlsIdentity::loadFile(directory.filePath("link"), false);
            else (void)TlsIdentity::loadFile(path, false);
        } catch (const std::exception& error) { failure = QString::fromUtf8(error.what()); }
        QCOMPARE(failure, expected);
        if (scenario == "missing" || scenario == "parent") QVERIFY(!QFile::exists(path));
        QCOMPARE(TlsIdentity::open(TlsIdentity::seal("still usable", key, "context"), key, "context"), QByteArray("still usable"));
    }
    void identityRoundTripAndInvalidSecrets() {
        const auto first = TlsIdentity::create();
        const auto second = TlsIdentity::create();
        QCOMPARE(first.id().size(), 64);
        QVERIFY(first.id() != second.id());
        QCOMPARE(first.id(), TlsIdentity::peerId(first.configuration().localCertificate()));
        auto copied = first;
        QCOMPARE(copied.id(), first.id());
        copied = second;
        QCOMPARE(copied.id(), second.id());
        QCOMPARE(first.id(), TlsIdentity::peerId(first.configuration().localCertificate()));
        QCOMPARE(TlsIdentity::fromPem(first.pem()).id(), first.id());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)TlsIdentity::fromPem("invalid"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)TlsIdentity::fromPem(first.configuration().localCertificate().toPem()
            + second.pem().mid(second.pem().indexOf("-----BEGIN PRIVATE KEY"))));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, (void)TlsIdentity::fromPem(QByteArray(16385, 'a')));
    }
    void twoHostsSeveralClientsAndTemporaryMute() {
        QTemporaryDir dir;
        Device a(dir.filePath("a"), "Anna"), b(dir.filePath("b"), "Bert"),
               c(dir.filePath("c"), "Chris"), d(dir.filePath("d"), "Dana");
        QVERIFY(a.channel.listen(QHostAddress::LocalHost));
        QVERIFY(b.channel.listen(QHostAddress::LocalHost));
        QVERIFY(a.channel.setChannelName("Wohnzimmer"));
        QVERIFY(b.channel.setChannelName("Küche"));
        QVERIFY(a.join(b.channel));
        QTRY_COMPARE_WITH_TIMEOUT(b.channel.requests().size(), 1, 5000);
        QVERIFY(b.channel.decide(a.channel.ownId(), true));
        QTRY_VERIFY(a.channel.joined());
        QVERIFY(a.channel.hosting());
        QVERIFY(a.channel.decide(b.channel.ownId(), true));
        QVERIFY(a.channel.decide(c.channel.ownId(), true));
        QVERIFY(a.channel.decide(d.channel.ownId(), true));
        QVERIFY(a.channel.setRequestsAllowed(false));
        QVERIFY(b.join(a.channel)); QVERIFY(c.join(a.channel)); QVERIFY(d.join(a.channel));
        QTRY_COMPARE(c.channel.participants().size(), 3);
        QTRY_COMPARE(d.channel.participants().size(), 3);
        QCOMPARE(a.channel.participants().size(), 1);
        QVERIFY(a.channel.requests().isEmpty());
        QVERIFY(c.session.setUserName("Neuer Name"));
        QTRY_COMPARE(participant(b.channel, c.channel.ownId()).value("name").toString(), QString("Neuer Name"));
        QVERIFY(c.session.setMuted(false));
        QTRY_COMPARE(participant(d.channel, c.channel.ownId()).value("muted").toBool(), false);
        QSignalSpy receivedB(&b.channel, &LocalChannel::audioReceived);
        QSignalSpy receivedD(&d.channel, &LocalChannel::audioReceived);
        QSignalSpy receivedA(&a.channel, &LocalChannel::audioReceived);
        QSignalSpy receivedC(&c.channel, &LocalChannel::audioReceived);
        squad::VoiceMixer encoder, decoder;
        std::array<float, 960> signal;
        for (size_t i = 0; i < signal.size(); ++i) signal[i] = float(0.2 * std::sin(2 * std::numbers::pi * 440 * i / 48000));
        const auto voice = encoder.encode(signal, 48000).first();
        QVERIFY(c.channel.sendAudio(voice));
        QTRY_COMPARE(receivedB.size(), 1); QTRY_COMPARE(receivedD.size(), 1);
        QCOMPARE(receivedD.first().at(0).toString(), c.channel.ownId());
        QCOMPARE(receivedD.first().at(1).toByteArray(), voice);
        QVERIFY(decoder.receive(c.channel.ownId(), receivedD.first().at(1).toByteArray(), 0));
        const auto rendered = decoder.render(48000, 0);
        QVERIFY(std::any_of(rendered.begin(), rendered.end(), [](auto sample) { return std::abs(sample) > 0.01; }));
        QCOMPARE(receivedC.size(), 0); QCOMPARE(receivedA.size(), 0);
        QVERIFY(c.session.setAudioTestActive(true));
        QTRY_COMPARE(participant(d.channel, c.channel.ownId()).value("available").toBool(), false);
        QVERIFY(!c.channel.sendAudio(voice));
        QVERIFY(c.session.setAudioTestActive(true));
        QVERIFY(!c.channel.sendAudio(voice));
        QVERIFY(c.session.setAudioTestActive(false));
        QTRY_COMPARE(participant(d.channel, c.channel.ownId()).value("available").toBool(), true);
        QVERIFY(c.channel.sendAudio(voice));
        QTRY_COMPARE(receivedD.size(), 2);
        QCOMPARE(receivedD.last().at(1).toByteArray(), voice);
        QVERIFY(c.session.setAudioTestActive(true));
        QVERIFY(c.session.setMuted(true));
        QVERIFY(c.session.setAudioTestActive(false));
        QVERIFY(!c.channel.sendAudio(voice));
        QVERIFY(c.session.setMuted(false));
        QVERIFY(!c.channel.sendAudio(QByteArray(8193, 'a')));
        QVERIFY(c.channel.leave());
        QTRY_COMPARE(d.channel.participants().size(), 2);
    }
    void admissionBackoffPersistenceAndPreapproval() {
        QTemporaryDir dir;
        qint64 now = 1000000;
        const auto identity = TlsIdentity::create();
        Device client(dir.filePath("client"), "Gleicher Name");
        Device stranger(dir.filePath("stranger"), "Gleicher Name");
        {
            Device host(dir.filePath("host"), "Host", identity, [&] { return now; });
            QVERIFY(host.channel.listen(QHostAddress::LocalHost));
            QVERIFY(client.join(host.channel));
            QTRY_COMPARE(host.channel.requests().size(), 1);
            QVERIFY(host.channel.decide(client.channel.ownId(), false));
            QTRY_VERIFY(client.channel.status().contains("rejected"));
            QTRY_VERIFY(host.channel.requests().isEmpty());
            QSignalSpy pendingChanges(&host.channel, &LocalChannel::requestsChanged);
            QVERIFY(client.join(host.channel));
            QTRY_VERIFY(client.channel.status().contains("60 seconds"));
            QTRY_VERIFY(host.channel.requests().isEmpty());
            QVERIFY(!client.channel.joined());
            QCOMPARE(pendingChanges.size(), 0);
            now += 60001;
            QVERIFY(client.join(host.channel));
            QTRY_COMPARE(host.channel.requests().size(), 1);
            QVERIFY(host.channel.decide(client.channel.ownId(), true));
            QTRY_VERIFY(client.channel.joined());
            QVERIFY(host.channel.setRequestsAllowed(false));
        }
        Device host(dir.filePath("host"), "Host neu", identity, [&] { return now; });
        QVERIFY(!host.channel.requestsAllowed());
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(client.join(host.channel)); QTRY_VERIFY(client.channel.joined());
        QVERIFY(stranger.join(host.channel));
        QTRY_VERIFY(stranger.channel.status().contains("rejected"));
        QVERIFY(host.channel.requests().isEmpty());
        QVERIFY(!stranger.channel.joined());
        QVERIFY(host.channel.decide(stranger.channel.ownId(), true));
        QVERIFY(stranger.join(host.channel)); QTRY_VERIFY(stranger.channel.joined());
        QVERIFY(host.channel.decide(client.channel.ownId(), false));
        QTRY_VERIFY(!client.channel.joined());
    }
    void pinningReconnectAndManualLeave() {
        QTemporaryDir dir;
        Device host(dir.filePath("host"), "Host"), client(dir.filePath("client"), "Client"), other(dir.filePath("other"), "Host");
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(client.channel.ownId(), true));
        QVERIFY(client.channel.join(other.channel.ownId(), "127.0.0.1", host.channel.port()));
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.status().contains("failed"), 5000);
        QVERIFY(!client.channel.joined()); QVERIFY(host.channel.requests().isEmpty());
        QVERIFY(client.session.setMuted(false));
        QVERIFY(client.join(host.channel)); QTRY_VERIFY(client.channel.joined());
        const auto port = host.channel.port();
        QVERIFY(host.channel.stopHost()); QTRY_VERIFY(!client.channel.joined());
        QVERIFY(host.channel.listen(QHostAddress::LocalHost, port));
        QTRY_VERIFY_WITH_TIMEOUT(client.channel.joined(), 5000);
        QVERIFY(client.session.transmissionAllowed());
        QTRY_COMPARE(client.channel.participants().size(), 1);
        QVERIFY(client.channel.leave());
        QVERIFY(host.channel.stopHost()); QVERIFY(host.channel.listen(QHostAddress::LocalHost, port));
        QTest::qWait(2200);
        QVERIFY(!client.channel.joined());
        QVERIFY(client.channel.joinedHostId().isEmpty());
        QVERIFY(!client.channel.join(host.channel.ownId(), "0.0.0.0", port));
        // Public unicast addresses are accepted. Do not contact a real service.
        QVERIFY(client.channel.join(host.channel.ownId(), "192.0.2.1", port));
        QVERIFY(client.channel.leave());
        QVERIFY(!client.channel.join("bad", "127.0.0.1", port));
    }
    void duplicateDeviceConnectionDoesNotFightReconnect() {
        QTemporaryDir dir;
        const auto identity = TlsIdentity::create();
        Device host(dir.filePath("host"), "Host"), oldClient(dir.filePath("old"), "Alt", identity),
               newClient(dir.filePath("new"), "Neu", identity);
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QVERIFY(host.channel.decide(identity.id(), true));
        QVERIFY(oldClient.join(host.channel)); QTRY_VERIFY(oldClient.channel.joined());
        QVERIFY(newClient.join(host.channel)); QTRY_VERIFY(newClient.channel.joined());
        QTRY_VERIFY(!oldClient.channel.joined());
        QTest::qWait(2300);
        QVERIFY(newClient.channel.joined());
        QVERIFY(!oldClient.channel.joined());
        QCOMPARE(newClient.channel.participants().size(), 1);
    }
#if QT_CONFIG(process)
    void sixtyFourClientsHaveOneConsistentRosterAndAudioFanout_data() {
        QTest::addColumn<int>("callbackDelayMs");
        QTest::addColumn<int>("sourceChannels");
        QTest::newRow("continuous-mono") << 0 << 1;
        QTest::newRow("continuous-stereo") << 0 << 2;
        QTest::newRow("delayed-audio-callbacks-mono") << 45 << 1;
        QTest::newRow("delayed-audio-callbacks-stereo") << 45 << 2;
    }
    void sixtyFourClientsHaveOneConsistentRosterAndAudioFanout() {
        // The hosted macOS network launcher intentionally resets environment
        // logging rules. Enable bounded diagnostics inside this test process.
        QLoggingCategory::setFilterRules(QStringLiteral("squadspeak.media.sender.debug=true"));
        const auto pacing = qEnvironmentVariable("SQUAD_SOAK_PACING", "realtime");
        QVERIFY2(pacing == "realtime" || pacing == "delivery",
            "SQUAD_SOAK_PACING must be realtime or delivery");
        const bool completeDelivery = pacing == "delivery";
        const bool deliveryPaced = instrumentedTiming || completeDelivery;
        QFETCH(int, callbackDelayMs);
        QFETCH(int, sourceChannels);
        QVERIFY(sourceChannels == 1 || sourceChannels == 2);
        const int participantCount = qEnvironmentVariableIsSet("SQUAD_SOAK_CLIENTS")
            ? qEnvironmentVariable("SQUAD_SOAK_CLIENTS").toInt() : 64;
        QVERIFY2(participantCount >= 8 && participantCount <= 256,
            "SQUAD_SOAK_CLIENTS must be between 8 and 256 for this local fixture");
        bool speakersOk = false;
        const int speakers = qEnvironmentVariableIsSet("SQUAD_SOAK_SPEAKERS")
            ? qEnvironmentVariable("SQUAD_SOAK_SPEAKERS").toInt(&speakersOk) : 4;
        QVERIFY2(!qEnvironmentVariableIsSet("SQUAD_SOAK_SPEAKERS") || speakersOk,
            "SQUAD_SOAK_SPEAKERS must be an integer");
        QVERIFY2(speakers >= 2 && speakers <= participantCount, "SQUAD_SOAK_SPEAKERS must be between 2 and the client count");
        QElapsedTimer elapsed; elapsed.start();
        QTemporaryDir dir;
        // Each process has its own native ICE poll loop and RTC worker pool.
        // Keep the relay separate and distribute simulated remote receivers.
        std::array<QProcess, 2> receivers;
        std::array<QByteArray, 2> receiverBuffers;
        std::array<QJsonObject, 2> receiverStates;
        std::array<int, 2> receiverFirst{}, receiverCount{};
        std::vector<int> audioCounts(participantCount), missingCounts(participantCount);
        bool receiverOutputValid = true;
        QObject receiverEvents;
        QProcess host;
        QByteArray readiness;
        QTimer receiverPoll;
        std::vector<std::unique_ptr<Device>> clients;
        const auto stopProcesses = qScopeGuard([&] {
            receiverPoll.stop();
            // Keep Qt servicing sockets while native media workers stop. Close
            // receivers before the relay, including when setup fails early.
            for (auto& process : receivers)
                if (process.state() != QProcess::NotRunning) process.closeWriteChannel();
            bool allExited = waitForEvents([&] {
                return std::all_of(receivers.begin(), receivers.end(), [](const auto& process) {
                    return process.state() == QProcess::NotRunning;
                });
            }, 5000);
            for (size_t group = 0; group < receivers.size(); ++group) {
                if (!receiverCount[group]) continue;
                auto& process = receivers[group];
                if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(5000); }
                qInfo().noquote() << "Receiver process" << group << "diagnostics:\n" << process.readAllStandardError();
                allExited &= process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
            }
            host.closeWriteChannel();
            const bool hostStopped = waitForEvents(&host, &QProcess::stateChanged,
                [&] { return host.state() == QProcess::NotRunning; }, 5000);
            if (!hostStopped) { host.kill(); host.waitForFinished(5000); }
            qInfo().noquote() << "Host process diagnostics:\n" << host.readAllStandardError();
            QVERIFY2(allExited, "Receiver processes must exit successfully");
            QVERIFY(hostStopped); QCOMPARE(host.exitStatus(), QProcess::NormalExit); QCOMPARE(host.exitCode(), 0);
        });
        for (size_t group = 0; group < receivers.size(); ++group) {
            receiverFirst[group] = speakers + (participantCount - speakers) * int(group) / 2;
            receiverCount[group] = speakers + (participantCount - speakers) * int(group + 1) / 2 - receiverFirst[group];
            if (!receiverCount[group]) continue;
            connect(&receivers[group], &QProcess::readyReadStandardOutput, &receiverEvents, [&, group] {
                auto& buffer = receiverBuffers[group];
                buffer += receivers[group].readAllStandardOutput();
                while (buffer.contains('\n')) {
                    const auto end = buffer.indexOf('\n');
                    const auto document = QJsonDocument::fromJson(buffer.left(end));
                    buffer.remove(0, end + 1);
                    const auto report = document.object();
                    const auto members = report.value("clients").toArray();
                    if (!document.isObject() || !report.value("ok").toBool() || members.size() != receiverCount[group]) {
                        receiverOutputValid = false; continue;
                    }
                    receiverStates[group] = report;
                    for (qsizetype i = 0; i < members.size(); ++i) {
                        const auto member = members[i].toObject();
                        const auto index = size_t(receiverFirst[group] + i);
                        audioCounts[index] = member.value("heard").toInt();
                        missingCounts[index] = member.value("missing").toInt();
                    }
                }
            });
            receivers[group].start(QCoreApplication::applicationFilePath(), {"--capacity-receivers", dir.path(),
                QString::number(receiverFirst[group]), QString::number(receiverCount[group])});
            QVERIFY(receivers[group].waitForStarted(5000));
        }
        QVERIFY(waitForEvents([&] {
            for (size_t group = 0; group < receivers.size(); ++group)
                if (receiverStates[group].value("clients").toArray().size() != receiverCount[group]) return false;
            return receiverOutputValid;
        }, 30000));
        int commandSerial = 0;
        const auto writeReceivers = [&](QJsonObject command) {
            command.insert("serial", ++commandSerial);
            const auto bytes = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
            for (size_t group = 0; group < receivers.size(); ++group)
                if (receiverCount[group] && receivers[group].write(bytes) != bytes.size()) return -1;
            return commandSerial;
        };
        const auto commandReceivers = [&](QJsonObject command) {
            const auto serial = writeReceivers(std::move(command));
            if (serial < 0) return false;
            return waitForEvents([&] {
                for (size_t group = 0; group < receivers.size(); ++group)
                    if (receiverCount[group] && receiverStates[group].value("serial").toInt() < serial) return false;
                return receiverOutputValid;
            }, 10000);
        };
        const auto allReceivers = [&](auto matches) {
            for (const auto& report : receiverStates)
                for (const auto member : report.value("clients").toArray())
                    if (!matches(member.toObject())) return false;
            return receiverOutputValid;
        };
        connect(&receiverPoll, &QTimer::timeout, &receiverEvents, [&] {
            receiverOutputValid &= writeReceivers({{"operation", "stats"}}) >= 0;
        });
        receiverPoll.start(deliveryPaced ? 10 : 100);
        QJsonArray approved;
        const auto firstIdentity = TlsIdentity::create();
        for (int i = 0; i < speakers; ++i) {
            clients.push_back(std::make_unique<Device>(dir.filePath(QString::number(i)), QString("Member %1").arg(i),
                i == 0 ? firstIdentity : TlsIdentity::create()));
            QVERIFY(clients.back()->session.setMuted(false));
            approved.append(clients.back()->channel.ownId());
        }
        for (const auto& report : receiverStates)
            for (const auto member : report.value("clients").toArray()) approved.append(member.toObject().value("id"));
        Device overflow(dir.filePath("overflow"), "Text member");
        auto replacement = std::make_unique<Device>(dir.filePath("replacement"), "Member 0", firstIdentity);
        approved.append(overflow.channel.ownId());
        QFile configuration(dir.filePath("host.channel"));
        QVERIFY(configuration.open(QIODevice::WriteOnly));
        const auto settings = QJsonDocument(QJsonObject{{"version", 1}, {"approved", approved},
            {"attempts", QJsonObject{}}, {"requestsAllowed", true}}).toJson(QJsonDocument::Compact);
        QCOMPARE(configuration.write(settings), settings.size()); configuration.close();
        connect(&host, &QProcess::readyReadStandardOutput, &host, [&] { readiness += host.readAllStandardOutput(); });
        host.start(QCoreApplication::applicationFilePath(), {"--capacity-host", dir.filePath("host")});
        QVERIFY(host.waitForStarted(5000));
        QVERIFY(waitForEvents(&host, &QProcess::readyReadStandardOutput, [&] { return readiness.contains('\n'); }, 10000));
        const auto endpoint = QJsonDocument::fromJson(readiness.trimmed()).object();
        const auto hostId = endpoint.value("id").toString();
        const auto hostPort = quint16(endpoint.value("port").toInt());
        QCOMPARE(hostId.size(), 64); QVERIFY(hostPort != 0);
        const auto joinHost = [&](Device& client) {
            return client.channel.join(hostId, "127.0.0.1", hostPort);
        };
        // Real devices generate their identities on independent event loops.
        // Finish this process's CPU-bound setup before starting network timers.
        for (const auto& client : clients) QVERIFY(joinHost(*client));
        QVERIFY(commandReceivers({{"operation", "join"}, {"host", hostId}, {"port", hostPort}}));
        for (const auto& client : clients) QTRY_COMPARE_WITH_TIMEOUT(
            client->channel.participants().size(), participantCount, 30000);
        QTRY_VERIFY(allReceivers([&](const QJsonObject& member) {
            return member.value("joined").toBool() && member.value("members").toInt() == participantCount;
        }));
        // Check admission before the load phase. Afterwards measured pressure
        // may correctly reject a new participant while existing speech works.
        QVERIFY(joinHost(overflow));
        // The host is already servicing 64 TLS peers. On a loaded native
        // runner, accept plus the new TLS handshake exceeds QtTest's 5 s
        // default although the participant is correctly admitted.
        QTRY_VERIFY_WITH_TIMEOUT(overflow.channel.joined(), 30000);
        QTRY_COMPARE(clients.back()->channel.participants().size(), participantCount + 1);
        QTRY_VERIFY(overflow.channel.chatReady());
        QVERIFY(overflow.channel.sendChat("Text and media remain available to a new participant."));
        QTRY_VERIFY(!memberMessages(clients.back()->channel).isEmpty());
        QTRY_VERIFY(overflow.channel.screenView().value("available").toBool());
        QVERIFY(overflow.channel.watchScreen(hostId, true));
        QTRY_COMPARE(overflow.channel.screenView().value("tier").toInt(), 1);
        QVERIFY(overflow.channel.leave());
        QVERIFY(overflow.channel.watchScreen(hostId, false));
        QTRY_COMPARE(clients.back()->channel.participants().size(), participantCount);
        // Use the same adaptive receive delay as the desktop audio path.
        std::array<squad::VoiceMixer, 2> playback{squad::VoiceMixer(2, 40), squad::VoiceMixer(2, 40)};
        std::array<int, 2> audibleFrames{};
        int renderedFrames = 0;
        qint64 lastInputMs = 0, lastOutputMs = 0, maxInputGapMs = 0, maxOutputGapMs = 0;
        qint64 maxSendUs = 0, maxRenderUs = 0;
        bool validAudio = true;
        QElapsedTimer sustained;
        QObject reception; // Disconnect stack captures before their state is destroyed.
        for (size_t i = 0; i < clients.size(); ++i) {
            connect(&clients[i]->channel, &LocalChannel::audioReceived, &clients[i]->channel,
                [&, i](const QString&, const QByteArray&, int missing) {
                    ++audioCounts[i];
                    missingCounts[i] += missing;
                });
            if (i < playback.size()) connect(&clients[i]->channel, &LocalChannel::audioReceived, &reception,
                [&, i](const QString& id, const QByteArray& bytes, int missing) {
                    if (sustained.isValid()) validAudio &= playback[i].receive(id, bytes, sustained.elapsed(), missing);
                });
        }
        // Microphone state is part of admission. Build the load fixture with
        // that state already saved, without a second 64-way settings burst.
        QVERIFY(waitForEvents([&] {
            for (const auto& client : clients) {
                const auto members = client->channel.participants();
                if (members.size() != participantCount || !std::all_of(members.begin(), members.end(),
                    [](const auto& entry) { return !entry.toMap().value("muted").toBool(); })) return false;
            }
            return allReceivers([](const QJsonObject& member) { return member.value("unmuted").toBool(); });
        }, 5000));
        const auto packet = capacityVoicePacket(sourceChannels);
        QCOMPARE(opus_packet_get_nb_channels(reinterpret_cast<const unsigned char*>(packet.constData())), sourceChannels);
        QCOMPARE(opus_packet_get_nb_samples(reinterpret_cast<const unsigned char*>(packet.constData()), int(packet.size()), 48000), 960);
        qInfo() << "Capacity source packet" << (sourceChannels == 1 ? "mono-32" : "stereo-128")
                << "bytes" << packet.size();
        // Establish and verify every source binding before measuring steady
        // fanout. Admission work must not become an unmeasured all-to-all
        // startup burst that already changes the receivers' quality tiers.
        for (int source = 0; source < participantCount; ++source) {
            if (source < speakers) QVERIFY(clients[size_t(source)]->channel.sendAudio(packet));
            else QVERIFY(commandReceivers({{"operation", "send"}, {"source", source}}));
            const bool delivered = waitForEvents([&] {
                for (size_t i = 0; i < audioCounts.size(); ++i)
                    if (audioCounts[i] != source + 1 - (int(i) <= source ? 1 : 0)) return false;
                return true;
            }, 5000);
            if (!delivered) for (size_t i = 0; i < audioCounts.size(); ++i) {
                const auto expected = source + 1 - (int(i) <= source ? 1 : 0);
                if (audioCounts[i] != expected) qWarning() << "Source binding" << source << "listener" << i
                    << "received/expected" << audioCounts[i] << expected << "reported gaps" << missingCounts[i];
            }
            QVERIFY2(delivered, qPrintable(QString("Source %1 did not reach every other participant").arg(source)));
        }
        QMap<int, int> startingTiers;
        for (const auto& client : clients) ++startingTiers[client->channel.receiveAudioBitrate()];
        for (const auto& report : receiverStates)
            for (const auto member : report.value("clients").toArray()) ++startingTiers[member.toObject().value("bitrate").toInt()];
        qInfo() << "Audio bitrates after connection and source setup" << startingTiers;
        // Recover from TLS setup and all source bindings before
        // measuring steady traffic on the independent host/client loops.
        // Five recovery steps need ten healthy 750 ms samples each, plus
        // ten seconds for admission and scheduler contention across peers.
        QVERIFY(waitForEvents([&] { return std::all_of(clients.begin(), clients.end(),
            [&](const auto& client) { return client->channel.receiveAudioBitrate() == 128; })
            && allReceivers([](const QJsonObject& member) { return member.value("bitrate").toInt() == 128; }); }, 50000));
        const auto connectedMs = elapsed.elapsed();
        bool soakOk = false;
        const int soakSeconds = qEnvironmentVariableIsSet("SQUAD_SOAK_SECONDS")
            ? qEnvironmentVariable("SQUAD_SOAK_SECONDS").toInt(&soakOk) : 10;
        QVERIFY2(!qEnvironmentVariableIsSet("SQUAD_SOAK_SECONDS") || soakOk,
            "SQUAD_SOAK_SECONDS must be an integer");
        QVERIFY2(soakSeconds >= 2 && soakSeconds <= 1800,
            "SQUAD_SOAK_SECONDS must be between 2 and 1800");
        QVERIFY(commandReceivers({{"operation", "reset"}}));
        if (!deliveryPaced) receiverPoll.stop();
        for (auto& count : audioCounts) count = 0;
        for (auto& count : missingCounts) count = 0;
        const int sustainedFrames = soakSeconds * 50;
        const auto callbacksDelayed = [callbackDelayMs](qint64 now) {
            // Defer capture/playback without also suspending the host and all
            // clients. A sleeping event loop can overshoot on busy runners.
            return now >= 200 && now % 200 < callbackDelayMs;
        };
        const auto receivedThrough = [&](int frames) {
            for (size_t i = 0; i < audioCounts.size(); ++i) {
                const int expected = frames * (speakers - (i < size_t(speakers) ? 1 : 0));
                if (audioCounts[i] + (completeDelivery ? 0 : missingCounts[i]) < expected) return false;
            }
            return true;
        };
        QTimer output;
        output.setTimerType(Qt::PreciseTimer);
        qint64 queuedUntilMs = 0, playedMs = 0;
        const auto advancePlayback = [&](qint64 now) {
            playedMs += std::max<qint64>(0, std::min(now, queuedUntilMs) - lastOutputMs);
            maxOutputGapMs = std::max(maxOutputGapMs, now - lastOutputMs);
            lastOutputMs = now;
        };
        connect(&output, &QTimer::timeout, &reception, [&] {
            const auto now = sustained.elapsed();
            if (callbacksDelayed(now)) return;
            advancePlayback(now);
            // Match AudioModel's 80 ms sink and 10 ms replenishment. The sink
            // consumes time even while this event loop is stalled. Refill at
            // most one 20 ms block, and keep underruns in the measured result.
            if (queuedUntilMs - now > 60) return;
            queuedUntilMs = std::max(queuedUntilMs, now) + 20;
            ++renderedFrames;
            QElapsedTimer work; work.start();
            for (size_t i = 0; i < playback.size(); ++i) {
                const auto samples = playback[i].render(48000, sustained.elapsed());
                if (std::any_of(samples.begin(), samples.end(), [](float sample) { return std::abs(sample) > 0.005f; }))
                    ++audibleFrames[i];
            }
            maxRenderUs = std::max(maxRenderUs, work.nsecsElapsed() / 1000);
        });
        QTimer input;
        input.setTimerType(Qt::PreciseTimer);
        int sentFrames = 0;
        bool validSend = true, deliveryTimedOut = false;
        qint64 sentAtMs = 0;
        connect(&input, &QTimer::timeout, &reception, [&] {
            const auto now = sustained.elapsed();
            if (callbacksDelayed(now)) return;
            // Delivery pacing bounds work in flight without reducing the
            // fanout or packet count. The default path measures realtime load.
            if (deliveryPaced && !receivedThrough(sentFrames)) {
                if (now - sentAtMs >= 5000) { deliveryTimedOut = true; input.stop(); }
                return;
            }
            maxInputGapMs = std::max(maxInputGapMs, now - lastInputMs);
            lastInputMs = now;
            // Capture is sample-clocked, not callback-clocked. AudioModel
            // drains all available samples after a delayed wakeup. Keep this
            // fixture's catch-up bounded by the receiver's six-frame queue.
            const auto due = deliveryPaced ? sentFrames + 1
                : std::min<qint64>({now / 20, sentFrames + 6, sustainedFrames});
            QElapsedTimer work; work.start();
            while (sentFrames < due) {
                for (int speaker = 0; speaker < speakers; ++speaker)
                    validSend &= clients[size_t(speaker)]->channel.sendAudio(packet);
                ++sentFrames;
            }
            maxSendUs = std::max(maxSendUs, work.nsecsElapsed() / 1000);
            sentAtMs = now;
            if (sentFrames == sustainedFrames) input.stop();
        });
        qInfo() << "Starting audio soak; simultaneous speakers" << speakers
                << "delivery paced" << deliveryPaced << "requested seconds" << soakSeconds << "setup ms" << connectedMs;
#ifndef Q_OS_WIN
        // Microsoft clock() measures wall time, unlike the POSIX CPU clock.
        const auto cpuStarted = std::clock();
        const auto remoteCpu = [&] {
            return std::accumulate(receiverStates.begin(), receiverStates.end(), 0.0,
                [](double sum, const auto& report) { return sum + report.value("cpu").toDouble(); });
        };
        const auto remoteCpuStarted = remoteCpu();
#endif
        sustained.start(); output.start(10); input.start(10);
        QVERIFY(waitForEvents(&input, &QTimer::timeout, [&] { return !input.isActive(); },
            soakSeconds * (deliveryPaced ? 15000 : 1500) + 5000));
        QVERIFY2(!deliveryTimedOut, "Delivery-paced fanout did not deliver a frame to every listener within 5 seconds");
        QCOMPARE(sentFrames, sustainedFrames);
        QVERIFY(validSend);
        output.stop();
        const auto playbackMs = sustained.elapsed();
        advancePlayback(playbackMs);
        receiverPoll.start(10);
        // RTP can report a gap only when a later packet arrives. Losing the
        // final packet cannot produce a gap report. Controlled delivery must
        // receive it too, within the same per-frame deadline.
        const bool drained = waitForEvents([&] { return receivedThrough(sustainedFrames); },
            completeDelivery ? 5000 : 500);
        QVERIFY2(!completeDelivery || drained, "Controlled fanout did not deliver every final packet within 5 seconds");
        QVERIFY(receiverOutputValid);
#ifndef Q_OS_WIN
        qInfo() << "Audio soak client processes CPU ms"
                << (std::clock() - cpuStarted) * 1000.0 / CLOCKS_PER_SEC + remoteCpu() - remoteCpuStarted;
#endif
        const auto receivedFrames = std::accumulate(audioCounts.begin(), audioCounts.end(), 0);
        const auto missingFrames = std::accumulate(missingCounts.begin(), missingCounts.end(), 0);
        qInfo() << participantCount << "clients; verified all-to-all bindings plus" << sustainedFrames
                << "frames per speaker; simultaneous speakers" << speakers << "requested soak seconds" << soakSeconds
                << "received packets" << receivedFrames << "reported gaps" << missingFrames << "rendered frames" << renderedFrames
                << "non-silent frames at listeners 0/1" << audibleFrames[0] << audibleFrames[1]
                << "setup ms" << connectedMs << "sustained ms" << playbackMs
                << "sink played/underrun ms" << playedMs << playbackMs - playedMs
                << "last input/output ms" << lastInputMs << lastOutputMs
                << "maximum input/output gap ms" << maxInputGapMs << maxOutputGapMs
                << "maximum send/render work us" << maxSendUs << maxRenderUs
                << "total ms" << elapsed.elapsed();
        for (size_t i = 0; i < audioCounts.size(); ++i) {
            const int expected = sustainedFrames * (speakers - (i < size_t(speakers) ? 1 : 0));
            QVERIFY(audioCounts[i] + missingCounts[i] <= expected);
            QVERIFY2(audioCounts[i] >= expected * (completeDelivery ? 100 : 95) / 100,
                qPrintable(QString("listener %1 received %2 / %3 packets; reported gaps %4")
                    .arg(i).arg(audioCounts[i]).arg(expected).arg(missingCounts[i])));
        }
        QVERIFY(validAudio);
        for (const auto frames : audibleFrames) QVERIFY(frames > 0);
        if (deliveryPaced) {
            qInfo() << "Delivery-paced run, not a realtime capacity measurement";
        } else {
            const auto expectedFrames = playbackMs / 20;
            QVERIFY2(renderedFrames >= expectedFrames * 9 / 10,
                qPrintable(QString("rendered %1 / expected %2 over %3 ms")
                    .arg(renderedFrames).arg(expectedFrames).arg(playbackMs)));
            QVERIFY2(playedMs >= playbackMs * 9 / 10,
                qPrintable(QString("sink played %1 / %2 ms; underrun %3 ms")
                    .arg(playedMs).arg(playbackMs).arg(playbackMs - playedMs)));
            for (const auto frames : audibleFrames) QVERIFY2(frames >= renderedFrames * 9 / 10,
                qPrintable(QString("non-silent %1 / rendered %2 over %3 ms").arg(frames).arg(renderedFrames).arg(playbackMs)));
        }
        for (const auto& client : clients) QVERIFY(client->channel.joined());
        QVERIFY(allReceivers([](const QJsonObject& member) { return member.value("joined").toBool(); }));
        sustained.invalidate();
        // Replacing a connection keeps one logical participant.
        QVERIFY(joinHost(*replacement));
        QTRY_VERIFY(replacement->channel.joined());
        QTRY_VERIFY(!clients.front()->channel.joined());
        clients.front() = std::move(replacement);
        QTRY_COMPARE(clients.back()->channel.participants().size(), participantCount);
        QVERIFY(!participant(clients.back()->channel, firstIdentity.id()).isEmpty());
        const auto remainingCount = participantCount / 2;
        for (int i = 0; i < std::min(speakers, remainingCount); ++i) QVERIFY(clients[size_t(i)]->channel.leave());
        QVERIFY(commandReceivers({{"operation", "leave"}, {"before", remainingCount}}));
        for (int i = remainingCount; i < speakers; ++i) QTRY_COMPARE(clients[size_t(i)]->channel.participants().size(), remainingCount);
        QTRY_VERIFY([&] {
            for (size_t group = 0; group < receivers.size(); ++group) {
                const auto members = receiverStates[group].value("clients").toArray();
                for (qsizetype i = 0; i < members.size(); ++i) {
                    const auto member = members[i].toObject();
                    const bool remaining = receiverFirst[group] + i >= remainingCount;
                    if (member.value("joined").toBool() != remaining
                        || (remaining && member.value("members").toInt() != remainingCount)) return false;
                }
            }
            return receiverOutputValid;
        }());
    }
#endif
    void pendingAdmissionBudgetReleasesAfterDisconnect() {
        QTemporaryDir dir;
        Device host(dir.filePath("password-host"), "Password host");
        QVERIFY(host.channel.setHostPassword("correct horse battery staple"));
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QTRY_VERIFY_WITH_TIMEOUT(host.channel.passwordProtected() && !host.channel.passwordBusy(), 10000);

        struct RawPeer final {
            std::unique_ptr<QSslSocket> socket;
            QByteArray buffer;
            bool encrypted = false;
            int passwordChallenges = 0;
        };
        TlsIdentity identity = TlsIdentity::create();
        std::vector<RawPeer> peers;
        peers.reserve(41);
        const auto startPeer = [&](int index) {
            auto& peer = peers[static_cast<size_t>(index)];
            peer.socket = std::make_unique<QSslSocket>();
            auto* socket = peer.socket.get();
            socket->setSslConfiguration(identity.configuration());
            QObject::connect(socket, &QSslSocket::sslErrors, socket,
                [socket](const QList<QSslError>& errors) { socket->ignoreSslErrors(errors); });
            QObject::connect(socket, &QSslSocket::encrypted, socket, [&, socket, index] {
                peers[static_cast<size_t>(index)].encrypted = true;
                const QJsonObject hello{
                    {"type", "hello"}, {"version", 1}, {"name", "Pending"},
                    {"available", true}, {"muted", false}, {"deafened", false},
                    {"avatar", "mossling"}, {"avatarId", "mossling"},
                    {"observer", false}, {"adaptiveAudio", true}, {"remoteOffers", true}};
                const auto data = QJsonDocument(hello).toJson(QJsonDocument::Compact);
                QByteArray frame(4, '\0');
                qToBigEndian<quint32>(quint32(data.size()), frame.data());
                frame.append(data);
                socket->write(frame);
            });
            QObject::connect(socket, &QSslSocket::readyRead, socket, [&, socket, index] {
                auto& raw = peers[static_cast<size_t>(index)];
                raw.buffer += socket->readAll();
                while (raw.buffer.size() >= 4) {
                    const auto size = qFromBigEndian<quint32>(raw.buffer.constData());
                    if (!size || size > 65536 || raw.buffer.size() < 4 + qsizetype(size)) return;
                    const auto message = QJsonDocument::fromJson(raw.buffer.mid(4, size)).object();
                    raw.buffer.remove(0, 4 + size);
                    if (message.value("type").toString() == "join"
                        && message.value("result").toString() == "password") ++raw.passwordChallenges;
                }
            });
            socket->connectToHostEncrypted(QStringLiteral("127.0.0.1"), host.channel.port());
        };
        peers.resize(40);
        for (int i = 0; i < 32; ++i) startPeer(i);
        QTRY_COMPARE_WITH_TIMEOUT(std::count_if(peers.begin(), peers.begin() + 32, [](const auto& peer) {
            return peer.encrypted && peer.socket->state() == QAbstractSocket::ConnectedState;
        }), 32, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(std::count_if(peers.begin(), peers.begin() + 32, [](const auto& peer) {
            return peer.passwordChallenges == 1;
        }), 32, 10000);
        // The encrypted hello and password challenge keep these peers alive past
        // the missing-hello timer; raw TCP ConnectedState is not the contract.
        QTest::qWait(6000);
        QCOMPARE(std::count_if(peers.begin(), peers.begin() + 32, [](const auto& peer) {
            return peer.encrypted && peer.socket->state() == QAbstractSocket::ConnectedState
                && peer.passwordChallenges == 1;
        }), 32);

        for (int i = 32; i < 40; ++i) startPeer(i);
        QTRY_VERIFY_WITH_TIMEOUT(std::all_of(peers.begin() + 32, peers.end(), [](const auto& peer) {
            return peer.socket->state() == QAbstractSocket::UnconnectedState;
        }), 10000);
        QCOMPARE(std::count_if(peers.begin() + 32, peers.end(), [](const auto& peer) {
            return peer.passwordChallenges > 0;
        }), 0);
        for (int i = 0; i < 32; ++i) {
            QVERIFY(peers[size_t(i)].socket->isEncrypted());
            QCOMPARE(peers[size_t(i)].socket->state(), QAbstractSocket::ConnectedState);
        }

        peers[0].socket->disconnectFromHost();
        QTRY_COMPARE(peers[0].socket->state(), QAbstractSocket::UnconnectedState);
        peers.push_back({});
        startPeer(40);
        QTRY_VERIFY_WITH_TIMEOUT(peers[40].encrypted && peers[40].passwordChallenges == 1
            && peers[40].socket->state() == QAbstractSocket::ConnectedState, 10000);
        for (int i = 1; i < 32; ++i)
            QCOMPARE(peers[size_t(i)].socket->state(), QAbstractSocket::ConnectedState);
        for (auto& peer : peers) if (peer.socket) peer.socket->disconnectFromHost();
    }
    void rotatedIdentitiesCannotFloodHostRequests() {
        QTemporaryDir dir;
        qint64 now = 1000000;
        Device host(dir.filePath("host"), "Host", TlsIdentity::create(), [&] { return now; });
        QVERIFY(host.channel.listen(QHostAddress::LocalHost));
        QSignalSpy events(&host.channel, &LocalChannel::channelEvent);
        std::vector<std::unique_ptr<Device>> clients;
        for (int i = 0; i < 3; ++i) {
            auto client = std::make_unique<Device>(dir.filePath(QString::number(i)), "Gast");
            QVERIFY(client->join(host.channel));
            clients.push_back(std::move(client));
            QTRY_COMPARE(host.channel.requests().size(), i + 1);
            QCOMPARE(events.size(), i + 1);
            QCOMPARE(events.last().first().toString(), QString("request"));
        }
        QSignalSpy requests(&host.channel, &LocalChannel::requestsChanged);
        Device fourth(dir.filePath("fourth"), "Noch ein Gast");
        QVERIFY(fourth.join(host.channel));
        QTRY_VERIFY(fourth.channel.status().contains("rejected"));
        QCOMPARE(host.channel.requests().size(), 3);
        QCOMPARE(requests.size(), 0);
        QCOMPARE(events.size(), 3);
        QVERIFY(host.channel.decide(fourth.channel.ownId(), true));
        QVERIFY(fourth.join(host.channel)); QTRY_VERIFY(fourth.channel.joined());
        QCOMPARE(events.size(), 3);
    }
    void invalidStorageAndAtomicWriteFailure() {
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("prefs"));
        LocalChannel channel(session, dir.filePath("missing/approvals"), TlsIdentity::create());
        QVERIFY(!channel.setRequestsAllowed(false));
        QVERIFY(channel.requestsAllowed());
        QVERIFY(!channel.decide("bad", true));
        QFile broken(dir.filePath("broken")); QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("{\"version\":1}"); broken.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, LocalChannel(session, broken.fileName(), TlsIdentity::create()));
    }
};
int main(int argc, char** argv) {
    qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1");
#if defined(Q_OS_IOS)
    QGuiApplication application(argc, argv);
#else
    QCoreApplication application(argc, argv);
#endif
#if QT_CONFIG(process)
    const auto arguments = application.arguments();
    if (arguments.size() == 5 && arguments[1] == "--capacity-receivers") {
        const auto first = arguments[3].toInt(), count = arguments[4].toInt();
        if (first < 2 || count < 1 || first + count > 256) return 2;
        std::vector<int> heard(size_t(first + count)), missing(size_t(first + count));
        std::vector<std::unique_ptr<Device>> clients;
        for (int index = first; index < first + count; ++index) {
            clients.push_back(std::make_unique<Device>(arguments[2] + "/" + QString::number(index),
                QString("Member %1").arg(index)));
            if (!clients.back()->session.setMuted(false)) return 1;
            QObject::connect(&clients.back()->channel, &LocalChannel::audioReceived, &clients.back()->channel,
                [&, index](const QString&, const QByteArray&, int gaps) { ++heard[size_t(index)]; missing[size_t(index)] += gaps; });
        }
        const auto report = [&](int serial, bool ok) {
            QJsonArray members;
            for (size_t i = 0; i < clients.size(); ++i) {
                const auto& channel = clients[i]->channel;
                const auto roster = channel.participants();
                members.append(QJsonObject{{"id", channel.ownId()}, {"joined", channel.joined()},
                    {"members", roster.size()}, {"bitrate", channel.receiveAudioBitrate()},
                    {"unmuted", !roster.isEmpty() && std::all_of(roster.begin(), roster.end(), [](const auto& member) {
                        return !member.toMap().value("muted").toBool();
                    })}, {"heard", heard[size_t(first) + i]}, {"missing", missing[size_t(first) + i]}});
            }
            const auto bytes = QJsonDocument(QJsonObject{{"serial", serial}, {"ok", ok}, {"clients", members},
                {"cpu", double(std::clock()) * 1000.0 / CLOCKS_PER_SEC}}).toJson(QJsonDocument::Compact);
            std::puts(bytes.constData()); std::fflush(stdout);
        };
        report(0, true);
        const auto packet = voicePacket();
        const auto result = controlledProcess(application, [&](const QJsonObject& request) {
            const auto operation = request.value("operation").toString();
            bool ok = true;
            for (size_t i = 0; i < clients.size(); ++i) {
                auto& client = *clients[i];
                if (operation == "join") {
                    ok &= client.channel.join(request.value("host").toString(),
                        "127.0.0.1", quint16(request.value("port").toInt()));
                    // Measure established capacity separately from the bounded
                    // simultaneous admission budget covered by its own test.
                    ok &= waitForEvents(&client.channel, &LocalChannel::stateChanged,
                        [&] { return client.channel.joined(); }, 5000);
                }
                else if (operation == "send" && int(i) + first == request.value("source").toInt()) ok &= client.channel.sendAudio(packet);
                else if (operation == "leave" && int(i) + first < request.value("before").toInt()) ok &= client.channel.leave();
                else if (operation == "reset") { heard[size_t(first) + i] = 0; missing[size_t(first) + i] = 0; }
            }
            report(request.value("serial").toInt(), ok);
        });
        clients.clear();
        qInfo() << "Capacity receiver devices closed";
        return result;
    }
    if (arguments.size() == 3 && arguments[1] == "--capacity-host") {
        QLoggingCategory::setFilterRules(QStringLiteral("squadspeak.media.sender.debug=true"));
        Device host(arguments[2], "Host");
        if (!host.listenWithUdp() || !host.channel.setScreenSharing(true)) return 1;
        const auto endpoint = QJsonDocument(QJsonObject{{"id", host.channel.ownId()}, {"port", host.channel.port()}})
            .toJson(QJsonDocument::Compact);
        std::puts(endpoint.constData()); std::fflush(stdout);
        return controlledProcess(application);
    }
#endif
    ChannelTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "channel_tests.moc"
