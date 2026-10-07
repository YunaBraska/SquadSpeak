#pragma once
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QSet>
#include <QElapsedTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

// Real HTTP and media decoding, paced at the source. No external station or
// audio hardware is needed to prove host distribution and stream recovery.
class RadioStation final : public QObject {
    QTcpServer server_;
public:
    int requests = 0;
    QSet<QTcpSocket*> sockets;
    bool running = true;
    bool audio = true;
    bool stalled = false;
    RadioStation() {
        if (!server_.listen(QHostAddress::LocalHost)) throw std::runtime_error("HTTP fixture could not listen");
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            auto* socket = server_.nextPendingConnection(); sockets.insert(socket);
            socket->setParent(this);
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] { sockets.remove(socket); socket->deleteLater(); });
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                if (!socket->readAll().contains("GET")) return;
                ++requests;
                if (stalled) return;
                if (!running) { socket->disconnectFromHost(); return; }
                if (!audio) {
                    const QByteArray body = "<html>not an audio stream</html>";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    return;
                }
                QByteArray header("RIFF\0\0\0\0WAVEfmt \20\0\0\0\1\0\1\0\200\273\0\0\0\167\1\0\2\0\20\0data\0\0\0\0", 44);
                constexpr quint32 length = 48000 * 2 * 60;
                qToLittleEndian<quint32>(36 + length, header.data() + 4);
                qToLittleEndian<quint32>(length, header.data() + 40);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: " + QByteArray::number(length + 44) + "\r\n\r\n" + header);
                auto* timer = new QTimer(socket); timer->setInterval(20);
                QElapsedTimer elapsed; elapsed.start();
                connect(timer, &QTimer::timeout, socket, [socket, elapsed, sample = qint64(0)]() mutable {
                    if (socket->state() != QAbstractSocket::ConnectedState || socket->bytesToWrite() > 32768) return;
                    // GUI and subprocess test loops can coalesce timer ticks.
                    // Keep the stream at 48 kHz wall time instead of slowing it
                    // down to one 20 ms audio frame per observed timer event.
                    const auto frames = std::clamp<qint64>(elapsed.elapsed() * 48 - sample, 0, 48000);
                    QByteArray pcm(frames * 2, '\0');
                    for (qint64 i = 0; i < frames; ++i, ++sample)
                        qToLittleEndian<qint16>(qint16(7000 * std::sin(2 * std::numbers::pi * 440 * sample / 48000)), pcm.data() + i * 2);
                    socket->write(pcm);
                });
                timer->start();
            });
        });
    }
    QString url() const { return "http://127.0.0.1:" + QString::number(server_.serverPort()) + "/radio.wav"; }
    void drop() { for (auto* socket : sockets.values()) socket->abort(); }
};
