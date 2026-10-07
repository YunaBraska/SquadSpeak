#include "audio_processor.hpp"
#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <numbers>
#include <stdexcept>

namespace squad {
namespace {
double decibels(double amplitude) noexcept {
    return std::max(-96.0, 20.0 * std::log10(std::max(amplitude, 1e-12)));
}
float normalized(float sample) noexcept {
    return std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
}
}

bool InputProfile::valid() const noexcept {
    return std::isfinite(gainDb) && gainDb >= -24 && gainDb <= 24
        && std::isfinite(highPassHz) && highPassHz >= 0 && highPassHz <= 20000
        && std::isfinite(lowPassHz) && lowPassHz >= 0 && lowPassHz <= 20000
        && (highPassAutomatic || lowPassHz == 0 || highPassHz < lowPassHz);
}

double InputProfile::initialHighPassHz() const noexcept {
    if (!highPassAutomatic) return highPassHz;
    return lowPassHz > 0 ? std::min(20.0, lowPassHz * 0.5) : 20.0;
}

AudioProcessor::AudioProcessor(double sampleRate, InputProfile profile)
    : sampleRate_(sampleRate) {
    if (!std::isfinite(sampleRate) || sampleRate < 1000)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Sample rate must be finite and at least 1000 Hz"));
    for (std::size_t i = 0; i < windowSize; ++i) {
        window_[i] = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / (windowSize - 1));
        windowSum_ += window_[i];
    }
    for (std::size_t i = 0; i < frequencies_.size(); ++i) {
        frequencies_[i] = 20 * std::pow(maximumFrequency() / 20, double(i) / (frequencies_.size() - 1));
    }
    pitchDecimation_ = std::ceil(sampleRate_ / 16000.0);
    pitchRate_ = sampleRate_ / pitchDecimation_;
    pitchLowPass1_ = cut(std::min(2000.0, pitchRate_ * 0.2), false);
    pitchLowPass2_ = pitchLowPass1_;
    bassAnalysis_ = cut(60, false);
    voiceAnalysis_ = cut(180, true);
    powerStep_ = 1 - std::exp(-1 / (sampleRate_ * 0.25));
    releaseStep_ = 1 - std::exp(-1 / (sampleRate_ * 0.5));
    analysisInterval_ = std::max<std::size_t>(1, std::lround(sampleRate_ * 0.02));
    configure(profile);
}

double AudioProcessor::maximumFrequency() const noexcept {
    return std::min(20000.0, sampleRate_ * 0.45);
}

AudioProcessor::Filter AudioProcessor::cut(double frequency, bool highPass) const {
    if (frequency == 0)
        return {};
    const auto omega = 2 * std::numbers::pi * frequency / sampleRate_;
    const auto cosine = std::cos(omega);
    const auto alpha = std::sin(omega) / std::sqrt(2.0);
    const auto divisor = 1 + alpha;
    const auto b0 = (highPass ? 1 + cosine : 1 - cosine) / (2 * divisor);
    return {b0, (highPass ? -2 : 2) * b0, b0,
            -2 * cosine / divisor, (1 - alpha) / divisor, 0, 0};
}

AudioProcessor& AudioProcessor::configure(InputProfile profile) {
    if (!profile.valid())
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Invalid gain or frequency range"));
    if (profile.highPassHz > maximumFrequency() || profile.lowPassHz > maximumFrequency())
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Filter frequency exceeds this device's sample rate"));
    profile_ = profile;
    gain_ = profile.gainAutomatic ? 1 : std::pow(10.0, profile.gainDb / 20.0);
    highPassHz_ = profile.initialHighPassHz();
    highPass_ = cut(highPassHz_, true);
    lowPass_ = cut(profile.lowPassHz, false);
    bassPower_ = voicePower_ = 0; analysisSamples_ = bassDominantSamples_ = 0;
    bassAnalysis_.z1 = bassAnalysis_.z2 = voiceAnalysis_.z1 = voiceAnalysis_.z2 = 0;
    return *this;
}

double AudioProcessor::Filter::process(double sample) noexcept {
    const auto result = b0 * sample + z1;
    z1 = b1 * sample - a1 * result + z2;
    z2 = b2 * sample - a2 * result;
    return result;
}

float AudioProcessor::process(float sample) noexcept {
    observeInput(sample);
    return filter(sample);
}

AudioProcessor& AudioProcessor::observeInput(float sample) noexcept {
    const auto input = normalized(sample);
    input_[inputCursor_] = input;
    inputCursor_ = (inputCursor_ + 1) % windowSize;
    inputFilled_ = std::min(inputFilled_ + 1, windowSize);
    const auto pitchSample = pitchLowPass2_.process(pitchLowPass1_.process(input));
    if (++pitchDecimationCount_ >= pitchDecimation_) {
        pitchDecimationCount_ = 0;
        pitch_[pitchCursor_] = pitchSample;
        pitchCursor_ = (pitchCursor_ + 1) % pitchSize;
        pitchFilled_ = std::min(pitchFilled_ + 1, pitchSize);
    }
    return *this;
}

float AudioProcessor::filter(float sample) noexcept {
    // Device and upstream processing must not poison recursive filters with
    // non-finite values. Record unclipped filter output for the clipping meter.
    const auto input = normalized(sample);
    if (profile_.highPassAutomatic) {
        const auto bass = bassAnalysis_.process(input), voice = voiceAnalysis_.process(input);
        bassPower_ += powerStep_ * (bass * bass - bassPower_);
        voicePower_ += powerStep_ * (voice * voice - voicePower_);
        if (++analysisSamples_ >= analysisInterval_) {
            analysisSamples_ = 0;
            const auto excess = bassPower_ > 1e-5 ? std::clamp((bassPower_ / std::max(voicePower_, 1e-12) - 1) / 2, 0.0, 1.0) : 0;
            bassDominantSamples_ = excess > 0 ? std::min(bassDominantSamples_ + analysisInterval_, std::size_t(sampleRate_)) : 0;
            const auto limit = profile_.lowPassHz > 0 ? std::min(100.0, profile_.lowPassHz * 0.5) : 100.0;
            const auto target = std::min(20 + (bassDominantSamples_ >= sampleRate_ * 0.8 ? 80 * excess : 0), limit);
            // Slow coefficient movement retains filter state and avoids clicks.
            highPassHz_ += std::clamp(target - highPassHz_, -0.8, 1.6);
            auto next = cut(highPassHz_, true);
            next.z1 = highPass_.z1; next.z2 = highPass_.z2; highPass_ = next;
        }
    }
    const auto filtered = lowPass_.process(highPass_.process(input));
    if (profile_.gainAutomatic) {
        const auto target = std::min(1.0, 0.89 / std::max(std::abs(filtered), 1e-12));
        gain_ = target < gain_ ? target : gain_ + releaseStep_ * (target - gain_);
    }
    const auto processed = filtered * gain_;
    output_[outputCursor_] = static_cast<float>(processed);
    outputCursor_ = (outputCursor_ + 1) % windowSize;
    outputFilled_ = std::min(outputFilled_ + 1, windowSize);
    return static_cast<float>(std::clamp(processed, -1.0, 1.0));
}

double AudioProcessor::gainDb() const noexcept { return decibels(gain_); }

AudioLevels AudioProcessor::levels() const noexcept {
    AudioLevels result;
    result.inputSpectrum.fill(-96);
    result.outputSpectrum.fill(-96);
    double inPower = 0, outPower = 0;
    for (std::size_t i = 0; i < inputFilled_; ++i) {
        inPower += double(input_[i]) * input_[i];
        result.clipped = result.clipped || std::abs(input_[i]) >= 1.0f;
    }
    for (std::size_t i = 0; i < outputFilled_; ++i) {
        outPower += double(output_[i]) * output_[i];
        result.clipped = result.clipped || std::abs(output_[i]) >= 1.0f;
    }
    if (inputFilled_) result.inputDb = decibels(std::sqrt(inPower / inputFilled_));
    if (outputFilled_) result.outputDb = decibels(std::sqrt(outPower / outputFilled_));
    result.fundamentalHz = fundamental();
    if (inputFilled_ < windowSize || outputFilled_ < windowSize)
        return result;
    result.spectrumReady = true;
    const auto inputMagnitude = magnitudes(input_, inputCursor_);
    result.inputSpectrum = displaySpectrum(inputMagnitude);
    result.outputSpectrum = displaySpectrum(magnitudes(output_, outputCursor_));
    const auto first = std::max<std::size_t>(1, std::ceil(20 * windowSize / sampleRate_));
    const auto last = std::size_t(maximumFrequency() * windowSize / sampleRate_);
    const auto peak = std::max_element(inputMagnitude.begin() + first, inputMagnitude.begin() + last + 1);
    if (decibels(*peak) > -70) {
        const auto bin = std::size_t(peak - inputMagnitude.begin());
        const auto left = decibels(inputMagnitude[bin - 1]);
        const auto middle = decibels(*peak);
        const auto right = decibels(inputMagnitude[bin + 1]);
        const auto curvature = left - 2 * middle + right;
        const auto offset = curvature < 0 ? std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5) : 0;
        result.strongestHz = std::clamp((bin + offset) * sampleRate_ / windowSize, 20.0, maximumFrequency());
    }
    return result;
}

std::array<double, AudioLevels::spectrumSize> AudioProcessor::filterResponse() const noexcept {
    std::array<double, AudioLevels::spectrumSize> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto omega = 2 * std::numbers::pi * frequencies_[i] / sampleRate_;
        const auto delay = std::polar(1.0, -omega);
        const auto delay2 = delay * delay;
        const auto transfer = [](const Filter& filter, const std::complex<double>& z, const std::complex<double>& z2) {
            const auto numerator = filter.b0 + filter.b1 * z + filter.b2 * z2;
            const auto denominator = 1.0 + filter.a1 * z + filter.a2 * z2;
            return numerator / denominator;
        };
        const auto magnitude = gain_ * std::abs(transfer(highPass_, delay, delay2) * transfer(lowPass_, delay, delay2));
        result[i] = decibels(magnitude);
    }
    return result;
}

std::array<double, AudioProcessor::windowSize / 2 + 1> AudioProcessor::magnitudes(const std::array<float, windowSize>& samples, std::size_t cursor) const noexcept {
    std::array<std::complex<double>, windowSize> bins;
    const auto mean = std::accumulate(samples.begin(), samples.end(), 0.0) / windowSize;
    for (std::size_t i = 0; i < windowSize; ++i)
        bins[i] = (samples[(cursor + i) % windowSize] - mean) * window_[i];
    // In-place radix-2 FFT: all frequency bins, without gaps between probes.
    for (std::size_t i = 1, reversed = 0; i < windowSize; ++i) {
        auto bit = windowSize / 2;
        for (; reversed & bit; bit /= 2) reversed ^= bit;
        reversed ^= bit;
        if (i < reversed) std::swap(bins[i], bins[reversed]);
    }
    for (std::size_t length = 2; length <= windowSize; length *= 2) {
        const auto rotation = std::polar(1.0, -2 * std::numbers::pi / length);
        for (std::size_t start = 0; start < windowSize; start += length) {
            std::complex<double> phase = 1;
            for (std::size_t j = 0; j < length / 2; ++j) {
                const auto even = bins[start + j];
                const auto odd = bins[start + j + length / 2] * phase;
                bins[start + j] = even + odd;
                bins[start + j + length / 2] = even - odd;
                phase *= rotation;
            }
        }
    }
    std::array<double, windowSize / 2 + 1> result;
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = std::abs(bins[i]) * (i == 0 || i == windowSize / 2 ? 1 : 2) / windowSum_;
    return result;
}

std::array<double, AudioLevels::spectrumSize> AudioProcessor::displaySpectrum(const std::array<double, windowSize / 2 + 1>& bins) const noexcept {
    std::array<double, AudioLevels::spectrumSize> result;
    const auto ratio = std::sqrt(frequencies_[1] / frequencies_[0]);
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto center = frequencies_[i] * windowSize / sampleRate_;
        const auto low = std::min<std::size_t>(std::ceil(center / ratio), bins.size() - 1);
        const auto high = std::min<std::size_t>(std::floor(center * ratio), bins.size() - 1);
        const auto nearest = std::min<std::size_t>(center, bins.size() - 2);
        auto amplitude = std::lerp(bins[nearest], bins[nearest + 1], center - nearest);
        if (low <= high)
            amplitude = std::max(amplitude, *std::max_element(bins.begin() + low, bins.begin() + high + 1));
        result[i] = decibels(amplitude);
    }
    return result;
}

double AudioProcessor::fundamental() const noexcept {
    if (pitchFilled_ < pitchSize) return 0;
    std::array<double, pitchSize> samples;
    const auto mean = std::accumulate(pitch_.begin(), pitch_.end(), 0.0) / pitchSize;
    double power = 0;
    for (std::size_t i = 0; i < pitchSize; ++i) {
        samples[i] = pitch_[(pitchCursor_ + i) % pitchSize] - mean;
        power += samples[i] * samples[i];
    }
    if (decibels(std::sqrt(power / pitchSize)) < -70) return 0;
    constexpr auto comparisonSize = pitchSize / 2;
    const auto maxLag = std::min<std::size_t>(pitchRate_ / 50.0, comparisonSize - 2);
    std::array<double, comparisonSize> difference{};
    std::array<double, comparisonSize> normalized{};
    normalized[0] = 1;
    double cumulative = 0;
    // YIN difference and cumulative mean normalization (de Cheveigne/Kawahara,
    // 2002, equations 6 and 8). Reject weak candidates instead of forcing a pitch.
    for (std::size_t lag = 1; lag <= maxLag + 1; ++lag) {
        for (std::size_t i = 0; i < comparisonSize; ++i) {
            const auto delta = samples[i] - samples[i + lag];
            difference[lag] += delta * delta;
        }
        cumulative += difference[lag];
        normalized[lag] = cumulative > 0 ? difference[lag] * lag / cumulative : 1;
    }
    for (std::size_t lag = 2; lag <= maxLag; ++lag) {
        if (normalized[lag] > normalized[lag - 1] || normalized[lag] > normalized[lag + 1]) continue;
        const auto normalizedCurvature = normalized[lag - 1] - 2 * normalized[lag] + normalized[lag + 1];
        const auto normalizedSlope = 0.5 * (normalized[lag + 1] - normalized[lag - 1]);
        const auto minimum = normalizedCurvature > 0
            ? normalized[lag] - normalizedSlope * normalizedSlope / (2 * normalizedCurvature) : normalized[lag];
        if (minimum >= 0.1) continue;
        const auto curvature = difference[lag - 1] - 2 * difference[lag] + difference[lag + 1];
        const auto offset = curvature > 0
            ? std::clamp(0.5 * (difference[lag - 1] - difference[lag + 1]) / curvature, -0.5, 0.5) : 0;
        const auto frequency = pitchRate_ / (lag + offset);
        // An earlier period outside our display range must not be replaced by
        // a lower subharmonic merely to force a value into the voice range.
        return frequency >= 49.5 && frequency <= std::min(1000.5, pitchRate_ * 0.2) ? frequency : 0;
    }
    return 0;
}

}
