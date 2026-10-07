#pragma once

#include "audio_profiles.hpp"
#include "audio_recording.hpp"
#include "voice_mixer.hpp"
#include "echo_canceller.hpp"
#include "output_monitor.hpp"
#include <QElapsedTimer>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QSoundEffect>
#include <QBuffer>
#include <QMediaDevices>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <array>
#include <memory>

class AudioModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList inputs READ inputs NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY devicesChanged)
    Q_PROPERTY(int inputIndex READ inputIndex NOTIFY devicesChanged)
    Q_PROPERTY(int outputIndex READ outputIndex NOTIFY devicesChanged)
    Q_PROPERTY(QString inputName READ inputName NOTIFY devicesChanged)
    Q_PROPERTY(QString outputName READ outputName NOTIFY devicesChanged)
    Q_PROPERTY(bool inputAvailable READ inputAvailable NOTIFY devicesChanged)
    Q_PROPERTY(bool outputAvailable READ outputAvailable NOTIFY devicesChanged)
    Q_PROPERTY(double gainDb READ gainDb NOTIFY profileChanged)
    Q_PROPERTY(double highPassHz READ highPassHz NOTIFY profileChanged)
    Q_PROPERTY(double lowPassHz READ lowPassHz NOTIFY profileChanged)
    Q_PROPERTY(bool gainAutomatic READ gainAutomatic NOTIFY profileChanged)
    Q_PROPERTY(bool highPassAutomatic READ highPassAutomatic NOTIFY profileChanged)
    Q_PROPERTY(double effectiveGainDb READ effectiveGainDb NOTIFY levelsChanged)
    Q_PROPERTY(double effectiveHighPassHz READ effectiveHighPassHz NOTIFY levelsChanged)
    Q_PROPERTY(QVariantList filterResponse READ filterResponse NOTIFY levelsChanged)
    Q_PROPERTY(double noiseSuppression READ noiseSuppression NOTIFY profileChanged)
    Q_PROPERTY(bool echoCancellation READ echoCancellation NOTIFY profileChanged)
    Q_PROPERTY(QString echoReference READ echoReference NOTIFY activityChanged)
    Q_PROPERTY(bool activationEnabled READ activationEnabled NOTIFY profileChanged)
    Q_PROPERTY(bool activationAutomatic READ activationAutomatic NOTIFY profileChanged)
    Q_PROPERTY(double activationThresholdDb READ activationThresholdDb NOTIFY activityChanged)
    Q_PROPERTY(double activationLevelDb READ activationLevelDb NOTIFY activityChanged)
    Q_PROPERTY(bool activationOpen READ activationOpen NOTIFY activityChanged)
    Q_PROPERTY(bool voiceSending READ voiceSending NOTIFY activityChanged)
    Q_PROPERTY(QVariantMap playbackLevels READ playbackLevels NOTIFY activityChanged)
    Q_PROPERTY(double transmitLevel READ transmitLevel NOTIFY activityChanged)
    Q_PROPERTY(double musicVolume READ musicVolume NOTIFY profileChanged)
    Q_PROPERTY(double outputVolume READ outputVolume NOTIFY profileChanged)
    Q_PROPERTY(bool deafened READ deafened NOTIFY profileChanged)
    Q_PROPERTY(bool automaticVolume READ automaticVolume NOTIFY profileChanged)
    Q_PROPERTY(double maximumFrequency READ maximumFrequency NOTIFY devicesChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool microphonePermissionDenied READ microphonePermissionDenied NOTIFY statusChanged)
    Q_PROPERTY(double inputDb READ inputDb NOTIFY levelsChanged)
    Q_PROPERTY(double outputDb READ outputDb NOTIFY levelsChanged)
    Q_PROPERTY(bool clipped READ clipped NOTIFY levelsChanged)
    Q_PROPERTY(double strongestHz READ strongestHz NOTIFY levelsChanged)
    Q_PROPERTY(double fundamentalHz READ fundamentalHz NOTIFY levelsChanged)
    Q_PROPERTY(bool spectrumReady READ spectrumReady NOTIFY levelsChanged)
    Q_PROPERTY(QVariantList inputSpectrum READ inputSpectrum NOTIFY levelsChanged)
    Q_PROPERTY(QVariantList outputSpectrum READ outputSpectrum NOTIFY levelsChanged)
    Q_PROPERTY(QObject* recording READ recording CONSTANT)
public:
    explicit AudioModel(const QString& settingsFile, QObject* parent = nullptr);
public slots:
    bool setPreviewActive(bool enabled) { return enabled ? start() : stop(); }
public:
    [[nodiscard]] QVariantList inputs() const;
    [[nodiscard]] QVariantList outputs() const;
    [[nodiscard]] int inputIndex() const;
    [[nodiscard]] int outputIndex() const;
    [[nodiscard]] QString inputName() const;
    [[nodiscard]] QString outputName() const;
    [[nodiscard]] bool inputAvailable() const { return !input_.isNull(); }
    [[nodiscard]] bool outputAvailable() const { return !output_.isNull(); }
    [[nodiscard]] bool inputReady() const;
    [[nodiscard]] bool outputReady() const { return outputAvailable() && !outputFailed_; }
    [[nodiscard]] double gainDb() const { return profile_.gainDb; }
    [[nodiscard]] double highPassHz() const { return profile_.highPassHz; }
    [[nodiscard]] double lowPassHz() const { return profile_.lowPassHz; }
    [[nodiscard]] bool gainAutomatic() const { return profile_.gainAutomatic; }
    [[nodiscard]] bool highPassAutomatic() const { return profile_.highPassAutomatic; }
    [[nodiscard]] double effectiveGainDb() const { return processor_ ? processor_->gainDb() : gainAutomatic() ? 0 : gainDb(); }
    [[nodiscard]] double effectiveHighPassHz() const { return processor_ ? processor_->highPassHz() : profile_.initialHighPassHz(); }
    Q_INVOKABLE bool setGainAutomatic(bool automatic);
    Q_INVOKABLE bool setHighPassAutomatic(bool automatic);
    [[nodiscard]] QVariantList filterResponse() const;
    [[nodiscard]] double noiseSuppression() const { return noiseSuppression_; }
    [[nodiscard]] bool echoCancellation() const { return echoCancellation_; }
    [[nodiscard]] QString echoReference() const;
    [[nodiscard]] bool activationEnabled() const { return activation_.enabled; }
    [[nodiscard]] bool activationAutomatic() const { return activation_.automatic; }
    [[nodiscard]] double activationThresholdDb() const { return mixer_.activationThresholdDb(); }
    [[nodiscard]] double activationLevelDb() const { return mixer_.activationLevelDb(); }
    [[nodiscard]] bool activationOpen() const { return mixer_.voiceActive(); }
    [[nodiscard]] bool voiceSending() const { return voiceActive_ && transmitEnabled_ && !testActive() && mixer_.voiceActive(); }
    [[nodiscard]] QVariantMap playbackLevels() const;
    [[nodiscard]] double transmitLevel() const;
    [[nodiscard]] double musicVolume() const { return musicVolume_; }
    Q_INVOKABLE bool setMusicVolume(double value);
    [[nodiscard]] double outputVolume() const { return outputVolume_; }
    [[nodiscard]] bool deafened() const { return deafened_; }
    bool setDeafened(bool deafened);
    [[nodiscard]] bool eventAudioEnabled() const { return !deafened_ && outputReady() && outputVolume_ > 0; }
    bool playEvent(const QString& kind);
    [[nodiscard]] double maximumFrequency() const;
    [[nodiscard]] bool running() const { return source_ != nullptr && wantsCapture_; }
    [[nodiscard]] bool testActive() const { return wantsCapture_ || testingOutput_; }
    [[nodiscard]] bool automaticVolume() const { return automaticVolume_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] bool microphonePermissionDenied() const;
    Q_INVOKABLE bool openMicrophoneSettings();
    [[nodiscard]] double inputDb() const { return levels_.inputDb; }
    [[nodiscard]] double outputDb() const { return levels_.outputDb; }
    [[nodiscard]] bool clipped() const { return levels_.clipped; }
    [[nodiscard]] double strongestHz() const { return levels_.strongestHz; }
    [[nodiscard]] double fundamentalHz() const { return levels_.fundamentalHz; }
    [[nodiscard]] bool spectrumReady() const { return levels_.spectrumReady; }
    [[nodiscard]] QVariantList inputSpectrum() const;
    [[nodiscard]] QVariantList outputSpectrum() const;
    Q_INVOKABLE bool selectInput(const QString& deviceId);
    Q_INVOKABLE bool selectOutput(const QString& deviceId);
    Q_INVOKABLE bool setGainDb(double value);
    Q_INVOKABLE bool setHighPassHz(double value);
    Q_INVOKABLE bool setLowPassHz(double value);
    Q_INVOKABLE bool setNoiseSuppression(double value);
    Q_INVOKABLE bool setEchoCancellation(bool enabled);
    Q_INVOKABLE bool setActivationEnabled(bool enabled);
    Q_INVOKABLE bool setActivationAutomatic(bool automatic);
    Q_INVOKABLE bool setActivationThresholdDb(double value);
    Q_INVOKABLE bool setOutputVolume(double value);
    Q_INVOKABLE bool setAutomaticVolume(bool value);
    Q_INVOKABLE double participantVolume(const QString& peer) const;
    Q_INVOKABLE bool setParticipantVolume(const QString& peer, double value);
    bool setVoiceState(bool joined, bool transmit, bool pushToTalk = false);
    bool setParticipants(const QVariantList& participants);
    bool receiveAudio(const QString& peer, const QByteArray& packet, int missing = 0);
    Q_INVOKABLE bool start();
    Q_INVOKABLE bool stop();
    Q_INVOKABLE bool testOutput();
    [[nodiscard]] AudioRecording* recording() { return &recording_; }
    Q_INVOKABLE bool startRecording();
    Q_INVOKABLE bool cancelRecording();
    Q_INVOKABLE bool openRecordings();

signals:
    void readinessChanged();
    void devicesChanged();
    void profileChanged();
    void runningChanged();
    void statusChanged();
    void microphoneAccessDenied();
    void levelsChanged();
    void audioPacket(const QByteArray& packet);
    void activityChanged();

private:
    void refreshDevices();
    void beginCapture();
    bool ensureCapture();
    void stopCapture();
    void beginPlayback();
    void playVoice();
    void consume();
    void monitorOutput();
    bool updateProfile(squad::InputProfile profile);
    bool updateActivation(squad::VoiceActivation activation);
    void applyActivation();
    bool select(bool input, const QString& deviceId);
    void setStatus(QString status);
    static QAudioDevice resolve(const QList<QAudioDevice>& devices, const QByteArray& selected, bool input);
    static QAudioFormat captureFormat(const QAudioDevice& device);
    squad::AudioProfiles profiles_;
    AudioRecording recording_;
    QTimer recordingWatchdog_;
    QMediaDevices devices_;
    QAudioDevice input_;
    QAudioDevice output_;
    squad::InputProfile profile_;
    std::array<double, squad::AudioLevels::spectrumSize> filterResponse_{};
    double noiseSuppression_ = 0;
    bool echoCancellation_ = true;
    squad::EchoCanceller echo_;
    OutputMonitor outputMonitor_;
    qint64 outputReferenceAt_ = -1;
    bool deviceReference_ = false;
    squad::VoiceActivation activation_;
    double outputVolume_ = 0.5;
    double musicVolume_ = 0.35;
    bool deafened_ = false;
    QSoundEffect eventSound_;
    std::unique_ptr<QAudioSource> source_;
    QIODevice* capture_ = nullptr;
    QAudioFormat format_;
    QByteArray pending_;
    std::unique_ptr<squad::AudioProcessor> processor_;
    QTimer meter_;
    squad::AudioLevels levels_;
    QString status_ = tr("Microphone display is off.");
    bool wantsCapture_ = false;
    QBuffer tone_;
    std::unique_ptr<QAudioSink> sink_;
    std::unique_ptr<QAudioSink> voiceSink_;
    QIODevice* playback_ = nullptr;
    QAudioFormat playbackFormat_;
    QTimer playbackTimer_;
    QElapsedTimer clock_;
    qint64 lastPacketAt_ = -1;
    qint64 activityPublishedAt_ = -50;
    squad::VoiceMixer mixer_;
    QVariantList participants_;
    QHash<QString, double> participantVolumes_;
    bool voiceActive_ = false;
    bool transmitEnabled_ = false;
    bool pushToTalk_ = false;
    bool testingOutput_ = false;
    bool automaticVolume_ = true;
    bool permissionPending_ = false;
    bool inputFailed_ = false;
    bool outputFailed_ = false;
    bool recordingRequested_ = false;
};
