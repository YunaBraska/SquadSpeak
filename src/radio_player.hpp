#pragma once

#include "voice_mixer.hpp"
#include <QAudioBufferOutput>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QPointer>

#include <QMediaPlayer>
#include <QTimer>
#include <QVariantList>
#include <deque>

class LocalChannel;

// Owns the host's saved stations and a single bounded, reconnecting stream.
// Decoded audio follows the same Opus transport as voice, without speech gates.
class RadioPlayer final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList stations READ stations NOTIFY stationsChanged)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY changed)
    Q_PROPERTY(QString stationName READ stationName NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool checking READ checking NOTIFY changed)
    Q_PROPERTY(QVariantMap catalogInfo READ catalogInfo CONSTANT)
public:
    explicit RadioPlayer(QString storageFile, QObject* parent = nullptr);
    ~RadioPlayer() override;
    [[nodiscard]] QVariantList stations() const;
    [[nodiscard]] QString selectedId() const { return saved_.value("selected").toString(); }
    [[nodiscard]] QString stationName() const;
    [[nodiscard]] bool active() const { return saved_.value("playing").toBool(); }
    [[nodiscard]] QString state() const { return state_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] bool checking() const { return probe_ != nullptr; }
    [[nodiscard]] QVariantMap catalogInfo() const;
    // Offline, bounded search across personal stations and the bundled directory.
    // Invalid query/page bounds return an empty page; at most 128 rows per call.
    Q_INVOKABLE QVariantMap searchStations(const QString& query, int offset = 0, int limit = 50) const;
    // Accepts a bounded asynchronous check. Completion is reported by stationSaved;
    // the saved list changes only after the URL delivers decodable audio.
    Q_INVOKABLE bool saveStation(const QString& id, const QString& name, const QString& url);
    Q_INVOKABLE bool cancelStationCheck();
    Q_INVOKABLE bool removeStation(const QString& id);
    Q_INVOKABLE bool play(const QString& id);
    Q_INVOKABLE bool stop();
    bool setAvailable(bool available);
    bool bind(LocalChannel& channel);
    Q_INVOKABLE RadioPlayer* forChannel(const QString& id);
signals:
    void changed();
    void stationsChanged();
    void audioPacket(const QByteArray& packet);
    void stationSaved(const QVariantMap& station, bool ok);
private:
    static QJsonObject station(const QString& id, const QString& name, const QString& url);
    static const QJsonObject& catalog();
    QJsonObject selectedStation() const;
    bool persist(QJsonObject next);
    void startStream();
    void reconnect(const QString& error);
    void halt();
    void receive(const QAudioBuffer& buffer);
    void finishStationCheck(bool valid, QString error = {});
    QString storageFile_;
    QJsonObject saved_{{"version", 1}, {"stations", QJsonArray{}}, {"selected", ""}, {"playing", false}};
    QString state_ = "stopped";
    QString error_;
    QMediaPlayer player_;
    QAudioBufferOutput output_;
    QMediaPlayer* probe_ = nullptr;
    QJsonObject pendingStation_;
    bool pendingUpdate_ = false;
    squad::VoiceMixer encoder_;
    std::deque<QByteArray> packets_;
    QTimer retry_, tick_;
    QElapsedTimer received_, delivered_, stable_;
    int attempts_ = 0;
    bool available_ = false;
    bool stopping_ = false;
    QPointer<LocalChannel> channel_;
    RadioPlayer* coordinator_ = nullptr;
    QHash<QString, QPointer<RadioPlayer>> children_;
    bool slotHeld() const;
    RadioPlayer* rootPlayer() const;
    int runningPlayers() const;
    void notifySlotChanged();
    void syncBoundAvailability();
    void syncBoundState();
    void syncChildren();
    RadioPlayer* childFor(LocalChannel& channel);
};
