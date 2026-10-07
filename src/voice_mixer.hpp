#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <array>
#include <deque>
#include <memory>
#include <map>
#include <span>
#include <vector>
#include <opus.h>
#include <samplerate.h>
#include <rnnoise.h>
#include <cstdlib>

namespace squad {

struct VoiceActivation final {
    bool enabled = false;
    bool automatic = true;
    double thresholdDb = -45;
    bool operator==(const VoiceActivation&) const = default;
    [[nodiscard]] bool valid() const noexcept;
};

// One capture stream and independent receive streams. PCM is mono, packets are
// 20 ms Opus frames at 48 kHz. Timestamps are from the caller's monotonic clock.
class VoiceMixer final {
public:
    VoiceMixer();
    // Denied capture still updates the preview, but produces no packets. A
    // permission transition discards buffered audio before examining new input.
    [[nodiscard]] QList<QByteArray> encode(std::span<const float> samples, int sampleRate, bool transmissionAllowed = true);
    [[nodiscard]] bool receive(const QString& peer, const QByteArray& packet, qint64 now, int missing = 0);
    [[nodiscard]] std::vector<float> render(int sampleRate, qint64 now);
    VoiceMixer& setGain(const QString& peer, double gain);
    VoiceMixer& setAutomatic(bool enabled);
    // Zero bypasses suppression. Positive values blend aligned original and
    // denoised capture before Opus; the pinned model adds 20 ms of history.
    VoiceMixer& setNoiseSuppression(double strength);
    VoiceMixer& setVoiceActivation(VoiceActivation activation);
    [[nodiscard]] double activationThresholdDb() const { return activationThresholdDb_; }
    [[nodiscard]] double activationLevelDb() const { return activationLevelDb_; }
    [[nodiscard]] bool voiceActive() const { return voiceActive_; }
    // RMS of audio admitted to the encoder, and of the last played peer frame.
    // These are audio activity measurements, not speech or emotion detection.
    [[nodiscard]] double transmitLevel() const { return transmitLevel_; }
    [[nodiscard]] double playbackLevel(const QString& peer, qint64 now) const;
    VoiceMixer& remove(const QString& peer);
    VoiceMixer& resetCapture();
    // Discard queued sound and decoder history while preserving local volume
    // settings. remove() also forgets that peer's configuration.
    VoiceMixer& resetPlayback();
private:
    struct Frame final { std::array<float, 960> samples; qint64 received; };
    struct Stream final {
        std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder{nullptr, opus_decoder_destroy};
        std::deque<Frame> frames;
        double gain = 1;
        double automaticGain = 1;
        double level = 0;
        qint64 played = -1;
    };
    static void resampleInto(SRC_STATE* state, std::span<const float> samples, double ratio,
        std::vector<float>& output);
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder_{nullptr, opus_encoder_destroy};
    std::unique_ptr<DenoiseState, decltype(&std::free)> denoiser_{nullptr, std::free};
    std::array<float, 960> delayedCapture_{};
    double noiseSuppression_ = 0;
    VoiceActivation activation_;
    double activationThresholdDb_ = -45;
    double activationLevelDb_ = -96;
    double transmitLevel_ = 0;
    int trailingFrames_ = 0;
    bool voiceActive_ = false;
    bool captureTransmissionAllowed_ = true;
    std::deque<std::array<float, 960>> preRoll_;
    std::unique_ptr<SRC_STATE, decltype(&src_delete)> inputConverter_{nullptr, src_delete};
    std::unique_ptr<SRC_STATE, decltype(&src_delete)> outputConverter_{nullptr, src_delete};
    std::map<QString, Stream> streams_;
    QHash<QString, double> gains_;
    std::vector<float> capture_;
    int inputRate_ = 0;
    int outputRate_ = 0;
    bool automatic_ = true;
    double limiter_ = 1;
    qint64 lastRender_ = -1;
};
}
