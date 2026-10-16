#include "echo_canceller.hpp"

#include <api/audio/audio_processing.h>
#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace squad {
namespace {
void validate(std::span<const float> samples, int rate, std::int64_t nowMs, int queuedMs, int channels) {
    if ((channels != 1 && channels != 2) || samples.size() % size_t(channels) || rate < 8000 || rate > 192000 || nowMs < 0
        || queuedMs < 0 || queuedMs > 500 || samples.size() > size_t(rate * channels)
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
    int renderChannels = 0;
    int captureRate = 0;
    int renderDelayMs = 0;
    bool referenceReady = false;
    std::int64_t renderedAt = -1;
    std::int64_t capturedAt = -1;
    explicit State(double strength) {
        if (!processor) throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Echo cancellation could not process audio"));
        webrtc::AudioProcessing::Config config;
        config.pipeline.multi_channel_render = true;
        config.pipeline.multi_channel_capture = true;
        config.echo_canceller.enabled = true;
        config.echo_canceller.suppression_gain = float(std::pow(4.0, 2 * strength - 1));
        config.echo_canceller.enforce_high_pass_filtering = false;
        config.high_pass_filter.enabled = false;
        config.gain_controller1.enabled = false;
        config.gain_controller2.enabled = false;
        config.noise_suppression.enabled = false;
        processor->ApplyConfig(config);
    }
};

EchoCanceller::EchoCanceller(int captureChannels) : captureChannels_(captureChannels) {
    if (captureChannels != 1 && captureChannels != 2)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Invalid echo cancellation audio or timing"));
}
EchoCanceller::~EchoCanceller() = default;
EchoCanceller& EchoCanceller::reset() { state_.reset(); return *this; }
EchoCanceller& EchoCanceller::setSuppression(std::optional<double> strength) {
    if (strength && (!std::isfinite(*strength) || *strength < 0 || *strength > 1))
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Invalid microphone profile"));
    const auto value = strength.value_or(0.5);
    if (strength_ != value) {
        strength_ = value;
        reset();
    }
    return *this;
}

EchoCanceller& EchoCanceller::render(std::span<const float> samples, int rate, std::int64_t nowMs, int queuedMs, int channels) {
    validate(samples, rate, nowMs, queuedMs, channels);
    if (rate % 100) return reset();
    if (strength_ == 0 || samples.empty()) return *this;
    if (state_ && (state_->renderRate != rate || state_->renderChannels != channels || nowMs < state_->renderedAt || nowMs - state_->renderedAt > 250)) reset();
    if (!state_) state_ = std::make_unique<State>(strength_);
    auto& state = *state_;
    state.renderRate = rate; state.renderChannels = channels;
    state.renderDelayMs = queuedMs;
    state.renderedAt = nowMs;
    state.render.insert(state.render.end(), samples.begin(), samples.end());
    const webrtc::StreamConfig format(rate, channels);
    const auto frames = format.num_frames() * channels;
    std::array<std::array<float, 1920>, 2> planar{};
    size_t used = 0;
    while (state.render.size() - used >= frames) {
        for (size_t f = 0; f < format.num_frames(); ++f)
            for (int c = 0; c < channels; ++c) planar[c][f] = state.render[used + f * channels + c];
        const float* input[] = {planar[0].data(), planar[1].data()};
        check(state.processor->AnalyzeReverseStream(input, format));
        state.referenceReady = true;
        used += frames;
    }
    state.render.erase(state.render.begin(), state.render.begin() + used);
    return *this;
}

std::vector<float> EchoCanceller::capture(std::span<const float> samples, int rate, std::int64_t nowMs, int queuedMs) {
    validate(samples, rate, nowMs, queuedMs, captureChannels_);
    if (rate % 100) reset();
    if (state_ && (nowMs < state_->renderedAt || nowMs - state_->renderedAt > 250
        || (state_->capturedAt >= 0 && (nowMs < state_->capturedAt || nowMs - state_->capturedAt > 250))
        || (state_->captureRate && state_->captureRate != rate))) reset();
    if (!state_ || !state_->referenceReady) return {samples.begin(), samples.end()};
    auto& state = *state_;
    state.captureRate = rate;
    state.capturedAt = nowMs;
    state.capture.insert(state.capture.end(), samples.begin(), samples.end());
    const webrtc::StreamConfig format(rate, captureChannels_);
    const auto frames = format.num_frames() * captureChannels_;
    std::array<std::array<float, 1920>, 2> planar{};
    std::vector<float> output(state.capture.size() / frames * frames);
    for (size_t used = 0; used < output.size(); used += frames) {
        for (size_t f = 0; f < format.num_frames(); ++f)
            for (int c = 0; c < captureChannels_; ++c) planar[c][f] = state.capture[used + f * captureChannels_ + c];
        const float* input[] = {planar[0].data(), planar[1].data()};
        float* destination[] = {planar[0].data(), planar[1].data()};
        check(state.processor->set_stream_delay_ms(std::min(500, state.renderDelayMs + queuedMs)));
        check(state.processor->ProcessStream(input, format, format, destination));
        for (size_t f = 0; f < format.num_frames(); ++f)
            for (int c = 0; c < captureChannels_; ++c) output[used + f * captureChannels_ + c] = planar[c][f];
    }
    state.capture.erase(state.capture.begin(), state.capture.begin() + output.size());
    if (std::any_of(output.begin(), output.end(), [](float sample) { return !std::isfinite(sample); }))
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Echo cancellation could not process audio"));
    return output;
}

}
