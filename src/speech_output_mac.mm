#include "speech_output.hpp"
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>
#include <samplerate.h>
#import <AVFoundation/AVFoundation.h>

struct SpeechOutput::Impl {
    struct State {
        std::mutex mutex;
        std::vector<float> samples;
        size_t offset = 0;
        int rate = 0;
        bool done = false;
        bool stopped = false;
        bool failed = false;
        QElapsedTimer deadline;
        std::unique_ptr<SRC_STATE, decltype(&src_delete)> converter{nullptr, src_delete};
    };
    AVSpeechSynthesizer* synthesizer = nil;
    std::shared_ptr<State> state;
};

SpeechOutput::SpeechOutput() : impl_(std::make_unique<Impl>()) {}
SpeechOutput::~SpeechOutput() { stop(); }

void SpeechOutput::stop() {
    if (!impl_->state) return;
    {
        std::lock_guard lock(impl_->state->mutex);
        impl_->state->stopped = true;
        impl_->state->samples.clear();
    }
    [impl_->synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    impl_->synthesizer = nil;
    impl_->state.reset();
}

QVariantList SpeechOutput::voices() {
    QVariantList result;
    for (AVSpeechSynthesisVoice* voice in AVSpeechSynthesisVoice.speechVoices)
        result.append(QVariantMap{{"id", QString::fromNSString(voice.identifier)},
            {"name", QString::fromNSString(voice.name)}, {"language", QString::fromNSString(voice.language)}});
    return result;
}

bool SpeechOutput::start(const QString& text, int sampleRate, const QString& voice) {
    if (text.trimmed().isEmpty() || text.size() > 400 || sampleRate < 8000 || sampleRate > 192000) return false;
    auto* selected = voice.isEmpty() ? nil : [AVSpeechSynthesisVoice voiceWithIdentifier:voice.toNSString()];
    if (!voice.isEmpty() && !selected) return false;
    stop();
    auto state = std::make_shared<Impl::State>();
    int error;
    state->converter.reset(src_new(SRC_SINC_FASTEST, 1, &error));
    if (!state->converter) return false;
    state->rate = sampleRate;
    state->deadline.start();
    impl_->state = state;
    impl_->synthesizer = [[AVSpeechSynthesizer alloc] init];
    // Use the system default voice and honor active assistive technology settings.
    // These preferences do not expose a reliable translation target language.
    auto* utterance = [AVSpeechUtterance speechUtteranceWithString:text.toNSString()];
    utterance.prefersAssistiveTechnologySettings = voice.isEmpty();
    if (selected) utterance.voice = selected;
    [impl_->synthesizer writeUtterance:utterance toBufferCallback:^(AVAudioBuffer* buffer) {
        std::lock_guard lock(state->mutex);
        if (state->stopped || state->done) return;
        auto* pcm = [buffer isKindOfClass:AVAudioPCMBuffer.class] ? (AVAudioPCMBuffer*)buffer : nil;
        if (!pcm || pcm.frameLength == 0) {
            state->failed = !pcm || state->samples.empty(); state->done = true; return;
        }
        const auto format = pcm.format;
        if (format.commonFormat != AVAudioPCMFormatFloat32 || !pcm.floatChannelData
                || format.channelCount == 0 || format.channelCount > 8
                || format.sampleRate < 8000 || format.sampleRate > 192000 || pcm.frameLength > 192000) {
            state->failed = state->done = true; return;
        }
        std::vector<float> mono(pcm.frameLength, 0);
        for (size_t f = 0; f < mono.size(); ++f)
            for (unsigned c = 0; c < format.channelCount; ++c) {
                const auto value = format.isInterleaved ? pcm.floatChannelData[0][f * format.channelCount + c]
                    : pcm.floatChannelData[c][f];
                mono[f] += std::isfinite(value) ? std::clamp(value, -1.f, 1.f) / format.channelCount : 0;
            }
        const double ratio = state->rate / format.sampleRate;
        const auto limit = size_t(state->rate) * 30;
        const auto previous = state->samples.size();
        const auto capacity = size_t(std::ceil(mono.size() * ratio)) + 256;
        if (previous + capacity > limit) { state->done = true; return; }
        state->samples.resize(previous + capacity);
        SRC_DATA conversion{};
        conversion.data_in = mono.data(); conversion.input_frames = long(mono.size());
        conversion.data_out = state->samples.data() + previous; conversion.output_frames = long(capacity);
        conversion.src_ratio = ratio;
        if (src_process(state->converter.get(), &conversion) || conversion.input_frames_used != long(mono.size())) {
            state->samples.resize(previous); state->failed = state->done = true; return;
        }
        state->samples.resize(previous + size_t(conversion.output_frames_gen));
    }];
    return true;
}

bool SpeechOutput::active() const {
    if (!impl_->state) return false;
    std::lock_guard lock(impl_->state->mutex);
    return !impl_->state->stopped && impl_->state->deadline.elapsed() < 35000
        && (!impl_->state->done || impl_->state->offset < impl_->state->samples.size());
}

bool SpeechOutput::failed() const {
    if (!impl_->state) return false;
    std::lock_guard lock(impl_->state->mutex);
    return impl_->state->failed || (!impl_->state->done && impl_->state->deadline.elapsed() >= 35000);
}

bool SpeechOutput::mix(std::span<float> output, double gain) {
    if (!std::isfinite(gain) || gain <= 0 || gain > 1 || !active()) return false;
    std::lock_guard lock(impl_->state->mutex);
    auto& state = *impl_->state;
    const auto count = std::min(output.size(), state.samples.size() - state.offset);
    for (size_t i = 0; i < count; ++i)
        output[i] += float(state.samples[state.offset++] * gain);
    return count > 0;
}
