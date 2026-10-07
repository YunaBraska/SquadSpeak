#pragma once

#include <QAudioFormat>
#include <QByteArrayView>
#include <QObject>
#include <QSaveFile>
#include <memory>

// A user-started, bounded diagnostic capture of device PCM before app DSP.
class AudioRecording final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool reviewing READ reviewing NOTIFY changed)
    Q_PROPERTY(bool complete READ complete NOTIFY changed)
    Q_PROPERTY(int step READ step NOTIFY changed)
    Q_PROPERTY(int stepCount READ stepCount CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString instruction READ instruction NOTIFY changed)
    Q_PROPERTY(int remainingSeconds READ remainingSeconds NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(bool speaking READ speaking NOTIFY changed)
    Q_PROPERTY(QString sentence READ sentence CONSTANT)
    Q_PROPERTY(QString directory READ directory CONSTANT)
    Q_PROPERTY(QString resultPath READ resultPath NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit AudioRecording(QString directory, QObject* parent = nullptr);
    [[nodiscard]] bool active() const { return capturing_; }
    [[nodiscard]] bool reviewing() const { return file_ && !capturing_; }
    [[nodiscard]] bool complete() const { return complete_; }
    [[nodiscard]] int step() const { return phase_; }
    [[nodiscard]] int stepCount() const { return 5; }
    [[nodiscard]] QString title() const;
    [[nodiscard]] QString instruction() const;
    [[nodiscard]] int remainingSeconds() const;
    [[nodiscard]] double progress() const;
    [[nodiscard]] bool speaking() const;
    [[nodiscard]] QString sentence() const;
    [[nodiscard]] QString directory() const { return directory_; }
    [[nodiscard]] QString resultPath() const { return resultPath_; }
    [[nodiscard]] QString error() const { return error_; }
    bool start(const QAudioFormat& format, const QByteArray& deviceId, const QString& deviceName);
    bool append(QByteArrayView frames);
    bool cancel(const QString& reason = {});
    Q_INVOKABLE bool finish();
    Q_INVOKABLE bool accept();
    Q_INVOKABLE bool reset();
signals:
    void changed();
private:
    [[nodiscard]] QByteArray header() const;
    QString directory_;
    QString basePath_;
    QString resultPath_;
    QString error_;
    QAudioFormat format_;
    QByteArray deviceId_;
    QString deviceName_;
    std::unique_ptr<QSaveFile> file_;
    int phase_ = 0;
    bool capturing_ = false;
    bool complete_ = false;
    qint64 frames_ = 0;
    qint64 startedAt_ = 0;
};
