#include "voice_mixer.hpp"
#include "audio_profiles.hpp"
#include "audio_processor.hpp"
#include <QTemporaryDir>
#include <QTest>
#include <QFile>
#include <QElapsedTimer>
#include <QtEndian>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <random>
#include <stdexcept>
#include <barrier>
#include <future>

namespace {
std::vector<float> speechFrame(int frame, int channels, double amplitude) {
    static const auto speech = [] {
        QFile file(":/audio/speech1.wav");
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing speech fixture");
        const auto bytes = file.readAll();
        if (bytes.size() != 44 + 384000 * 2) throw std::runtime_error("Invalid speech fixture");
        std::vector<float> samples(384000);
        float peak = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            samples[i] = qFromLittleEndian<qint16>(bytes.constData() + 44 + i * 2) / 32768.0f;
            peak = std::max(peak, std::abs(samples[i]));
        }
        if (peak == 0) throw std::runtime_error("Silent speech fixture");
        for (auto& sample : samples) sample /= peak;
        return samples;
    }();
    std::vector<float> result(size_t(960 * channels));
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = float(amplitude * speech[(size_t(frame * 960) + i / size_t(channels)) % speech.size()]);
    return result;
}
std::vector<float> tone(double amplitude, int rate = 48000, int frames = 960, double frequency = 440) {
    std::vector<float> result(size_t(frames), 0);
    for (int i = 0; i < frames; ++i) result[size_t(i)] = float(amplitude * std::sin(2 * std::numbers::pi * frequency * i / rate));
    return result;
}
double rms(const std::vector<float>& signal) {
    return std::sqrt(std::inner_product(signal.begin(), signal.end(), signal.begin(), 0.0) / signal.size());
}
std::vector<float> stereoTone(double left, double right, int frames = 960) {
    std::vector<float> result(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        result[size_t(i) * 2] = float(left * std::sin(2 * std::numbers::pi * 440 * i / 48000));
        result[size_t(i) * 2 + 1] = float(right * std::sin(2 * std::numbers::pi * 997 * i / 48000));
    }
    return result;
}
}
class MixerTests final : public QObject {
    Q_OBJECT
private slots:
    void capturePreviewIsAlignedAndDoesNotChangePackets_data() {
        QTest::addColumn<int>("channels"); QTest::addColumn<int>("rate"); QTest::addColumn<double>("strength");
        for (int channels : {1, 2}) for (int rate : {44100, 48000}) for (double strength : {0.0, 0.5, 1.0})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(channels).arg(rate).arg(strength))) << channels << rate << strength;
    }

    void capturePreviewIsAlignedAndDoesNotChangePackets() {
        QFETCH(int, channels); QFETCH(int, rate); QFETCH(double, strength);
        squad::VoiceMixer observed(channels), unobserved(channels);
        observed.setNoiseSuppression(strength); unobserved.setNoiseSuppression(strength);
        squad::AudioProcessor analysis(48000);
        std::vector<float> current, previous(size_t(squad::VoiceMixer::suppressionDelaySamples() * channels), 0);
        int frames = 0;
        const auto filter = [](std::span<float> samples) { for (auto& sample : samples) sample *= 0.25f; };
        const auto capture = [&](std::span<float> samples) {
            QCOMPARE(samples.size(), size_t(960 * channels));
            current.assign(samples.begin(), samples.end()); filter(samples);
        };
        const auto observer = [&](std::span<const float> raw, std::span<const float> filtered) {
            QCOMPARE(raw.size(), current.size()); QCOMPARE(filtered.size(), raw.size());
            const auto& aligned = strength > 0 ? previous : current;
            for (size_t i = 0; i < raw.size(); ++i) {
                QCOMPARE(raw[i], aligned[i]); QVERIFY(std::isfinite(filtered[i]));
                if (strength == 0) QCOMPARE(filtered[i], raw[i] * 0.25f);
            }
            for (size_t i = 0; i < raw.size(); i += size_t(channels)) {
                analysis.observeInput({raw[i], raw[i + channels - 1]});
                analysis.observeOutput({filtered[i], filtered[i + channels - 1]});
            }
            previous.insert(previous.end(), current.begin(), current.end());
            previous.erase(previous.begin(), previous.begin() + current.size());
            ++frames;
        };
        std::mt19937 random(47);
        std::uniform_real_distribution<float> noise(-0.4f, 0.4f);
        // Irregular chunks exercise resampling and the incomplete-frame buffer.
        for (int chunk = 0; chunk < 300; ++chunk) {
            std::vector<float> input(size_t((chunk % 3 + 1) * 277 * channels));
            for (auto& sample : input) sample = noise(random);
            const auto packets = observed.encode(input, rate, true, capture, observer);
            QCOMPARE(packets, unobserved.encode(input, rate, true, filter));
        }
        QVERIFY(frames > 100);
        const auto levels = analysis.levels(); QVERIFY(levels.spectrumReady);
        QVERIFY(levels.inputDb > -25);
        QVERIFY(levels.outputDb < levels.inputDb - (strength == 1 ? 20 : 11));
        // Muting drops old capture but keeps the local preview alive.
        previous.assign(previous.size(), 0);
        const auto beforeMute = frames;
        QVERIFY(observed.encode(tone(0.1, rate, 2000 * channels), rate, false, capture, observer).isEmpty());
        QVERIFY(frames > beforeMute); QCOMPARE(observed.transmitLevel(), 0.0);
        observed.resetCapture(); previous.assign(previous.size(), 0);
        QVERIFY(observed.encode({}, rate, false, capture, observer).isEmpty());
        QVERIFY(observed.encode(tone(0.1, rate, 2000 * channels), rate, false, capture, observer).isEmpty());
        observed.setVoiceActivation({true, false, 0}); previous.assign(previous.size(), 0);
        const auto beforeGate = frames;
        QVERIFY(observed.encode(tone(0.1, rate, 2000 * channels), rate, true, capture, observer).isEmpty());
        QVERIFY(frames > beforeGate); QVERIFY(!observed.voiceActive());
    }

    void captureFilterRejectsInvalidOutputBeforeObservationOrEncoding() {
        squad::VoiceMixer mixer;
        bool observed = false;
        const auto observer = [&](auto, auto) { observed = true; };
        for (const auto value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1.01f}) {
            const auto invalid = [value](std::span<float> frame) { frame[0] = value; };
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)mixer.encode(tone(0.2), 48000, true, invalid, observer));
            QVERIFY(!observed); mixer.resetCapture();
            QVERIFY(!mixer.encode(tone(0.2), 48000).isEmpty());
        }
    }

    void stereoSourcePreservesIndependentChannels() {
        squad::VoiceMixer source(2);
        int error = 0;
        const std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
            opus_decoder_create(48000, 2, &error), opus_decoder_destroy);
        QVERIFY(decoder && error == OPUS_OK);
        const auto input = stereoTone(0.8, 0.2);
        const auto packets = source.encode(input, 48000);
        QCOMPARE(packets.size(), 1);
        std::vector<float> output(1920);
        QCOMPARE(opus_decode_float(decoder.get(), reinterpret_cast<const unsigned char*>(packets.first().constData()),
            packets.first().size(), output.data(), 960, 0), 960);
        std::vector<float> left(960), right(960);
        for (int i = 0; i < 960; ++i) { left[size_t(i)] = output[size_t(i) * 2]; right[size_t(i)] = output[size_t(i) * 2 + 1]; }
        QVERIFY(rms(left) > rms(right) * 1.5);
        QVERIFY(rms(right) > 0.03);
        const auto cross = std::inner_product(left.begin(), left.end(), right.begin(), 0.0);
        QVERIFY(std::abs(cross) < std::sqrt(std::inner_product(left.begin(), left.end(), left.begin(), 0.0)
            * std::inner_product(right.begin(), right.end(), right.begin(), 0.0)) * 0.4);
        QVERIFY(!source.encode(stereoTone(-0.8, -0.2), 48000).isEmpty());
    }

    void stereoResamplingAndResetRemainFrameAligned() {
        squad::VoiceMixer source(2);
        const auto packets = source.encode(stereoTone(0.7, 0.15, 960), 24000);
        QVERIFY(packets.size() >= 1);
        source.resetCapture();
        QVERIFY(source.encode(stereoTone(0.4, 0.1, 480), 48000).isEmpty());
        QVERIFY(source.encode(stereoTone(0.4, 0.1, 480), 48000).size() == 1);
    }

    void stereoGainLimiterAndMuteAreLinkedAcrossChannels() {
        squad::VoiceMixer source(2), receiver(2);
        receiver.setAutomatic(false).setGain("peer", 2.0);
        const auto packet = source.encode(stereoTone(1.0, 0.4), 48000).first();
        QVERIFY(receiver.receive("peer", packet, 0));
        const auto output = receiver.render(48000, 0);
        QVERIFY(output.size() == 1920);
        float peak = 0;
        for (const auto sample : output) peak = std::max(peak, std::abs(sample));
        QVERIFY(peak <= 0.951f);
        std::vector<float> left, right;
        for (size_t i = 0; i < 960; ++i) { left.push_back(output[i * 2]); right.push_back(output[i * 2 + 1]); }
        QVERIFY(rms(left) > rms(right) * 1.5);
        receiver.setGain("peer", 0);
        QVERIFY(receiver.receive("peer", packet, 20));
        QCOMPARE(rms(receiver.render(48000, 20)), 0.0);
    }

    void monoAndStereoPacketsDecodeAtEachSupportedBoundary() {
        squad::VoiceMixer monoSource, stereoSource(2), monoReceiver, stereoReceiver(2);
        const auto monoPacket = monoSource.encode(tone(0.3), 48000).first();
        const auto stereoPacket = stereoSource.encode(stereoTone(0.3, 0.2), 48000).first();
        QVERIFY(stereoReceiver.receive("mono", monoPacket, 0));
        QVERIFY(monoReceiver.receive("stereo", stereoPacket, 0));
        const auto stereo = stereoReceiver.render(48000, 0);
        const auto mono = monoReceiver.render(48000, 0);
        QCOMPARE(stereo.size(), size_t(1920));
        QCOMPARE(mono.size(), size_t(960));
        for (size_t i = 0; i < 960; ++i) QVERIFY(std::abs(stereo[i * 2] - stereo[i * 2 + 1]) < 0.02f);
        QVERIFY(std::all_of(stereo.begin(), stereo.end(), [](float sample) { return std::isfinite(sample); }));
        QVERIFY(std::all_of(mono.begin(), mono.end(), [](float sample) { return std::isfinite(sample); }));
    }

    void sourcePreservesFullbandAudioAt64Kilobits() {
        squad::VoiceMixer source;
        int error = 0;
        const std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder(
            opus_decoder_create(48000, 1, &error), opus_decoder_destroy);
        QVERIFY(decoder && error == OPUS_OK);
        qint64 bytes = 0;
        std::array<float, 960> input{}, output{};
        for (int frame = 0; frame < 100; ++frame) {
            for (size_t i = 0; i < input.size(); ++i) {
                const auto time = double(frame * 960 + i) / 48000;
                input[i] = 0;
                for (const int frequency : {173, 439, 997, 2039, 4001, 7907, 12101, 17011})
                    input[i] += float(0.06 * std::sin(2 * std::numbers::pi * frequency * time));
            }
            const auto packets = source.encode(input, 48000);
            QCOMPARE(packets.size(), 1);
            const auto& packet = packets.first(); bytes += packet.size();
            QCOMPARE(opus_packet_get_nb_samples(reinterpret_cast<const unsigned char*>(packet.constData()), int(packet.size()), 48000), 960);
            QCOMPARE(opus_decode_float(decoder.get(), reinterpret_cast<const unsigned char*>(packet.constData()), int(packet.size()), output.data(), 960, 0), 960);
            QVERIFY(std::all_of(output.begin(), output.end(), [](float sample) { return std::isfinite(sample) && std::abs(sample) <= 1; }));
            QCOMPARE(opus_packet_get_bandwidth(reinterpret_cast<const unsigned char*>(packet.constData())), OPUS_BANDWIDTH_FULLBAND);
        }
        // VBR may spend more on complex frames. Measure two seconds, not one packet.
        const auto bitrate = bytes * 8 / 2;
        QVERIFY2(bitrate > 48000 && bitrate < 90000, qPrintable(QString::number(bitrate)));
    }

    void peerReplacementUsesNormalMixingAndNeverChangesOtherPeers() {
        squad::VoiceMixer encoder, original, translated;
        original.setAutomatic(false); translated.setAutomatic(false);
        const auto packet = encoder.encode(tone(0.1), 48000).first();
        for (auto* mixer : {&original, &translated}) {
            QVERIFY(mixer->receive("alice", packet, 0));
            QVERIFY(mixer->receive("bob", packet, 0));
            mixer->setGain("alice", 0.5); mixer->setGain("bob", 0.25);
        }
        std::vector<float> bob;
        auto normal = original.render(48000, 0, [&](const QString& id, std::span<float> frame, qint64) {
            if (id == "bob") bob.assign(frame.begin(), frame.end());
        });
        auto replaced = translated.render(48000, 0, [&](const QString& id, std::span<float> frame, qint64) {
            if (id == "alice") std::fill(frame.begin(), frame.end(), 0.1f);
        });
        for (size_t i = 0; i < replaced.size(); ++i) QVERIFY(std::abs(replaced[i] - (0.05f + bob[i] * 0.25f)) < 0.00001);
        QVERIFY(normal != replaced);
        auto tail = translated.render(48000, 20, [](const QString& id, std::span<float> frame, qint64) {
            if (id == "alice") std::fill(frame.begin(), frame.end(), 0.2f);
        });
        for (auto sample : tail) QVERIFY(std::abs(sample - 0.1f) < 0.00001);
        translated.remove("alice");
        auto departed = translated.render(48000, 40, [](const QString& id, std::span<float> frame, qint64) {
            if (id == "alice") std::fill(frame.begin(), frame.end(), 0.2f);
        });
        QCOMPARE(rms(departed), 0.0);
    }
    void avatarActivityFollowsAdmittedAndPlayedAudioWithoutChangingPackets() {
        squad::VoiceMixer source, reference, receiver;
        const auto signal = tone(0.2);
        QCOMPARE(source.transmitLevel(), 0.0);
        QVERIFY(source.encode(signal, 48000, false).isEmpty()); QCOMPARE(source.transmitLevel(), 0.0);
        const auto packets = source.encode(signal, 48000, true);
        QCOMPARE(packets, reference.encode(signal, 48000));
        QVERIFY(std::abs(source.transmitLevel() - rms(signal)) < 0.00001);
        const auto heldLevel = source.transmitLevel();
        QVERIFY(source.encode(tone(0.2, 48000, 100), 48000).isEmpty());
        QCOMPARE(source.transmitLevel(), heldLevel);
        QCOMPARE(receiver.playbackLevel("peer", 100), 0.0);
        QVERIFY(receiver.receive("peer", packets.first(), 100));
        QCOMPARE(receiver.playbackLevel("peer", 100), 0.0);
        receiver.setGain("peer", 0);
        QCOMPARE(rms(receiver.render(48000, 100)), 0.0);
        QVERIFY(receiver.playbackLevel("peer", 100) > 0.01);
        QCOMPARE(receiver.playbackLevel("unknown", 100), 0.0);
        QCOMPARE(receiver.playbackLevel("peer", 99), 0.0);
        QVERIFY(receiver.playbackLevel("peer", 220) > 0.01);
        QCOMPARE(receiver.playbackLevel("peer", 221), 0.0);
        receiver.remove("peer"); QCOMPARE(receiver.playbackLevel("peer", 100), 0.0);
        source.resetCapture(); QCOMPARE(source.transmitLevel(), 0.0);
        source.setVoiceActivation({true, false, -20});
        QVERIFY(source.encode(tone(0.005), 48000).isEmpty()); QCOMPARE(source.transmitLevel(), 0.0);
    }

    void missingPacketsUseBoundedConcealmentAndRecover_data() {
        QTest::addColumn<int>("channels");
        QTest::newRow("mono") << 1;
        QTest::newRow("stereo") << 2;
    }
    void missingPacketsUseBoundedConcealmentAndRecover() {
        QFETCH(int, channels);
        squad::VoiceMixer source(channels), receiver(channels);
        receiver.setAutomatic(false);
        const auto signal = channels == 1 ? tone(0.2, 48000, 20 * 960) : stereoTone(0.2, 0.15, 20 * 960);
        const auto packets = source.encode(signal, 48000);
        QCOMPARE(packets.size(), 20);
        for (int n = 0; n < 5; ++n) {
            QVERIFY(receiver.receive("voice", packets[n], n * 20));
            QVERIFY(rms(receiver.render(48000, n * 20)) > 0.01);
        }
        QVERIFY(!receiver.receive("voice", packets[7], 140, -1));
        QVERIFY(!receiver.receive("voice", packets[7], 140, 7));
        QVERIFY(receiver.receive("voice", packets[7], 140, 2));
        for (int n = 0; n < 3; ++n) {
            const auto output = receiver.render(48000, 140 + n * 20);
            QVERIFY(std::all_of(output.begin(), output.end(), [](float sample) { return std::isfinite(sample) && std::abs(sample) <= 1; }));
            QVERIFY(rms(output) > 0.001);
        }
        QVERIFY(receiver.receive("voice", packets[14], 280, 6));
        for (int n = 0; n < 7; ++n) (void)receiver.render(48000, 280 + n * 20);
        QCOMPARE(rms(receiver.render(48000, 420)), 0.0);
        QVERIFY(receiver.receive("voice", packets[15], 440));
        QVERIFY(rms(receiver.render(48000, 440)) > 0.01);
    }

    void mixingTwentyStreamsBenchmark() {
        squad::VoiceMixer source, receiver;
        const auto packet = source.encode(tone(0.03), 48000).first();
        QStringList peers;
        for (int i = 0; i < 20; ++i) peers.append(QString::number(i));
        qint64 now = 0;
        std::vector<float> output;
        QBENCHMARK {
            for (const auto& peer : peers) (void)receiver.receive(peer, packet, now);
            output = receiver.render(48000, now);
            now += 20;
        }
        QCOMPARE(output.size(), size_t(960));
    }

    void capturePreviewBenchmark_data() {
        QTest::addColumn<bool>("measure");
        QTest::newRow("without-preview") << false;
        QTest::newRow("with-preview") << true;
    }
    void capturePreviewBenchmark() {
        QFETCH(bool, measure);
        squad::VoiceMixer source(2);
        source.setNoiseSuppression(1);
        squad::AudioProcessor processor(48000, {0, 80, 12000});
        const auto input = stereoTone(0.2, 0.1);
        const squad::VoiceMixer::CaptureFilter filter = [&](std::span<float> frame) {
            for (size_t i = 0; i < frame.size(); i += 2) {
                const auto value = processor.filter({frame[i], frame[i + 1]});
                frame[i] = value[0]; frame[i + 1] = value[1];
            }
        };
        squad::VoiceMixer::CaptureObserver observer;
        if (measure) observer = [&](std::span<const float> raw, std::span<const float> filtered) {
            for (size_t i = 0; i < raw.size(); i += 2)
                processor.observeInput({raw[i], raw[i + 1]}).observeOutput({filtered[i], filtered[i + 1]});
        };
        QList<QByteArray> packets;
        QBENCHMARK { packets = source.encode(input, 48000, true, filter, observer); }
        QCOMPARE(packets.size(), 1);
    }

    void diagnosticPreviewCannotReplayAudioAfterTransmissionResumes() {
        squad::VoiceMixer preview, fresh;
        preview.setVoiceActivation({true, false, -30});
        fresh.setVoiceActivation({true, false, -30});
        const auto quiet = tone(0.005, 48000, 960, 770), speech = tone(0.3);
        QVERIFY(preview.encode(speech, 48000, false).isEmpty());
        QVERIFY(preview.voiceActive());
        for (int i = 0; i < 20; ++i) QVERIFY(preview.encode(quiet, 48000, false).isEmpty());
        QVERIFY(!preview.voiceActive());
        QCOMPARE(preview.encode(speech, 48000, true), fresh.encode(speech, 48000));
        // Also discard a partial diagnostic frame and reset denoiser/codec state.
        preview.setNoiseSuppression(0.5);
        fresh.setNoiseSuppression(0.5);
        QVERIFY(preview.encode(tone(0.2, 48000, 480), 48000, false).isEmpty());
        QCOMPARE(preview.encode(speech, 48000, true), fresh.encode(speech, 48000));
    }
    void activationAnalysisPreservesFilteredAudio_data() {
        QTest::addColumn<int>("channels");
        QTest::addColumn<double>("suppression");
        for (const int channels : {1, 2}) for (const double suppression : {0.0, 0.5})
            QTest::newRow(qPrintable(QString("%1ch-%2").arg(channels).arg(suppression))) << channels << suppression;
    }
    void activationAnalysisPreservesFilteredAudio() {
        QFETCH(int, channels); QFETCH(double, suppression);
        squad::VoiceMixer gated(channels), continuous(channels);
        gated.setNoiseSuppression(suppression).setVoiceActivation({true, true, -45});
        continuous.setNoiseSuppression(suppression);
        std::vector<float> expected, actual;
        for (int frame = 0; frame < 200; ++frame) {
            const auto input = speechFrame(frame, channels, frame % 50 < 25 ? 0.0001 : 0.3);
            (void)continuous.encode(input, 48000, true, {}, [&](auto, auto output) { expected.assign(output.begin(), output.end()); });
            (void)gated.encode(input, 48000, true, {}, [&](auto, auto output) { actual.assign(output.begin(), output.end()); });
            QVERIFY(actual == expected);
        }
    }

    void activationPreservesWordEdgesAndStopsAfterTheTail() {
        squad::VoiceMixer gated, reference;
        gated.setVoiceActivation({true, false, -30});
        const std::array<float, 960> silence{};
        const auto quiet = tone(0.005), speech = tone(0.3);
        for (int i = 0; i < 5; ++i) QVERIFY(gated.encode(silence, 48000).isEmpty());
        QVERIFY(gated.encode(quiet, 48000).isEmpty());
        QVERIFY(!gated.voiceActive());
        auto expected = reference.encode(silence, 48000);
        expected.append(reference.encode(quiet, 48000));
        expected.append(reference.encode(speech, 48000));
        QCOMPARE(gated.encode(speech, 48000), expected);
        QVERIFY(gated.voiceActive());
        for (int i = 0; i < 15; ++i) {
            QCOMPARE(gated.encode(quiet, 48000), reference.encode(quiet, 48000));
            QVERIFY(gated.voiceActive());
        }
        QVERIFY(gated.encode(quiet, 48000).isEmpty());
        QVERIFY(!gated.voiceActive());
        QVERIFY(gated.activationLevelDb() < gated.activationThresholdDb());
        QCOMPARE(gated.activationThresholdDb(), -30);
    }
    void activationResetAndModeChangeDiscardBufferedAudio() {
        squad::VoiceMixer gated, fresh;
        gated.setVoiceActivation({true, false, -30});
        fresh.setVoiceActivation({true, false, -30});
        const auto quiet = tone(0.005), speech = tone(0.3);
        QVERIFY(gated.encode(quiet, 48000).isEmpty());
        gated.resetCapture();
        QCOMPARE(gated.encode(speech, 48000), fresh.encode(speech, 48000));
        gated.resetCapture();
        QVERIFY(!gated.voiceActive());
        QVERIFY(gated.encode(quiet, 48000).isEmpty());
        gated.setVoiceActivation({false, false, -30});
        squad::VoiceMixer continuous;
        QCOMPARE(gated.encode(quiet, 48000), continuous.encode(quiet, 48000));
        QVERIFY(gated.voiceActive());
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, gated.setVoiceActivation({true, false, -81}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, gated.setVoiceActivation({true, false, 1}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, gated.setVoiceActivation({true, true, std::numeric_limits<double>::quiet_NaN()}));
    }
    void automaticThresholdLearnsBackgroundAndManualThresholdStaysFixed() {
        squad::VoiceMixer gated;
        gated.setVoiceActivation({true, true, -45});
        std::mt19937 random(193);
        for (int frame = 0; frame < 150; ++frame) {
            std::array<float, 960> background;
            for (auto& sample : background) sample = float((double(random()) / std::mt19937::max() - 0.5) * 0.002);
            const auto previous = gated.activationThresholdDb();
            QVERIFY(gated.encode(background, 48000).isEmpty());
            const auto change = gated.activationThresholdDb() - previous;
            QVERIFY(change >= -0.120001 && change <= 0.020001);
        }
        QVERIFY(gated.activationThresholdDb() < -50 && gated.activationThresholdDb() > -60);
        const auto learned = gated.activationThresholdDb();
        gated.resetCapture();
        QCOMPARE(gated.activationThresholdDb(), learned);
        gated.setVoiceActivation({true, false, -38});
        for (int i = 0; i < 100; ++i) {
            (void)gated.encode(tone(i % 2 ? 0.001 : 0.3), 48000);
            QCOMPARE(gated.activationThresholdDb(), -38);
        }
        gated.setVoiceActivation({true, true, -38});
        for (int i = 0; i < 100; ++i) (void)gated.encode(std::array<float, 960>{}, 48000);
        QVERIFY(gated.activationThresholdDb() < -45);
    }
    void automaticMeterShowsTheLevelActuallyComparedWithTheThreshold() {
        squad::VoiceMixer mixer;
        mixer.setVoiceActivation({true, true, -45}).setNoiseSuppression(1);
        std::mt19937 random(193);
        double shown = 0;
        const auto observe = [&](std::span<const float>, std::span<const float> output) {
            const auto power = std::inner_product(output.begin(), output.end(), output.begin(), 0.0) / output.size();
            shown = power > 0 ? std::max(-96.0, 10 * std::log10(power)) : -96;
        };
        for (int i = 0; i < 100; ++i) {
            std::array<float, 960> noise;
            for (auto& sample : noise) sample = float((double(random()) / std::mt19937::max() - 0.5) * 0.1);
            (void)mixer.encode(noise, 48000, true, {}, observe);
            QVERIFY(std::abs(mixer.activationLevelDb() - shown) < 0.001);
        }
    }
    void automaticMinimumBoundsSensitivityWithoutChangingPttOrManualAudio_data() {
        QTest::addColumn<int>("channels");
        QTest::newRow("mono") << 1;
        QTest::newRow("stereo") << 2;
    }
    void automaticMinimumBoundsSensitivityWithoutChangingPttOrManualAudio() {
        QFETCH(int, channels);
        squad::VoiceMixer limited(channels), normal(channels);
        limited.setVoiceActivation({true, true, -60, true, -35});
        normal.setVoiceActivation({true, true, -60});
        QCOMPARE(limited.activationThresholdDb(), -35);
        const std::vector<float> silence(size_t(960 * channels), 0);
        for (int i = 0; i < 300; ++i) {
            QVERIFY(limited.encode(silence, 48000).isEmpty());
            QVERIFY(normal.encode(silence, 48000).isEmpty());
        }
        QCOMPARE(limited.activationThresholdDb(), -35);
        QCOMPARE(normal.activationThresholdDb(), -70);
        int normalPackets = 0, limitedPackets = 0;
        for (int i = 0; i < 100; ++i) {
            const auto quiet = speechFrame(i, channels, 0.005);
            QVERIFY(limited.encode(quiet, 48000).isEmpty());
            normalPackets += normal.encode(quiet, 48000).size();
        }
        QVERIFY(normalPackets > 0);
        for (int i = 0; i < 100; ++i) {
            const auto loud = speechFrame(i, channels, 0.3);
            limitedPackets += limited.encode(loud, 48000).size();
        }
        QVERIFY(limitedPackets > 0);
        QVERIFY(limited.encode(silence, 48000, false).isEmpty());
        std::mt19937 random(193);
        for (int i = 0; i < 150; ++i) {
            std::vector<float> noise(size_t(960 * channels));
            for (auto& sample : noise) sample = float((double(random()) / std::mt19937::max() - 0.5) * 0.1);
            (void)limited.encode(noise, 48000);
        }
        QVERIFY2(limited.activationThresholdDb() > -35, "Auto can still raise the threshold above the minimum");
        limited.setVoiceActivation({true, true, -60, false, -35});
        for (int i = 0; i < 100; ++i) (void)limited.encode(silence, 48000);
        QCOMPARE(limited.activationThresholdDb(), -70);
        for (const auto [ptt, automatic] : {std::pair{false, false}, std::pair{true, false}, std::pair{true, true}}) {
            limited.setVoiceActivation({!ptt, automatic, -60, true, 0});
            normal.setVoiceActivation({!ptt, automatic, -60});
            if (!automatic) QCOMPARE(limited.activationThresholdDb(), -60);
            for (int i = 0; i < 25; ++i) {
                const auto quiet = channels == 1 ? tone(0.005) : stereoTone(0.005, 0.004);
                QCOMPARE(limited.encode(quiet, 48000), normal.encode(quiet, 48000));
            }
        }
        for (double invalid : {-81.0, 1.0, std::numeric_limits<double>::quiet_NaN()})
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, limited.setVoiceActivation({true, true, -45, true, invalid}));
    }

    void automaticThresholdDoesNotApplyUnselectedNoiseSuppression_data() {
        QTest::addColumn<int>("channels");
        QTest::newRow("mono") << 1;
        QTest::newRow("stereo") << 2;
    }
    void automaticThresholdDoesNotApplyUnselectedNoiseSuppression() {
        QFETCH(int, channels);
        squad::VoiceMixer gated(channels), continuous(channels);
        gated.setVoiceActivation({true, true, -45});
        std::deque<std::vector<float>> history;
        int sent = 0;
        for (int frame = 0; frame < 100; ++frame) {
            const auto speech = speechFrame(frame, channels, 0.3);
            history.push_back(speech);
            if (history.size() > 4) history.pop_front();
            const auto packets = gated.encode(speech, 48000);
            QVERIFY(size_t(packets.size()) <= history.size());
            for (qsizetype p = 0; p < packets.size(); ++p)
                QCOMPARE(packets[p], continuous.encode(history[history.size() - packets.size() + p], 48000).first());
            sent += packets.size();
        }
        QVERIFY(sent > 0);
        // An unchanged configuration must not discard an unfinished frame.
        const auto half = channels == 1 ? tone(0.3, 48000, 480) : stereoTone(0.3, 0.2, 480);
        QVERIFY(gated.encode(half, 48000).isEmpty());
        gated.setVoiceActivation({true, true, -45});
        // Compare a fresh equivalent stream instead of assuming a tone is speech.
        squad::VoiceMixer unchanged(channels);
        unchanged.setVoiceActivation({true, true, -45});
        for (int frame = 0; frame < 100; ++frame) {
            const auto speech = speechFrame(frame, channels, 0.3);
            (void)unchanged.encode(speech, 48000);
        }
        QVERIFY(unchanged.encode(half, 48000).isEmpty());
        QCOMPARE(gated.encode(half, 48000), unchanged.encode(half, 48000));
    }
    void suppressionBlendKeepsTheSignalAligned_data() {
        QTest::addColumn<int>("channels");
        QTest::newRow("mono") << 1;
        QTest::newRow("stereo") << 2;
    }
    void suppressionBlendKeepsTheSignalAligned() {
        QFETCH(int, channels);
        std::array<squad::VoiceMixer, 3> senders = {squad::VoiceMixer(channels), squad::VoiceMixer(channels), squad::VoiceMixer(channels)};
        std::array<squad::VoiceMixer, 3> receivers = {squad::VoiceMixer(channels), squad::VoiceMixer(channels), squad::VoiceMixer(channels)};
        std::array<std::vector<float>, 3> decoded;
        for (size_t mode = 0; mode < senders.size(); ++mode) {
            senders[mode].setNoiseSuppression(mode * 0.25);
            receivers[mode].setAutomatic(false);
        }
        // Changing pitch and amplitude avoids an ambiguous lag from a periodic tone.
        for (int frame = 0; frame < 150; ++frame) {
            std::vector<float> input(size_t(960 * channels));
            for (int channel = 0; channel < channels; ++channel) for (size_t i = 0; i < 960; ++i) {
                const double time = (frame * 960 + i) / 48000.0;
                const auto phase = 2 * std::numbers::pi * (180 * time + 40 * time * time + 8 * std::sin(3 * time));
                const auto channelPhase = phase + channel * 0.37;
                input[i * size_t(channels) + size_t(channel)] = float((0.2 * std::sin(channelPhase) + 0.08 * std::sin(2 * channelPhase) + 0.04 * std::sin(3 * channelPhase))
                    * (0.6 + 0.4 * std::cos(13 * time)));
            }
            for (size_t mode = 0; mode < senders.size(); ++mode) {
                QVERIFY(receivers[mode].receive("speaker", senders[mode].encode(input, 48000).first(), frame * 20));
                const auto samples = receivers[mode].render(48000, frame * 20);
                decoded[mode].insert(decoded[mode].end(), samples.begin(), samples.end());
            }
        }
        double blendEnergy = 0, errorEnergy = 0;
        const auto offset = size_t(squad::VoiceMixer::suppressionDelaySamples() * channels);
        for (size_t i = size_t(48000 * channels); i < size_t(120000 * channels); ++i) {
            const double dry = decoded[0][i], wet = decoded[2][i + offset];
            const auto expectedBlend = (dry + wet) * 0.5;
            const auto error = decoded[1][i + offset] - expectedBlend;
            blendEnergy += expectedBlend * expectedBlend; errorEnergy += error * error;
        }
        // Includes the real lossy codec; this pins the filter's declared delay
        // and catches a dry blend shifted by either half or all of an Opus frame.
        QVERIFY(std::sqrt(errorEnergy / blendEnergy) < 0.08);
    }
    void suppressionReducesNoiseThroughTheCodecAndStrengthIsAdjustable() {
        squad::VoiceMixer original, mild, strong, originalReceiver, mildReceiver, strongReceiver;
        originalReceiver.setAutomatic(false); mildReceiver.setAutomatic(false); strongReceiver.setAutomatic(false);
        mild.setNoiseSuppression(0.5); strong.setNoiseSuppression(1);
        std::mt19937 random(4281);
        double unprocessed = 0, partlyProcessed = 0, processed = 0;
        for (int frame = 0; frame < 250; ++frame) {
            std::array<float, 960> noise;
            for (auto& sample : noise) sample = (double(random()) / std::mt19937::max() - 0.5) * 0.1;
            QVERIFY(originalReceiver.receive("speaker", original.encode(noise, 48000).first(), frame * 20));
            QVERIFY(mildReceiver.receive("speaker", mild.encode(noise, 48000).first(), frame * 20));
            QVERIFY(strongReceiver.receive("speaker", strong.encode(noise, 48000).first(), frame * 20));
            const auto a = originalReceiver.render(48000, frame * 20);
            const auto b = mildReceiver.render(48000, frame * 20);
            const auto c = strongReceiver.render(48000, frame * 20);
            if (frame >= 150) { unprocessed += rms(a); partlyProcessed += rms(b); processed += rms(c); }
        }
        QVERIFY2(processed < unprocessed * 0.5, qPrintable(QString("off=%1 full=%2").arg(unprocessed).arg(processed)));
        QVERIFY(partlyProcessed < unprocessed * 0.8);
        QVERIFY(partlyProcessed > processed * 1.5);
    }
    void suppressionAcceptsDeviceRatesAndIrregularChunks_data() {
        QTest::addColumn<int>("rate");
        for (const auto rate : {8000, 16000, 44100, 48000, 96000, 192000})
            QTest::newRow(qPrintable(QString::number(rate))) << rate;
    }
    void suppressionAcceptsDeviceRatesAndIrregularChunks() {
        QFETCH(int, rate);
        squad::VoiceMixer complete, fragmented, receiver;
        complete.setNoiseSuppression(0.6); fragmented.setNoiseSuppression(0.6);
        complete.setVoiceActivation({true, true, -45}); fragmented.setVoiceActivation({true, true, -45});
        receiver.setAutomatic(false);
        const auto input = tone(0.3, rate, rate / 2);
        const auto expected = complete.encode(input, rate);
        QList<QByteArray> actual;
        for (size_t start = 0; start < input.size(); start += 127)
            actual.append(fragmented.encode(std::span(input).subspan(start, std::min(size_t(127), input.size() - start)), rate));
        QCOMPARE(actual, expected);
        QVERIFY(actual.size() >= 24 && actual.size() <= 25);
        for (int i = 0; i < actual.size(); ++i) {
            QVERIFY(receiver.receive("speaker", actual[i], i * 20));
            for (const auto sample : receiver.render(rate, i * 20)) QVERIFY(std::isfinite(sample) && std::abs(sample) <= 1);
        }
    }
    void suppressionResetBypassAndIndependentStreams() {
        squad::VoiceMixer first, second, clean;
        const auto half = tone(0.2, 48000, 480);
        QVERIFY(first.encode(half, 48000).isEmpty());
        first.setNoiseSuppression(0.7);
        QVERIFY(first.encode(half, 48000).isEmpty());
        first.resetCapture();
        const auto input = tone(0.25, 48000, 960 * 5);
        const auto expected = second.setNoiseSuppression(0.7).encode(input, 48000);
        QCOMPARE(first.encode(input, 48000), expected);
        first.resetCapture();
        QCOMPARE(first.encode(input, 48000), expected);
        first.setNoiseSuppression(0);
        QCOMPARE(first.encode(input, 48000), clean.encode(input, 48000));
        first.setNoiseSuppression(0.7);
        for (int frame = 0; frame < 100; ++frame) (void)first.encode(speechFrame(frame, 1, 0.3), 48000);
        first.resetCapture();
        second.resetCapture();
        QCOMPARE(first.encode(std::array<float, 960>{}, 48000), second.encode(std::array<float, 960>{}, 48000));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, first.setNoiseSuppression(-0.1));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, first.setNoiseSuppression(1.1));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, first.setNoiseSuppression(std::numeric_limits<double>::quiet_NaN()));
        // Repeating a setting must not cut a partially buffered frame.
        first.resetCapture(); second.resetCapture();
        QVERIFY(first.encode(half, 48000).isEmpty());
        first.setNoiseSuppression(0.7);
        const auto repeated = first.encode(half, 48000);
        std::vector<float> doubled(half); doubled.insert(doubled.end(), half.begin(), half.end());
        QCOMPARE(repeated, second.encode(doubled, 48000));
    }
    void suppressionAdjustmentPreservesBufferedSpeech() {
        squad::VoiceMixer adjusted, reference;
        adjusted.setNoiseSuppression(0.5); reference.setNoiseSuppression(0.75);
        for (int i = 0; i < 30; ++i) {
            const auto frame = speechFrame(i, 1, 0.3);
            QCOMPARE(adjusted.encode(frame, 48000).size(), 1);
            QCOMPARE(reference.encode(frame, 48000).size(), 1);
        }
        const auto frame = speechFrame(30, 1, 0.3);
        QVERIFY(adjusted.encode(std::span(frame).first(480), 48000).isEmpty());
        std::vector<float> expected, actual;
        QCOMPARE(reference.encode(frame, 48000, true, {}, [&](auto, auto filtered) {
            expected.assign(filtered.begin(), filtered.end());
        }).size(), 1);
        adjusted.setNoiseSuppression(0.75);
        QCOMPARE(adjusted.encode(std::span(frame).subspan(480), 48000, true, {}, [&](auto, auto filtered) {
            actual.assign(filtered.begin(), filtered.end());
        }).size(), 1);
        QCOMPARE(actual, expected);
    }
    void suppressionKeepsStereoStreamsIndependent() {
        squad::VoiceMixer stereo(2), left, right;
        for (auto* mixer : {&stereo, &left, &right}) mixer->setNoiseSuppression(0.5);
        for (int frame = 0; frame < 160; ++frame) {
            if (frame == 80) for (auto* mixer : {&stereo, &left, &right}) mixer->resetCapture();
            const auto a = speechFrame(frame, 1, 0.45);
            auto b = speechFrame(frame + 45, 1, 0.03);
            if (frame < 40 || frame >= 120) std::fill(b.begin(), b.end(), 0);
            std::vector<float> input(1920), stereoOutput, leftOutput, rightOutput;
            for (size_t i = 0; i < a.size(); ++i) { input[i * 2] = a[i]; input[i * 2 + 1] = b[i]; }
            QCOMPARE(stereo.encode(input, 48000, true, {}, [&](auto, auto output) {
                stereoOutput.assign(output.begin(), output.end());
            }).size(), 1);
            QCOMPARE(left.encode(a, 48000, true, {}, [&](auto, auto output) {
                leftOutput.assign(output.begin(), output.end());
            }).size(), 1);
            QCOMPARE(right.encode(b, 48000, true, {}, [&](auto, auto output) {
                rightOutput.assign(output.begin(), output.end());
            }).size(), 1);
            for (size_t i = 0; i < a.size(); ++i) {
                // Independent capture models must not share recurrent tensors.
                // The established stereo speech-protection envelope is shared.
#ifdef SQUADSPEAK_DENOISE_CANDIDATE
                QVERIFY(std::abs(stereoOutput[2 * i] - leftOutput[i]) < 1e-6f);
                QVERIFY(std::abs(stereoOutput[2 * i + 1] - rightOutput[i]) < 1e-6f);
#else
                QVERIFY(std::isfinite(stereoOutput[2 * i]) && std::isfinite(stereoOutput[2 * i + 1]));
#endif
            }
        }
    }
    void suppressionRunsIndependentConcurrentCaptures() {
        const auto capture = [](int offset) {
            squad::VoiceMixer mixer(2);
            mixer.setNoiseSuppression(0.5);
            QList<QByteArray> packets;
            for (int frame = 0; frame < 80; ++frame) {
                if (frame == 40) mixer.resetCapture();
                packets.append(mixer.encode(speechFrame(frame + offset, 2, 0.3), 48000));
            }
            return packets;
        };
        const auto expectedA = capture(0), expectedB = capture(30);
        std::barrier start(3);
        auto a = std::async(std::launch::async, [&] { start.arrive_and_wait(); return capture(0); });
        auto b = std::async(std::launch::async, [&] { start.arrive_and_wait(); return capture(30); });
        start.arrive_and_wait();
        QCOMPARE(a.get(), expectedA); QCOMPARE(b.get(), expectedB);
    }
    void previewCanAttachToAnAlreadyRunningCapture() {
        squad::VoiceMixer observed, late;
        observed.setNoiseSuppression(0.5); late.setNoiseSuppression(0.5);
        std::vector<float> expected, actual;
        const auto observe = [&](auto raw, auto) { expected.assign(raw.begin(), raw.end()); };
        for (int frame = 0; frame < 20; ++frame) {
            const auto input = speechFrame(frame, 1, 0.3);
            QCOMPARE(observed.encode(input, 48000, true, {}, observe), late.encode(input, 48000));
        }
        const auto input = speechFrame(20, 1, 0.3);
        QCOMPARE(observed.encode(input, 48000, true, {}, observe), late.encode(input, 48000, true, {},
            [&](auto raw, auto) { actual.assign(raw.begin(), raw.end()); }));
        QVERIFY(actual == expected);
    }
    void captureProcessingBudget_data() {
        QTest::addColumn<int>("channels");
        QTest::newRow("mono") << 1;
        QTest::newRow("stereo") << 2;
    }
    void captureProcessingBudget() {
        QFETCH(int, channels);
        squad::VoiceMixer sender(channels);
        sender.setNoiseSuppression(0.5).setVoiceActivation({true, true, -45});
        std::vector<qint64> durations;
        durations.reserve(400);
        for (int frame = 0; frame < 400; ++frame) {
            const auto input = speechFrame(frame, channels, 0.3);
            QElapsedTimer timer; timer.start();
            (void)sender.encode(input, 48000);
            durations.push_back(timer.nsecsElapsed());
        }
        const auto total = std::accumulate(durations.begin(), durations.end(), qint64(0));
        std::sort(durations.begin(), durations.end());
        qInfo() << "capture ms, mean/p95/maximum" << double(total) / 400e6
                << durations[379] / 1e6 << durations.back() / 1e6;
        QVERIFY2(total < 8000000000LL, "Capture processing cannot keep up with the supplied eight seconds of speech");
    }
#ifdef SQUADSPEAK_DENOISE_CANDIDATE
    void candidatePreservesCleanDetailsAcrossLevelChanges_data() {
        QTest::addColumn<int>("channels");
        QTest::newRow("mono") << 1;
        QTest::newRow("stereo") << 2;
    }
    void candidatePreservesCleanDetailsAcrossLevelChanges() {
        QFETCH(int, channels);
        squad::VoiceMixer sender(channels), receiver(channels);
        sender.setNoiseSuppression(0.5); receiver.setAutomatic(false);
        std::vector<float> original, filtered;
        const auto delay = size_t(squad::VoiceMixer::suppressionDelaySamples() * channels);
        for (int frame = 0; frame < 405; ++frame) {
            // Abrupt near/far changes and silence happen without resetting capture.
            const double amplitude = frame >= 400 ? 0 : frame < 80 ? 0.03 : frame < 160 ? 0.6 : frame < 240 ? 0.01 : frame < 300 ? 0 : 0.3;
            const auto input = speechFrame(frame, channels, amplitude);
            original.insert(original.end(), input.begin(), input.end());
            const auto packets = sender.encode(input, 48000, true, {}, [&](auto, auto output) {
                filtered.insert(filtered.end(), output.begin(), output.end());
            });
            QCOMPARE(packets.size(), 1);
            QVERIFY(receiver.receive("speaker", packets.first(), frame * 20));
            for (const auto value : receiver.render(48000, frame * 20)) QVERIFY(std::isfinite(value) && std::abs(value) <= 0.95001);
        }
        double referenceEnergy = 0, resultEnergy = 0, dot = 0;
        for (size_t i = 0; i + delay < original.size(); ++i) {
            referenceEnergy += double(original[i]) * original[i];
            resultEnergy += double(filtered[i + delay]) * filtered[i + delay];
            dot += double(original[i]) * filtered[i + delay];
        }
        QVERIFY(dot / std::sqrt(referenceEnergy * resultEnergy) > 0.97);
        for (const auto range : {std::pair{0, 80}, {80, 160}, {160, 240}, {300, 400}}) {
            double before = 0, after = 0;
            for (size_t i = size_t(range.first * 960 * channels); i < size_t(range.second * 960 * channels); ++i) {
                before += double(original[i]) * original[i];
                after += double(filtered[i + delay]) * filtered[i + delay];
            }
            const auto change = 10 * std::log10(after / before);
            QVERIFY2(std::abs(change) < 3, qPrintable(QString("Clean level segment %1 changed by %2 dB").arg(range.first).arg(change)));
        }
        double weakest = 0;
        const auto hop = size_t(480 * channels);
        for (size_t i = 0; i + delay + hop <= original.size(); i += hop) {
            double before = 0, after = 0;
            for (size_t j = 0; j < hop; ++j) {
                before += double(original[i + j]) * original[i + j];
                after += double(filtered[i + j + delay]) * filtered[i + j + delay];
            }
            if (before / hop > 1e-8) weakest = std::min(weakest, 10 * std::log10(std::max(after, 1e-30) / before));
        }
        QVERIFY2(weakest > -6.5, qPrintable(QString("Clean detail lost %1 dB during level change").arg(-weakest)));
    }
#endif
    void codecStreamingAndDeviceRates_data() {
        QTest::addColumn<int>("rate");
        QTest::newRow("narrow-device") << 16000;
        QTest::newRow("cd-device") << 44100;
        QTest::newRow("voice-device") << 48000;
        QTest::newRow("high-rate-device") << 96000;
    }
    void codecStreamingAndDeviceRates() {
        QFETCH(int, rate);
        squad::VoiceMixer sender, receiver;
        receiver.setAutomatic(false);
        int packets = 0;
        double energy = 0;
        for (int i = 0; i < 60; ++i) {
            for (const auto& packet : sender.encode(tone(0.2, rate, rate / 50), rate)) {
                QVERIFY(receiver.receive("speaker", packet, i * 20));
                const auto audio = receiver.render(rate, i * 20);
                if (!audio.empty()) energy += rms(audio);
                ++packets;
            }
        }
        QVERIFY(packets >= 58 && packets <= 60);
        QVERIFY(energy / packets > 0.10 && energy / packets < 0.18);
        QVERIFY(rms(receiver.render(rate, 2000)) < 0.02);
    }

    void resamplingChunkBoundariesRemainStable_data() {
        QTest::addColumn<int>("rate");
        for (const auto rate : {16000, 44100, 96000, 192000})
            QTest::newRow(qPrintable(QString::number(rate))) << rate;
    }

    void resamplingChunkBoundariesRemainStable() {
        QFETCH(int, rate);
        const auto input = tone(0.17, rate, rate / 2, 713);
        squad::VoiceMixer complete, fragmented;
        const auto expected = complete.encode(input, rate);
        QList<QByteArray> actual;
        constexpr std::array<size_t, 5> chunks{1, 127, 511, 193, 73};
        size_t offset = 0;
        size_t chunk = 0;
        while (offset < input.size()) {
            const auto count = std::min(chunks[chunk % chunks.size()], input.size() - offset);
            actual.append(fragmented.encode(std::span(input).subspan(offset, count), rate));
            offset += count;
            ++chunk;
        }
        QCOMPARE(actual, expected);
    }

    void perPersonEqualizationPreservesManualGainAndSilence() {
        squad::VoiceMixer quietSender, loudSender, quietReceiver, loudReceiver;
        double quiet = 0, loud = 0;
        for (int i = 0; i < 240; ++i) {
            QVERIFY(quietReceiver.receive("quiet", quietSender.encode(tone(0.02), 48000).first(), i * 20));
            QVERIFY(loudReceiver.receive("loud", loudSender.encode(tone(0.5), 48000).first(), i * 20));
            quiet = rms(quietReceiver.render(48000, i * 20));
            loud = rms(loudReceiver.render(48000, i * 20));
        }
        QVERIFY2(std::abs(quiet - loud) < 0.015, qPrintable(QString("quiet=%1 loud=%2").arg(quiet).arg(loud)));
        quietReceiver.setGain("quiet", 0.5);
        QVERIFY(quietReceiver.receive("quiet", quietSender.encode(tone(0.02), 48000).first(), 4800));
        QVERIFY(std::abs(rms(quietReceiver.render(48000, 4800)) - quiet / 2) < 0.01);
        quietReceiver.setGain("quiet", 0);
        QVERIFY(quietReceiver.receive("quiet", quietSender.encode(tone(0.02), 48000).first(), 4820));
        QCOMPARE(rms(quietReceiver.render(48000, 4820)), 0.0);
        QCOMPARE(rms(loudReceiver.render(48000, 5000)), 0.0);
    }
    void equalizationFollowsTheSameSpeakerMovingNearAndFar() {
        squad::VoiceMixer sender, receiver, steadySender, steadyReceiver;
        int frame = 0;
        double previousSteady = 0;
        for (const double amplitude : {0.04, 0.5, 0.02, 0.25}) {
            double adjusted = 0, steady = 0;
            for (int settling = 0; settling < 180; ++settling, ++frame) {
                const auto now = frame * 20;
                QVERIFY(receiver.receive("moving", sender.encode(tone(amplitude), 48000).first(), now));
                QVERIFY(steadyReceiver.receive("steady", steadySender.encode(tone(0.1), 48000).first(), now));
                adjusted = rms(receiver.render(48000, now));
                steady = rms(steadyReceiver.render(48000, now));
                QVERIFY(std::isfinite(adjusted));
            }
            QVERIFY2(std::abs(adjusted - steady) < 0.015,
                qPrintable(QString("input=%1 moving=%2 steady=%3").arg(amplitude).arg(adjusted).arg(steady)));
            if (previousSteady > 0) QVERIFY(std::abs(steady - previousSteady) < 0.005);
            previousSteady = steady;
        }
    }
    void localMuteDoesNotStopLearningOrGetUndoneByNormalization() {
        squad::VoiceMixer sender, receiver;
        receiver.setGain("source", 0);
        for (int frame = 0; frame < 250; ++frame) {
            const auto packet = sender.encode(tone(0.04), 48000).first();
            QVERIFY(receiver.receive("source", packet, frame * 20));
            QCOMPARE(rms(receiver.render(48000, frame * 20)), 0.0);
        }
        receiver.setGain("source", 0.25);
        for (int frame = 250; frame < 350; ++frame) {
            QVERIFY(receiver.receive("source", sender.encode(tone(0.04), 48000).first(), frame * 20));
            const auto level = rms(receiver.render(48000, frame * 20));
            QVERIFY2(level > 0.02 && level < 0.03, qPrintable(QString::number(level)));
        }
        // A later rise at the source is corrected without erasing local gain.
        for (int frame = 350; frame < 450; ++frame) {
            QVERIFY(receiver.receive("source", sender.encode(tone(0.4), 48000).first(), frame * 20));
            const auto output = receiver.render(48000, frame * 20);
            for (const auto sample : output) QVERIFY(std::isfinite(sample) && std::abs(sample) <= 0.951);
            if (frame > 380) {
                const auto level = rms(output);
                QVERIFY2(level > 0.02 && level < 0.03, qPrintable(QString::number(level)));
            }
        }
    }
    void automaticLevelChangesDoNotCreateFrameBoundaryClicks_data() {
        QTest::addColumn<int>("channels");
        QTest::newRow("mono") << 1;
        QTest::newRow("stereo") << 2;
    }
    void automaticLevelChangesDoNotCreateFrameBoundaryClicks() {
        QFETCH(int, channels);
        squad::VoiceMixer sender(channels), reference(channels), receiver(channels);
        reference.setAutomatic(false);
        double previousGain = 1;
        int boundaries = 0;
        for (int frame = 0; frame < 180; ++frame) {
            const double amplitude = frame < 80 || frame >= 130 ? 0.015 : 0.08;
            const auto input = channels == 1 ? tone(amplitude) : stereoTone(amplitude, amplitude * 0.7);
            const auto packet = sender.encode(input, 48000).first();
            QVERIFY(reference.receive("speaker", packet, frame * 20));
            QVERIFY(receiver.receive("speaker", packet, frame * 20));
            const auto raw = reference.render(48000, frame * 20);
            const auto adjusted = receiver.render(48000, frame * 20);
            if (frame > 0 && std::abs(raw.front()) > 0.0001) {
                const auto gain = adjusted.front() / raw.front();
                QVERIFY2(std::abs(gain - previousGain) < 0.005,
                    qPrintable(QString("Gain jumped from %1 to %2 at frame %3").arg(previousGain).arg(gain).arg(frame)));
                ++boundaries;
            }
            QVERIFY(std::abs(raw.back()) > 0.0001);
            previousGain = adjusted.back() / raw.back();
        }
        QVERIFY(boundaries > 150);
        receiver.setGain("speaker", 0);
        QVERIFY(receiver.receive("speaker", sender.encode(channels == 1 ? tone(0.08) : stereoTone(0.08, 0.08), 48000).first(), 3600));
        QCOMPARE(rms(receiver.render(48000, 3600)), 0.0);
    }
    void playbackJitterPreservesEveryDecodedFrame_data() {
        QTest::addColumn<int>("channels"); QTest::addColumn<bool>("jitter");
        QTest::addColumn<bool>("initialBatch");
        for (int channels : {1, 2}) for (bool jitter : {false, true}) for (bool batch : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(channels).arg(jitter).arg(batch))) << channels << jitter << batch;
    }
    void playbackJitterPreservesEveryDecodedFrame() {
        QFETCH(int, channels); QFETCH(bool, jitter); QFETCH(bool, initialBatch);
        squad::VoiceMixer sender(channels), reference(channels), receiver(channels, 40);
        reference.setAutomatic(false); receiver.setAutomatic(false);
        QList<QByteArray> packets;
        std::vector<std::vector<float>> expected;
        std::vector<int> arrivals;
        for (int frame = 0; frame < 400; ++frame) {
            const auto packet = sender.encode(speechFrame(frame, channels, 0.5), 48000).first();
            packets.append(packet);
            QVERIFY(reference.receive("speaker", packet, frame * 20));
            expected.push_back(reference.render(48000, frame * 20));
            const int late = jitter && frame % 23 == 10 ? 28 : 0;
            arrivals.push_back(initialBatch && frame < 6 ? 100
                : std::max(arrivals.empty() ? 0 : arrivals.back(), frame * 20 + late));
        }
        size_t delivered = 0;
        for (int now = 0; now < 8100; now += 10) {
            while (delivered < expected.size() && arrivals[delivered] <= now) {
                QVERIFY(receiver.receive("speaker", packets[qsizetype(delivered)], arrivals[delivered]));
                ++delivered;
            }
            if (now % 20 != 0) continue;
            const auto actual = receiver.render(48000, now);
            if (now < 100) QCOMPARE(rms(actual), 0.0);
            else QVERIFY2(actual == expected[size_t((now - 100) / 20)],
                qPrintable(QString("Decoded frame changed or disappeared at %1 ms").arg(now)));
        }
        QCOMPARE(delivered, expected.size());
    }

    void playbackSpeechPauseDoesNotIncreaseDelay_data() {
        QTest::addColumn<int>("channels"); QTest::addColumn<int>("pause");
        QTest::addColumn<int>("reportedMissing");
        for (int channels : {1, 2}) for (int pause : {200, 220, 260, 400}) for (int missing : {0, 6})
            QTest::newRow(qPrintable(QString("%1ch-%2ms-%3missing").arg(channels).arg(pause).arg(missing)))
                << channels << pause << missing;
    }
    void playbackSpeechPauseDoesNotIncreaseDelay() {
        QFETCH(int, channels); QFETCH(int, pause); QFETCH(int, reportedMissing);
        squad::VoiceMixer sender(channels), reference(channels), receiver(channels, 40);
        reference.setAutomatic(false); receiver.setAutomatic(false);
        std::vector<std::vector<float>> expected;
        std::vector<int> arrivals;
        size_t played = 0;
        for (int now = 0; now < 1800; now += 20) {
            const int first = now < 400 ? now / 20 : -1;
            const int second = now >= 380 + pause && now < 780 + pause ? (now - 380 - pause) / 20 + 20 : -1;
            const int frame = first >= 0 ? first : second;
            if (frame >= 0) {
                const auto packet = sender.encode(speechFrame(frame, channels, 0.5), 48000).first();
                QVERIFY(reference.receive("speaker", packet, now));
                expected.push_back(reference.render(48000, now));
                arrivals.push_back(now);
                // TS3 can advance packet numbers across a speech pause. A new
                // utterance must not prepend concealment for the silent gap.
                QVERIFY(receiver.receive("speaker", packet, now, frame == 20 ? reportedMissing : 0));
            }
            const auto actual = receiver.render(48000, now);
            if (rms(actual) == 0) continue;
            QVERIFY(played < expected.size());
            QVERIFY2(actual == expected[played],
                qPrintable(QString("Frame %1 changed after speech pause at %2 ms").arg(played).arg(now)));
            QVERIFY2(now - arrivals[played] <= 100, "A speech pause increased delay on a jitter-free connection");
            ++played;
        }
        QCOMPARE(played, size_t(40));
    }
    void playbackRecoversBufferAfterSpeechPause_data() {
        playbackAdaptsToBurstArrivalsWithoutDroppingSamples_data();
    }
    void playbackRecoversBufferAfterSpeechPause() {
        QFETCH(int, channels); QFETCH(int, arrivalGap); QFETCH(bool, renderJitter);
        squad::VoiceMixer sender(channels), reference(channels), receiver(channels, 40);
        reference.setAutomatic(false); receiver.setAutomatic(false);
        QList<QByteArray> packets;
        std::vector<std::vector<float>> expected;
        std::vector<int> arrivals;
        const int batchAt = 9 + arrivalGap / 20;
        for (int frame = 0; frame < 100; ++frame) {
            const auto packet = sender.encode(speechFrame(frame, channels, 0.5), 48000).first();
            packets.append(packet);
            const int generatedAt = frame < 20 ? frame * 20 : 600 + (frame - 20) * 20;
            QVERIFY(reference.receive("speaker", packet, generatedAt));
            expected.push_back(reference.render(48000, generatedAt));
            const int phase = (frame < 20 ? frame : frame - 20) % 26;
            arrivals.push_back(phase >= 10 && phase < batchAt
                ? generatedAt + (batchAt - phase) * 20 : generatedAt);
        }
        size_t delivered = 0, played = 0;
        // Keep the jitter budget learned before the speech pause. Refilling it
        // must absorb the same arrival pattern when speech resumes.
        for (int now = 0; now < 2400; now += 10) {
            while (delivered < packets.size() && arrivals[delivered] <= now) {
                QVERIFY(receiver.receive("speaker", packets[qsizetype(delivered)], now));
                ++delivered;
            }
            if (now % 40 != 0 && now % 40 != (renderJitter ? 10 : 20)) continue;
            const auto actual = receiver.render(48000, now);
            if (rms(actual) == 0) {
                QVERIFY2(played <= 20 || played == expected.size(),
                    qPrintable(QString("Speech interrupted after pause at %1 ms, frame %2").arg(now).arg(played)));
                continue;
            }
            QVERIFY(played < expected.size());
            QCOMPARE(actual, expected[played]);
            const auto generatedAt = played < 20 ? played * 20 : 600 + (played - 20) * 20;
            QVERIFY(now - qint64(generatedAt) <= 180);
            ++played;
        }
        QCOMPARE(played, expected.size());
    }
    void playbackAdaptsToBurstArrivalsWithoutDroppingSamples_data() {
        QTest::addColumn<int>("channels");
        QTest::addColumn<int>("arrivalGap");
        QTest::addColumn<bool>("renderJitter");
        for (int channels : {1, 2}) for (int gap : {100, 180}) for (bool jitter : {false, true})
            QTest::newRow(qPrintable(QString("%1ch-%2ms-%3").arg(channels).arg(gap).arg(jitter)))
                << channels << gap << jitter;
    }
    void playbackAdaptsToBurstArrivalsWithoutDroppingSamples() {
        QFETCH(int, channels); QFETCH(int, arrivalGap); QFETCH(bool, renderJitter);
        squad::VoiceMixer sender(channels), reference(channels), receiver(channels, 40);
        reference.setAutomatic(false); receiver.setAutomatic(false);
        const int batchAt = 9 + arrivalGap / 20;
        const int deadline = arrivalGap == 100 ? 120 : 180;
        QList<QByteArray> packets;
        std::vector<std::vector<float>> expected;
        std::vector<int> arrivals;
        for (int frame = 0; frame < 800; ++frame) {
            const auto packet = sender.encode(speechFrame(frame, channels, 0.5), 48000).first();
            packets.append(packet);
            QVERIFY(reference.receive("speaker", packet, frame * 20));
            expected.push_back(reference.render(48000, frame * 20));
            // TS3 captures contain recurrent 100-150 ms gaps. Include the
            // upper supported 160 ms jitter plus one nominal packet interval.
            const int phase = frame % 26;
            arrivals.push_back(frame < 100 && phase >= 10 && phase < batchAt
                ? (frame + batchAt - phase) * 20 : frame * 20);
        }
        size_t delivered = 0, played = 0;
        for (int now = 0; now < 16000 + deadline; now += 10) {
            while (delivered < packets.size() && arrivals[delivered] <= now) {
                QVERIFY(receiver.receive("speaker", packets[qsizetype(delivered)], now));
                ++delivered;
            }
            if (now % 40 != 0 && now % 40 != (renderJitter ? 10 : 20)) continue;
            const auto actual = receiver.render(48000, now);
            if (rms(actual) == 0) {
                // The first unexpected stall can exhaust the initial 100 ms.
                // Once measured, repeating it must not interrupt speech again.
                QVERIFY2(now < (arrivalGap == 100 ? 120 : 500) || played == expected.size(),
                    qPrintable(QString("Playback gap at %1 ms").arg(now)));
                continue;
            }
            QVERIFY(played < expected.size());
            QVERIFY2(actual == expected[played], qPrintable(QString("Frame %1 changed or disappeared at %2 ms").arg(played).arg(now)));
            QVERIFY(now - qint64(played) * 20 <= deadline);
            ++played;
        }
        QCOMPARE(played, expected.size());
        // Recover the low delay at an utterance boundary after stable reception.
        QVERIFY(receiver.receive("speaker", packets.first(), 17000));
        QCOMPARE(rms(receiver.render(48000, 17000)), 0.0);
        QCOMPARE(rms(receiver.render(48000, 17020)), 0.0);
        QVERIFY(reference.receive("speaker", packets.first(), 17000));
        const auto resumed = reference.render(48000, 17000);
        QVERIFY(rms(resumed) > 0);
        QCOMPARE(receiver.render(48000, 17040), resumed);
        // A recovered connection can become bursty again. Growing the delay
        // may pause playback briefly, but must preserve every decoded sample.
        packets.clear(); expected.clear(); arrivals.clear();
        for (int frame = 0; frame < 100; ++frame) {
            const auto packet = sender.encode(speechFrame(frame + 800, channels, 0.5), 48000).first();
            packets.append(packet);
            QVERIFY(reference.receive("speaker", packet, 18000 + frame * 20));
            expected.push_back(reference.render(48000, 18000 + frame * 20));
            const int phase = frame % 26;
            arrivals.push_back(18000 + (phase >= 10 && phase < batchAt ? frame + batchAt - phase : frame) * 20);
        }
        delivered = 0; played = 0;
        for (int now = 18000; now < 20040 + deadline; now += 10) {
            while (delivered < packets.size() && arrivals[delivered] <= now) {
                QVERIFY(receiver.receive("speaker", packets[qsizetype(delivered)], arrivals[delivered]));
                ++delivered;
            }
            if (now % 40 != 0 && now % 40 != (renderJitter ? 10 : 20)) continue;
            const auto actual = receiver.render(48000, now);
            if (rms(actual) == 0) {
                QVERIFY2(now < 18400 || played == expected.size(), qPrintable(QString("Recovered playback gap at %1 ms").arg(now)));
                continue;
            }
            QVERIFY(played < expected.size());
            QVERIFY2(actual == expected[played],
                qPrintable(QString("Recovered frame %1 changed or disappeared at %2 ms").arg(played).arg(now)));
            ++played;
        }
        QCOMPARE(played, expected.size());
    }

    void playbackBufferRecoversWithoutReplayingOldAudio() {
        squad::VoiceMixer sender, receiver(1, 40);
        receiver.setAutomatic(false);
        const auto packet = sender.encode(tone(0.3), 48000).first();
        for (const auto start : {0, 1000}) {
            QVERIFY(receiver.receive("speaker", packet, start));
            QCOMPARE(rms(receiver.render(48000, start)), 0.0);
            QCOMPARE(rms(receiver.render(48000, start + 20)), 0.0);
            QVERIFY(rms(receiver.render(48000, start + 100)) > 0.01);
            QCOMPARE(rms(receiver.render(48000, start + 120)), 0.0);
        }
        // A suspended renderer must not play stale frames or skip fresh prefill.
        QVERIFY(receiver.receive("speaker", packet, 2000));
        QVERIFY(receiver.receive("speaker", packet, 3000));
        QCOMPARE(rms(receiver.render(48000, 3000)), 0.0);
        QVERIFY(rms(receiver.render(48000, 3100)) > 0.01);
        QCOMPARE(rms(receiver.render(48000, 3120)), 0.0);
        QVERIFY(receiver.receive("speaker", packet, 4000));
        receiver.remove("speaker");
        QCOMPARE(rms(receiver.render(48000, 4100)), 0.0);
        QVERIFY(receiver.receive("speaker", packet, 5000));
        receiver.resetPlayback();
        QCOMPARE(rms(receiver.render(48000, 5100)), 0.0);
        QVERIFY(receiver.receive("speaker", packet, 6000));
        QCOMPARE(rms(receiver.render(48000, 6221)), 0.0);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, squad::VoiceMixer(1, -1));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, squad::VoiceMixer(1, 101));
    }

    void delayedBatchDoesNotRestartOngoingPlayback() {
        squad::VoiceMixer sender, reference, receiver(1, 40);
        reference.setAutomatic(false); receiver.setAutomatic(false);
        QList<QByteArray> packets;
        std::vector<std::vector<float>> expected;
        for (int frame = 0; frame < 30; ++frame) {
            const auto packet = sender.encode(speechFrame(frame, 1, 0.5), 48000).first();
            packets.append(packet);
            QVERIFY(reference.receive("speaker", packet, frame * 20));
            expected.push_back(reference.render(48000, frame * 20));
        }
        size_t delivered = 0, played = 0;
        for (int now = 0; now < 840; now += 20) {
            while (delivered < packets.size()) {
                const auto arrival = delivered >= 10 && delivered < 16 ? 320 : int(delivered) * 20;
                if (arrival > now) break;
                QVERIFY(receiver.receive("speaker", packets[qsizetype(delivered++)], now));
            }
            const auto actual = receiver.render(48000, now);
            if (rms(actual) == 0) {
                QVERIFY2(now < 100 || now == 300 || played == expected.size(),
                    qPrintable(QString("Late batch restarted prefill at %1 ms").arg(now)));
                continue;
            }
            QVERIFY(played < expected.size());
            QCOMPARE(actual, expected[played++]);
        }
        QCOMPARE(played, expected.size());
    }

    void playbackBufferKeepsPeersAndTranslationIndependent() {
        squad::VoiceMixer sender, receiver(1, 40);
        receiver.setAutomatic(false);
        const auto packet = sender.encode(tone(0.3), 48000).first();
        QVERIFY(receiver.receive("early", packet, 0));
        QVERIFY(receiver.receive("late", packet, 40));
        QHash<QString, double> levels;
        const auto observe = [&](const QString& id, std::span<float> frame, qint64) {
            levels.insert(id, rms(std::vector<float>(frame.begin(), frame.end())));
        };
        QVERIFY(rms(receiver.render(48000, 100, observe)) > 0.01);
        QVERIFY(levels.value("early") > 0.01);
        QCOMPARE(levels.value("late"), 0.0);
        QVERIFY(rms(receiver.render(48000, 140, observe)) > 0.01);
        QCOMPARE(levels.value("early"), 0.0);
        QVERIFY(levels.value("late") > 0.01);
    }

    void simultaneousSpeakersLimiterAndStaleFrames() {
        squad::VoiceMixer sender, receiver;
        receiver.setAutomatic(false);
        const auto packet = sender.encode(tone(0.9), 48000).first();
        for (int i = 0; i < 20; ++i) QVERIFY(receiver.receive(QString::number(i), packet, 100));
        const auto mixed = receiver.render(48000, 100);
        QVERIFY(rms(mixed) > 0.1);
        for (const auto sample : mixed) QVERIFY(std::isfinite(sample) && std::abs(sample) <= 0.951);
        QVERIFY(receiver.receive("stale", packet, 100));
        QCOMPARE(rms(receiver.render(48000, 221)), 0.0);
        for (int i = 0; i < 20; ++i) QVERIFY(receiver.receive("burst", packet, 300));
        for (int i = 0; i < 7; ++i) QVERIFY(rms(receiver.render(48000, 300)) > 0);
        QCOMPARE(rms(receiver.render(48000, 300)), 0.0);
        QVERIFY(receiver.receive("leaving", packet, 400));
        receiver.remove("leaving");
        QCOMPARE(rms(receiver.render(48000, 400)), 0.0);
    }
    void playbackResetPreservesLocalVolume() {
        squad::VoiceMixer sender, reset, continuous;
        for (auto* receiver : {&reset, &continuous}) receiver->setGain("radio", 0.2);
        for (int frame = 0; frame < 100; ++frame) {
            const auto packet = sender.encode(tone(0.3), 48000).first();
            if (frame == 40) reset.resetPlayback();
            QVERIFY(reset.receive("radio", packet, frame * 20));
            QVERIFY(continuous.receive("radio", packet, frame * 20));
            const auto actual = rms(reset.render(48000, frame * 20));
            const auto expected = rms(continuous.render(48000, frame * 20));
            if (frame > 80) QVERIFY(std::abs(actual - expected) < 0.002);
        }
        reset.setGain("silent", 0).resetPlayback();
        QVERIFY(reset.receive("silent", sender.encode(tone(0.3), 48000).first(), 2020));
        QCOMPARE(rms(reset.render(48000, 2020)), 0.0);
    }
    void resetDropsPartialCaptureAndOldPlayback() {
        squad::VoiceMixer sender, receiver;
        QVERIFY(sender.encode(tone(0.2, 48000, 480), 48000).isEmpty());
        sender.resetCapture();
        QVERIFY(sender.encode(tone(0.2, 48000, 480), 48000).isEmpty());
        const auto packets = sender.encode(tone(0.2, 48000, 480), 48000);
        QCOMPARE(packets.size(), 1);
        QVERIFY(receiver.receive("speaker", packets.first(), 0));
        receiver.resetPlayback();
        QCOMPARE(rms(receiver.render(48000, 0)), 0.0);
        QVERIFY(!receiver.receive("speaker", QByteArray(5000, 'x'), 0));
        QVERIFY(!receiver.receive("", packets.first(), 0));
        QVERIFY(!receiver.receive("speaker", QByteArray{}, 0));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, (void)sender.encode(tone(0.2), 100));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, sender.setGain("speaker", 3));
        auto bad = tone(0.2); bad[0] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, (void)sender.encode(bad, 48000));
    }
    void outputSettingsAreLocalAndDeviceBound() {
        QTemporaryDir dir;
        const auto path = dir.filePath("audio.ini");
        {
            squad::AudioProfiles profiles(path);
            profiles.saveParticipant("headset", "alice", 0.4).saveParticipant("speaker", "alice", 1.2)
                .saveAutomaticVolume("headset", false).saveOutput("headset", 0.25);
        }
        const squad::AudioProfiles profiles(path);
        QCOMPARE(profiles.participant("headset", "alice"), 0.4);
        QCOMPARE(profiles.participant("speaker", "alice"), 1.2);
        QCOMPARE(profiles.participant("headset", "bob"), 1.0);
        QCOMPARE(profiles.output("headset"), 0.25);
        QVERIFY(!profiles.automaticVolume("headset"));
        QVERIFY(profiles.automaticVolume("speaker"));
    }
};
QTEST_GUILESS_MAIN(MixerTests)
#include "mixer_tests.moc"
