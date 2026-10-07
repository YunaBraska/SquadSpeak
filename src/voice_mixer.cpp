#include "voice_mixer.hpp"
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace squad {
namespace {
void validRate(int rate) {
    if (rate < 8000 || rate > 192000) throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Audio sample rate must be between 8000 and 192000 Hz"));
}
}
bool VoiceActivation::valid() const noexcept {
    return std::isfinite(thresholdDb) && thresholdDb >= -80 && thresholdDb <= 0;
}
VoiceMixer::VoiceMixer() {
    if (rnnoise_get_frame_size() != 480) throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Unexpected noise suppression frame size"));
    // Upstream's create helper does not check malloc before initialization.
    denoiser_.reset(static_cast<DenoiseState*>(std::malloc(rnnoise_get_size())));
    if (!denoiser_ || rnnoise_init(denoiser_.get(), nullptr) != 0)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression could not be initialized"));
    int error;
    encoder_.reset(opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error));
    if (!encoder_) throw std::runtime_error(opus_strerror(error));
    if (opus_encoder_ctl(encoder_.get(), OPUS_SET_BITRATE(32000)) != OPUS_OK)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Opus bitrate could not be configured"));
    inputConverter_.reset(src_new(SRC_SINC_FASTEST, 1, &error));
    if (!inputConverter_) throw std::runtime_error(src_strerror(error));
    outputConverter_.reset(src_new(SRC_SINC_FASTEST, 1, &error));
    if (!outputConverter_) throw std::runtime_error(src_strerror(error));
}

void VoiceMixer::resampleInto(SRC_STATE* state, std::span<const float> samples, double ratio,
    std::vector<float>& output) {
    const auto originalSize = output.size();
    if (ratio == 1) {
        output.insert(output.end(), samples.begin(), samples.end());
        return;
    }
    const auto capacity = size_t(std::ceil(samples.size() * ratio)) + 256;
    output.resize(originalSize + capacity);
    SRC_DATA data{};
    data.data_in = samples.data(); data.input_frames = long(samples.size());
    data.data_out = output.data() + originalSize; data.output_frames = long(capacity);
    data.src_ratio = ratio;
    const auto error = src_process(state, &data);
    if (error || data.input_frames_used != long(samples.size())) {
        output.resize(originalSize);
        if (error) throw std::runtime_error(src_strerror(error));
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Audio resampling buffer is insufficient"));
    }
    output.resize(originalSize + size_t(data.output_frames_gen));
}

QList<QByteArray> VoiceMixer::encode(std::span<const float> samples, int sampleRate, bool transmissionAllowed) {
    validRate(sampleRate);
    if (samples.size() > size_t(sampleRate) || std::any_of(samples.begin(), samples.end(), [](auto value) { return !std::isfinite(value); }))
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Capture chunk is too large or contains invalid samples"));
    if (inputRate_ != sampleRate || captureTransmissionAllowed_ != transmissionAllowed) {
        resetCapture(); inputRate_ = sampleRate; captureTransmissionAllowed_ = transmissionAllowed;
    }
    resampleInto(inputConverter_.get(), samples, 48000.0 / sampleRate, capture_);
    QList<QByteArray> packets;
    if (!transmissionAllowed) transmitLevel_ = 0;
    const auto appendPacket = [&](const float* frame) {
        if (!transmissionAllowed) return;
        QByteArray packet(4000, '\0');
        const auto bytes = opus_encode_float(encoder_.get(), frame, 960,
            reinterpret_cast<unsigned char*>(packet.data()), int(packet.size()));
        if (bytes < 0) throw std::runtime_error(opus_strerror(bytes));
        packet.resize(bytes);
        packets.append(packet);
        double energy = 0;
        for (size_t i = 0; i < 960; ++i) energy += double(frame[i]) * frame[i];
        const auto level = std::sqrt(energy / 960);
        transmitLevel_ = packets.size() == 1 ? level : std::max(transmitLevel_, level);
    };
    size_t used = 0;
    while (capture_.size() - used >= 960) {
        const float* frame = capture_.data() + used;
        double energy = 0;
        for (size_t i = 0; i < 960; ++i) energy += double(frame[i]) * frame[i];
        activationLevelDb_ = energy > 0 ? std::max(-96.0, 10 * std::log10(energy / 960)) : -96;
        std::array<float, 960> suppressed;
        double speechProbability = 0;
        if (noiseSuppression_ > 0 || (activation_.enabled && activation_.automatic)) {
            for (size_t i = 0; i < suppressed.size(); ++i)
                suppressed[i] = std::clamp(frame[i], -1.0f, 1.0f) * 32768.0f;
            const auto firstProbability = rnnoise_process_frame(denoiser_.get(), suppressed.data(), suppressed.data());
            const auto secondProbability = rnnoise_process_frame(denoiser_.get(), suppressed.data() + 480, suppressed.data() + 480);
            speechProbability = std::max(firstProbability, secondProbability);
            if (!std::isfinite(firstProbability) || !std::isfinite(secondProbability))
                throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Speech analysis produced an invalid probability"));
        }
        if (noiseSuppression_ > 0) {
            for (size_t i = 0; i < suppressed.size(); ++i) {
                const auto original = std::clamp(frame[i], -1.0f, 1.0f);
                const auto value = suppressed[i] / 32768.0 * noiseSuppression_
                    + delayedCapture_[i] * (1.0 - noiseSuppression_);
                if (!std::isfinite(value)) throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression produced invalid audio"));
                suppressed[i] = float(std::clamp(value, -1.0, 1.0));
                delayedCapture_[i] = original;
            }
            frame = suppressed.data();
        }
        if (activation_.enabled && activation_.automatic && speechProbability < 0.2) {
            // Follow likely background with headroom. A slow upward change avoids
            // quickly chasing quiet speech when classification is uncertain.
            const auto target = std::clamp(activationLevelDb_ + 10, -70.0, -20.0);
            activationThresholdDb_ += std::clamp(target - activationThresholdDb_, -0.12, 0.02);
        }
        const bool aboveThreshold = activationLevelDb_ >= activationThresholdDb_;
        voiceActive_ = !activation_.enabled || aboveThreshold || trailingFrames_ > 0;
        if (aboveThreshold) trailingFrames_ = 15;
        else if (trailingFrames_ > 0) --trailingFrames_;
        if (voiceActive_) {
            for (const auto& previous : preRoll_) appendPacket(previous.data());
            preRoll_.clear();
            appendPacket(frame);
        } else {
            std::array<float, 960> previous;
            std::copy_n(frame, previous.size(), previous.begin());
            if (preRoll_.size() == 2) preRoll_.pop_front();
            preRoll_.push_back(previous);
        }
        used += 960;
    }
    capture_.erase(capture_.begin(), capture_.begin() + used);
    if (used > 0 && packets.isEmpty()) transmitLevel_ = 0;
    return packets;
}

bool VoiceMixer::receive(const QString& peer, const QByteArray& packet, qint64 now, int missing) {
    if (peer.isEmpty() || packet.isEmpty() || packet.size() > 4000 || now < 0 || missing < 0 || missing > 6) return false;
    const auto* data = reinterpret_cast<const unsigned char*>(packet.constData());
    if (opus_packet_get_nb_samples(data, int(packet.size()), 48000) != 960) return false;
    auto& stream = streams_[peer];
    if (!stream.decoder) {
        int error;
        stream.decoder.reset(opus_decoder_create(48000, 1, &error));
        if (!stream.decoder) { streams_.erase(peer); throw std::runtime_error(opus_strerror(error)); }
        stream.gain = gains_.value(peer, 1);
    }
    Frame frame{{}, now};
    // Recover decoder continuity with Opus packet-loss concealment. Queue bounds
    // still cap latency; late network frames never grow the playback backlog.
    for (int gap = 0; gap < missing; ++gap) {
        if (opus_decode_float(stream.decoder.get(), nullptr, 0, frame.samples.data(), 960, 0) != 960) return false;
        if (stream.frames.size() == 6) stream.frames.pop_front();
        stream.frames.push_back(frame);
    }
    if (opus_decode_float(stream.decoder.get(), data, int(packet.size()), frame.samples.data(), 960, 0) != 960) return false;
    if (stream.frames.size() == 6) stream.frames.pop_front();
    stream.frames.push_back(frame);
    return true;
}

std::vector<float> VoiceMixer::render(int sampleRate, qint64 now) {
    validRate(sampleRate);
    if (now < 0) throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Playback clock must be nonnegative"));
    if (lastRender_ >= 0 && (now < lastRender_ || now - lastRender_ > 120))
        src_reset(outputConverter_.get());
    lastRender_ = now;
    std::array<float, 960> mixed{};
    for (auto& [id, stream] : streams_) {
        while (!stream.frames.empty() && now - stream.frames.front().received > 120) stream.frames.pop_front();
        if (stream.frames.empty()) continue;
        const auto frame = stream.frames.front(); stream.frames.pop_front();
        double energy = 0;
        for (const auto sample : frame.samples) energy += double(sample) * sample;
        const auto rms = std::sqrt(energy / frame.samples.size());
        stream.level = rms; stream.played = now;
        // Silence and very quiet noise never drive upward gain adaptation.
        if (automatic_ && rms > 0.0032) {
            const auto target = std::clamp(0.1 / rms, 0.125, 8.0);
            const auto coefficient = target < stream.automaticGain ? 0.4 : 0.04;
            stream.automaticGain += coefficient * (target - stream.automaticGain);
        }
        const auto gain = stream.gain * (automatic_ ? stream.automaticGain : 1.0);
        for (size_t i = 0; i < mixed.size(); ++i) mixed[i] += float(frame.samples[i] * gain);
    }
    float peak = 0;
    for (const auto sample : mixed) peak = std::max(peak, std::abs(sample));
    const double target = peak > 0.95 ? 0.95 / peak : 1;
    limiter_ = target < limiter_ ? target : limiter_ + 0.15 * (target - limiter_);
    for (auto& sample : mixed) sample *= float(limiter_);
    if (outputRate_ != sampleRate) { src_reset(outputConverter_.get()); outputRate_ = sampleRate; }
    std::vector<float> output;
    resampleInto(outputConverter_.get(), mixed, sampleRate / 48000.0, output);
    return output;
}

double VoiceMixer::playbackLevel(const QString& peer, qint64 now) const {
    const auto found = streams_.find(peer);
    if (found == streams_.end() || found->second.played < 0 || now < found->second.played
        || now - found->second.played > 120) return 0;
    return found->second.level;
}

VoiceMixer& VoiceMixer::setGain(const QString& peer, double gain) {
    if (peer.isEmpty() || !std::isfinite(gain) || gain < 0 || gain > 2)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Participant gain must be between 0 and 2"));
    gains_.insert(peer, gain);
    if (streams_.contains(peer)) streams_.at(peer).gain = gain;
    return *this;
}
VoiceMixer& VoiceMixer::setAutomatic(bool enabled) { automatic_ = enabled; return *this; }
VoiceMixer& VoiceMixer::setNoiseSuppression(double strength) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression strength must be between 0 and 1"));
    if (noiseSuppression_ == strength) return *this;
    resetCapture();
    noiseSuppression_ = strength;
    return *this;
}
VoiceMixer& VoiceMixer::setVoiceActivation(VoiceActivation activation) {
    if (!activation.valid()) throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Voice activation threshold must be between -80 and 0 dBFS"));
    if (activation_ == activation) return *this;
    activation_ = activation;
    activationThresholdDb_ = activation.thresholdDb;
    return resetCapture();
}
VoiceMixer& VoiceMixer::remove(const QString& peer) {
    gains_.remove(peer); streams_.erase(peer); return *this;
}
VoiceMixer& VoiceMixer::resetCapture() {
    capture_.clear(); src_reset(inputConverter_.get()); opus_encoder_ctl(encoder_.get(), OPUS_RESET_STATE);
    delayedCapture_.fill(0);
    preRoll_.clear(); trailingFrames_ = 0; voiceActive_ = false;
    activationLevelDb_ = -96; transmitLevel_ = 0;
    // Same embedded model already validated during construction; no allocation.
    rnnoise_init(denoiser_.get(), nullptr);
    inputRate_ = 0;
    return *this;
}
VoiceMixer& VoiceMixer::resetPlayback() {
    streams_.clear(); src_reset(outputConverter_.get()); outputRate_ = 0; limiter_ = 1; lastRender_ = -1;
    return *this;
}
}
