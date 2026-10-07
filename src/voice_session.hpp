#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QStringList>

// Owns the user's persistent session preferences and temporary transmission
// restrictions. Analysis may capture audio while transmission is restricted.
class VoiceSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool animatedAvatars READ animatedAvatars NOTIFY preferencesChanged)
    Q_PROPERTY(QString avatar READ avatar NOTIFY preferencesChanged)
    Q_PROPERTY(QStringList avatars READ avatars CONSTANT)
    Q_PROPERTY(bool supporterEnabled READ supporterEnabled NOTIFY preferencesChanged)
    Q_PROPERTY(QString language READ language NOTIFY preferencesChanged)
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)
    Q_PROPERTY(QString theme READ theme NOTIFY preferencesChanged)
    Q_PROPERTY(QString palette READ palette NOTIFY preferencesChanged)
    Q_PROPERTY(bool deafened READ deafened NOTIFY preferencesChanged)
    Q_PROPERTY(bool eventSounds READ eventSounds NOTIFY preferencesChanged)
    Q_PROPERTY(QString userName READ userName NOTIFY preferencesChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY preferencesChanged)
    Q_PROPERTY(bool pushToTalk READ pushToTalk NOTIFY preferencesChanged)
    Q_PROPERTY(bool pttHeld READ pttHeld NOTIFY presenceChanged)
    Q_PROPERTY(bool pttButtonHeld READ pttButtonHeld NOTIFY preferencesChanged)
    Q_PROPERTY(bool pttInputHeld READ pttInputHeld NOTIFY presenceChanged)
    Q_PROPERTY(int pttKeyCode READ pttKeyCode NOTIFY preferencesChanged)
    Q_PROPERTY(QString pttKeyName READ pttKeyName NOTIFY preferencesChanged)
    Q_PROPERTY(QString remotePttKeyName READ remotePttKeyName NOTIFY preferencesChanged)
    Q_PROPERTY(bool available READ available NOTIFY presenceChanged)
    Q_PROPERTY(bool transmissionAllowed READ transmissionAllowed NOTIFY presenceChanged)
    Q_PROPERTY(QString presenceText READ presenceText NOTIFY presenceChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
    // An empty path keeps transient session state without loading a personal profile.
    explicit VoiceSession(QString settingsFile = {}, QObject* parent = nullptr);
    [[nodiscard]] bool animatedAvatars() const { return preferences_.value("animatedAvatars").toBool(); }
    Q_INVOKABLE bool setAnimatedAvatars(bool enabled);
    [[nodiscard]] QString avatar() const;
    [[nodiscard]] bool supporterEnabled() const { return supporterEnabled_; }
    // Runtime entitlement only; imported profile preferences cannot grant it.
    bool setSupporterEnabled(bool enabled);
    [[nodiscard]] static QStringList avatars();
    [[nodiscard]] QString language() const { return preferences_.value("language").toString(); }
    [[nodiscard]] static QVariantList languages();
    Q_INVOKABLE bool setLanguage(const QString& language);
    [[nodiscard]] QString theme() const { return preferences_.value("theme").toString(); }
    Q_INVOKABLE bool setTheme(const QString& theme);
    [[nodiscard]] QString palette() const { return preferences_.value("palette").toString(); }
    Q_INVOKABLE bool setPalette(const QString& palette);
    [[nodiscard]] bool deafened() const { return preferences_.value("deafened").toBool(); }
    [[nodiscard]] bool eventSounds() const { return preferences_.value("eventSounds").toBool(); }
    [[nodiscard]] static bool validAvatar(const QString& avatar);
    Q_INVOKABLE static QString avatarFallback(const QString& avatar);
    Q_INVOKABLE bool setAvatar(const QString& avatar);
    Q_INVOKABLE bool setDeafened(bool deafened);
    Q_INVOKABLE bool setEventSounds(bool enabled);
    [[nodiscard]] QString userName() const { return preferences_.value("userName").toString(); }
    [[nodiscard]] QString legacyChannelName() const { return legacyChannelName_; }
    [[nodiscard]] bool muted() const { return preferences_.value("muted").toBool(); }
    [[nodiscard]] bool pushToTalk() const { return preferences_.value("pushToTalk").toBool(); }
    [[nodiscard]] bool pttButtonHeld() const { return preferences_.value("pttButtonHeld").toBool(); }
    [[nodiscard]] bool pttKeyHeld() const { return preferences_.value("pttKeyHeld").toBool(); }
    [[nodiscard]] bool pttInputHeld() const { return pttButtonHeld() || (pttLocal_ ? pttKeyHeld() : remotePttKeyHeld()); }
    [[nodiscard]] bool remotePttKeyHeld() const { return preferences_.value("remotePttKeyHeld").toBool(); }
    [[nodiscard]] qint64 pttInputRevision() const { return preferences_.value("pttRevision").toInteger(); }
    [[nodiscard]] bool pttHeld() const { return (pttLocal_ && pttInputHeld()) || remotePttHeld_; }
    [[nodiscard]] int pttKeyCode() const { return preferences_.value("pttKeyCode").toInt(); }
    [[nodiscard]] QString pttKeyName() const;
    [[nodiscard]] int remotePttKeyCode() const { return preferences_.value("remotePttKeyCode").toInt(); }
    [[nodiscard]] QString remotePttKeyName() const;
    [[nodiscard]] QList<int> pttKeyCodes(bool remote = false) const;
    [[nodiscard]] bool available() const { return !settingsOpen_ && !testActive_; }
    [[nodiscard]] bool inputReady() const { return inputReady_; }
    [[nodiscard]] bool outputReady() const { return outputReady_; }
    bool setAudioReadiness(bool inputReady, bool outputReady);
    [[nodiscard]] bool transmissionAllowed() const { return available() && !muted() && (!pushToTalk() || pttHeld()); }
    [[nodiscard]] QString presenceText() const;
    [[nodiscard]] QString error() const { return error_; }
    // Trims surrounding whitespace; rejects empty, oversized or control-bearing names.
    [[nodiscard]] static QString validatedName(const QString& name);
    Q_INVOKABLE bool setUserName(const QString& name);
    Q_INVOKABLE bool setMuted(bool muted);
    Q_INVOKABLE bool setPushToTalk(bool enabled);
    Q_INVOKABLE bool setPttButtonHeld(bool held);
    bool setPttKeyHeld(bool held, bool remote = false);
    bool setPttShortcut(int nativeKey, const QString& name, bool remote = false);
    bool setPttShortcut(const QList<int>& nativeKeys, const QStringList& names, bool remote = false);
    bool releasePttKey(bool remote = false);
    // Derived from the channel's authenticated, persisted controller states.
    bool setRemotePttHeld(bool held);
    bool setPttLocal(bool local);
    Q_INVOKABLE bool releasePttInput();
    Q_INVOKABLE bool setAudioSettingsOpen(bool open);
    Q_INVOKABLE bool setAudioTestActive(bool active);
signals:
    void preferencesChanged();
    void presenceChanged();
    void errorChanged();
private:
    [[nodiscard]] bool save(QJsonObject preferences);
    [[nodiscard]] bool edit(const QString& key, const QJsonValue& value);
    bool fail(QString message);
    QString settingsFile_;
    QString legacyChannelName_;
    QJsonObject preferences_{{"version", 2}, {"userName", "Member"},
        {"avatar", "mossling"}, {"theme", "system"}, {"palette", "plum"}, {"language", "en"}, {"deafened", false}, {"eventSounds", true},
        {"muted", true}, {"animatedAvatars", true}, {"pushToTalk", false}, {"pttButtonHeld", false}, {"pttKeyHeld", false},
        {"pttKeyCode", -1}, {"pttKeyName", ""}, {"pttRevision", 0},
        {"remotePttKeyHeld", false}, {"remotePttKeyCode", -1}, {"remotePttKeyName", ""},
        {"pttKeyCodes", QJsonArray{}}, {"pttKeyNames", QJsonArray{}},
        {"remotePttKeyCodes", QJsonArray{}}, {"remotePttKeyNames", QJsonArray{}}};
    QString error_;
    bool remotePttHeld_ = false;
    bool pttLocal_ = true;
    bool settingsOpen_ = false;
    bool testActive_ = false;
    bool supporterEnabled_ = false;
    bool inputReady_ = true;
    bool outputReady_ = true;
};
