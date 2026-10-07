#include "output_monitor.hpp"
#include <QScopeGuard>
#include <atomic>
#include <chrono>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

void OutputMonitor::capture(const QByteArray& deviceId) {
    @autoreleasepool {
        if (@available(macOS 14.2, *)) {
            const auto uid = QString::fromUtf8(deviceId).toNSString();
            CFStringRef nativeUid = (__bridge CFStringRef)uid;
            AudioDeviceID device = kAudioObjectUnknown;
            AudioObjectPropertyAddress property{kAudioHardwarePropertyTranslateUIDToDevice,
                kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
            UInt32 size = sizeof(device);
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, sizeof(nativeUid), &nativeUid,
                    &size, &device) != noErr || device == kAudioObjectUnknown) {
                fail(QStringLiteral("The selected output device is unavailable.")); return;
            }
            property = {kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain};
            if (AudioObjectGetPropertyDataSize(device, &property, 0, nullptr, &size) != noErr
                    || size != sizeof(AudioStreamID)) {
                fail(QStringLiteral("The selected output device needs one audio stream.")); return;
            }
            auto* description = [[CATapDescription alloc] initExcludingProcesses:@[] andDeviceUID:uid withStream:0];
            description.name = @"SquadSpeak echo reference";
            description.privateTap = YES;
            description.muteBehavior = CATapUnmuted;
            AudioObjectID tap = kAudioObjectUnknown;
            if (AudioHardwareCreateProcessTap(description, &tap) != noErr || tap == kAudioObjectUnknown) {
                fail(QStringLiteral("Allow system audio capture to monitor this output device.")); return;
            }
            const auto releaseTap = qScopeGuard([&] { AudioHardwareDestroyProcessTap(tap); });
            AudioStreamBasicDescription format{};
            property = {kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
            size = sizeof(format);
            if (AudioObjectGetPropertyData(tap, &property, 0, nullptr, &size, &format) != noErr
                    || format.mFormatID != kAudioFormatLinearPCM || !(format.mFormatFlags & kAudioFormatFlagIsFloat)
                    || (format.mFormatFlags & kAudioFormatFlagIsBigEndian) || format.mBitsPerChannel != 32
                    || !format.mChannelsPerFrame || format.mChannelsPerFrame > 32
                    || format.mSampleRate < 8000 || format.mSampleRate > 192000 || int(format.mSampleRate) % 100) {
                fail(QStringLiteral("The selected output monitor format is unsupported.")); return;
            }
            NSDictionary* properties = @{
                @kAudioAggregateDeviceNameKey: @"SquadSpeak echo reference",
                @kAudioAggregateDeviceUIDKey: NSUUID.UUID.UUIDString,
                @kAudioAggregateDeviceIsPrivateKey: @YES,
                @kAudioAggregateDeviceTapAutoStartKey: @NO,
                @kAudioAggregateDeviceTapListKey: @[@{
                    @kAudioSubTapUIDKey: description.UUID.UUIDString,
                    @kAudioSubTapDriftCompensationKey: @YES
                }]
            };
            AudioDeviceID aggregate = kAudioObjectUnknown;
            if (AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)properties, &aggregate) != noErr) {
                fail(QStringLiteral("The selected output monitor could not be opened.")); return;
            }
            const auto releaseAggregate = qScopeGuard([&] { AudioHardwareDestroyAggregateDevice(aggregate); });
            struct Context {
                OutputMonitor* owner;
                int rate;
                UInt32 channels;
                std::atomic<bool> invalid{false};
                std::atomic<qint64> receivedAt{0};
            } context{this, int(format.mSampleRate), format.mChannelsPerFrame};
            AudioDeviceIOProcID proc = nullptr;
            const auto callback = [](AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input,
                    const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void* data) -> OSStatus {
                auto& state = *static_cast<Context*>(data);
                if (!input || !input->mNumberBuffers || input->mNumberBuffers > state.channels) return noErr;
                size_t frames = 0;
                UInt32 channels = 0;
                for (UInt32 b = 0; b < input->mNumberBuffers; ++b) {
                    const auto& buffer = input->mBuffers[b];
                    if (!buffer.mNumberChannels || buffer.mNumberChannels > state.channels
                            || buffer.mDataByteSize % (sizeof(float) * buffer.mNumberChannels)) {
                        state.invalid = true; return noErr;
                    }
                    const auto count = buffer.mDataByteSize / (sizeof(float) * buffer.mNumberChannels);
                    if (b && count != frames) { state.invalid = true; return noErr; }
                    frames = count; channels += buffer.mNumberChannels;
                }
                if (channels != state.channels) { state.invalid = true; return noErr; }
                if (frames) state.receivedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::array<float, 1920> mono{};
                for (size_t offset = 0; offset < frames; offset += mono.size()) {
                    const auto count = std::min(mono.size(), frames - offset);
                    std::fill_n(mono.begin(), count, 0);
                    for (UInt32 b = 0; b < input->mNumberBuffers; ++b) {
                        const auto& buffer = input->mBuffers[b];
                        const auto* samples = static_cast<const float*>(buffer.mData);
                        if (!samples) continue;
                        for (size_t f = 0; f < count; ++f)
                            for (UInt32 c = 0; c < buffer.mNumberChannels; ++c)
                                mono[f] += samples[(offset + f) * buffer.mNumberChannels + c] / float(channels);
                    }
                    state.owner->append(std::span(mono).first(count), state.rate);
                }
                return noErr;
            };
            if (AudioDeviceCreateIOProcID(aggregate, callback, &context, &proc) != noErr) {
                fail(QStringLiteral("The output monitor callback could not be opened.")); return;
            }
            const auto releaseProc = qScopeGuard([&] { AudioDeviceDestroyIOProcID(aggregate, proc); });
            if (stopping_) return;
            if (AudioDeviceStart(aggregate, proc) != noErr) {
                fail(QStringLiteral("Allow system audio capture to monitor this output device.")); return;
            }
            const auto stopDevice = qScopeGuard([&] { AudioDeviceStop(aggregate, proc); });
            context.receivedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            while (!stopping_) {
                UInt32 alive = 0; size = sizeof(alive);
                property = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
                if (context.invalid || AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &alive) != noErr || !alive) {
                    fail(QStringLiteral("The selected output monitor stopped.")); return;
                }
                AudioStreamBasicDescription current{}; size = sizeof(current);
                property = {kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
                if (AudioObjectGetPropertyData(tap, &property, 0, nullptr, &size, &current) != noErr
                        || current.mSampleRate != format.mSampleRate || current.mFormatFlags != format.mFormatFlags
                        || current.mChannelsPerFrame != format.mChannelsPerFrame || current.mBitsPerChannel != format.mBitsPerChannel) {
                    fail(QStringLiteral("The selected output monitor format changed.")); return;
                }
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (now - context.receivedAt > 5000) {
                    fail(QStringLiteral("The selected output monitor provided no audio samples.")); return;
                }
                std::unique_lock waitLock(mutex_);
                wake_.wait_for(waitLock, std::chrono::milliseconds(250), [this] { return stopping_.load(); });
            }
        } else {
            fail(QStringLiteral("Output device monitoring requires macOS 14.2 or later."));
        }
    }
}
