#pragma once

#include <QObject>
#include <QHostAddress>
#include <QJsonObject>
#include <QTimer>
#include <memory>

namespace squad {
// An admitted TLS connection owns one transport. All public calls and signals
// belong to its Qt thread; library callbacks enter a bounded lifetime-guarded mailbox.
class MediaTransport final : public QObject {
    Q_OBJECT
public:
    enum class Medium { Audio, Data };
    MediaTransport(bool offerer, quint16 localPort, QHostAddress remoteAddress,
                   quint16 remotePort, Medium medium = Medium::Audio, QObject* parent = nullptr);
    ~MediaTransport() override;
    bool receive(const QJsonObject& message);
    bool send(const QString& source, const QByteArray& opus, int missing = 0);
    bool sendDatagram(const QByteArray& bytes);
    void retainSources(const QStringList& sources);
    [[nodiscard]] bool udpActive() const;
signals:
    void signaling(const QJsonObject& message);
    void audio(const QString& source, const QByteArray& opus, int missing);
    void quality(int condition);
    void writable();
    void datagramReceived(const QByteArray& bytes);
private:
    struct State;
    std::shared_ptr<State> state_;
    QTimer timer_;
    void start();
    void tick();
    void packet(const QByteArray& bytes);
    void accept(quint32 ssrc, quint16 sequence, const QByteArray& opus, bool datagram);
    void stopUdp();
    void restartUdp();
};
}
