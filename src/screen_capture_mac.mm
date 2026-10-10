#include "screen_share.hpp"
#include "voice_mixer.hpp"
#include <QGuiApplication>
#include <deque>
#include <mutex>
#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreVideo/CoreVideo.h>

// Native callbacks retain at most one image and 100 ms of mono audio. The Qt
// owner drains them on its thread; capture never queues a callback per frame.
@interface SquadScreenCapture : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    SCStream* stream;
    SCStreamConfiguration* configuration;
    NSArray* sources;
    dispatch_queue_t queue;
    std::mutex mutex;
    QImage image;
    std::deque<float> audio;
    QString error;
    squad::VoiceMixer encoder;
    bool loading;
    bool started;
    bool wantsVideo;
    bool wantsAudio;
    quint64 sourceRevision;
    CGDirectDisplayID displayId;
    CGWindowID windowId;
}
@end

@implementation SquadScreenCapture
- (void)stream:(SCStream*)capture didStopWithError:(NSError*)failure {
    std::lock_guard lock(mutex);
    if (capture != stream) return;
    error = QString::fromNSString(failure.localizedDescription);
}
- (void)stream:(SCStream*)capture didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (!CMSampleBufferIsValid(sample) || !CMSampleBufferDataIsReady(sample)) return;
    if (type == SCStreamOutputTypeScreen) {
        { std::lock_guard lock(mutex); if (capture != stream || !wantsVideo) return; }
        const auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (!attachments || CFArrayGetCount(attachments) == 0) return;
        auto* metadata = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
        if ([metadata[SCStreamFrameInfoStatus] integerValue] != SCFrameStatusComplete) return;
        const auto pixels = CMSampleBufferGetImageBuffer(sample);
        if (!pixels || CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA) return;
        if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
        const auto width = CVPixelBufferGetWidth(pixels), height = CVPixelBufferGetHeight(pixels);
        QImage frame;
        if (width <= 3840 && height <= 2160)
            frame = QImage(static_cast<const uchar*>(CVPixelBufferGetBaseAddress(pixels)), int(width), int(height),
                qsizetype(CVPixelBufferGetBytesPerRow(pixels)), QImage::Format_ARGB32).copy();
        CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
        std::lock_guard lock(mutex);
        if (capture == stream && wantsVideo) image = std::move(frame);
    } else if (type == SCStreamOutputTypeAudio) {
        { std::lock_guard lock(mutex); if (capture != stream || !wantsAudio) return; }
        const auto format = CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sample));
        if (!format || format->mFormatID != kAudioFormatLinearPCM || format->mSampleRate != 48000
            || format->mChannelsPerFrame != 1 || format->mBitsPerChannel != 32
            || !(format->mFormatFlags & kAudioFormatFlagIsFloat) || (format->mFormatFlags & kAudioFormatFlagIsBigEndian)) return;
        AudioBufferList list{};
        CMBlockBufferRef block = nullptr;
        const auto status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, nullptr, &list, sizeof(list),
            nullptr, nullptr, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
        if (status == noErr && list.mNumberBuffers == 1 && list.mBuffers[0].mData) {
            const auto* values = static_cast<const float*>(list.mBuffers[0].mData);
            const auto count = list.mBuffers[0].mDataByteSize / sizeof(float);
            std::lock_guard lock(mutex);
            if (capture == stream && wantsAudio) {
                const auto keep = std::min<size_t>(count, 4800);
                audio.insert(audio.end(), values + count - keep, values + count);
                if (audio.size() > 4800) audio.erase(audio.begin(), audio.end() - 4800);
            }
        }
        if (block) CFRelease(block);
    }
}
@end

bool ScreenShare::refreshNativeSources() {
    if (!native_) {
        auto* state = [SquadScreenCapture new];
        state->queue = dispatch_queue_create("app.squadspeak.capture", DISPATCH_QUEUE_SERIAL);
        native_ = (__bridge_retained void*)state;
    }
    auto* state = (__bridge SquadScreenCapture*)native_;
    if (state->loading) return true;
    state->loading = true;
    const auto revision = ++state->sourceRevision;
    const QPointer<ScreenShare> owner(this);
    sources_.clear(); emit sourcesChanged();
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent* content, NSError* failure) {
        dispatch_async(dispatch_get_main_queue(), ^{
            state->loading = false;
            if (!owner || state->sourceRevision != revision) return;
            if (failure) { owner->fail(QString::fromNSString(failure.localizedDescription)); return; }
            auto* entries = [NSMutableArray new];
            for (SCDisplay* display in content.displays) {
                QString name = QString::number(display.displayID);
                for (NSScreen* screen in NSScreen.screens)
                    if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == display.displayID)
                        name = QString::fromNSString(screen.localizedName);
                [entries addObject:display];
                owner->sources_.append(QVariantMap{{"index", owner->sources_.size()}, {"name", name}, {"kind", "screen"}});
            }
            for (SCWindow* window in content.windows) {
                if (!window.owningApplication || CGRectIsEmpty(window.frame)) continue;
                [entries addObject:window];
                const auto name = QString::fromNSString(window.owningApplication.applicationName)
                    + " - " + QString::fromNSString(window.title ? window.title : @"");
                owner->sources_.append(QVariantMap{{"index", owner->sources_.size()}, {"name", name}, {"kind", "window"}});
            }
            state->sources = entries;
            owner->error_.clear(); emit owner->sourcesChanged(); emit owner->changed();
        });
    }];
    return true;
}

bool ScreenShare::startNative(int index) {
    auto* state = (__bridge SquadScreenCapture*)native_;
    if (!state || index < 0 || NSUInteger(index) >= state->sources.count) return fail(tr("The selected screen is no longer available."));
    SCContentFilter* filter;
    QSize size;
    id source = state->sources[NSUInteger(index)];
    if ([source isKindOfClass:SCDisplay.class]) {
        SCDisplay* display = source;
        state->displayId = display.displayID; state->windowId = 0;
        filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        size = QSize(int(display.width), int(display.height));
    } else {
        SCWindow* window = source;
        state->windowId = window.windowID; state->displayId = 0;
        filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
        CGFloat scale = 1;
        for (NSScreen* screen in NSScreen.screens) scale = std::max(scale, screen.backingScaleFactor);
        size = QSize(int(window.frame.size.width * scale), int(window.frame.size.height * scale));
    }
    size.scale(QSize(3840, 2160).boundedTo(size), Qt::KeepAspectRatio);
    auto* config = [SCStreamConfiguration new];
    config.width = std::max(2, size.width()); config.height = std::max(2, size.height());
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.minimumFrameInterval = CMTimeMake(1, 30); config.queueDepth = 3;
    config.capturesAudio = audioEnabled_; config.excludesCurrentProcessAudio = YES;
    config.sampleRate = 48000; config.channelCount = 1;
    state->configuration = config;
    auto* stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:state];
    state->started = false;
    { std::lock_guard lock(state->mutex); state->stream = stream; }
    NSError* error = nil;
    if (![stream addStreamOutput:state type:SCStreamOutputTypeScreen sampleHandlerQueue:state->queue error:&error]
        || ![stream addStreamOutput:state type:SCStreamOutputTypeAudio sampleHandlerQueue:state->queue error:&error])
        return fail(QString::fromNSString(error.localizedDescription));
    { std::lock_guard lock(state->mutex); state->error.clear(); state->wantsAudio = audioEnabled_; }
    const QPointer<ScreenShare> owner(this);
    [stream startCaptureWithCompletionHandler:^(NSError* failure) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!owner || state->stream != stream) { [stream stopCaptureWithCompletionHandler:nil]; return; }
            if (failure) owner->fail(QString::fromNSString(failure.localizedDescription));
            else { state->started = true; owner->updateNativeAudio(); }
        });
    }];
    captureTick_.start();
    sourceCheck_.start();
    return true;
}

bool ScreenShare::updateNativeAudio() {
    auto* state = (__bridge SquadScreenCapture*)native_;
    if (!state || !state->stream) return false;
    { std::lock_guard lock(state->mutex); state->wantsAudio = audioEnabled_; state->audio.clear(); }
    state->encoder.resetCapture();
    if (captureHost_) captureHost_->setScreenAudio(false);
    // An update while startCapture is pending can be lost by ScreenCaptureKit.
    // Apply the latest user choice after startup instead of racing that request.
    if (!state->started) return true;
    if (state->configuration.capturesAudio == audioEnabled_) {
        if (captureHost_) captureHost_->setScreenAudio(audioEnabled_);
        return true;
    }
    state->configuration.capturesAudio = audioEnabled_;
    const auto stream = state->stream;
    const bool enabled = audioEnabled_;
    const QPointer<ScreenShare> owner(this);
    [stream updateConfiguration:state->configuration completionHandler:^(NSError* failure) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!owner || state->stream != stream || owner->audioEnabled_ != enabled) return;
            if (failure) owner->fail(QString::fromNSString(failure.localizedDescription));
            else if (owner->captureHost_) owner->captureHost_->setScreenAudio(enabled);
        });
    }];
    return true;
}

void ScreenShare::pollNative() {
    auto* state = (__bridge SquadScreenCapture*)native_;
    if (!state) return;
    if (!capturing()) { stop(); return; }
    QImage frame;
    QString error;
    std::array<float, 960> samples{};
    bool hasAudio = false;
    {
        std::lock_guard lock(state->mutex);
        state->wantsVideo = !localSinks_.isEmpty() || (active() && !captureHost_->screenTiers().isEmpty());
        frame = std::move(state->image); error = std::move(state->error);
        if (audioEnabled_ && state->audio.size() >= samples.size()) {
            std::copy_n(state->audio.begin(), samples.size(), samples.begin());
            state->audio.erase(state->audio.begin(), state->audio.begin() + samples.size());
            hasAudio = true;
        }
    }
    if (!error.isEmpty()) { fail(error); return; }
    if (!frame.isNull()) input_.setVideoFrame(QVideoFrame(frame));
    if (hasAudio) {
        try { for (const auto& packet : state->encoder.encode(samples, 48000)) captureHost_->sendScreenAudio(packet); }
        catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
    }
}

void ScreenShare::checkNativeSource() {
    auto* state = (__bridge SquadScreenCapture*)native_;
    if (!state || !capturing()) return;
    if (state->displayId && !CGDisplayIsActive(state->displayId)) {
        fail(tr("The shared screen was removed. Select a source to start again.")); return;
    }
    if (state->windowId) {
        const auto windows = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, state->windowId);
        const bool exists = windows && CFArrayGetCount(windows) > 0;
        if (windows) CFRelease(windows);
        if (!exists) fail(tr("The shared window was closed. Select a source to start again."));
    }
}

void ScreenShare::stopNative() {
    captureTick_.stop();
    auto* state = (__bridge SquadScreenCapture*)native_;
    if (!state) return;
    state->encoder.resetCapture();
    ++state->sourceRevision;
    state->started = false;
    auto* stream = state->stream;
    { std::lock_guard lock(state->mutex); state->stream = nil; }
    { std::lock_guard lock(state->mutex); state->wantsVideo = false; state->wantsAudio = false; state->image = {}; state->audio.clear(); }
    [stream removeStreamOutput:state type:SCStreamOutputTypeScreen error:nil];
    [stream removeStreamOutput:state type:SCStreamOutputTypeAudio error:nil];
    [stream stopCaptureWithCompletionHandler:nil];
}

void ScreenShare::disposeNative() {
    if (native_) { CFBridgingRelease(native_); native_ = nullptr; }
}
