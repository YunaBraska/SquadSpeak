#pragma once

#include "local_channel.hpp"
#include "video_codec.hpp"
#include <QMediaCaptureSession>
#include <QScreenCapture>
#include <QWindowCapture>
#include <QVideoSink>
#include <QVideoFrame>
#include <QThreadPool>
#include <QScreen>
#include <array>
#include <functional>

// Owns explicit local capture and the bounded encode/decode jobs. Host capture
// outlives the owner's channel membership, but never an explicit stop or restart.
class ScreenShare final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool audioAvailable READ audioAvailable CONSTANT)
    Q_PROPERTY(bool audioEnabled READ audioEnabled NOTIFY changed)
    Q_PROPERTY(bool computerAudio READ computerAudio NOTIFY changed)
    Q_PROPERTY(bool watching READ watching NOTIFY changed)
    Q_PROPERTY(double bitrate READ bitrate NOTIFY statisticsChanged)
    Q_PROPERTY(QString hostId READ hostId NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
public:
    explicit ScreenShare(LocalChannel& channel, QObject* parent = nullptr);
    ~ScreenShare() override;
    [[nodiscard]] bool active() const { return captureHost_ && captureHost_->screenSharing(); }
    [[nodiscard]] bool watching() const { return !sinks_.isEmpty(); }
    [[nodiscard]] QString hostId() const { return captureHost_ ? captureHost_->channelId() : QString{}; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QVariantList sources() const { return sources_; }
    [[nodiscard]] bool audioAvailable() const;
    [[nodiscard]] bool audioEnabled() const { return audioEnabled_; }
    [[nodiscard]] bool computerAudio() const { return computerAudio_; }
    [[nodiscard]] double bitrate() const { return bitrate_; }
    Q_INVOKABLE bool previewSource(int index, QObject* sink);
    Q_INVOKABLE bool stopPreview();
    Q_INVOKABLE bool setAudioEnabled(bool enabled);
    Q_INVOKABLE bool refreshSources();
    Q_INVOKABLE bool start(int index, const QString& hostId = {});
    Q_INVOKABLE bool stop();
    Q_INVOKABLE bool attach(const QString& hostId, QObject* sink, bool preview = false);
    Q_INVOKABLE bool detach(QObject* sink);
signals:
    void changed();
    void sourcesChanged();
    void statisticsChanged();
private:
    bool startCapture(int index);
    [[nodiscard]] bool capturing() const { return active() || previewing_; }
    void encodeFrame();
    void decodeFrame(const QString& host, qint64 serial, const QJsonObject& format, const QByteArray& packet);
    bool fail(const QString& message);
    LocalChannel& channel_;
    QPointer<LocalChannel> captureHost_;
    QMediaCaptureSession capture_;
    QScreenCapture screen_;
    QWindowCapture window_;
    QVideoSink input_;
    QVideoFrame latest_;
    QScreen* selectedScreen_ = nullptr;
    QList<QPointer<QScreen>> screens_;
    QList<QCapturableWindow> windows_;
    QVariantList sources_;
    QHash<QObject*, QString> sinks_;
    QSet<QObject*> previews_;
    QSet<QVideoSink*> localSinks_;
    bool previewing_ = false;
    int selectedSource_ = -1;
    double bitrate_ = 0;
    qint64 rateAt_ = 0, rateBytes_ = 0;
    QHash<QString, QImage> images_;
    QHash<QString, std::shared_ptr<VideoCodec>> decoders_;
    std::shared_ptr<std::array<VideoCodec, 4>> encoders_;
    std::array<qint64, 4> sent_{};
    QThreadPool encoderWorker_, workers_;
    QElapsedTimer clock_;
    QTimer tick_, sourceCheck_;
    qint64 generation_ = 0;
    bool encoding_ = false, decoding_ = false;
    std::function<void()> pendingDecode_;
    QString error_;
    bool audioEnabled_ = false;
    bool computerAudio_ = true;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
    void* native_ = nullptr;
    QTimer captureTick_;
    bool refreshNativeSources();
    bool startNative(int index);
    bool updateNativeAudio();
    void pollNative();
    void checkNativeSource();
    void stopNative();
    void disposeNative();
#endif
};
