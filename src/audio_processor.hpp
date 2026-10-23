#pragma once

#include <array>
#include <cstddef>

namespace squad {

struct InputProfile final {
    double gainDb = 0.0;
    double highPassHz = 0.0;
    double lowPassHz = 0.0;
    bool gainAutomatic = false;
    bool highPassAutomatic = false;
    bool gainEnabled = true;
    bool highPassEnabled = true;
    bool lowPassEnabled = true;
    bool noiseEnabled = true;
    bool echoEnabled = true;
    bool operator==(const InputProfile&) const = default;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] double initialHighPassHz() const noexcept;
};

struct AudioLevels final {
    static constexpr std::size_t spectrumSize = 256;
    double inputDb = -96.0;
    double outputDb = -96.0;
    double strongestHz = 0.0;
    // A periodicity estimate, not a speech or speaker classification. Zero
    // means no sufficiently periodic candidate in the current search range.
    double fundamentalHz = 0.0;
    bool spectrumReady = false;
    bool clipped = false;
    std::array<double, spectrumSize> inputSpectrum{};
    std::array<double, spectrumSize> outputSpectrum{};
};

// Stateful mono/stereo processing with linked gain and filter controls. Call from one owning audio thread; no allocation
// occurs in process(). Zero cut frequencies bypass their respective filters.
class AudioProcessor final {
public:
    explicit AudioProcessor(double sampleRate, InputProfile profile = {});
    AudioProcessor& configure(InputProfile profile);
    [[nodiscard]] float process(float sample) noexcept;
    // Separate observation preserves the raw meter when an upstream processor
    // buffers frames or removes echo before this stage's gain and cuts.
    AudioProcessor& observeInput(float sample) noexcept;
    AudioProcessor& observeOutput(float sample) noexcept;
    // Stereo analysis combines channel powers, never opposite-phase samples.
    AudioProcessor& observeInput(std::array<float, 2> samples) noexcept;
    AudioProcessor& observeOutput(std::array<float, 2> samples) noexcept;
    // Filtering does not record measurements. process() observes both sides
    // for standalone use; a complete capture pipeline observes its endpoints.
    [[nodiscard]] float filter(float sample) noexcept;
    [[nodiscard]] std::array<float, 2> filter(std::array<float, 2> samples) noexcept;
    [[nodiscard]] AudioLevels levels() const noexcept;
    // Transfer of gain and frequency cuts only, in dB. Echo cancellation and
    // denoising are measured separately. Bins match the measured spectra.
    [[nodiscard]] std::array<double, AudioLevels::spectrumSize> filterResponse() const noexcept;
    [[nodiscard]] double maximumFrequency() const noexcept;
    [[nodiscard]] double gainDb() const noexcept;
    [[nodiscard]] double highPassHz() const noexcept { return highPassHz_; }

private:
    struct Filter final {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        std::array<double, 2> z1{}, z2{};
        [[nodiscard]] double process(double sample, std::size_t channel = 0) noexcept;
    };
    static constexpr std::size_t windowSize = 4096;
    static constexpr std::size_t pitchSize = 1024;
    [[nodiscard]] Filter cut(double frequency, bool highPass) const;
    [[nodiscard]] std::array<double, windowSize / 2 + 1> magnitudes(const std::array<float, windowSize>& samples, std::size_t cursor) const noexcept;
    [[nodiscard]] std::array<double, AudioLevels::spectrumSize> displaySpectrum(const std::array<double, windowSize / 2 + 1>& magnitudes) const noexcept;
    [[nodiscard]] double fundamental() const noexcept;
    double sampleRate_;
    double gain_ = 1.0;
    InputProfile profile_;
    double highPassHz_ = 0, bassPower_ = 0, voicePower_ = 0;
    double powerStep_ = 0, releaseStep_ = 0;
    std::size_t analysisSamples_ = 0, analysisInterval_ = 1;
    std::size_t bassDominantSamples_ = 0;
    Filter bassAnalysis_;
    Filter voiceAnalysis_;
    Filter highPass_;
    Filter lowPass_;
    std::array<std::array<float, windowSize>, 2> input_{};
    std::array<std::array<float, windowSize>, 2> output_{};
    std::array<double, windowSize> window_{};
    std::array<double, AudioLevels::spectrumSize> frequencies_{};
    double windowSum_ = 0.0;
    std::size_t inputCursor_ = 0, outputCursor_ = 0;
    std::size_t inputFilled_ = 0, outputFilled_ = 0;
    // Bounded analysis rate keeps pitch work independent of device sample rate.
    // These filters affect analysis only, never the returned audio samples.
    Filter pitchLowPass1_;
    Filter pitchLowPass2_;
    double pitchDecimation_ = 1;
    double pitchDecimationCount_ = 0;
    double pitchRate_ = 0;
    std::array<double, pitchSize> pitch_{};
    std::size_t pitchCursor_ = 0;
    std::size_t pitchFilled_ = 0;
};

}
