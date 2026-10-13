#pragma once

#include "speech_output.hpp"
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QTimer>
#include <QVariantMap>
#include <QTextDocument>
#include <map>
#include <memory>
#include <span>

// Client-local language preferences and bounded Apple processing. Nothing here
// writes transcripts to chat history or opens a network connection.
class LocalLanguage final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool preparing READ preparing NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantList languages READ languages NOTIFY capabilitiesChanged)
    Q_PROPERTY(QVariantList speechLanguages READ speechLanguages NOTIFY capabilitiesChanged)
    Q_PROPERTY(QVariantList voices READ voices NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool speechAvailable READ speechAvailable NOTIFY capabilitiesChanged)
    Q_PROPERTY(QVariantMap systemVoice READ systemVoice NOTIFY capabilitiesChanged)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit LocalLanguage(QString settingsFile = {}, QObject* parent = nullptr);
    ~LocalLanguage() override;
    [[nodiscard]] static bool supported();
    [[nodiscard]] bool preparing() const { return !preparing_.isEmpty(); }
    [[nodiscard]] bool available() const { return capabilities_.value("available").toBool(); }
    [[nodiscard]] bool speechAvailable() const { return capabilities_.value("speechAvailable").toBool(); }
    [[nodiscard]] QVariantMap systemVoice() const { return capabilities_.value("systemVoice").toObject().toVariantMap(); }
    [[nodiscard]] bool busy() const { return translations_.size() >= 4; }
    [[nodiscard]] QVariantList languages() const { return capabilities_.value("languages").toArray().toVariantList(); }
    [[nodiscard]] QVariantList speechLanguages() const { return capabilities_.value("speechLanguages").toArray().toVariantList(); }
    [[nodiscard]] QVariantList voices() const { return voices_; }
    [[nodiscard]] int revision() const { return revision_; }
    [[nodiscard]] QString error() const { return error_; }
    Q_INVOKABLE QVariantMap member(const QString& id) const;
    Q_INVOKABLE bool configure(const QString& id, const QString& source, const QString& target,
                               const QString& voice, bool incoming);
    Q_INVOKABLE QString channelTarget(const QString& id) const;
    Q_INVOKABLE bool setChannelTarget(const QString& id, const QString& target);
    Q_INVOKABLE bool prepare(const QString& id);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool openSettings(bool speech = false);
    Q_INVOKABLE void cancelPreparation();
    Q_INVOKABLE bool prepareTranslation(const QString& key);
    Q_INVOKABLE bool request(const QString& key, const QString& markdown, const QString& source, const QString& target);
    Q_INVOKABLE QVariantMap result(const QString& key) const;
    Q_INVOKABLE void cancel(const QString& key);
    Q_INVOKABLE void clearResults();
    void setMembers(const QStringList& members, const QString& channel = {});
    void suspend(bool suspended);
    [[nodiscard]] bool voiceProcessing() const;
    void processVoice(const QString& id, std::span<float> samples, qint64 now);
signals:
    void changed();
    void capabilitiesChanged();
    void prepared(const QString& id);
private:
    struct Translation {
        QString key, original, source, target, peer;
        std::unique_ptr<QTextDocument> document;
        struct Part { int position, length; QString text; };
        QList<Part> parts;
        QStringList replacements;
        qint64 started = 0;
    };
    struct Voice {
        qint64 request = 0;
        qint64 waitingSince = -1;
        bool ready = false;
        QString transcript, status;
        SpeechOutput output;
    };
    void event(qint64 request, const QJsonObject& event);
    void next(qint64 request);
    void startMember(const QString& id);
    void stopMember(const QString& id);
    void failMember(QString id);
    bool save(QJsonObject preferences);
    void notify();
    QString settingsFile_, error_, channel_;
    QVariantList voices_ = SpeechOutput::voices();
    QJsonObject preferences_, capabilities_;
    QHash<QString, QVariantMap> results_;
    std::map<qint64, Translation> translations_;
    std::map<QString, std::unique_ptr<Voice>> active_;
    QHash<qint64, QString> preparing_;
    QStringList members_;
    void* native_ = nullptr;
    qint64 nextRequest_ = 0;
    QElapsedTimer clock_;
    QTimer deadline_, systemRefresh_;
    int revision_ = 0;
    bool suspended_ = false;
};
