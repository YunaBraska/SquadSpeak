#include "voice_session.hpp"

#include <QFile>
#include <QDir>
#include <QLocale>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTranslator>
#include <stdexcept>
#include <utility>

VoiceSession::VoiceSession(QString settingsFile, QObject* parent)
    : QObject(parent), settingsFile_(std::move(settingsFile)) {
    static const auto initialized = [] {
        Q_INIT_RESOURCE(squad_session_translations);
#ifdef SQUAD_STANDARD_TRANSLATIONS
        Q_INIT_RESOURCE(standard_translations);
#endif
        return true;
    }();
    (void)initialized;
    if (settingsFile_.isEmpty()) return;
    QFile file(settingsFile_);
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8192)
        throw std::runtime_error(tr("Session settings could not be read.").toUtf8().constData());
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto object = document.object();
    // A corrupt field must not hide a valid saved language. This catalog is
    // local to validation; constructing a session never changes the app locale.
    const auto savedLanguage = object.contains("language") ? object.value("language").toString() : QStringLiteral("en");
    const auto supported = languages();
    const bool knownLanguage = std::any_of(supported.cbegin(), supported.cend(), [&](const auto& item) {
        return item.toMap().value("code").toString() == savedLanguage;
    });
    const auto invalid = [&](const QString& key) {
        QTranslator catalog;
        if (knownLanguage && savedLanguage != "en") (void)catalog.load(":/i18n/squadspeak_" + savedLanguage + ".qm");
        auto message = catalog.translate("VoiceSession", "Saved setting is invalid: %1");
        if (message.isEmpty()) message = tr("Saved setting is invalid: %1");
        return std::runtime_error(message.arg(key).toUtf8().constData());
    };
    if (error.error != QJsonParseError::NoError || !document.isObject()) throw invalid("session");
    if (object.value("version").toInt() != 1 && object.value("version").toInt() != 2) throw invalid("version");
    if (!object.value("userName").isString()) throw invalid("userName");
    if (!object.value("muted").isBool()) throw invalid("muted");
    if (!knownLanguage) throw invalid("language");
    const auto savedName = [&](const QString& value, const QString& key) {
        try { return validatedName(value); }
        catch (const std::invalid_argument&) { throw invalid(key); }
    };
    preferences_.insert("language", savedLanguage);
    preferences_.insert("userName", savedName(object.value("userName").toString(), "userName"));
    if (object.contains("channelName")) legacyChannelName_ = savedName(object.value("channelName").toString(), "channelName");
    preferences_.insert("muted", object.value("muted"));
    for (const auto* key : {"deafened", "eventSounds", "remotePttKeyHeld"}) {
        if (!object.contains(key)) continue;
        if (!object.value(key).isBool()) throw invalid(key);
        preferences_.insert(key, object.value(key));
    }
    if (object.contains("avatar")) {
        if (!validAvatar(object.value("avatar").toString())) throw invalid("avatar");
        preferences_.insert("avatar", object.value("avatar"));
    }
    if (object.contains("theme")) {
        const auto theme = object.value("theme").toString();
        if (theme != "system" && theme != "light" && theme != "dark")
            throw invalid("theme");
        preferences_.insert("theme", theme);
    }
    if (object.contains("palette")) {
        const auto palette = object.value("palette").toString();
        if (palette != "plum" && palette != "ocean" && palette != "forest" && palette != "graphite")
            throw invalid("palette");
        preferences_.insert("palette", palette);
    }
    if (object.contains("remotePttKeyCode")) {
        const auto code = object.value("remotePttKeyCode").toInt(-2);
        if (code < -1 || code > 65535 || object.value("remotePttKeyCode").toDouble(-2) != code
            || !object.value("remotePttKeyName").isString()
            || (code == -1 && !object.value("remotePttKeyName").toString().isEmpty()))
            throw invalid("remotePttKeyCode");
        if (code >= 0) (void)savedName(object.value("remotePttKeyName").toString(), "remotePttKeyName");
        preferences_.insert("remotePttKeyCode", code); preferences_.insert("remotePttKeyName", object.value("remotePttKeyName"));
    }
    if (object.contains("animatedAvatars")) {
        if (!object.value("animatedAvatars").isBool()) throw invalid("animatedAvatars");
        preferences_.insert("animatedAvatars", object.value("animatedAvatars"));
    }
    if (object.value("version").toInt() == 2) {
        for (const auto* key : {"pushToTalk", "pttButtonHeld", "pttKeyHeld"}) {
            if (!object.value(key).isBool()) throw invalid(key);
            preferences_.insert(key, object.value(key));
        }
        const auto code = object.value("pttKeyCode").toInt(-2);
        if (!object.value("pttKeyCode").isDouble() || code < -1 || code > 65535
            || object.value("pttKeyCode").toDouble() != code || !object.value("pttKeyName").isString()
            || (code == -1 && !object.value("pttKeyName").toString().isEmpty()))
            throw invalid("pttKeyCode");
        if (code >= 0) (void)savedName(object.value("pttKeyName").toString(), "pttKeyName");
        preferences_.insert("pttKeyCode", code);
        preferences_.insert("pttKeyName", object.value("pttKeyName"));
        const auto readBinding = [&](const char* codesKey, const char* namesKey, const char* legacyCodeKey, const char* legacyNameKey) {
            if (!object.contains(codesKey) && !object.contains(namesKey)) {
                const auto legacyCode = preferences_.value(legacyCodeKey).toInt();
                const auto legacyName = preferences_.value(legacyNameKey).toString();
                QJsonArray codes;
                QJsonArray names;
                if (legacyCode >= 0) {
                    codes.append(legacyCode);
                    names.append(legacyName);
                }
                preferences_.insert(codesKey, codes);
                preferences_.insert(namesKey, names);
                return;
            }
            if (!object.value(codesKey).isArray() || !object.value(namesKey).isArray())
                throw invalid(codesKey);
            const auto codes = object.value(codesKey).toArray();
            const auto names = object.value(namesKey).toArray();
            if (codes.size() != names.size())
                throw invalid(codesKey);
            if (codes.isEmpty()) {
                if (preferences_.value(legacyCodeKey).toInt(-1) >= 0 || !preferences_.value(legacyNameKey).toString().isEmpty())
                    throw invalid(codesKey);
                preferences_.insert(codesKey, codes);
                preferences_.insert(namesKey, names);
                preferences_.insert(legacyCodeKey, -1);
                preferences_.insert(legacyNameKey, QString{});
                return;
            }
            QStringList uniqueCodes;
            int labelLength = 0;
            for (int i = 0; i < codes.size(); ++i) {
                const auto code = codes.at(i).toInt(-2);
                if (!codes.at(i).isDouble() || code < 0 || code > 65535 || codes.at(i).toDouble(-2) != code
                    || !names.at(i).isString())
                    throw invalid(codesKey);
                (void)savedName(names.at(i).toString(), namesKey);
                if (uniqueCodes.contains(QString::number(code)))
                    throw invalid(codesKey);
                uniqueCodes.append(QString::number(code));
                labelLength += names.at(i).toString().size();
            }
            if (codes.size() > 16 || labelLength > 256)
                throw invalid(codesKey);
            preferences_.insert(codesKey, codes);
            preferences_.insert(namesKey, names);
            preferences_.insert(legacyCodeKey, codes.first());
            preferences_.insert(legacyNameKey, names.first());
        };
        readBinding("pttKeyCodes", "pttKeyNames", "pttKeyCode", "pttKeyName");
        readBinding("remotePttKeyCodes", "remotePttKeyNames", "remotePttKeyCode", "remotePttKeyName");
        if (object.contains("pttRevision")) {
            const auto revision = object.value("pttRevision").toInteger(-1);
            if (revision < 0 || revision > 9007199254740991LL || object.value("pttRevision").toDouble(-1) != double(revision))
                throw invalid("pttRevision");
            preferences_.insert("pttRevision", revision);
        }
    }
}

QString VoiceSession::validatedName(const QString& value) {
    const auto name = value.trimmed();
    const auto points = name.toUcs4();
    if (points.isEmpty() || points.size() > 64)
        throw std::invalid_argument(tr("The name must contain 1 to 64 characters.").toUtf8().constData());
    for (const auto point : points) {
        const auto category = QChar::category(point);
        if (category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            throw std::invalid_argument(tr("The name may not contain control characters or line breaks.").toUtf8().constData());
    }
    return name;
}

QString VoiceSession::presenceText() const {
    if (!available()) return tr("Unavailable - audio settings or tone test");
    if (muted()) return tr("Microphone muted");
    if (pushToTalk()) return pttHeld() ? tr("Push-to-talk held") : tr("Ready - push-to-talk released");
    return tr("Microphone unmuted");
}

bool VoiceSession::fail(QString message) {
    error_ = std::move(message);
    emit errorChanged();
    return false;
}

bool VoiceSession::save(QJsonObject preferences) {
    if (preferences == preferences_) {
        if (!error_.isEmpty()) { error_.clear(); emit errorChanged(); }
        return true;
    }
    const auto data = QJsonDocument(preferences).toJson(QJsonDocument::Compact);
    if (!settingsFile_.isEmpty()) {
        QSaveFile file(settingsFile_);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
            return fail(tr("Settings could not be saved: %1").arg(file.errorString()));
    }
    preferences_ = std::move(preferences);
    error_.clear();
    emit errorChanged();
    emit preferencesChanged();
    emit presenceChanged();
    return true;
}

bool VoiceSession::edit(const QString& key, const QJsonValue& value) {
    auto next = preferences_;
    if ((key == "pttButtonHeld" || key == "pttKeyHeld" || key == "remotePttKeyHeld") && next.value(key) != value) {
        if (pttInputRevision() == 9007199254740991LL) return fail(tr("The push-to-talk sequence cannot be incremented further."));
        next.insert("pttRevision", pttInputRevision() + 1);
    }
    next.insert(key, value);
    return save(std::move(next));
}

bool VoiceSession::setUserName(const QString& name) {
    try { return edit("userName", validatedName(name)); }
    catch (const std::invalid_argument& error) { return fail(QString::fromUtf8(error.what())); }
}

bool VoiceSession::setTheme(const QString& theme) {
    if (theme != "system" && theme != "light" && theme != "dark")
        return fail(tr("Choose System, Light or Dark."));
    return edit("theme", theme);
}

QVariantList VoiceSession::languages() {
    // Stable language codes identify catalogs; labels remain readable when the
    // surrounding interface is using another language.
    static const auto result = [] {
        QVariantList entries;
        QStringList codes{"en"};
        for (const auto& file : QDir(":/i18n").entryList({"squadspeak_*.qm"}, QDir::Files, QDir::Name))
            codes.append(file.sliced(11, file.size() - 14));
        for (const auto& code : codes) {
            const QLocale locale(code);
            QString label = locale.nativeLanguageName();
            if (code == "en") label = QStringLiteral("English");
            else if (code == "cnr") label = QStringLiteral("Crnogorski");
            entries.append(QVariantMap{{"code", code}, {"label", label}});
        }
        return entries;
    }();
    return result;
}

bool VoiceSession::setLanguage(const QString& language) {
    const auto supported = languages();
    if (std::none_of(supported.begin(), supported.end(), [&](const auto& item) { return item.toMap().value("code").toString() == language; }))
        return fail(tr("Choose a language from the list."));
    return edit("language", language);
}

bool VoiceSession::setPalette(const QString& palette) {
    if (palette != "plum" && palette != "ocean" && palette != "forest" && palette != "graphite")
        return fail(tr("Choose a colour palette from the list."));
    return edit("palette", palette);
}

bool VoiceSession::setMuted(bool muted) {
    return edit("muted", muted);
}

bool VoiceSession::setAnimatedAvatars(bool enabled) { return edit("animatedAvatars", enabled); }
QString VoiceSession::avatar() const {
    const auto selected = preferences_.value("avatar").toString();
    return !supporterEnabled_ && avatars().indexOf(selected) >= 10 ? avatarFallback(selected) : selected;
}
bool VoiceSession::setSupporterEnabled(bool enabled) {
    if (enabled && SQUADSPEAK_STORE_BUILD) return false;
    if (supporterEnabled_ == enabled) return true;
    supporterEnabled_ = enabled;
    emit preferencesChanged();
    return true;
}
bool VoiceSession::validAvatar(const QString& avatar) {
    return !avatar.isEmpty() && avatar.size() <= 64 && std::all_of(avatar.begin(), avatar.end(), [](QChar c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
QString VoiceSession::avatarFallback(const QString& avatar) {
    // This baseline catalog and mapping remain stable across app versions.
    const QStringList baseline{"mossling", "courier", "mechanic"};
    if (baseline.contains(avatar)) return avatar;
    quint32 hash = 0;
    for (const auto c : avatar) hash = hash * 31 + c.unicode();
    return baseline.at(hash % 3);
}
QStringList VoiceSession::avatars() {
    static const QStringList available{
        "mossling", "courier", "mechanic", "ember-dragon", "moon-owl", "mushroom-sage",
        "coral-diver", "clockwork-beetle", "crystal-golem", "cloud-shepherd", "star-moth",
        "lantern-ghost", "desert-fennec", "octopus-pilot", "snail-captain", "orchid-mantis",
        "comet-axolotl", "frost-yeti", "raccoon-alchemist", "sunflower-knight"};
    return available;
}
bool VoiceSession::setAvatar(const QString& avatar) {
    if (!avatars().contains(avatar)) return fail(tr("Choose an available avatar."));
    if (!supporterEnabled_ && avatars().indexOf(avatar) >= 10) return fail(tr("This avatar requires Supporter."));
    return edit("avatar", avatar);
}
bool VoiceSession::setDeafened(bool deafened) { return edit("deafened", deafened); }
bool VoiceSession::setEventSounds(bool enabled) { return edit("eventSounds", enabled); }

bool VoiceSession::setPushToTalk(bool enabled) { return edit("pushToTalk", enabled); }
bool VoiceSession::setPttButtonHeld(bool held) { return edit("pttButtonHeld", held); }
bool VoiceSession::setPttKeyHeld(bool held, bool remote) { return edit(remote ? "remotePttKeyHeld" : "pttKeyHeld", held); }

QList<int> VoiceSession::pttKeyCodes(bool remote) const {
    const auto array = preferences_.value(remote ? "remotePttKeyCodes" : "pttKeyCodes").toArray();
    if (!array.isEmpty()) {
        QList<int> result;
        result.reserve(array.size());
        for (const auto& value : array) result.append(value.toInt());
        return result;
    }
    const auto legacy = remote ? remotePttKeyCode() : pttKeyCode();
    return legacy >= 0 ? QList<int>{legacy} : QList<int>{};
}

QString VoiceSession::pttKeyName() const {
    const auto names = preferences_.value("pttKeyNames").toArray();
    if (!names.isEmpty()) {
        QStringList result;
        for (const auto& value : names) result.append(value.toString());
        return result.join(QStringLiteral(" + "));
    }
    return preferences_.value("pttKeyName").toString();
}

QString VoiceSession::remotePttKeyName() const {
    const auto names = preferences_.value("remotePttKeyNames").toArray();
    if (!names.isEmpty()) {
        QStringList result;
        for (const auto& value : names) result.append(value.toString());
        return result.join(QStringLiteral(" + "));
    }
    return preferences_.value("remotePttKeyName").toString();
}

bool VoiceSession::setPttShortcut(int nativeKey, const QString& name, bool remote) {
    if (nativeKey < -1 || nativeKey > 65535 || (nativeKey == -1 && !name.isEmpty())) return fail(tr("Invalid push-to-talk binding."));
    return setPttShortcut(nativeKey < 0 ? QList<int>{} : QList<int>{nativeKey},
        nativeKey < 0 ? QStringList{} : QStringList{name}, remote);
}

bool VoiceSession::setPttShortcut(const QList<int>& nativeKeys, const QStringList& names, bool remote) {
    if (remote ? remotePttKeyHeld() : pttKeyHeld()) return fail(tr("Release the held push-to-talk key first."));
    if (nativeKeys.size() != names.size() || nativeKeys.size() > 16)
        return fail(tr("Invalid push-to-talk binding."));
    int labelLength = 0;
    for (int i = 0; i < nativeKeys.size(); ++i) {
        if (nativeKeys.at(i) < 0 || nativeKeys.at(i) > 65535 || names.at(i).isEmpty())
            return fail(tr("Invalid push-to-talk binding."));
        try { (void)validatedName(names.at(i)); }
        catch (const std::invalid_argument& error) { return fail(QString::fromUtf8(error.what())); }
        if (nativeKeys.indexOf(nativeKeys.at(i)) != i) return fail(tr("Invalid push-to-talk binding."));
        labelLength += names.at(i).size();
    }
    if (labelLength > 256) return fail(tr("Invalid push-to-talk binding."));
    auto next = preferences_;
    const auto codesKey = remote ? "remotePttKeyCodes" : "pttKeyCodes";
    const auto namesKey = remote ? "remotePttKeyNames" : "pttKeyNames";
    const auto codeKey = remote ? "remotePttKeyCode" : "pttKeyCode";
    const auto nameKey = remote ? "remotePttKeyName" : "pttKeyName";
    QJsonArray codes;
    QJsonArray savedNames;
    for (int i = 0; i < nativeKeys.size(); ++i) {
        codes.append(nativeKeys.at(i));
        savedNames.append(validatedName(names.at(i)));
    }
    next.insert(codesKey, codes); next.insert(namesKey, savedNames);
    next.insert(codeKey, nativeKeys.isEmpty() ? -1 : nativeKeys.first());
    next.insert(nameKey, nativeKeys.isEmpty() ? QString{} : validatedName(names.first()));
    return save(std::move(next));
}

bool VoiceSession::releasePttKey(bool remote) {
    const auto key = remote ? "remotePttKeyHeld" : "pttKeyHeld";
    if (!preferences_.value(key).toBool()) return true;
    return edit(key, false);
}

bool VoiceSession::setRemotePttHeld(bool held) {
    if (remotePttHeld_ == held) return true;
    remotePttHeld_ = held;
    emit presenceChanged();
    return true;
}

bool VoiceSession::setPttLocal(bool local) {
    if (pttLocal_ == local) return true;
    pttLocal_ = local;
    emit presenceChanged();
    return true;
}

bool VoiceSession::releasePttInput() {
    if (!pttButtonHeld() && !pttKeyHeld() && !remotePttKeyHeld()) return true;
    if (pttInputRevision() == 9007199254740991LL) return fail(tr("The push-to-talk sequence cannot be incremented further."));
    auto next = preferences_;
    next.insert("pttButtonHeld", false); next.insert("pttKeyHeld", false); next.insert("remotePttKeyHeld", false);
    next.insert("pttRevision", pttInputRevision() + 1);
    return save(std::move(next));
}

bool VoiceSession::setAudioSettingsOpen(bool open) {
    if (settingsOpen_ == open) return true;
    settingsOpen_ = open;
    emit presenceChanged();
    return true;
}

bool VoiceSession::setAudioReadiness(bool inputReady, bool outputReady) {
    if (inputReady_ == inputReady && outputReady_ == outputReady) return true;
    inputReady_ = inputReady;
    outputReady_ = outputReady;
    emit presenceChanged();
    return true;
}

bool VoiceSession::setAudioTestActive(bool active) {
    if (testActive_ == active) return true;
    testActive_ = active;
    emit presenceChanged();
    return true;
}
