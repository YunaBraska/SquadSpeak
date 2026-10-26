#include "audio_profiles.hpp"
#include <QCoreApplication>
#include <QSet>

#include <cmath>
#include <stdexcept>

namespace squad {

AudioProfiles::AudioProfiles(const QString& filename, std::function<qint64()> clock)
    : settings_(filename, QSettings::IniFormat), clock_(std::move(clock)) {
    settings_.setFallbacksEnabled(false);
    if (settings_.status() != QSettings::NoError)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Audio settings could not be read"));
    if (settings_.contains("output/independentVolumes") && settings_.value("output/independentVolumes").toString() != "true")
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored output profile is invalid"));
    if (!settings_.contains("output/independentVolumes")) {
        settings_.beginGroup("output");
        const auto devices = settings_.childGroups();
        settings_.endGroup();
        QVariantMap migrated;
        for (const auto& device : devices) {
            const auto id = QByteArray::fromHex(device.toLatin1());
            const auto prefix = key(id, false);
            const auto master = output(id);
            bool valid;
            const auto music = settings_.value(prefix + "musicVolume", 0.35).toDouble(&valid);
            if (!valid || !std::isfinite(music) || music < 0 || music > 1)
                throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored music volume is invalid"));
            migrated.insert(prefix + "musicVolume", music * master);
            migrated.insert(prefix + "notificationVolume", master);
        }
        if (!migrated.isEmpty()) write(migrated);
    }
    observeContacts({});
}

AudioProfiles& AudioProfiles::observeContacts(const QStringList& ids) {
    constexpr qint64 year = 365LL * 86400000;
    const auto now = clock_();
    QMap<QString, QStringList> entries;
    for (const auto& key : settings_.allKeys()) {
        const auto parts = key.split('/');
        if (parts.size() == 4 && parts[0] == "output"
            && (parts[2] == "participants" || parts[2] == "automaticParticipants" || parts[2] == "music"))
            entries[parts[3]].append(key);
    }
    QSet<QString> seen;
    for (const auto& id : ids) seen.insert(QString::fromLatin1(id.toUtf8().toHex()));
    QVariantMap changes;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const auto key = "contacts/" + it.key();
        bool valid = false;
        auto lastSeen = settings_.value(key).toLongLong(&valid);
        // Older files have no timestamp. Give them a full retention period.
        if (!valid || lastSeen < 0 || lastSeen > now) {
            lastSeen = now;
            changes.insert(key, now);
        }
        if (now - lastSeen >= year && !seen.contains(it.key())) {
            for (const auto& entry : it.value()) changes.insert(entry, {});
            changes.insert(key, {});
        } else if (seen.contains(it.key()) && now - lastSeen >= 60000) {
            changes.insert(key, now);
        }
    }
    for (const auto& key : settings_.allKeys())
        if (key.startsWith("contacts/") && !entries.contains(key.mid(9))) changes.insert(key, {});
    return changes.isEmpty() ? *this : write(changes);
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
    const auto boolean = [&](const QString& name, bool fallback) {
        const auto value = settings_.value(prefix + name, fallback);
        if (value.toString() != "true" && value.toString() != "false")
            throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored microphone profile is invalid"));
        return value.toBool();
    };
    profile.gainAutomatic = boolean("gainAutomatic", !settings_.contains(prefix + "gainDb"));
    profile.highPassAutomatic = boolean("highPassAutomatic", !settings_.contains(prefix + "highPassHz"));
    profile.gainEnabled = boolean("gainEnabled", true);
    profile.highPassEnabled = boolean("highPassEnabled", true);
    profile.lowPassEnabled = boolean("lowPassEnabled", true);
    profile.noiseEnabled = boolean("noiseEnabled", true);
    profile.echoEnabled = boolean("echoEnabled", true);
    if (!gainOk || !highOk || !lowOk || !profile.valid())
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored microphone profile is invalid"));
    return profile;
}

double AudioProfiles::musicVolume(const QByteArray& deviceId, const QString& source) const {
    const auto prefix = key(deviceId, false);
    const auto entry = source.isEmpty() ? prefix + "musicVolume" : prefix + "music/" + source.toUtf8().toHex();
    if (!source.isEmpty() && !settings_.contains(entry)) return musicVolume(deviceId);
    bool valid;
    const auto volume = settings_.value(entry, 0.175).toDouble(&valid);
    if (!valid || !std::isfinite(volume) || volume < 0 || volume > 1)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored music volume is invalid"));
    return volume;
}

AudioProfiles& AudioProfiles::saveMusicVolume(const QByteArray& deviceId, double volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Music volume must be between 0 and 1"));
    return write({{key(deviceId, false) + "musicVolume", volume}});
}

AudioProfiles& AudioProfiles::saveChannelMusicVolume(const QByteArray& deviceId, const QString& source, std::optional<double> volume) {
    if (source.isEmpty()) throw std::invalid_argument("A channel music profile requires a source ID");
    if (volume && (!std::isfinite(*volume) || *volume < 0 || *volume > 1))
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Music volume must be between 0 and 1"));
    return write({{key(deviceId, false) + "music/" + source.toUtf8().toHex(), volume ? QVariant(*volume) : QVariant{}}});
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

double AudioProfiles::notificationVolume(const QByteArray& deviceId) const {
    bool valid;
    const auto volume = settings_.value(key(deviceId, false) + "notificationVolume", 0.5).toDouble(&valid);
    if (!valid || !std::isfinite(volume) || volume < 0 || volume > 1)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored output profile is invalid"));
    return volume;
}

AudioProfiles& AudioProfiles::saveNotificationVolume(const QByteArray& deviceId, double volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Output volume must be between 0 and 1"));
    return write({{key(deviceId, false) + "notificationVolume", volume}});
}

bool AudioProfiles::readBotMessages(const QByteArray& deviceId) const {
    const auto value = settings_.value(key(deviceId, false) + "readBotMessages", false);
    if (value.toString() != "true" && value.toString() != "false")
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored output profile is invalid"));
    return value.toBool();
}

AudioProfiles& AudioProfiles::saveReadBotMessages(const QByteArray& deviceId, bool enabled) {
    return write({{key(deviceId, false) + "readBotMessages", enabled}});
}

QString AudioProfiles::botVoice(const QByteArray& deviceId) const {
    return settings_.value(key(deviceId, false) + "botVoice").toString();
}

AudioProfiles& AudioProfiles::saveBotVoice(const QByteArray& deviceId, const QString& voiceId) {
    return write({{key(deviceId, false) + "botVoice", voiceId}});
}

bool AudioProfiles::automaticVolume(const QByteArray& deviceId, const QString& peer) const {
    const auto value = peer.isEmpty()
        ? settings_.value(key(deviceId, false) + "automaticVolume", true)
        : settings_.value(key(deviceId, false) + "automaticParticipants/" + peer.toUtf8().toHex(), automaticVolume(deviceId));
    if (value.toString() != "true" && value.toString() != "false")
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored automatic volume setting is invalid"));
    return value.toBool();
}

double AudioProfiles::noiseSuppression(const QByteArray& deviceId) const {
    bool valid;
    const auto value = settings_.value(key(deviceId, true) + "noiseSuppression", 0.5).toDouble(&valid);
    if (!valid || !std::isfinite(value) || value < 0 || value > 1)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored noise suppression strength is invalid"));
    return value;
}

std::optional<double> AudioProfiles::echoSuppression(const QByteArray& deviceId) const {
    const auto prefix = key(deviceId, true);
    if (settings_.contains(prefix + "echoSuppression")) {
        bool valid;
        const auto strength = settings_.value(prefix + "echoSuppression").toDouble(&valid);
        if (!valid || !std::isfinite(strength) || strength < 0 || strength > 1)
            throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored microphone profile is invalid"));
        return strength;
    }
    const auto legacy = settings_.value(prefix + "echoCancellation", true).toString();
    if (legacy != "true" && legacy != "false")
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored microphone profile is invalid"));
    return legacy == "false" ? std::optional(0.0) : std::nullopt;
}

AudioProfiles& AudioProfiles::saveEchoSuppression(const QByteArray& deviceId, std::optional<double> strength) {
    if (strength && (!std::isfinite(*strength) || *strength < 0 || *strength > 1))
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Invalid microphone profile"));
    const auto prefix = key(deviceId, true);
    return write({{prefix + "echoSuppression", strength ? QVariant(*strength) : QVariant{}},
                  {prefix + "echoCancellation", QVariant{}}});
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
    const auto minimum = settings_.value(prefix + "minimumEnabled", false);
    bool valid, minimumValid;
    VoiceActivation result{enabled.toBool(), automatic.toBool(), settings_.value(prefix + "thresholdDb", -45.0).toDouble(&valid),
        minimum.toBool(), settings_.value(prefix + "minimumDb", -45.0).toDouble(&minimumValid)};
    if ((enabled.toString() != "true" && enabled.toString() != "false")
        || (automatic.toString() != "true" && automatic.toString() != "false")
        || (minimum.toString() != "true" && minimum.toString() != "false") || !valid || !minimumValid || !result.valid())
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Stored voice activation settings are invalid"));
    return result;
}

AudioProfiles& AudioProfiles::saveVoiceActivation(const QByteArray& deviceId, VoiceActivation activation) {
    if (!activation.valid()) throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Voice activation threshold must be between -80 and 0 dBFS"));
    const auto prefix = key(deviceId, true) + "activation/";
    return write({{prefix + "enabled", activation.enabled}, {prefix + "automatic", activation.automatic},
                  {prefix + "thresholdDb", activation.thresholdDb},
                  {prefix + "minimumEnabled", activation.minimumEnabled}, {prefix + "minimumDb", activation.minimumDb}});
}

AudioProfiles& AudioProfiles::saveParticipant(const QByteArray& deviceId, const QString& peer, double volume) {
    if (peer.isEmpty() || !std::isfinite(volume) || volume < 0 || volume > 2)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Participant volume must be between 0 and 2"));
    return write({{key(deviceId, false) + "participants/" + peer.toUtf8().toHex(), volume}});
}

AudioProfiles& AudioProfiles::saveAutomaticVolume(const QByteArray& deviceId, bool enabled, const QString& peer) {
    return write({{key(deviceId, false) + (peer.isEmpty() ? QStringLiteral("automaticVolume")
        : "automaticParticipants/" + peer.toUtf8().toHex()), enabled}});
}

AudioProfiles& AudioProfiles::saveInput(const QByteArray& deviceId, InputProfile profile) {
    if (!profile.valid())
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Invalid microphone profile"));
    const auto prefix = key(deviceId, true);
    return write({{prefix + "gainDb", profile.gainDb},
                  {prefix + "highPassHz", profile.highPassHz},
                  {prefix + "lowPassHz", profile.lowPassHz},
                  {prefix + "gainAutomatic", profile.gainAutomatic},
                  {prefix + "highPassAutomatic", profile.highPassAutomatic},
                  {prefix + "gainEnabled", profile.gainEnabled},
                  {prefix + "highPassEnabled", profile.highPassEnabled},
                  {prefix + "lowPassEnabled", profile.lowPassEnabled},
                  {prefix + "noiseEnabled", profile.noiseEnabled},
                  {prefix + "echoEnabled", profile.echoEnabled}});
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

AudioProfiles& AudioProfiles::write(QVariantMap values) {
    if (settings_.status() != QSettings::NoError)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Audio settings are unavailable; fix file access and restart the app"));
    QVariantMap previous;
    QVariantMap contacts;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const auto parts = it.key().split('/');
        if (it.value().isValid() && parts.size() == 4 && parts[0] == "output"
            && (parts[2] == "participants" || parts[2] == "automaticParticipants" || parts[2] == "music")
            && !settings_.contains("contacts/" + parts[3])) contacts.insert("contacts/" + parts[3], clock_());
    }
    values.insert(contacts);
    values.insert("output/independentVolumes", true);
    for (auto entry = values.cbegin(); entry != values.cend(); ++entry) {
        previous.insert(entry.key(), settings_.value(entry.key()));
        if (entry.value().isValid()) settings_.setValue(entry.key(), entry.value());
        else settings_.remove(entry.key());
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
