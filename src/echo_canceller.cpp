#include "echo_canceller.hpp"

#include <api/audio/audio_processing.h>
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace squad {
namespace {
void validate(std::span<const float> samples, int rate, std::int64_t nowMs, int queuedMs) {
    if (rate < 8000 || rate > 192000 || nowMs < 0
        || queuedMs < 0 || queuedMs > 500 || samples.size() > size_t(rate)
        || std::any_of(samples.begin(), samples.end(), [](float sample) { return !std::isfinite(sample) || std::abs(sample) > 1; }))
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Invalid echo cancellation audio or timing"));
}
void check(int result) {
    if (result != webrtc::AudioProcessing::kNoError)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Echo cancellation could not process audio"));
}
}

struct EchoCanceller::State final {
    rtc::scoped_refptr<webrtc::AudioProcessing> processor = webrtc::AudioProcessingBuilder().Create();
    std::vector<float> render;
    std::vector<float> capture;
    int renderRate = 0;
    int captureRate = 0;
    int renderDelayMs = 0;
    bool referenceReady = false;
    std::int64_t renderedAt = -1;
    std::int64_t capturedAt = -1;
    State() {
        if (!processor) throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Echo cancellation could not process audio"));
        webrtc::AudioProcessing::Config config;
        config.echo_canceller.enabled = true;
        config.echo_canceller.enforce_high_pass_filtering = false;
        config.high_pass_filter.enabled = false;
        config.gain_controller1.enabled = false;
        config.gain_controller2.enabled = false;
        config.noise_suppression.enabled = false;
        processor->ApplyConfig(config);
    }
};

EchoCanceller::EchoCanceller() = default;
EchoCanceller::~EchoCanceller() = default;
EchoCanceller& EchoCanceller::reset() { state_.reset(); return *this; }
EchoCanceller& EchoCanceller::setEnabled(bool enabled) {
    if (enabled_ != enabled) { enabled_ = enabled; reset(); }
    return *this;
}

EchoCanceller& EchoCanceller::render(std::span<const float> samples, int rate, std::int64_t nowMs, int queuedMs) {
    validate(samples, rate, nowMs, queuedMs);
    if (rate % 100) return reset();
    if (!enabled_ || samples.empty()) return *this;
    if (state_ && (state_->renderRate != rate || nowMs < state_->renderedAt || nowMs - state_->renderedAt > 250)) reset();
    if (!state_) state_ = std::make_unique<State>();
    auto& state = *state_;
    state.renderRate = rate;
    state.renderDelayMs = queuedMs;
    state.renderedAt = nowMs;
    state.render.insert(state.render.end(), samples.begin(), samples.end());
    const webrtc::StreamConfig format(rate, 1);
    const auto frames = format.num_frames();
    size_t used = 0;
    while (state.render.size() - used >= frames) {
        const float* input[] = {state.render.data() + used};
        check(state.processor->AnalyzeReverseStream(input, format));
        state.referenceReady = true;
        used += frames;
    }
    state.render.erase(state.render.begin(), state.render.begin() + used);
    return *this;
}

std::vector<float> EchoCanceller::capture(std::span<const float> samples, int rate, std::int64_t nowMs, int queuedMs) {
    validate(samples, rate, nowMs, queuedMs);
    if (rate % 100) reset();
    if (state_ && (nowMs < state_->renderedAt || nowMs - state_->renderedAt > 250
        || (state_->capturedAt >= 0 && (nowMs < state_->capturedAt || nowMs - state_->capturedAt > 250))
        || (state_->captureRate && state_->captureRate != rate))) reset();
    if (!state_ || !enabled_ || !state_->referenceReady) return {samples.begin(), samples.end()};
    auto& state = *state_;
    state.captureRate = rate;
    state.capturedAt = nowMs;
    state.capture.insert(state.capture.end(), samples.begin(), samples.end());
    const webrtc::StreamConfig format(rate, 1);
    const auto frames = format.num_frames();
    std::vector<float> output(state.capture.size() / frames * frames);
    for (size_t used = 0; used < output.size(); used += frames) {
        const float* input[] = {state.capture.data() + used};
        float* destination[] = {output.data() + used};
        check(state.processor->set_stream_delay_ms(std::min(500, state.renderDelayMs + queuedMs)));
        check(state.processor->ProcessStream(input, format, format, destination));
    }
    state.capture.erase(state.capture.begin(), state.capture.begin() + output.size());
    if (std::any_of(output.begin(), output.end(), [](float sample) { return !std::isfinite(sample); }))
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Echo cancellation could not process audio"));
    return output;
}

}
