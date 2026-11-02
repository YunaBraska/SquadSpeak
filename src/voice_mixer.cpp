#include "voice_mixer.hpp"
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace squad {
namespace {
constexpr int maximumPlaybackDelayMs = 160;

void validRate(int rate) {
    if (rate < 8000 || rate > 192000)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Audio sample rate must be between 8000 and 192000 Hz"));
}
}

bool VoiceActivation::valid() const noexcept {
    return std::isfinite(thresholdDb) && thresholdDb >= -80 && thresholdDb <= 0
        && std::isfinite(minimumDb) && minimumDb >= -80 && minimumDb <= 0;
}

VoiceMixer::VoiceMixer(int channels, int playbackDelayMs) : channels_(channels), playbackDelayMs_(playbackDelayMs) {
    if (channels_ != 1 && channels_ != 2)
        throw std::invalid_argument("Voice mixer must have one or two channels");
    if (playbackDelayMs_ < 0 || playbackDelayMs_ > 100)
        throw std::invalid_argument("Playback delay must be between 0 and 100 ms");
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

QList<QByteArray> VoiceMixer::encode(std::span<const float> samples, int sampleRate,
    bool transmissionAllowed, const CaptureFilter& filter, const CaptureObserver& observer) {
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
        QByteArray packet(packetLimit_, '\0');
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
        std::array<float, 1920> original{};
        std::copy_n(capture_.data() + used, frameSamples, original.begin());
        if (filter) {
            const auto captured = std::span(capture_).subspan(used, frameSamples);
            filter(captured);
            if (std::any_of(captured.begin(), captured.end(), [](float value) { return !std::isfinite(value) || std::abs(value) > 1; }))
                throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Capture chunk is too large or contains invalid samples"));
        }
        const float* frame = capture_.data() + used;
        double energy = 0;
        for (size_t i = 0; i < frameSamples; ++i) energy += double(frame[i]) * frame[i];
        activationLevelDb_ = energy > 0 ? std::max(-96.0, 10 * std::log10(energy / frameSamples)) : -96;
        double speechLevelDb = activationLevelDb_;
        std::array<float, 1920> suppressed{};
        std::array<float, 1920> alignedPreview{};
        double speechProbability = 0;
        double speechConfidence = 0;
        bool speechFrame = false;
        if (noiseSuppression_ > 0 || (activation_.enabled && activation_.automatic)) {
            // RNNoise holds its recurrent state during digital silence. Forget
            // stale loud-speech history after a quiet second, before a softer onset.
            silenceFrames_ = activationLevelDb_ < -80 ? std::min(51, silenceFrames_ + 1) : 0;
            if (silenceFrames_ == 50)
                for (int channel = 0; channel < channels_; ++channel)
                    rnnoise_init(denoisers_[size_t(channel)].get(), nullptr);
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
                speechProbability = std::max(speechProbability, double(std::max(firstProbability, secondProbability)));
                speechConfidence = std::max(speechConfidence, double(std::min(firstProbability, secondProbability)));
            }
            speechFrame = speechConfidence >= 0.6;
        }
        if (noiseSuppression_ > 0) {
#ifdef SQUADSPEAK_DENOISE_CANDIDATE
            std::array<float, 1920> noiseOnly{};
            const auto halfSamples = size_t(480 * channels_);
            for (size_t i = 0; i < frameSamples; ++i) {
                const auto position = i % halfSamples;
                noiseOnly[i] = delayedNoise_[position];
                delayedNoise_[position] = suppressed[i] / 32768.0f;
            }
            float speechSnr = 0;
            if (!squad_denoise_process(neuralDenoiser_.get(), frame, suppressed.data(), &speechSnr))
                throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression produced invalid audio"));
            // Use the model's existing speech/noise estimate. Requiring speech
            // in both 10 ms halves retains temporal confirmation.
            speechFrame = speechFrame || speechSnr >= -10;
#endif
            const auto previousProtection = speechProtection_;
            // Retain natural consonants and timbre during speech, then release
            // smoothly into full noise reduction over the following pauses.
            speechProtection_ = std::max(speechConfidence >= 0.8 ? speechProbability : 0.0, speechProtection_ * 0.9);
            double suppressedEnergy = 0;
            for (size_t i = 0; i < frameSamples; ++i) {
                const auto dry = std::clamp(frame[i], -1.0f, 1.0f);
                const auto progress = double(i / size_t(channels_) + 1) / 960;
                const auto protection = previousProtection + progress * (speechProtection_ - previousProtection);
#ifdef SQUADSPEAK_DENOISE_CANDIDATE
                // At high manual strengths, use the existing noise estimate in
                // pauses. The default blend stays entirely on the neural model.
                const auto noiseOnlyMix = std::max(0.0, 2.0 * noiseSuppression_ - 1.0) * (1.0 - protection);
                const auto denoised = suppressed[i] * (1.0 - noiseOnlyMix) + noiseOnly[i] * noiseOnlyMix;
                const auto requested = noiseSuppression_ <= 0.5 ? noiseSuppression_ * 0.8 : noiseSuppression_ * 1.2 - 0.2;
                const auto strength = requested - std::max(0.0, requested - 0.5) * protection;
#else
                const auto denoised = suppressed[i] / 32768.0f;
                const auto strength = noiseSuppression_ * (1.0 - 0.5 * protection);
#endif
                const auto value = denoised * strength + delayedCapture_[delayPosition_] * (1.0 - strength);
                if (!std::isfinite(value)) throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression produced invalid audio"));
                suppressed[i] = float(std::clamp(value, -1.0, 1.0));
                suppressedEnergy += double(suppressed[i]) * suppressed[i];
                delayedCapture_[delayPosition_] = dry;
                alignedPreview[i] = delayedPreview_[delayPosition_];
                delayedPreview_[delayPosition_] = original[i];
                delayPosition_ = (delayPosition_ + 1) % size_t(suppressionDelaySamples() * channels_);
            }
            frame = suppressed.data();
            speechLevelDb = suppressedEnergy > 0 ? 10 * std::log10(suppressedEnergy / frameSamples) : -96;
        }
        if (observer) {
            observer(std::span(noiseSuppression_ > 0 ? alignedPreview : original).first(frameSamples),
                std::span(frame, frameSamples));
        }
        if (activation_.enabled && activation_.automatic && speechProbability < 0.2 && !speechFrame) {
            const auto target = std::clamp(speechLevelDb + 10, -70.0, -20.0);
            activationThresholdDb_ += std::clamp(target - activationThresholdDb_, -0.12, 0.02);
        }
        if (activation_.automatic && activation_.minimumEnabled)
            activationThresholdDb_ = std::max(activationThresholdDb_, activation_.minimumDb);
        if (activation_.automatic) activationLevelDb_ = std::max(-96.0, speechLevelDb);
        // speechFrame already confirms both 10 ms halves. Requiring a second
        // complete frame delays word edges beyond the retained pre-roll.
        const bool aboveThreshold = activationLevelDb_ >= activationThresholdDb_
            && (!activation_.automatic || speechFrame);
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
            if (preRoll_.size() == (activation_.automatic ? 12u : 2u)) preRoll_.pop_front();
            preRoll_.push_back(std::move(previous));
        }
        used += frameSamples;
    }
    capture_.erase(capture_.begin(), capture_.begin() + used);
    if (used > 0 && packets.isEmpty()) transmitLevel_ = 0;
    return packets;
}

QList<QByteArray> VoiceMixer::pace(QList<QByteArray> packets, qint64 now) {
    if (now < 0) throw std::invalid_argument("Capture clock must be nonnegative");
    if (outgoing_.isEmpty() && packets.isEmpty()) return {};
    // A delayed callback can supply several frames of continuous capture.
    // Only reset after silence preceding that batch, not its arrival delay.
    if (outgoing_.isEmpty() && (nextPacketAt_ < 0
        || now - nextPacketAt_ > 120 + (packets.size() - 1) * 20)) nextPacketAt_ = now;
    // Catch up within the transport's six-frame budget, never dump an entire
    // retained word into the receiver's smaller jitter buffer in one callback.
    if (now - nextPacketAt_ > 100) {
        const auto stale = std::min<qsizetype>(outgoing_.size(), (now - nextPacketAt_ - 100 + 19) / 20);
        outgoing_.remove(0, stale);
        nextPacketAt_ = std::max(nextPacketAt_ + stale * 20, now - 100);
    }
    outgoing_.append(std::move(packets));
    const auto due = now >= nextPacketAt_ ? (now - nextPacketAt_) / 20 + 1 : 0;
    // Catch-up packets leave now, so only future packets consume the retained
    // pre-roll budget. Trimming them first cuts speech after a delayed callback.
    const auto capacity = 18 + std::max<qint64>(0, due - 1);
    if (outgoing_.size() > capacity) outgoing_.remove(0, outgoing_.size() - capacity);
    const auto count = std::min<qsizetype>(outgoing_.size(), due);
    auto ready = outgoing_.sliced(0, count);
    outgoing_.remove(0, count);
    nextPacketAt_ += count * 20;
    return ready;
}

VoiceMixer& VoiceMixer::setPacketLimit(int bytes) {
    if (bytes < 1 || bytes > 4000) throw std::invalid_argument("Invalid Opus packet limit");
    packetLimit_ = bytes;
    return *this;
}

bool VoiceMixer::receive(const QString& peer, const QByteArray& packet, qint64 now, int missing, int missingFrameSamples, bool last) {
    if (peer.isEmpty() || packet.size() > 4000 || now < 0 || missing < 0
        || (missingFrameSamples != 480 && missingFrameSamples != 960)
        || missing > 5760 / missingFrameSamples) return false;
    if (packet.isEmpty()) {
        const auto found = streams_.find(peer);
        if (!last || found == streams_.end()) return false;
        auto& stream = found->second;
        if (stream.partialSamples > 0) {
            stream.partial.received = now;
            std::fill(stream.partial.samples.begin() + stream.partialSamples, stream.partial.samples.end(), 0);
            const auto frameLimit = size_t(std::max(7, stream.playbackDelay / 20 + 6));
            if (stream.frames.size() >= frameLimit) stream.frames.pop_front();
            stream.frames.push_back(stream.partial); stream.partialSamples = 0;
        }
        return true;
    }
    const auto* data = reinterpret_cast<const unsigned char*>(packet.constData());
    const int packetSamples = opus_packet_get_nb_samples(data, int(packet.size()), 48000);
    if (packetSamples < 120 || packetSamples > 5760 || packetSamples % 120 != 0) return false;
    auto& stream = streams_[peer];
    if (!stream.decoder) {
        int error;
        stream.decoder.reset(opus_decoder_create(48000, channels_, &error));
        if (!stream.decoder) { streams_.erase(peer); throw std::runtime_error(opus_strerror(error)); }
        stream.gain = gains_.value(peer, 1);
    }
    const auto gap = now - stream.receivedAt;
    // A pause beyond the jitter budget starts a new utterance only after
    // playback drains. Do not train network jitter on intentional silence.
    if (stream.receivedAt < 0 || (gap > maximumPlaybackDelayMs + 20
        && (now - stream.nextFrameAt > maximumPlaybackDelayMs + 20
            || (stream.frames.empty() && now > stream.nextFrameAt)))) {
        stream.frames.clear();
        stream.partialSamples = 0;
        if (stream.receivedAt < 0) stream.playbackDelay = playbackDelayMs_ > 0 ? 100 : 0;
        else if (stream.receivedAt - stream.lastJitterAt > 10000) stream.playbackDelay = playbackDelayMs_;
        stream.lastJitterAt = now;
        stream.readyAt = now + stream.playbackDelay;
        stream.nextFrameAt = stream.readyAt;
        // The previous utterance has drained. Concealing its sequence gap
        // would replay silence and displace frames from the new utterance.
        missing = 0;
    } else if (playbackDelayMs_ > 0) {
        // Excess over one packet interval, rounded to whole 20 ms frames.
        const int needed = int(std::clamp<qint64>(((gap - stream.packetDurationMs + 19) / 20) * 20, playbackDelayMs_, maximumPlaybackDelayMs));
        if (needed > playbackDelayMs_) stream.lastJitterAt = now;
        if (needed > stream.playbackDelay) {
            // Extend the pending playback deadline, not the arrival time.
            // Restarting a full delay after a late batch would overflow it.
            stream.readyAt = std::max(stream.readyAt, stream.nextFrameAt) + needed - stream.playbackDelay;
            stream.playbackDelay = needed;
        }
    }
    stream.receivedAt = now;
    // An exhausted queue needs its target depth again, even when a short
    // speech pause did not cross the new-utterance timeout. A complete batch
    // can still release this deadline below without waiting twice.
    if (playbackDelayMs_ > 0 && stream.frames.empty() && now > stream.nextFrameAt)
        stream.readyAt = now + stream.playbackDelay;
    // A delayed application loop receives its catch-up batch before it can
    // refill the output device. Retain that batch in addition to jitter depth,
    // within the same 120 ms freshness reserve enforced by render().
    const auto frameLimit = size_t(std::max(7, stream.playbackDelay / 20 + 6));
    std::array<float, 11520> decoded{};
    const auto append = [&](int frames) {
        const auto count = size_t(frames * channels_);
        const auto frameSamples = size_t(960 * channels_);
        for (size_t offset = 0; offset < count;) {
            const auto take = std::min(count - offset, frameSamples - stream.partialSamples);
            std::copy_n(decoded.data() + offset, take, stream.partial.samples.data() + stream.partialSamples);
            stream.partialSamples += take; offset += take;
            if (stream.partialSamples == frameSamples) {
                stream.partial.received = now;
                if (stream.frames.size() >= frameLimit) stream.frames.pop_front();
                stream.frames.push_back(stream.partial); stream.partialSamples = 0;
            }
        }
    };
    for (int gap = 0; gap < missing; ++gap) {
        if (opus_decode_float(stream.decoder.get(), nullptr, 0, decoded.data(), missingFrameSamples, 0) != missingFrameSamples) return false;
        append(missingFrameSamples);
    }
    if (opus_decode_float(stream.decoder.get(), data, int(packet.size()), decoded.data(), packetSamples, 0) != packetSamples) return false;
    append(packetSamples);
    stream.packetDurationMs = packetSamples / 48;
    if (last && stream.partialSamples > 0) {
        stream.partial.received = now;
        std::fill(stream.partial.samples.begin() + stream.partialSamples, stream.partial.samples.end(), 0);
        if (stream.frames.size() >= frameLimit) stream.frames.pop_front();
        stream.frames.push_back(stream.partial); stream.partialSamples = 0;
    }
    if (stream.frames.size() > size_t(stream.playbackDelay / 20))
        stream.readyAt = std::min(stream.readyAt, now);
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
        while (!stream.frames.empty() && now - stream.frames.front().received > 120 + stream.playbackDelay)
            stream.frames.pop_front();
        const bool ready = !stream.frames.empty() && now >= stream.readyAt;
        if (!ready && !filter) continue;
        Frame frame;
        frame.received = now;
        if (ready) {
            frame = std::move(stream.frames.front()); stream.frames.pop_front();
            stream.nextFrameAt = now + 20;
        }
        if (filter) filter(id, std::span<float>(frame.samples.data(), frameSamples), now);
        double energy = 0;
        for (size_t i = 0; i < frameSamples; ++i) energy += double(frame.samples[i]) * frame.samples[i];
        const auto rms = std::sqrt(energy / frameSamples);
        stream.level = rms; stream.played = now;
        const auto previousGain = stream.automaticGain;
        const auto automatic = automaticPeers_.value(id, automatic_);
        if (automatic && rms > 0.0032) {
            // Follow sustained speaker levels, not individual quiet syllables.
            // Hold the last 400 ms of active audio, with fast loudness reduction.
            stream.recentLevels[stream.levelCursor] = rms;
            stream.levelCursor = (stream.levelCursor + 1) % stream.recentLevels.size();
            const auto level = *std::max_element(stream.recentLevels.begin(), stream.recentLevels.end());
            const auto target = std::clamp(0.1 / level, 0.125, 8.0);
            const auto coefficient = target < stream.automaticGain ? 0.4 : 0.04;
            stream.automaticGain += coefficient * (target - stream.automaticGain);
        }
        auto gain = stream.gain * (automatic ? previousGain : 1.0);
        const auto step = automatic ? stream.gain * (stream.automaticGain - previousGain) / 960 : 0.0;
        // Keep automatic level changes continuous across decoded frame boundaries.
        for (size_t i = 0; i < frameSamples; i += size_t(channels_)) {
            gain += step;
            for (int channel = 0; channel < channels_; ++channel)
                mixed[i + size_t(channel)] += float(frame.samples[i + size_t(channel)] * gain);
        }
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
VoiceMixer& VoiceMixer::setAutomatic(bool enabled, const QString& peer) {
    if (peer.isEmpty()) automatic_ = enabled;
    else automaticPeers_.insert(peer, enabled);
    return *this;
}
VoiceMixer& VoiceMixer::setNoiseSuppression(double strength) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1)
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression strength must be between 0 and 1"));
    if (noiseSuppression_ == strength) return *this;
#ifdef SQUADSPEAK_DENOISE_CANDIDATE
    if (strength > 0 && !neuralDenoiser_) {
        neuralDenoiser_.reset(squad_denoise_create(channels_));
        if (!neuralDenoiser_)
            throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression could not be initialized"));
    }
#endif
    if ((noiseSuppression_ == 0) != (strength == 0)) resetCapture();
    noiseSuppression_ = strength;
    return *this;
}
VoiceMixer& VoiceMixer::setVoiceActivation(VoiceActivation activation) {
    if (!activation.valid())
        throw std::invalid_argument(QT_TRANSLATE_NOOP("AudioModel", "Voice activation threshold must be between -80 and 0 dBFS"));
    if (activation_ == activation) return *this;
    activation_ = activation;
    activationThresholdDb_ = activation.automatic && activation.minimumEnabled
        ? std::max(activation.thresholdDb, activation.minimumDb) : activation.thresholdDb;
    return resetCapture();
}
VoiceMixer& VoiceMixer::remove(const QString& peer) {
    gains_.remove(peer);
    automaticPeers_.remove(peer);
    streams_.erase(peer);
    return *this;
}
VoiceMixer& VoiceMixer::resetCapture() {
    capture_.clear();
    src_reset(inputConverter_.get());
    opus_encoder_ctl(encoder_.get(), OPUS_RESET_STATE);
    delayedCapture_.fill(0.0f);
    delayedPreview_.fill(0.0f);
    delayPosition_ = 0;
#ifdef SQUADSPEAK_DENOISE_CANDIDATE
    delayedNoise_.fill(0.0f);
    if (neuralDenoiser_ && !squad_denoise_reset(neuralDenoiser_.get()))
        throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Noise suppression could not be initialized"));
#endif
    preRoll_.clear();
    outgoing_.clear();
    nextPacketAt_ = -1;
    trailingFrames_ = 0;
    silenceFrames_ = 0;
    speechProtection_ = 0;
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
