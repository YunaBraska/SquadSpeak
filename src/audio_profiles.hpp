#pragma once

#include "audio_processor.hpp"
#include "voice_mixer.hpp"
#include <QSettings>
#include <QString>

namespace squad {

// Owns durable settings, keyed by opaque device ID and direction. Selection
// aliases such as the system default are stored separately from profiles.
class AudioProfiles final {
public:
    explicit AudioProfiles(const QString& filename);
    [[nodiscard]] InputProfile input(const QByteArray& deviceId) const;
    [[nodiscard]] double musicVolume(const QByteArray& deviceId) const;
    AudioProfiles& saveMusicVolume(const QByteArray& deviceId, double volume);
    [[nodiscard]] double output(const QByteArray& deviceId) const;
    [[nodiscard]] double participant(const QByteArray& deviceId, const QString& peer) const;
    [[nodiscard]] bool automaticVolume(const QByteArray& deviceId) const;
    [[nodiscard]] double noiseSuppression(const QByteArray& deviceId) const;
    [[nodiscard]] bool echoCancellation(const QByteArray& deviceId) const;
    [[nodiscard]] VoiceActivation voiceActivation(const QByteArray& deviceId) const;
    AudioProfiles& saveInput(const QByteArray& deviceId, InputProfile profile);
    AudioProfiles& saveOutput(const QByteArray& deviceId, double volume);
    AudioProfiles& saveParticipant(const QByteArray& deviceId, const QString& peer, double volume);
    AudioProfiles& saveAutomaticVolume(const QByteArray& deviceId, bool enabled);
    AudioProfiles& saveNoiseSuppression(const QByteArray& deviceId, double strength);
    AudioProfiles& saveEchoCancellation(const QByteArray& deviceId, bool enabled);
    AudioProfiles& saveVoiceActivation(const QByteArray& deviceId, VoiceActivation activation);
    [[nodiscard]] QByteArray selection(bool input) const;
    AudioProfiles& select(bool input, const QByteArray& deviceId);

private:
    [[nodiscard]] static QString key(const QByteArray& deviceId, bool input);
    AudioProfiles& write(const QVariantMap& values);
    mutable QSettings settings_;
};

}
