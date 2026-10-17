#include "voice_mixer.hpp"
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace squad {
namespace {
void validRate(int rate) {
    if (rate < 8000 || rate > 192000)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Audio sample rate must be between 8000 and 192000 Hz"));
}
}

bool VoiceActivation::valid() const noexcept {
    return std::isfinite(thresholdDb) && thresholdDb >= -80 && thresholdDb <= 0;
}

VoiceMixer::VoiceMixer(int channels) : channels_(channels) {
    if (channels_ != 1 && channels_ != 2)
        throw std::invalid_argument("Voice mixer must have one or two channels");
    if (rnnoise_get_frame_size() != 480)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Unexpected noise suppression frame size"));
    for (int channel = 0; channel < channels_; ++channel) {
        denoisers_[size_t(channel)].reset(static_cast<DenoiseState*>(std::malloc(rnnoise_get_size())));
        if (!denoisers_[size_t(channel)] || rnnoise_init(denoisers_[size_t(channel)].get(), nullptr) != 0)
            throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression could not be initialized"));
    }
    int error = 0;
    const auto application = channels_ == 1 ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO;
    encoder_.reset(opus_encoder_create(48000, channels_, application, &error));
    if (!encoder_) throw std::runtime_error(opus_strerror(error));
    if (opus_encoder_ctl(encoder_.get(), OPUS_SET_BITRATE(channels_ == 1 ? 64000 : 128000)) != OPUS_OK)
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Opus bitrate could not be configured"));
    inputConverter_.reset(src_new(SRC_SINC_FASTEST, channels_, &error));
    if (!inputConverter_) throw std::runtime_error(src_strerror(error));
    outputConverter_.reset(src_new(SRC_SINC_FASTEST, channels_, &error));
    if (!outputConverter_) throw std::runtime_error(src_strerror(error));
    delayedCapture_.fill(0.0f);
}

void VoiceMixer::resampleInto(SRC_STATE* state, std::span<const float> samples, double ratio,
    std::vector<float>& output) const {
    const auto originalSize = output.size();
    const auto inputFrames = samples.size() / size_t(channels_);
    if (ratio == 1) {
        output.insert(output.end(), samples.begin(), samples.end());
        return;
    }
    const auto capacityFrames = size_t(std::ceil(inputFrames * ratio)) + 256;
    output.resize(originalSize + capacityFrames * size_t(channels_));
    SRC_DATA data{};
    data.data_in = samples.data();
    data.input_frames = long(inputFrames);
    data.data_out = output.data() + originalSize;
    data.output_frames = long(capacityFrames);
    data.src_ratio = ratio;
    const auto error = src_process(state, &data);
    if (error || data.input_frames_used != long(inputFrames)) {
        output.resize(originalSize);
        if (error) throw std::runtime_error(src_strerror(error));
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Audio resampling buffer is insufficient"));
    }
    output.resize(originalSize + size_t(data.output_frames_gen) * size_t(channels_));
}

QList<QByteArray> VoiceMixer::encode(std::span<const float> samples, int sampleRate, bool transmissionAllowed) {
    validRate(sampleRate);
    if (samples.size() % size_t(channels_) != 0 || samples.size() > size_t(sampleRate) * size_t(channels_)
        || std::any_of(samples.begin(), samples.end(), [](auto value) { return !std::isfinite(value); }))
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Capture chunk is too large or contains invalid samples"));
    if (inputRate_ != sampleRate || captureTransmissionAllowed_ != transmissionAllowed) {
        resetCapture();
        inputRate_ = sampleRate;
        captureTransmissionAllowed_ = transmissionAllowed;
    }
    resampleInto(inputConverter_.get(), samples, 48000.0 / sampleRate, capture_);
    QList<QByteArray> packets;
    if (!transmissionAllowed) transmitLevel_ = 0;
    const auto frameSamples = size_t(960 * channels_);
    const auto appendPacket = [&](const float* frame) {
        if (!transmissionAllowed) return;
        QByteArray packet(4000, '\0');
        const auto bytes = opus_encode_float(encoder_.get(), frame, 960,
            reinterpret_cast<unsigned char*>(packet.data()), int(packet.size()));
        if (bytes < 0) throw std::runtime_error(opus_strerror(bytes));
        packet.resize(bytes);
        packets.append(packet);
        double energy = 0;
        for (size_t i = 0; i < frameSamples; ++i) energy += double(frame[i]) * frame[i];
        const auto level = std::sqrt(energy / frameSamples);
        transmitLevel_ = packets.size() == 1 ? level : std::max(transmitLevel_, level);
    };
    size_t used = 0;
    while (capture_.size() - used >= frameSamples) {
        const float* frame = capture_.data() + used;
        double energy = 0;
        for (size_t i = 0; i < frameSamples; ++i) energy += double(frame[i]) * frame[i];
        activationLevelDb_ = energy > 0 ? std::max(-96.0, 10 * std::log10(energy / frameSamples)) : -96;
        double speechLevelDb = activationLevelDb_;
        std::array<float, 1920> suppressed{};
        double speechProbability = 0;
        bool speechFrame = false;
        if (noiseSuppression_ > 0 || (activation_.enabled && activation_.automatic)) {
            double probability = 0;
            bool channelSpeech = false;
            std::array<float, 480> input{};
            for (int channel = 0; channel < channels_; ++channel) {
                for (size_t i = 0; i < 480; ++i)
                    input[i] = std::clamp(frame[i * size_t(channels_) + size_t(channel)], -1.0f, 1.0f) * 32768.0f;
                const auto firstProbability = rnnoise_process_frame(denoisers_[size_t(channel)].get(), input.data(), input.data());
                for (size_t i = 0; i < 480; ++i)
                    suppressed[i * size_t(channels_) + size_t(channel)] = input[i];
                for (size_t i = 0; i < 480; ++i)
                    input[i] = std::clamp(frame[(i + 480) * size_t(channels_) + size_t(channel)], -1.0f, 1.0f) * 32768.0f;
                const auto secondProbability = rnnoise_process_frame(denoisers_[size_t(channel)].get(), input.data(), input.data());
                for (size_t i = 0; i < 480; ++i)
                    suppressed[(i + 480) * size_t(channels_) + size_t(channel)] = input[i];
                if (!std::isfinite(firstProbability) || !std::isfinite(secondProbability))
                    throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Speech analysis produced an invalid probability"));
                probability = std::max(probability, double(std::max(firstProbability, secondProbability)));
                channelSpeech = channelSpeech || std::min(firstProbability, secondProbability) >= 0.8;
            }
            speechProbability = probability;
            speechFrame = channelSpeech;
        }
        if (noiseSuppression_ > 0) {
            double suppressedEnergy = 0;
            for (size_t i = 0; i < frameSamples; ++i) {
                const auto original = std::clamp(frame[i], -1.0f, 1.0f);
                const auto denoised = suppressed[i] / 32768.0f;
                const auto value = denoised * noiseSuppression_ + delayedCapture_[i] * (1.0 - noiseSuppression_);
                if (!std::isfinite(value)) throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression produced invalid audio"));
                suppressed[i] = float(std::clamp(value, -1.0, 1.0));
                suppressedEnergy += double(suppressed[i]) * suppressed[i];
                delayedCapture_[i] = original;
            }
            frame = suppressed.data();
            speechLevelDb = suppressedEnergy > 0 ? 10 * std::log10(suppressedEnergy / frameSamples) : -96;
        }
        if (activation_.enabled && activation_.automatic && speechProbability < 0.2) {
            const auto target = std::clamp(activationLevelDb_ + 10, -70.0, -20.0);
            activationThresholdDb_ += std::clamp(target - activationThresholdDb_, -0.12, 0.02);
        }
        speechFrames_ = speechFrame ? std::min(2, speechFrames_ + 1) : 0;
        const bool aboveThreshold = (activation_.automatic ? speechLevelDb : activationLevelDb_) >= activationThresholdDb_
            && (!activation_.automatic || speechFrames_ == 2);
        voiceActive_ = !activation_.enabled || aboveThreshold || trailingFrames_ > 0;
        if (aboveThreshold) trailingFrames_ = 15;
        else if (trailingFrames_ > 0) --trailingFrames_;
        if (voiceActive_) {
            for (const auto& previous : preRoll_) appendPacket(previous.data());
            preRoll_.clear();
            appendPacket(frame);
        } else {
            std::array<float, 1920> previous{};
            std::copy_n(frame, frameSamples, previous.begin());
            if (preRoll_.size() == 2) preRoll_.pop_front();
            preRoll_.push_back(std::move(previous));
        }
        used += frameSamples;
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
        stream.decoder.reset(opus_decoder_create(48000, channels_, &error));
        if (!stream.decoder) { streams_.erase(peer); throw std::runtime_error(opus_strerror(error)); }
        stream.gain = gains_.value(peer, 1);
    }
    Frame frame;
    frame.received = now;
    for (int gap = 0; gap < missing; ++gap) {
        if (opus_decode_float(stream.decoder.get(), nullptr, 0, frame.samples.data(), 960, 0) != 960) return false;
        if (stream.frames.size() == 6) stream.frames.pop_front();
        stream.frames.push_back(frame);
    }
    if (opus_decode_float(stream.decoder.get(), data, int(packet.size()), frame.samples.data(), 960, 0) != 960) return false;
    if (stream.frames.size() == 6) stream.frames.pop_front();
    stream.frames.push_back(std::move(frame));
    return true;
}

std::vector<float> VoiceMixer::render(int sampleRate, qint64 now, const PlaybackFilter& filter) {
    validRate(sampleRate);
    if (now < 0) throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Playback clock must be nonnegative"));
    if (lastRender_ >= 0 && (now < lastRender_ || now - lastRender_ > 120)) src_reset(outputConverter_.get());
    lastRender_ = now;
    const auto frameSamples = size_t(960 * channels_);
    std::array<float, 1920> mixed{};
    for (auto& [id, stream] : streams_) {
        while (!stream.frames.empty() && now - stream.frames.front().received > 120) stream.frames.pop_front();
        if (stream.frames.empty() && !filter) continue;
        Frame frame;
        frame.received = now;
        if (!stream.frames.empty()) { frame = std::move(stream.frames.front()); stream.frames.pop_front(); }
        if (filter) filter(id, std::span<float>(frame.samples.data(), frameSamples), now);
        double energy = 0;
        for (size_t i = 0; i < frameSamples; ++i) energy += double(frame.samples[i]) * frame.samples[i];
        const auto rms = std::sqrt(energy / frameSamples);
        stream.level = rms; stream.played = now;
        if (automatic_ && rms > 0.0032) {
            const auto target = std::clamp(0.1 / rms, 0.125, 8.0);
            const auto coefficient = target < stream.automaticGain ? 0.4 : 0.04;
            stream.automaticGain += coefficient * (target - stream.automaticGain);
        }
        const auto gain = stream.gain * (automatic_ ? stream.automaticGain : 1.0);
        for (size_t i = 0; i < frameSamples; ++i) mixed[i] += float(frame.samples[i] * gain);
    }
    float peak = 0;
    for (const auto sample : mixed) peak = std::max(peak, std::abs(sample));
    const double target = peak > 0.95 ? 0.95 / peak : 1;
    limiter_ = target < limiter_ ? target : limiter_ + 0.15 * (target - limiter_);
    for (auto& sample : mixed) sample *= float(limiter_);
    if (outputRate_ != sampleRate) { src_reset(outputConverter_.get()); outputRate_ = sampleRate; }
    std::vector<float> output;
    resampleInto(outputConverter_.get(), std::span<const float>(mixed.data(), frameSamples), sampleRate / 48000.0, output);
    return output;
}

double VoiceMixer::playbackLevel(const QString& peer, qint64 now) const {
    const auto found = streams_.find(peer);
    if (found == streams_.end() || found->second.played < 0 || now < found->second.played || now - found->second.played > 120) return 0;
    return found->second.level;
}
VoiceMixer& VoiceMixer::setGain(const QString& peer, double gain) {
    if (peer.isEmpty() || !std::isfinite(gain) || gain < 0 || gain > 2)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Participant gain must be between 0 and 2"));
    gains_.insert(peer, gain);
    if (streams_.contains(peer)) streams_.at(peer).gain = gain;
    return *this;
}
VoiceMixer& VoiceMixer::setAutomatic(bool enabled) {
    automatic_ = enabled;
    return *this;
}
VoiceMixer& VoiceMixer::setNoiseSuppression(double strength) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression strength must be between 0 and 1"));
    if (noiseSuppression_ == strength) return *this;
    resetCapture();
    noiseSuppression_ = strength;
    return *this;
}
VoiceMixer& VoiceMixer::setVoiceActivation(VoiceActivation activation) {
    if (!activation.valid())
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Voice activation threshold must be between -80 and 0 dBFS"));
    if (activation_ == activation) return *this;
    activation_ = activation;
    activationThresholdDb_ = activation.thresholdDb;
    return resetCapture();
}
VoiceMixer& VoiceMixer::remove(const QString& peer) {
    gains_.remove(peer);
    streams_.erase(peer);
    return *this;
}
VoiceMixer& VoiceMixer::resetCapture() {
    capture_.clear();
    src_reset(inputConverter_.get());
    opus_encoder_ctl(encoder_.get(), OPUS_RESET_STATE);
    delayedCapture_.fill(0.0f);
    preRoll_.clear();
    trailingFrames_ = 0;
    speechFrames_ = 0;
    voiceActive_ = false;
    activationLevelDb_ = -96;
    transmitLevel_ = 0;
    for (int channel = 0; channel < channels_; ++channel)
        rnnoise_init(denoisers_[size_t(channel)].get(), nullptr);
    inputRate_ = 0;
    return *this;
}
VoiceMixer& VoiceMixer::resetPlayback() {
    streams_.clear();
    src_reset(outputConverter_.get());
    outputRate_ = 0;
    limiter_ = 1;
    lastRender_ = -1;
    return *this;
}
}
