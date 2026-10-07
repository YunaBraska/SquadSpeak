#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace squad {

// Owns one microphone/render pair on the caller's serialized audio path.
// PCM is mono in [-1, 1]. Render is the sound accepted by the output device,
// after local volume/mute; delays describe device queues, not network latency.
class EchoCanceller final {
public:
    EchoCanceller();
    ~EchoCanceller();
    EchoCanceller& setEnabled(bool enabled);
    EchoCanceller& reset();
    EchoCanceller& render(std::span<const float> samples, int rate,
        std::int64_t nowMs, int queuedMs);
    // Active processing retains at most a partial 10 ms frame. With no recent
    // render reference, when disabled, or for rates without integral 10 ms
    // frames, returns the input unchanged.
    [[nodiscard]] std::vector<float> capture(std::span<const float> samples,
        int rate, std::int64_t nowMs, int queuedMs);
private:
    struct State;
    std::unique_ptr<State> state_;
    bool enabled_ = true;
};

}
