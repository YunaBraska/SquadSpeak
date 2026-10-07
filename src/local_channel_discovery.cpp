#include "local_channel.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkDatagram>
#include <QSslSocket>
#include <algorithm>
#include <cerrno>

namespace {
Q_LOGGING_CATEGORY(discoveryLog, "squadspeak.discovery", QtWarningMsg)
constexpr quint16 discoveryPort = 48762;
const QHostAddress discoveryGroup(QStringLiteral("239.255.85.73"));
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

LocalChannel::Discovery::Discovery(LocalChannel& owner) : owner_(owner) {
    connect(&socket_, &QUdpSocket::readyRead, this, &Discovery::receive);
    heartbeat_.setInterval(1500);
    connect(&heartbeat_, &QTimer::timeout, this, &Discovery::announce);
}

LocalChannel::Discovery::~Discovery() { clearSearch(); }

bool LocalChannel::Discovery::start() {
    if (owner_.service_) return false;
    if (!owner_.ready()) return owner_.setStatus(LocalChannel::tr("Device identity is not ready yet."), false);
    if (socket_.state() == QAbstractSocket::BoundState) return true;
    heartbeat_.start();
    if (!socket_.bind(QHostAddress::AnyIPv4, discoveryPort,
                         QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        error = LocalChannel::tr("Local channel discovery unavailable: %1").arg(socket_.errorString());
        emit owner_.hostsChanged(); return false;
    }
    socket_.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    interfaces_.clear();
    socket_.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    announce();
    return socket_.state() == QAbstractSocket::BoundState;
}

QList<LocalChannel::Discovery::Target> LocalChannel::Discovery::targets() const {
    QList<Target> result;
    QSet<QString> seen;
    const auto add = [&](const QString& address, quint16 port) {
        const auto key = address + ':' + QString::number(port);
        if (!seen.contains(key)) { seen.insert(key); result.append({address, port}); }
    };
    for (quint16 port = 48763; port <= 48783; ++port) add("127.0.0.1", port);
    if (owner_.server_.isListening() && owner_.server_.serverAddress().isLoopback()) return result;

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

bool LocalChannel::Discovery::search(bool enabled) {
    if (!enabled) {
        clearSearch();
        searching = false;
        for (auto it = hosts.begin(); it != hosts.end();) {
            it->scanned = false;
            if (it.key() != owner_.ownId() && !it->direct && owner_.clock_() - it->seen > 6000) it = hosts.erase(it);
            else ++it;
        }
        emit owner_.hostsChanged();
        emit owner_.discoverySearchChanged();
        return true;
    }
    if (searching) return true;
    if (!owner_.ready()) return owner_.setStatus(LocalChannel::tr("Device identity is not ready yet."), false);
    queue_ = targets();
    searching = true;
    emit owner_.discoverySearchChanged();
    pump();
    return true;
}

void LocalChannel::Discovery::clearSearch() {
    queue_.clear();
    const auto sockets = probes_.keys();
    probes_.clear();
    for (auto* socket : sockets) {
        socket->abort();
        socket->deleteLater();
    }
}

void LocalChannel::Discovery::finishProbe(QSslSocket* socket) {
    if (!probes_.contains(socket)) return;
    probes_.remove(socket);
    socket->abort();
    socket->deleteLater();
    if (searching) pump();
}

void LocalChannel::Discovery::pump() {
    if (!searching) return;
    while (probes_.size() < 16 && !queue_.isEmpty()) {
        const auto target = queue_.takeFirst();
        auto* socket = new QSslSocket(this);
        probes_.insert(socket, {target, {}});
        socket->setReadBufferSize(2 * maximumFrame);
        socket->setSslConfiguration(owner_.identity_->configuration());
        connect(socket, &QSslSocket::sslErrors, this, [this, socket](const QList<QSslError>& errors) {
            if (probes_.contains(socket) && !TlsIdentity::peerId(socket->peerCertificate()).isEmpty()
                && acceptableCertificateErrors(errors)) socket->ignoreSslErrors(errors);
        });
        connect(socket, &QSslSocket::encrypted, this, [this, socket] {
            if (probes_.contains(socket)) writeMessage(socket, {{"type", "discover"}, {"version", 1}});
        });
        connect(socket, &QSslSocket::readyRead, this, [this, socket] {
            auto it = probes_.find(socket);
            if (it == probes_.end()) return;
            auto buffer = std::move(it->buffer);
            const auto targetPort = it->target.port;
            readMessages(socket, buffer, [this, socket, targetPort](const QJsonObject& message) {
                const auto it = probes_.find(socket);
                if (it == probes_.end()) return;
                const auto id = TlsIdentity::peerId(socket->peerCertificate());
                if (message.value("type") == "discovery" && message.value("version").toInt() == 1
                    && message.value("hosting").toBool() && validId(id) && id != owner_.ownId()
                    && displayName(message.value("channel")) && (hosts.size() < 1024 || hosts.contains(id))) {
                    owner_.receiveDirectory(id, message, socket->peerAddress().toString(), targetPort, false, true);
                }
                finishProbe(socket);
            });
            if (probes_.contains(socket)) probes_[socket].buffer = std::move(buffer);
        });
        connect(socket, &QSslSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
            finishProbe(socket);
        });
        connect(socket, &QSslSocket::disconnected, this, [this, socket] { finishProbe(socket); });
        QTimer::singleShot(1500, socket, [this, socket] { finishProbe(socket); });
        socket->connectToHostEncrypted(target.address, target.port, target.address, QIODevice::ReadWrite,
                                       QAbstractSocket::IPv4Protocol);
    }
    if (queue_.isEmpty() && probes_.isEmpty()) {
        searching = false;
        emit owner_.discoverySearchChanged();
    }
}

void LocalChannel::Discovery::announce() {
    if (owner_.service_) { owner_.service_->discovery_.announce(); return; }
    if (heartbeat_.isActive() && socket_.state() != QAbstractSocket::BoundState) {
        start(); return;
    }
    const auto now = owner_.clock_();
    for (auto it = hosts.begin(); it != hosts.end();) {
        if (it.key() != owner_.ownId() && !it->direct && !it->scanned && now - it->seen > 6000) it = hosts.erase(it);
        else ++it;
    }
    if (socket_.state() == QAbstractSocket::BoundState) {
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
        if (std::any_of(interfaces_.cbegin(), interfaces_.cend(), [&](const auto& previous) {
                return std::none_of(current.cbegin(), current.cend(),
                    [&](const auto& value) { return same(previous, value); });
            })) {
            // A removed address may no longer be usable for leaving its old
            // group. Rebinding drops every stale membership on all backends.
            socket_.close();
            start();
            return;
        }
        for (const auto& interface : current) {
            if (std::any_of(interfaces_.cbegin(), interfaces_.cend(),
                    [&](const auto& value) { return same(interface, value); })) continue;
            if (socket_.joinMulticastGroup(discoveryGroup, interface)) interfaces_.append(interface);
            else qCDebug(discoveryLog) << "join" << interface.name() << socket_.errorString();
        }
        if (interfaces_.isEmpty()) {
            socket_.close();
            error = LocalChannel::tr("The network does not allow local multicast discovery.");
            emit owner_.hostsChanged(); return;
        }
        error.clear();
    }
    if (owner_.hosting()) {
        for (const auto& entry : owner_.channelDirectory()) {
            const auto value = entry.toObject();
            hosts.insert(value.value("id").toString(), {value.value("name").toString(), "127.0.0.1", owner_.port(), now, false, false, owner_.ownId()});
        }
        if (socket_.state() == QAbstractSocket::BoundState) {
            const auto data = QJsonDocument(QJsonObject{{"protocol", "squadspeak/1"}, {"id", owner_.ownId()},
                {"name", owner_.channelName_}, {"port", owner_.port()}, {"channels", owner_.channelDirectory()}}).toJson(QJsonDocument::Compact);
            bool sentAny = false;
            QString failure;
            for (const auto& interface : interfaces_) {
                socket_.setMulticastInterface(interface);
                const auto sent = socket_.writeDatagram(data, discoveryGroup, discoveryPort);
                const auto nativeError = errno;
                if (sent >= 0) sentAny = true;
                else failure = socket_.errorString();
                qCDebug(discoveryLog) << "announce" << interface.name() << sent
                    << (sent < 0 ? socket_.errorString() : QString{}) << "socket" << socket_.error()
                    << "errno" << (sent < 0 ? nativeError : 0);
            }
            error = sentAny ? QString{} : LocalChannel::tr("Local channel discovery unavailable: %1").arg(failure);
            if (!sentAny) socket_.close();
        }
    }
    emit owner_.hostsChanged();
}

void LocalChannel::Discovery::receive() {
    while (socket_.hasPendingDatagrams()) {
        const auto datagram = socket_.receiveDatagram(8192);
        qCDebug(discoveryLog) << "received" << datagram.senderAddress() << datagram.data().size() << "local" << localAddress(datagram.senderAddress());
        if (!localAddress(datagram.senderAddress())) continue;
        const auto object = QJsonDocument::fromJson(datagram.data()).object();
        const auto id = object.value("id").toString();
        const auto port = object.value("port").toInt();
        if (object.value("protocol") != "squadspeak/1" || !validId(id) || id == owner_.ownId()
            || !displayName(object.value("name")) || port < 1 || port > 65535) continue;
        if (hosts.size() >= 1024 && !hosts.contains(id)) continue;
        auto directory = object; directory.insert("channel", object.value("name"));
        if (!owner_.receiveDirectory(id, directory, datagram.senderAddress().toString(), quint16(port), false, hosts.value(id).scanned)) continue;
        for (const auto& c : owner_.clients_) if (c->deviceId == id && c->reconnectWanted && hosts.contains(c->id)) {
            // Preserve explicit DNS names across endpoint changes on every channel.
            if (!QHostAddress(c->address).isNull()) {
                c->address = datagram.senderAddress().toString(); c->port = port;
            }
        }
        emit owner_.hostsChanged();
    }
}
