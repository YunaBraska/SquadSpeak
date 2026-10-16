#include "voice_mixer.hpp"
#include "audio_profiles.hpp"
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <numbers>
#include <numeric>
#include <random>

namespace {
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
        for (int n = 0; n < 6; ++n) (void)receiver.render(48000, 280 + n * 20);
        QCOMPARE(rms(receiver.render(48000, 400)), 0.0);
        QVERIFY(receiver.receive("voice", packets[15], 420));
        QVERIFY(rms(receiver.render(48000, 420)) > 0.01);
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
            const auto speech = channels == 1 ? tone(0.3, 48000, 960, 120 + frame)
                                              : stereoTone(0.3, 0.2, 960);
            history.push_back(speech);
            if (history.size() > 3) history.pop_front();
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
            const auto speech = channels == 1 ? tone(0.3, 48000, 960, 120 + frame)
                                               : stereoTone(0.3, 0.2, 960);
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
            senders[mode].setNoiseSuppression(mode * 0.5);
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
        double dryEnergy = 0, wetEnergy = 0, cross = 0, blendEnergy = 0, errorEnergy = 0;
        const auto offset = size_t(960 * channels);
        for (size_t i = size_t(48000 * channels); i < size_t(120000 * channels); ++i) {
            const double dry = decoded[0][i], wet = decoded[2][i + offset];
            const auto expectedBlend = (dry + wet) * 0.5;
            const auto error = decoded[1][i + offset] - expectedBlend;
            dryEnergy += dry * dry; wetEnergy += wet * wet; cross += dry * wet;
            blendEnergy += expectedBlend * expectedBlend; errorEnergy += error * error;
        }
        // Includes the real lossy codec; this pins RNNoise's extra 20 ms of delay
        // and catches a dry blend shifted by either half or all of an Opus frame.
        QVERIFY(cross / std::sqrt(dryEnergy * wetEnergy) > 0.98);
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
        (void)first.encode(input, 48000);
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
        for (int i = 0; i < 6; ++i) QVERIFY(rms(receiver.render(48000, 300)) > 0);
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
