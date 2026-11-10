#pragma once

#include "audio_model.hpp"
#include "chat_content.hpp"
#include "local_channel.hpp"
#include "voice_session.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QTimer>

// One desktop/mobile policy for cues and optional local speech. Headless hosts
// never instantiate this owner. Protocol events remain independent of playback.
class AppFeedback final : public QObject {
    Q_OBJECT
public:
    AppFeedback(AudioModel& audio, VoiceSession& session, LocalChannel& channel, QObject* parent = nullptr)
        : QObject(parent), audio_(audio), session_(session), channel_(channel) {
        connect(&channel, &LocalChannel::channelEvent, this, [this](const QString& kind, const QString& text) {
            if (!quitting_ && session_.eventSounds()) audio_.playEvent(kind, text);
        });
        connect(&channel, &LocalChannel::chatNotification, this, [this](const QString&, const QVariantMap& message) {
            if (quitting_ || !session_.eventSounds()) return;
            const bool announcement = message.value("event").toMap().value("kind") == "announcement";
            audio_.playEvent(announcement ? "announcement" : "message", announcement ? ChatContent{}.plainText(message) : QString{});
        });
        connect(&session, &VoiceSession::preferencesChanged, this, [this] {
            if (!session_.eventSounds()) audio_.cancelNotifications();
        });
        connect(&audio, &AudioModel::notificationsFinished, this, &AppFeedback::finishShutdown);
        deadline_.setSingleShot(true);
        deadline_.setInterval(6000);
        connect(&deadline_, &QTimer::timeout, this, &AppFeedback::finishShutdown);
    }

    bool start() {
        return !quitting_ && session_.eventSounds()
            && audio_.playEvent("start", tr("SquadSpeak started."));
    }

    void shutdown() {
        if (quitting_) return;
        audio_.cancelNotifications();
        quitting_ = true;
        channel_.leave();
        audio_.setVoiceState(false, false);
        audio_.stop();
        if (session_.eventSounds() && audio_.playEvent("quit", tr("SquadSpeak is closing."))) deadline_.start();
        else finishShutdown();
    }

signals:
    void shutdownReady();

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() != QEvent::Quit || ready_) return false;
        shutdown();
        event->ignore();
        return true;
    }

private:
    void finishShutdown() {
        if (!quitting_ || ready_) return;
        ready_ = true;
        deadline_.stop();
        audio_.cancelNotifications();
        emit shutdownReady();
    }
    AudioModel& audio_;
    VoiceSession& session_;
    LocalChannel& channel_;
    QTimer deadline_;
    bool quitting_ = false;
    bool ready_ = false;
};
