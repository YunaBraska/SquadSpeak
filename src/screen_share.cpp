#include "screen_share.hpp"
#include <QGuiApplication>
#include <QFutureWatcher>
#include <QPromise>
#include <QScopeGuard>
#include <QOperatingSystemVersion>
#include <algorithm>
#include <utility>

ScreenShare::ScreenShare(LocalChannel& channel, QObject* parent) : QObject(parent), channel_(channel), captureHost_(&channel) {
    // Native encoder contexts (notably Media Foundation) initialize per-thread
    // state. Retain one encoding thread through use and final destruction.
    encoderWorker_.setMaxThreadCount(1); encoderWorker_.setExpiryTimeout(-1);
    workers_.setMaxThreadCount(1); clock_.start();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
    captureTick_.setInterval(20); captureTick_.setTimerType(Qt::PreciseTimer);
    connect(&captureTick_, &QTimer::timeout, this, &ScreenShare::pollNative);
#endif
    capture_.setVideoSink(&input_);
    connect(&input_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
        if (active() && selectedScreen_ && screen_.screen() != selectedScreen_) {
            fail(tr("The shared screen was removed. Select a source to start again.")); return;
        }
        if (active()) latest_ = frame;
    });
    connect(&screen_, &QScreenCapture::errorOccurred, this, [this](QScreenCapture::Error, const QString& text) { fail(text); });
    connect(&window_, &QWindowCapture::errorOccurred, this, [this](QWindowCapture::Error, const QString& text) { fail(text); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* removed) {
        if (active() && capture_.screenCapture() && selectedScreen_ == removed) fail(tr("The shared screen was removed. Select a source to start again."));
    });
    connect(&channel_, &LocalChannel::screenChanged, this, [this] {
        if (!active() && (tick_.isActive() || screen_.isActive() || window_.isActive())) stop();
        for (const auto& id : decoders_.keys()) if (channel_.screenInfo(id).value("tier", -1).toInt() < 0) {
            images_.remove(id); decoders_.remove(id);
            for (auto* sink : sinks_.keys()) if (sinks_.value(sink) == id) static_cast<QVideoSink*>(sink)->setVideoFrame({});
        }
        emit changed();
    });
    connect(&channel_, &LocalChannel::screenFrameReceived, this, &ScreenShare::decodeFrame);
    tick_.setInterval(34); tick_.setTimerType(Qt::PreciseTimer);
    connect(&tick_, &QTimer::timeout, this, &ScreenShare::encodeFrame);
    sourceCheck_.setInterval(1000);
    connect(&sourceCheck_, &QTimer::timeout, this, [this] {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
        checkNativeSource();
#else
        if (active() && capture_.windowCapture() && !QWindowCapture::capturableWindows().contains(window_.window()))
            fail(tr("The shared window was closed. Select a source to start again."));
#endif
    });
}
ScreenShare::~ScreenShare() {
    stop(); encoderWorker_.waitForDone(); workers_.waitForDone();
    capture_.setVideoSink(nullptr);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
    disposeNative();
#endif
}
bool ScreenShare::fail(const QString& message) { stop(); error_ = message; emit changed(); return false; }
bool ScreenShare::audioAvailable() const {
#ifdef Q_OS_MACOS
    return true;
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    return QGuiApplication::platformName() == "xcb" || QGuiApplication::platformName().startsWith("wayland");
#elif defined(Q_OS_WIN)
    return QOperatingSystemVersion::current() >= QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 20348);
#else
    return false;
#endif
}
bool ScreenShare::setAudioEnabled(bool enabled) {
    if (enabled && (!active() || !audioAvailable())) return false;
    if (audioEnabled_ == enabled) return true;
    audioEnabled_ = enabled;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
    if (active() && !updateNativeAudio()) return false;
#endif
    emit changed(); return true;
}
bool ScreenShare::refreshSources() {
    if (active()) return false;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
    return refreshNativeSources();
#else
    screens_.clear(); sources_.clear(); windows_ = QWindowCapture::capturableWindows();
    for (auto* screen : QGuiApplication::screens()) {
        screens_.append(screen);
        sources_.append(QVariantMap{{"index", sources_.size()}, {"name", screen->name()}, {"kind", "screen"}});
    }
    for (const auto& window : windows_)
        sources_.append(QVariantMap{{"index", sources_.size()}, {"name", window.description()}, {"kind", "window"}});
    emit sourcesChanged(); return true;
#endif
}
bool ScreenShare::start(int index, const QString& id) {
    if (watching()) return fail(tr("Close a video view before opening another stream."));
    if (active() || index < 0 || index >= sources_.size()) return false;
    auto* owner = channel_.ownChannel(id.isEmpty() ? channel_.channelId() : id);
    if (!owner) return false;
    captureHost_ = owner;
    audioEnabled_ = false;
    computerAudio_ = sources_[index].toMap().value("kind") == "screen";
    if (!captureHost_->setScreenSharing(true)) return fail(tr("Screen sharing requires an active Supporter pass on the owning host."));
    error_.clear(); ++generation_;
    try { encoders_ = std::make_shared<std::array<VideoCodec, 4>>(); }
    catch (const std::exception& error) { return fail(QString::fromUtf8(error.what())); }
    sent_.fill(-1000);
#ifdef Q_OS_MACOS
    if (!startNative(index)) return false;
#else
    if (index < screens_.size()) {
        if (!screens_[index]) return fail(tr("The selected screen is no longer available."));
        capture_.setWindowCapture(nullptr); capture_.setScreenCapture(&screen_);
        selectedScreen_ = screens_[index]; screen_.setScreen(selectedScreen_); screen_.start();
    } else {
        const auto selected = windows_.value(index - screens_.size());
        if (!selected.isValid()) return fail(tr("The selected window is no longer available."));
        capture_.setScreenCapture(nullptr); capture_.setWindowCapture(&window_);
        window_.setWindow(selected); window_.start();
    }
#if defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
    if (!startNative(index)) return false;
#endif
    sourceCheck_.start();
#endif
    if (!active()) return false;
    tick_.start(); emit changed(); return true;
}
bool ScreenShare::stop() {
    ++generation_; tick_.stop(); sourceCheck_.stop();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
    stopNative();
#endif
    audioEnabled_ = false;
    computerAudio_ = true;
    screen_.stop(); window_.stop(); latest_ = {}; selectedScreen_ = nullptr;
    capture_.setScreenCapture(nullptr); capture_.setWindowCapture(nullptr);
    if (auto encoders = std::exchange(encoders_, {}))
        encoderWorker_.start([encoders = std::move(encoders)]() mutable { encoders.reset(); });
    if (captureHost_) captureHost_->setScreenSharing(false); emit changed(); return true;
}
bool ScreenShare::attach(const QString& id, QObject* object, bool preview) {
    auto* sink = qobject_cast<QVideoSink*>(object);
    if (!sink) return false;
    QSet<QString> channels{id};
    for (auto* other : sinks_.keys()) if (other != object) channels.insert(sinks_.value(other));
    // Preview and separate window share a single subscription and decoder.
    if (channels.size() > 1) return false;
    bool fullSize = !preview;
    for (auto* other : sinks_.keys()) if (sinks_.value(other) == id && !previews_.contains(other) && other != object) fullSize = true;
    if (!channel_.watchScreen(id, true, fullSize ? 0 : 3, fullSize)) return false;
    if (sinks_.contains(sink) && sinks_.value(sink) != id) detach(sink);
    if (!sinks_.contains(sink)) connect(sink, &QObject::destroyed, this, [this, object] { detach(object); });
    sinks_.insert(sink, id);
    if (preview) previews_.insert(sink); else previews_.remove(sink);
    if (images_.contains(id)) sink->setVideoFrame(QVideoFrame(images_.value(id)));
    emit changed(); return true;
}
bool ScreenShare::detach(QObject* object) {
    auto* sink = object;
    if (!sinks_.contains(sink)) return false;
    const auto id = sinks_.take(sink); previews_.remove(sink);
    disconnect(sink, &QObject::destroyed, this, nullptr);
    if (auto* video = qobject_cast<QVideoSink*>(sink)) video->setVideoFrame({});
    if (!sinks_.values().contains(id)) { channel_.watchScreen(id, false); images_.remove(id); decoders_.remove(id); }
    else {
        bool fullSize = false;
        for (auto* other : sinks_.keys()) if (sinks_.value(other) == id && !previews_.contains(other)) fullSize = true;
        channel_.watchScreen(id, true, fullSize ? 0 : 3, fullSize);
    }
    emit changed(); return true;
}
void ScreenShare::encodeFrame() {
    if (!active() || encoding_ || !latest_.isValid()) return;
    const auto now = clock_.elapsed();
    QSet<int> tiers;
    QSet<int> keys;
    for (int tier : captureHost_->screenTiers()) if (now - sent_[size_t(tier)] >= VideoCodec::framePeriod(tier)) {
        tiers.insert(tier); sent_[size_t(tier)] = now;
        if (captureHost_->screenNeedsKeyFrame(tier)) keys.insert(tier);
    }
    if (tiers.isEmpty()) return;
    const auto frame = latest_;
    if (frame.width() > 16384 || frame.height() > 16384 || qint64(frame.width()) * frame.height() > 67108864) {
        fail(tr("The selected source exceeds the capture size limit.")); return;
    }
    encoding_ = true;
    const auto generation = generation_;
    using Result = QMap<int, QPair<QJsonObject, QByteArray>>;
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, generation, now] {
        encoding_ = false; watcher->deleteLater();
        if (generation != generation_ || !active()) return;
        try {
            const auto result = watcher->result();
            const bool overloaded = std::any_of(result.cbegin(), result.cend(), [](const auto& frame) { return frame.first.value("overloaded").toBool(); });
            captureHost_->reportScreenEncodeTime(overloaded ? 60000 : int(std::min<qint64>(60000, clock_.elapsed() - now)));
            for (auto it = result.cbegin(); it != result.cend(); ++it)
                if (!it->second.isEmpty()) captureHost_->sendScreenFrame(it.key(), it->first, it->second, it->first.value("key").toBool());
        } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
    });
    QPromise<Result> promise; watcher->setFuture(promise.future());
    encoderWorker_.start([promise = std::move(promise), encoders = encoders_, frame, tiers, keys]() mutable {
        promise.start();
        try {
            Result result;
            for (int tier : tiers) result.insert(tier, (*encoders)[size_t(tier)].encode(frame, tier, keys.contains(tier)));
            promise.addResult(std::move(result));
        } catch (...) { promise.setException(std::current_exception()); }
        promise.finish();
    });
}
void ScreenShare::decodeFrame(const QString& host, qint64 serial, const QJsonObject& format, const QByteArray& packet) {
    if (!sinks_.values().contains(host)) { channel_.watchScreen(host, false); return; }
    auto& decoder = decoders_[host];
    try { if (!decoder) decoder = std::make_shared<VideoCodec>(); }
    catch (const std::exception& error) {
        channel_.watchScreen(host, false); error_ = QString::fromUtf8(error.what()); emit changed(); return;
    }
    // Reopening a view must not queue old decoders behind a slow frame. Keep
    // one active job and only the newest pending source across reconnects.
    if (decoding_) {
        pendingDecode_ = [this, host, serial, format, packet, decoder] {
            if (decoders_.value(host) == decoder) decodeFrame(host, serial, format, packet);
        };
        return;
    }
    decoding_ = true;
    auto* watcher = new QFutureWatcher<QImage>(this);
    const auto began = clock_.elapsed();
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, host, serial, began, decoder] {
        decoding_ = false; watcher->deleteLater();
        const auto next = qScopeGuard([this] {
            if (auto pending = std::exchange(pendingDecode_, {})) pending();
        });
        if (decoders_.value(host) != decoder) return;
        try {
            const auto image = watcher->result();
            if (!image.isNull()) {
                images_.insert(host, image);
                for (auto* sink : sinks_.keys()) if (sinks_.value(sink) == host) static_cast<QVideoSink*>(sink)->setVideoFrame(QVideoFrame(image));
            }
            channel_.acknowledgeScreen(host, serial, int(std::min<qint64>(60000, clock_.elapsed() - began)));
        } catch (const std::exception& error) {
            channel_.watchScreen(host, false); decoders_.remove(host); images_.remove(host);
            error_ = QString::fromUtf8(error.what()); emit changed();
        }
    });
    QPromise<QImage> promise; watcher->setFuture(promise.future());
    workers_.start([promise = std::move(promise), decoder, format, packet]() mutable {
        promise.start();
        try { promise.addResult(decoder->decode(format, packet)); }
        catch (...) { promise.setException(std::current_exception()); }
        promise.finish();
    });
}
