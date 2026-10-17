#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace squad {

// Owns one microphone/render pair on the caller's serialized audio path.
// PCM is interleaved mono/stereo in [-1, 1]. Render is the sound accepted by the output device,
// after local volume/mute; delays describe device queues, not network latency.
class EchoCanceller final {
public:
    explicit EchoCanceller(int captureChannels = 1);
    ~EchoCanceller();
    // Auto (nullopt) preserves adaptive upstream defaults. Manual 0 disables
    // AEC, 0.5 matches Auto, and 1 uses the strongest residual suppression.
    EchoCanceller& setSuppression(std::optional<double> strength);
    EchoCanceller& reset();
    EchoCanceller& render(std::span<const float> samples, int rate,
        std::int64_t nowMs, int queuedMs, int channels = 1);
    // Active processing retains at most a partial 10 ms frame. With no recent
    // render reference, when disabled, or for rates without integral 10 ms
    // frames, returns the input unchanged.
    [[nodiscard]] std::vector<float> capture(std::span<const float> samples,
        int rate, std::int64_t nowMs, int queuedMs);
private:
    struct State;
    std::unique_ptr<State> state_;
    double strength_ = 0.5;
    int captureChannels_;
};

}
