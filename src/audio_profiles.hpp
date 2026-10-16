#pragma once

#include "audio_processor.hpp"
#include "voice_mixer.hpp"
#include <QSettings>
#include <QString>
#include <optional>

namespace squad {

// Owns durable settings, keyed by opaque device ID and direction. Selection
// aliases such as the system default are stored separately from profiles.
class AudioProfiles final {
public:
    explicit AudioProfiles(const QString& filename);
    [[nodiscard]] InputProfile input(const QByteArray& deviceId) const;
    [[nodiscard]] double musicVolume(const QByteArray& deviceId, const QString& source = {}) const;
    AudioProfiles& saveMusicVolume(const QByteArray& deviceId, double volume);
    AudioProfiles& saveChannelMusicVolume(const QByteArray& deviceId, const QString& source, std::optional<double> volume);
    [[nodiscard]] double notificationVolume(const QByteArray& deviceId) const;
    AudioProfiles& saveNotificationVolume(const QByteArray& deviceId, double volume);
    [[nodiscard]] bool readBotMessages(const QByteArray& deviceId) const;
    AudioProfiles& saveReadBotMessages(const QByteArray& deviceId, bool enabled);
    [[nodiscard]] QString botVoice(const QByteArray& deviceId) const;
    AudioProfiles& saveBotVoice(const QByteArray& deviceId, const QString& voiceId);
    [[nodiscard]] double output(const QByteArray& deviceId) const;
    [[nodiscard]] double participant(const QByteArray& deviceId, const QString& peer) const;
    [[nodiscard]] bool automaticVolume(const QByteArray& deviceId) const;
    [[nodiscard]] double noiseSuppression(const QByteArray& deviceId) const;
    [[nodiscard]] std::optional<double> echoSuppression(const QByteArray& deviceId) const;
    [[nodiscard]] VoiceActivation voiceActivation(const QByteArray& deviceId) const;
    AudioProfiles& saveInput(const QByteArray& deviceId, InputProfile profile);
    AudioProfiles& saveOutput(const QByteArray& deviceId, double volume);
    AudioProfiles& saveParticipant(const QByteArray& deviceId, const QString& peer, double volume);
    AudioProfiles& saveAutomaticVolume(const QByteArray& deviceId, bool enabled);
    AudioProfiles& saveNoiseSuppression(const QByteArray& deviceId, double strength);
    AudioProfiles& saveEchoSuppression(const QByteArray& deviceId, std::optional<double> strength);
    AudioProfiles& saveVoiceActivation(const QByteArray& deviceId, VoiceActivation activation);
    [[nodiscard]] QByteArray selection(bool input) const;
    AudioProfiles& select(bool input, const QByteArray& deviceId);

private:
    [[nodiscard]] static QString key(const QByteArray& deviceId, bool input);
    AudioProfiles& write(QVariantMap values);
    mutable QSettings settings_;
};

}
