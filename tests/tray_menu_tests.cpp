#include "local_channel.hpp"
#include "radio_player.hpp"
#include "tray_menu.hpp"
#include "voice_session.hpp"
#include <QAction>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QTemporaryDir>
#include <QtTest>

class TrayMenuTests final : public QObject {
    Q_OBJECT
private slots:
    void localVoiceActionsAppearOnlyWhileJoined() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        VoiceSession session(directory.filePath("session.json"));
        LocalChannel channel(session, directory.filePath("channel.json"), TlsIdentity::create());
        RadioPlayer radio(directory.filePath("radio.json"));
        QMenu menu;
        addTrayQuickActions(menu, session, channel, radio);

        auto* mute = menu.findChild<QAction*>("trayMuteInput");
        auto* deafen = menu.findChild<QAction*>("trayMuteOutput");
        auto* leave = menu.findChild<QAction*>("trayLeaveVoice");
        auto* radioAction = menu.findChild<QAction*>("trayRadio");
        QVERIFY(mute);
        QVERIFY(deafen);
        QVERIFY(leave);
        QVERIFY(radioAction);
        QVERIFY(!mute->isVisible());
        QVERIFY(!deafen->isVisible());
        QVERIFY(!leave->isVisible());
        QVERIFY(!radioAction->isEnabled());

        QVERIFY(channel.listen(QHostAddress::LocalHost, 0));
        QVERIFY(channel.joinSaved(channel.ownId()));
        QTRY_VERIFY(channel.joined());
        QVERIFY(mute->isVisible());
        QVERIFY(deafen->isVisible());
        QVERIFY(leave->isVisible());
        QVERIFY(mute->isChecked());
        QVERIFY(!deafen->isChecked());

        mute->trigger();
        QVERIFY(!session.muted());
        QVERIFY(!mute->isChecked());
        deafen->trigger();
        QVERIFY(session.deafened());
        QVERIFY(deafen->isChecked());

        QVERIFY(session.setMuted(true));
        QVERIFY(mute->isChecked());
        QVERIFY(session.setDeafened(false));
        QVERIFY(!deafen->isChecked());

        leave->trigger();
        QTRY_VERIFY(!channel.joined());
        QVERIFY(!mute->isVisible());
        QVERIFY(!deafen->isVisible());
        QVERIFY(!leave->isVisible());
    }

    void remoteVoiceActionsUseTargetStateAndControl() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        VoiceSession targetSession(directory.filePath("target-session.json"));
        LocalChannel target(targetSession, directory.filePath("target-channel.json"), TlsIdentity::create());
        VoiceSession controllerSession(directory.filePath("controller-session.json"));
        LocalChannel controller(controllerSession, directory.filePath("controller-channel.json"), TlsIdentity::create());
        RadioPlayer radio(directory.filePath("controller-radio.json"));

        QVERIFY(target.listen(QHostAddress::LocalHost, 0));
        QVERIFY(controller.pairAddress("127.0.0.1:" + QString::number(target.port())));
        QTRY_COMPARE(target.controlRequests().size(), 1);
        QVERIFY(target.decideControl(controller.ownId(), true));
        QTRY_VERIFY(controller.controlConnected());
        QVERIFY(target.setRemotePermission(controller.ownId(), true));
        QVERIFY(controller.setRemoteMode(true));
        QTRY_VERIFY(controller.remoteAllowed());
        QTRY_VERIFY(!controller.remoteView().value("joined").toBool());

        QMenu menu;
        addTrayQuickActions(menu, controllerSession, controller, radio);
        auto* mute = menu.findChild<QAction*>("trayMuteInput");
        auto* deafen = menu.findChild<QAction*>("trayMuteOutput");
        auto* leave = menu.findChild<QAction*>("trayLeaveVoice");
        auto* radioAction = menu.findChild<QAction*>("trayRadio");
        QVERIFY(mute);
        QVERIFY(deafen);
        QVERIFY(leave);
        QVERIFY(radioAction);
        QVERIFY(!mute->isVisible());
        QVERIFY(!deafen->isVisible());
        QVERIFY(!leave->isVisible());
        QVERIFY(mute->isChecked());
        QVERIFY(!deafen->isChecked());
        QVERIFY(!radioAction->isEnabled());

        QVERIFY(controller.remoteAction("join", {{"hostId", target.ownId()}}));
        QTRY_VERIFY(target.joined());
        QTRY_VERIFY(controller.remoteView().value("joined").toBool());
        QVERIFY(mute->isVisible());
        QVERIFY(deafen->isVisible());
        QVERIFY(leave->isVisible());

        mute->trigger();
        QTRY_VERIFY(!targetSession.muted());
        QTRY_VERIFY(!mute->isChecked());
        deafen->trigger();
        QTRY_VERIFY(targetSession.deafened());
        QTRY_VERIFY(deafen->isChecked());

        leave->trigger();
        QTRY_VERIFY(!target.joined());
        QTRY_VERIFY(!controller.remoteView().value("joined").toBool());
        QVERIFY(!mute->isVisible());
        QVERIFY(!deafen->isVisible());
        QVERIFY(!leave->isVisible());
    }

    void radioToggleStartsAndStopsOnlyLocalSelectedState() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("radio.json");
        const QString stationId = "45d3b1a6-79bb-4e73-9123-c2fbe701ad9c";
        const QJsonObject station{{"id", stationId}, {"name", "Local test stream"},
            {"url", "http://127.0.0.1:1/stream"}};
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto settings = QJsonDocument(QJsonObject{{"version", 1}, {"stations", QJsonArray{station}},
            {"selected", stationId}, {"playing", false}}).toJson();
        QCOMPARE(file.write(settings), qint64(settings.size()));
        file.close();

        VoiceSession session(directory.filePath("session.json"));
        LocalChannel channel(session, directory.filePath("channel.json"), TlsIdentity::create());
        RadioPlayer radio(path);
        QMenu menu;
        addTrayQuickActions(menu, session, channel, radio);
        auto* radioAction = menu.findChild<QAction*>("trayRadio");
        QVERIFY(radioAction);
        QVERIFY(radioAction->isEnabled());
        QVERIFY(!radioAction->isChecked());

        radioAction->trigger();
        QVERIFY(radio.active());
        QVERIFY(radioAction->isChecked());
        radioAction->trigger();
        QVERIFY(!radio.active());
        QVERIFY(!radioAction->isChecked());
    }
};

QTEST_MAIN(TrayMenuTests)
#include "tray_menu_tests.moc"
