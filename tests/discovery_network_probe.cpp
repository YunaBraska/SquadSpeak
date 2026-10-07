#include "local_channel.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QDebug>

// Driven by discovery-network.sh in separate network namespaces. No interface
// provider is substituted: joining, announcements, TLS and chat are real.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3 || (args[1] != "receiver" && args[1] != "sender")) return 2;
    const bool receiver = args[1] == "receiver";
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    VoiceSession session(directory.filePath("session.json"));
    LocalChannel channel(session, directory.filePath("channel.json"), TlsIdentity::create());
    if (!session.setUserName(args[2]) || !channel.setChannelName(args[2])
        || !channel.listen(QHostAddress::AnyIPv4, 0) || !channel.startDiscovery()) return 2;
    const auto mark = [](const QString& name) {
        QFile file("/evidence/" + name);
        return file.open(QIODevice::WriteOnly) && file.write("ready\n") == 6;
    };
    if (!mark(args[2] + ".ready")) return 2;
    QTimer poll;
    poll.setInterval(100);
    int phase = 1;
    QString target;
    bool sent = false;
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (QFile::exists("/evidence/stop")) { app.quit(); return; }
        if (!receiver) {
            // Requiring the request's ID to be discovered proves that the
            // receiver also starts announcing on its newly attached network.
            for (const auto& pending : channel.requests()) {
                const auto id = pending.toMap().value("id").toString();
                for (const auto& host : channel.hosts())
                    if (host.toMap().value("id").toString() == id && !channel.decide(id, true))
                        app.exit(3);
            }
            return;
        }
        const auto name = QString("sender-%1").arg(phase);
        if (target.isEmpty()) {
            for (const auto& value : channel.hosts()) {
                const auto host = value.toMap();
                if (host.value("name").toString() != name) continue;
                target = host.value("id").toString();
                if (!channel.join(target, host.value("address").toString(), host.value("port").toInt()))
                    app.exit(3);
                break;
            }
        }
        const auto message = "Network change verified with " + name;
        if (channel.joined() && channel.joinedHostId() == target && !sent) {
            sent = channel.sendChat(message);
            if (!sent) app.exit(3);
        }
        if (!sent) return;
        for (const auto& value : channel.messages()) {
            const auto item = value.toMap();
            if (item.value("text").toString() != message) continue;
            qInfo().noquote() << message;
            if (!mark(name + ".passed")) { app.exit(3); return; }
            if (phase == 2) { app.quit(); return; }
            channel.leave();
            ++phase;
            target.clear();
            sent = false;
            return;
        }
    });
    poll.start();
    QTimer::singleShot(90000, &app, [&] {
        qCritical() << "Network change timed out" << args[2] << channel.discoveryError() << channel.status();
        app.exit(4);
    });
    return app.exec();
}
