#include "audio_profiles.hpp"
#include <QCoreApplication>

#include <cmath>
#include <stdexcept>

namespace squad {

AudioProfiles::AudioProfiles(const QString& filename) : settings_(filename, QSettings::IniFormat) {
    settings_.setFallbacksEnabled(false);
    if (settings_.status() != QSettings::NoError)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Audio settings could not be read"));
}

QString AudioProfiles::key(const QByteArray& deviceId, bool input) {
    if (deviceId.isEmpty())
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "A profile requires an actual device ID"));
    return QString(input ? "input/" : "output/") + QString::fromLatin1(deviceId.toHex()) + '/';
}

InputProfile AudioProfiles::input(const QByteArray& deviceId) const {
    const auto prefix = key(deviceId, true);
    InputProfile profile;
    bool gainOk, highOk, lowOk;
    profile.gainDb = settings_.value(prefix + "gainDb", 0.0).toDouble(&gainOk);
    profile.highPassHz = settings_.value(prefix + "highPassHz", 0.0).toDouble(&highOk);
    profile.lowPassHz = settings_.value(prefix + "lowPassHz", 0.0).toDouble(&lowOk);
    const auto automatic = [&](const QString& name, const QString& manual) {
        const auto value = settings_.value(prefix + name, !settings_.contains(prefix + manual));
        if (value.toString() != "true" && value.toString() != "false")
            throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored microphone profile is invalid"));
        return value.toBool();
    };
    profile.gainAutomatic = automatic("gainAutomatic", "gainDb");
    profile.highPassAutomatic = automatic("highPassAutomatic", "highPassHz");
    if (!gainOk || !highOk || !lowOk || !profile.valid())
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored microphone profile is invalid"));
    return profile;
}

double AudioProfiles::musicVolume(const QByteArray& deviceId) const {
    bool valid;
    const auto volume = settings_.value(key(deviceId, false) + "musicVolume", 0.35).toDouble(&valid);
    if (!valid || !std::isfinite(volume) || volume < 0 || volume > 1)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored music volume is invalid"));
    return volume;
}

AudioProfiles& AudioProfiles::saveMusicVolume(const QByteArray& deviceId, double volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Music volume must be between 0 and 1"));
    return write({{key(deviceId, false) + "musicVolume", volume}});
}

double AudioProfiles::output(const QByteArray& deviceId) const {
    bool valid;
    const auto volume = settings_.value(key(deviceId, false) + "volume", 0.5).toDouble(&valid);
    if (!valid || !std::isfinite(volume) || volume < 0 || volume > 1)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored output profile is invalid"));
    return volume;
}

double AudioProfiles::participant(const QByteArray& deviceId, const QString& peer) const {
    bool valid;
    const auto value = settings_.value(key(deviceId, false) + "participants/" + peer.toUtf8().toHex(), 1.0).toDouble(&valid);
    if (!valid || !std::isfinite(value) || value < 0 || value > 2)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored participant volume is invalid"));
    return value;
}

bool AudioProfiles::automaticVolume(const QByteArray& deviceId) const {
    const auto value = settings_.value(key(deviceId, false) + "automaticVolume", true);
    if (value.toString() != "true" && value.toString() != "false")
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored automatic volume setting is invalid"));
    return value.toBool();
}

double AudioProfiles::noiseSuppression(const QByteArray& deviceId) const {
    bool valid;
    const auto value = settings_.value(key(deviceId, true) + "noiseSuppression", 0.0).toDouble(&valid);
    if (!valid || !std::isfinite(value) || value < 0 || value > 1)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored noise suppression strength is invalid"));
    return value;
}

bool AudioProfiles::echoCancellation(const QByteArray& deviceId) const {
    const auto value = settings_.value(key(deviceId, true) + "echoCancellation", true);
    if (value.toString() != "true" && value.toString() != "false")
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored microphone profile is invalid"));
    return value.toBool();
}

AudioProfiles& AudioProfiles::saveEchoCancellation(const QByteArray& deviceId, bool enabled) {
    return write({{key(deviceId, true) + "echoCancellation", enabled}});
}

AudioProfiles& AudioProfiles::saveNoiseSuppression(const QByteArray& deviceId, double strength) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression strength must be between 0 and 1"));
    return write({{key(deviceId, true) + "noiseSuppression", strength}});
}

VoiceActivation AudioProfiles::voiceActivation(const QByteArray& deviceId) const {
    const auto prefix = key(deviceId, true) + "activation/";
    const auto enabled = settings_.value(prefix + "enabled", true);
    const auto automatic = settings_.value(prefix + "automatic", true);
    bool valid;
    VoiceActivation result{enabled.toBool(), automatic.toBool(), settings_.value(prefix + "thresholdDb", -45.0).toDouble(&valid)};
    if ((enabled.toString() != "true" && enabled.toString() != "false")
        || (automatic.toString() != "true" && automatic.toString() != "false") || !valid || !result.valid())
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored voice activation settings are invalid"));
    return result;
}

AudioProfiles& AudioProfiles::saveVoiceActivation(const QByteArray& deviceId, VoiceActivation activation) {
    if (!activation.valid()) throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Voice activation threshold must be between -80 and 0 dBFS"));
    const auto prefix = key(deviceId, true) + "activation/";
    return write({{prefix + "enabled", activation.enabled}, {prefix + "automatic", activation.automatic},
                  {prefix + "thresholdDb", activation.thresholdDb}});
}

AudioProfiles& AudioProfiles::saveParticipant(const QByteArray& deviceId, const QString& peer, double volume) {
    if (peer.isEmpty() || !std::isfinite(volume) || volume < 0 || volume > 2)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Participant volume must be between 0 and 2"));
    return write({{key(deviceId, false) + "participants/" + peer.toUtf8().toHex(), volume}});
}

AudioProfiles& AudioProfiles::saveAutomaticVolume(const QByteArray& deviceId, bool enabled) {
    return write({{key(deviceId, false) + "automaticVolume", enabled}});
}

AudioProfiles& AudioProfiles::saveInput(const QByteArray& deviceId, InputProfile profile) {
    if (!profile.valid())
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Invalid microphone profile"));
    const auto prefix = key(deviceId, true);
    return write({{prefix + "gainDb", profile.gainDb},
                  {prefix + "highPassHz", profile.highPassHz},
                  {prefix + "lowPassHz", profile.lowPassHz},
                  {prefix + "gainAutomatic", profile.gainAutomatic},
                  {prefix + "highPassAutomatic", profile.highPassAutomatic}});
}

AudioProfiles& AudioProfiles::saveOutput(const QByteArray& deviceId, double volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Output volume must be between 0 and 1"));
    return write({{key(deviceId, false) + "volume", volume}});
}

QByteArray AudioProfiles::selection(bool input) const {
    return settings_.value(input ? "selection/input" : "selection/output").toByteArray();
}

AudioProfiles& AudioProfiles::select(bool input, const QByteArray& deviceId) {
    return write({{input ? "selection/input" : "selection/output", deviceId}});
}

AudioProfiles& AudioProfiles::write(const QVariantMap& values) {
    if (settings_.status() != QSettings::NoError)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Audio settings are unavailable; fix file access and restart the app"));
    QVariantMap previous;
    for (auto entry = values.cbegin(); entry != values.cend(); ++entry) {
        previous.insert(entry.key(), settings_.value(entry.key()));
        settings_.setValue(entry.key(), entry.value());
    }
    settings_.sync();
    if (settings_.status() != QSettings::NoError) {
        for (auto entry = previous.cbegin(); entry != previous.cend(); ++entry) {
            if (entry.value().isValid()) settings_.setValue(entry.key(), entry.value());
            else settings_.remove(entry.key());
        }
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Audio settings could not be saved"));
    }
    return *this;
}

}
