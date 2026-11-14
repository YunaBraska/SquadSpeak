#include "tray_menu.hpp"
#include "local_channel.hpp"
#include "radio_player.hpp"
#include "voice_session.hpp"
#include <QAction>
#include <QCoreApplication>
#include <QMenu>

void addTrayQuickActions(QMenu& menu, VoiceSession& session, LocalChannel& channel, RadioPlayer& radio,
                         const std::function<void()>& leaveRequested) {
    const auto translate = [](const char* text) { return QCoreApplication::translate("Channels", text); };
    auto* mute = menu.addAction(translate("Mute microphone"));
    mute->setObjectName("trayMuteInput");
    mute->setCheckable(true);

    auto* deafen = menu.addAction(translate("Mute speakers"));
    deafen->setObjectName("trayMuteOutput");
    deafen->setCheckable(true);

    auto* leave = menu.addAction(translate("Leave"));
    leave->setObjectName("trayLeaveVoice");

    auto* radioAction = menu.addAction(translate("Radio"));
    radioAction->setObjectName("trayRadio");
    radioAction->setCheckable(true);

    const auto update = [mute, deafen, leave, radioAction, &session, &channel, &radio, translate] {
        const bool remote = channel.remoteMode();
        const auto remoteView = channel.remoteView();
        const bool muted = remote ? remoteView.value("muted").toBool() : session.muted();
        const bool deafened = remote ? remoteView.value("deafened").toBool() : session.deafened();
        const bool joined = remote ? remoteView.value("joined").toBool() : channel.joined();
        mute->setText(translate(muted ? "Unmute microphone" : "Mute microphone"));
        mute->setChecked(muted);
        mute->setVisible(joined);
        mute->setEnabled(!remote || channel.remoteAllowed());
        deafen->setText(translate(deafened ? "Enable speakers" : "Mute speakers"));
        deafen->setChecked(deafened);
        deafen->setVisible(joined);
        deafen->setEnabled(!remote || channel.remoteAllowed());
        leave->setText(translate("Leave"));
        leave->setVisible(joined);
        leave->setEnabled(!remote || channel.remoteAllowed());
        radioAction->setText(translate("Radio"));
        radioAction->setChecked(radio.active());
        radioAction->setEnabled(!remote && (radio.active() || !radio.selectedId().isEmpty()));
    };
    QObject::connect(mute, &QAction::triggered, &menu, [&session, &channel, update](bool checked) {
        const bool accepted = channel.remoteMode()
            ? channel.remoteAction("mute", {{"value", checked}})
            : session.setMuted(checked);
        if (!accepted) update();
    });
    QObject::connect(deafen, &QAction::triggered, &menu, [&session, &channel, update](bool checked) {
        const bool accepted = channel.remoteMode()
            ? channel.remoteAction("deafen", {{"value", checked}})
            : session.setDeafened(checked);
        if (!accepted) update();
    });
    QObject::connect(leave, &QAction::triggered, &menu, [&channel, update, leaveRequested] {
        if (leaveRequested) { leaveRequested(); return; }
        const bool accepted = channel.remoteMode() ? channel.remoteAction("leave") : channel.leave();
        if (!accepted) update();
    });
    QObject::connect(radioAction, &QAction::triggered, &menu, [&channel, &radio, update](bool checked) {
        if (channel.remoteMode()) { update(); return; }
        const bool accepted = checked ? radio.play(radio.selectedId()) : radio.stop();
        if (!accepted) update();
    });
    QObject::connect(&menu, &QMenu::aboutToShow, &menu, update);
    QObject::connect(&session, &VoiceSession::preferencesChanged, &menu, update);
    QObject::connect(&channel, &LocalChannel::stateChanged, &menu, update);
    QObject::connect(&channel, &LocalChannel::remoteChanged, &menu, update);
    QObject::connect(&radio, &RadioPlayer::changed, &menu, update);
    update();
}
