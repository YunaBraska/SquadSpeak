#include "audio_processor.hpp"
#include "audio_profiles.hpp"
#include "echo_canceller.hpp"

#include <QtTest>
#include <QtEndian>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <cmath>
#include <limits>
#include <numbers>
#include <random>

class AudioTests final : public QObject {
    Q_OBJECT
private slots:
    void eventSoundAssetsAreDistinctBoundedPcm() {
        QSet<QByteArray> hashes;
        for (const auto* name : {"join", "leave", "kick", "ban", "message", "announcement"}) {
            QFile file(QString(":/qt/qml/SquadSpeak/ui/sounds/%1.wav").arg(name));
            QVERIFY2(file.open(QIODevice::ReadOnly), name);
            const auto bytes = file.readAll();
            QVERIFY(bytes.size() >= 44);
            QCOMPARE(bytes.first(4), "RIFF"); QCOMPARE(bytes.sliced(8, 8), "WAVEfmt ");
            const auto u16 = [&](int offset) { return qFromLittleEndian<quint16>(bytes.constData() + offset); };
            const auto u32 = [&](int offset) { return qFromLittleEndian<quint32>(bytes.constData() + offset); };
            QCOMPARE(u32(4), bytes.size() - 8); QCOMPARE(u32(16), 16);
            QCOMPARE(u16(20), 1); QCOMPARE(u16(22), 1); QCOMPARE(u16(34), 16);
            QVERIFY(u32(24) == 24000 || u32(24) == 48000);
            QCOMPARE(u32(28), u32(24) * 2); QCOMPARE(u16(32), 2);
            QCOMPARE(bytes.sliced(36, 4), "data"); QCOMPARE(u32(40), bytes.size() - 44);
            QVERIFY(u32(40) >= u32(24) / 5 && u32(40) <= u32(24) * 2);
            int peak = 0;
            for (qsizetype offset = 44; offset + 1 < bytes.size(); offset += 2)
                peak = std::max(peak, std::abs(int(qFromLittleEndian<qint16>(bytes.constData() + offset))));
            QVERIFY(peak > 1000 && peak < 10000);
            QCOMPARE(u16(44), 0); QCOMPARE(u16(int(bytes.size()) - 2), 0);
            hashes.insert(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        }
        QCOMPARE(hashes.size(), 6);
    }

    void echoDefaultsAndDeviceProfilesPersist() {
        QTemporaryDir directory;
        const auto path = directory.filePath("audio.ini");
        {
            squad::AudioProfiles profiles(path);
            QVERIFY(profiles.echoCancellation("a"));
            profiles.saveEchoCancellation("a", false).saveEchoCancellation("b", true);
        }
        squad::AudioProfiles loaded(path);
        QVERIFY(!loaded.echoCancellation("a"));
        QVERIFY(loaded.echoCancellation("b"));
        QVERIFY(loaded.echoCancellation("new-device"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, loaded.echoCancellation({}));
        QSettings raw(path, QSettings::IniFormat);
        raw.setValue("input/61/echoCancellation", "invalid"); raw.sync();
        squad::AudioProfiles corrupt(path);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, corrupt.echoCancellation("a"));
    }

    void echoBypassReferenceLossAndResetPreserveInput() {
        const std::vector<float> samples(480, 0.125f);
        squad::EchoCanceller echo;
        QCOMPARE(echo.capture(samples, 48000, 0, 0), samples);
        echo.render(std::span(samples).first(1), 48000, 0, 20);
        QCOMPARE(echo.capture(samples, 48000, 0, 0), samples);
        echo.setEnabled(false).render(samples, 48000, 0, 20);
        QCOMPARE(echo.capture(samples, 48000, 0, 0), samples);
        echo.setEnabled(true).render(samples, 48000, 10, 20);
        QCOMPARE(echo.capture(samples, 48000, 300, 0), samples);
        for (int i = 0; i < 10; ++i) {
            echo.render(samples, 48000, 400 + i, 20).reset();
            QCOMPARE(echo.capture(samples, 48000, 400 + i, 0), samples);
        }
        echo.render(samples, 48000, 600, 20);
        QCOMPARE(echo.capture(samples, 48000, 500, 0), samples);
    }

    void echoHandlesFragmentedNativeRates_data() {
        QTest::addColumn<int>("rate");
        QTest::addColumn<int>("renderRate");
        for (const auto rate : {8000, 16000, 44100, 48000, 96000, 192000})
            QTest::newRow(qPrintable(QString::number(rate))) << rate << rate;
        QTest::newRow("16k-input-48k-output") << 16000 << 48000;
        QTest::newRow("48k-input-44k-output") << 48000 << 44100;
        QTest::newRow("44k-input-48k-output") << 44100 << 48000;
    }

    void echoHandlesFragmentedNativeRates() {
        QFETCH(int, rate); QFETCH(int, renderRate);
        squad::EchoCanceller echo;
        const std::vector<float> reference(size_t(renderRate / 100), 0);
        const auto fragment = size_t(rate / 300);
        std::vector<float> input(size_t(rate / 100));
        double inputEnergy = 0, outputEnergy = 0;
        size_t outputSamples = 0;
        for (int frame = 0; frame < 100; ++frame) {
            for (size_t i = 0; i < input.size(); ++i)
                input[i] = float(0.15 * std::sin(2 * std::numbers::pi * 440 * (frame * input.size() + i) / rate));
            echo.render(reference, renderRate, frame * 10, 20);
            const auto first = echo.capture(std::span(input).first(fragment), rate, frame * 10, 10);
            const auto second = echo.capture(std::span(input).subspan(fragment), rate, frame * 10, 10);
            outputSamples += first.size() + second.size();
            for (auto sample : first) { QVERIFY(std::isfinite(sample)); outputEnergy += sample * sample; }
            for (auto sample : second) { QVERIFY(std::isfinite(sample)); outputEnergy += sample * sample; }
            for (auto sample : input) inputEnergy += sample * sample;
        }
        QCOMPARE(outputSamples, size_t(rate));
        QVERIFY(outputEnergy / inputEnergy > 0.8);
        QVERIFY(outputEnergy / inputEnergy < 1.2);
    }

    void echoReducesKnownPlaybackReflection() {
        constexpr int rate = 48000, delay = 1440, frames = 800;
        std::mt19937 random(42);
        std::uniform_real_distribution<float> noise(-0.2f, 0.2f);
        std::vector<float> reference(frames * 480);
        float previous = 0;
        for (auto& sample : reference) { previous = 0.7f * previous + 0.3f * noise(random); sample = previous; }
        squad::EchoCanceller echo;
        squad::AudioProcessor analysis(rate);
        double inputEnergy = 0, outputEnergy = 0;
        for (int frame = 0; frame < frames; ++frame) {
            const auto offset = frame * 480;
            echo.render(std::span(reference).subspan(offset, 480), rate, frame * 10, 30);
            std::array<float, 480> capture{};
            for (int i = 0; i < 480; ++i)
                if (offset + i >= delay) capture[i] = reference[size_t(offset + i - delay)] * 0.5f;
            for (auto sample : capture) analysis.observeInput(sample);
            const auto output = echo.capture(capture, rate, frame * 10, 0);
            for (auto sample : output) (void)analysis.filter(sample);
            QCOMPARE(output.size(), capture.size());
            if (frame < 400) continue;
            for (auto value : capture) inputEnergy += value * value;
            for (auto value : output) { QVERIFY(std::isfinite(value)); outputEnergy += value * value; }
        }
        QVERIFY2(outputEnergy / inputEnergy < 0.04,
            qPrintable(QString::number(10 * std::log10(outputEnergy / inputEnergy))));
        QVERIFY(analysis.levels().inputDb - analysis.levels().outputDb > 10);
    }

    void echoPreservesNearSignalWithClockDrift_data() {
        QTest::addColumn<int>("driftPpm");
        QTest::newRow("slower-capture-clock") << -200;
        QTest::newRow("same-clock") << 0;
        QTest::newRow("faster-capture-clock") << 200;
    }
    void echoPreservesNearSignalWithClockDrift() {
        QFETCH(int, driftPpm);
        constexpr int rate = 48000, frames = 1200, delay = 1440;
        constexpr double nearAmplitude = 0.05, frequency = 700;
        std::mt19937 random(42);
        std::uniform_real_distribution<float> noise(-0.2f, 0.2f);
        std::vector<float> reference(frames * 480 + 480);
        float previous = 0;
        for (auto& sample : reference) { previous = 0.7f * previous + 0.3f * noise(random); sample = previous; }
        squad::EchoCanceller echo;
        double nearSin = 0, nearCos = 0, nearEnergy = 0;
        double farInput = 0, farOutput = 0;
        int nearSamples = 0;
        for (int frame = 0; frame < frames; ++frame) {
            const int offset = frame * 480;
            echo.render(std::span(reference).subspan(offset, 480), rate, frame * 10, 30);
            std::array<float, 480> capture{};
            for (int i = 0; i < 480; ++i) {
                const double position = (offset + i) * (1 + driftPpm / 1000000.0) - delay;
                if (position >= 0) {
                    const auto index = size_t(position);
                    capture[i] = float(0.5 * std::lerp(reference[index], reference[index + 1], position - index));
                }
                if (frame >= 400 && frame < 800)
                    capture[i] += float(nearAmplitude * std::sin(2 * std::numbers::pi * frequency * (offset + i) / rate));
            }
            const auto output = echo.capture(capture, rate, frame * 10, 0);
            QCOMPARE(output.size(), capture.size());
            for (int i = 0; i < 480; ++i) {
                QVERIFY(std::isfinite(output[size_t(i)]));
                if (frame >= 600 && frame < 800) {
                    const double phase = 2 * std::numbers::pi * frequency * (offset + i) / rate;
                    nearSin += output[size_t(i)] * std::sin(phase);
                    nearCos += output[size_t(i)] * std::cos(phase);
                    nearEnergy += output[size_t(i)] * output[size_t(i)]; ++nearSamples;
                } else if (frame >= 1000) {
                    farInput += capture[size_t(i)] * capture[size_t(i)];
                    farOutput += output[size_t(i)] * output[size_t(i)];
                }
            }
        }
        // Quadrature projection tolerates AEC latency without hiding loss of
        // the wanted signal. Far-end-only recovery also rejects mere bypass.
        const double amplitude = 2 * std::hypot(nearSin, nearCos) / nearSamples;
        const double gain = amplitude / nearAmplitude;
        const double nearFraction = amplitude * amplitude * nearSamples / (2 * nearEnergy);
        const double echoRatio = farOutput / farInput;
        qInfo() << "clock drift ppm" << driftPpm << "near gain" << gain
                << "wanted energy fraction" << nearFraction << "remaining echo energy" << echoRatio;
        QVERIFY(gain > 0.75 && gain < 1.25);
        QVERIFY(nearFraction > 0.8);
        QVERIFY(echoRatio < 0.1);
    }

    void rawAnalysisSurvivesBufferedUpstreamFiltering() {
        squad::AudioProcessor raw(48000), buffered(48000, {-6, 0, 0});
        for (int i = 0; i < 6000; ++i) {
            const auto sample = i == 5999 ? 1.0f : float(0.2 * std::sin(2 * std::numbers::pi * 440 * i / 48000));
            (void)raw.process(sample);
            buffered.observeInput(sample);
        }
        // Upstream frame buffering changes output size and timing. It must not
        // change the raw spectrum or conceal the full-scale device sample.
        for (int i = 0; i < 4800; ++i)
            (void)buffered.filter(float(0.04 * std::sin(2 * std::numbers::pi * 730 * i / 48000)));
        const auto levels = buffered.levels(), original = raw.levels();
        QCOMPARE(levels.inputDb, original.inputDb);
        QCOMPARE(levels.inputSpectrum, original.inputSpectrum);
        QCOMPARE(levels.fundamentalHz, original.fundamentalHz);
        QVERIFY(levels.clipped);
        QVERIFY(levels.spectrumReady);
        QVERIFY(levels.outputDb < -35 && levels.outputDb > -38);
        QVERIFY(levels.outputSpectrum != levels.inputSpectrum);
        squad::AudioProcessor outputOnly(48000);
        (void)outputOnly.filter(0.1f);
        QCOMPARE(outputOnly.levels().inputDb, -96.0);
        QVERIFY(std::abs(outputOnly.levels().outputDb + 20) < 0.001);
        QVERIFY(!outputOnly.levels().spectrumReady);
    }

    void echoRejectsInvalidInput() {
        squad::EchoCanceller echo;
        const std::vector<float> samples(480, 0.1f);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, echo.capture(samples, 0, 0, 0));
        echo.render(samples, 48000, 0, 0);
        QCOMPARE(echo.capture(samples, 48001, 0, 0), samples);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, echo.render(samples, 48000, -1, 0));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, echo.render(samples, 48000, 0, 501));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, echo.capture(std::vector<float>(48001), 48000, 0, 0));
        const std::array<float, 1> invalid{std::numeric_limits<float>::quiet_NaN()};
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, echo.capture(invalid, 48000, 0, 0));
        QCOMPARE(echo.capture(samples, 48000, 0, 0), samples);
    }

    void echoChunkBoundariesDoNotChangeTheSignal() {
        squad::EchoCanceller complete, fragmented;
        std::array<float, 960> reference{}, microphone{};
        for (int block = 0; block < 100; ++block) {
            for (size_t i = 0; i < reference.size(); ++i) {
                const double phase = (block * 960 + i) / 48000.0;
                reference[i] = float(0.1 * std::sin(2 * std::numbers::pi * 730 * phase));
                microphone[i] = float(reference[i] * 0.4 + 0.15 * std::sin(2 * std::numbers::pi * 440 * phase));
            }
            complete.render(reference, 48000, block * 20, 20);
            fragmented.render(std::span(reference).first(201), 48000, block * 20, 20);
            fragmented.render(std::span(reference).subspan(201), 48000, block * 20, 20);
            const auto expected = complete.capture(microphone, 48000, block * 20, 10);
            std::vector<float> actual;
            int used = 0;
            for (const auto count : {101, 379, 17, 463}) {
                const auto part = fragmented.capture(std::span(microphone).subspan(used, count), 48000, block * 20, 10);
                actual.insert(actual.end(), part.begin(), part.end());
                used += count;
            }
            QCOMPARE(actual, expected);
        }
        // Neither a device reset nor turning processing off retains a fragment
        // from the preceding microphone in the next output block.
        QVERIFY(fragmented.capture(std::span(microphone).first(17), 48000, 2000, 0).empty());
        fragmented.reset();
        QCOMPARE(fragmented.capture(microphone, 44100, 2000, 0), std::vector<float>(microphone.begin(), microphone.end()));
    }

    void bypassPreservesSamples() {
        squad::AudioProcessor audio(48000);
        for (int i = 0; i < 5000; ++i) {
            const auto sample = float(0.3 * std::sin(i * 0.1));
            QCOMPARE(audio.process(sample), sample);
        }
    }

    void gainAndClippingAreMeasured() {
        squad::AudioProcessor audio(48000, {6, 0, 0});
        for (int i = 0; i < 5000; ++i) (void)audio.process(0.25f);
        const auto levels = audio.levels();
        QVERIFY(std::abs((levels.outputDb - levels.inputDb) - 6) < 0.01);
        QVERIFY(!levels.clipped);
        for (int i = 0; i < 3000; ++i) QCOMPARE(audio.process(0.8f), 1.0f);
        QVERIFY(audio.levels().clipped);
    }

    void automaticInputProtectionPreservesCleanSpeechAndReducesRumble() {
        for (const auto rate : {8000, 44100, 48000, 96000}) {
            squad::AudioProcessor audio(rate, {0, 0, 0, true, true});
            double inputRumble = 0, outputRumble = 0, inputVoice = 0, outputVoice = 0;
            for (int i = 0; i < rate * 3; ++i) {
                const auto angle = 2 * std::numbers::pi * i / rate;
                const auto input = float(0.7 * std::sin(angle * 25) + 0.05 * std::sin(angle * 440));
                const auto output = audio.process(input);
                QVERIFY(std::isfinite(output)); QVERIFY(std::abs(output) <= 0.890001);
                if (i >= rate * 2) {
                    inputRumble += input * std::sin(angle * 25); outputRumble += output * std::sin(angle * 25);
                    inputVoice += input * std::sin(angle * 440); outputVoice += output * std::sin(angle * 440);
                }
            }
            QVERIFY(audio.highPassHz() >= 90); QVERIFY(audio.highPassHz() <= 100);
            QVERIFY(std::abs(outputRumble / inputRumble) < 0.1);
            QVERIFY(std::abs(outputVoice / inputVoice) > 0.9);
            for (int i = 0; i < rate * 4; ++i) (void)audio.process(float(0.2 * std::sin(2 * std::numbers::pi * 440 * i / rate)));
            QVERIFY(audio.highPassHz() <= 20.01);
            QVERIFY(std::abs(audio.gainDb()) < 0.01);
            const auto response = audio.filterResponse();
            QVERIFY(std::isfinite(response.front()));
        }
    }
    void automaticGainLeavesHeadroomWithoutHidingDamagedInput() {
        squad::AudioProcessor audio(48000, {0, 0, 0, true, false});
        for (int i = 0; i < 48000; ++i) {
            const float input = std::clamp(float(2 * std::sin(i * 0.1)), -1.0f, 1.0f);
            QVERIFY(std::abs(audio.process(input)) <= 0.890001f);
        }
        QVERIFY(audio.levels().clipped); // Raw capture still needs a user warning.
        QVERIFY(audio.gainDb() < -0.9);
        for (int i = 0; i < 240000; ++i) (void)audio.process(0.1f);
        QVERIFY(std::abs(audio.gainDb()) < 0.01);
        QVERIFY(!audio.levels().clipped);
        audio.configure({6, 0, 0, false, false});
        QVERIFY(std::abs(audio.process(0.1f) - 0.199526f) < 0.00001f);
        QCOMPARE(audio.highPassHz(), 0);
    }
    void automaticLowCutRespectsManualHighCutAndRejectsInvalidOverride() {
        squad::AudioProcessor audio(8000, {0, 300, 100, true, true});
        for (int i = 0; i < 24000; ++i) (void)audio.process(float(0.4 * std::sin(i * 0.02)));
        QVERIFY(audio.highPassHz() <= 50);
        const auto cut = audio.highPassHz();
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, audio.configure({0, 300, 100, false, false}));
        QCOMPARE(audio.highPassHz(), cut);
        for (const auto sample : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), -std::numeric_limits<float>::infinity()})
            QVERIFY(std::isfinite(audio.process(sample)));
    }
    void reducingGainDoesNotHideClippedInput() {
        squad::AudioProcessor audio(48000, {-12, 0, 0});
        for (int i = 0; i < 4096; ++i) (void)audio.process(i % 2 ? 1.0f : -1.0f);
        QVERIFY(audio.levels().outputDb < -11);
        QVERIFY(audio.levels().clipped);
        for (int i = 0; i < 4096; ++i) (void)audio.process(0.25f);
        QVERIFY(!audio.levels().clipped);
    }

    void filterResponseMatchesConfiguredTransfer() {
        const auto neutral = squad::AudioProcessor(48000).filterResponse();
        for (const auto value : neutral) QVERIFY(std::abs(value) < 1e-9);
        const auto gain = squad::AudioProcessor(48000, {6, 0, 0}).filterResponse();
        for (const auto value : gain) QVERIFY(std::abs(value - 6.0) < 1e-9);

        squad::AudioProcessor filtered(48000, {0, 300, 6000});
        const auto response = filtered.filterResponse();
        auto nearest = [](const auto& values, double frequency) {
            const auto index = std::size_t(std::round(std::log(frequency / 20.0)
                / std::log(20000.0 / 20.0) * (values.size() - 1)));
            return values[std::min(index, values.size() - 1)];
        };
        QVERIFY(nearest(response, 50) < -20);
        QVERIFY(nearest(response, 1000) > -1);
        QVERIFY(nearest(response, 10000) < -10);
    }

    void filterResponseAgreesWithSteadyToneMeasurements_data() {
        QTest::addColumn<double>("sampleRate");
        QTest::addColumn<double>("gainDb");
        QTest::addColumn<double>("highPassHz");
        QTest::addColumn<double>("lowPassHz");
        QTest::addColumn<int>("bin");
        QTest::newRow("44100 highpass attenuation") << 44100.0 << 3.0 << 120.0 << 7000.0 << 12;
        QTest::newRow("44100 lowpass passband") << 44100.0 << -4.0 << 0.0 << 9000.0 << 150;
        QTest::newRow("48000 combined passband") << 48000.0 << 6.0 << 300.0 << 6000.0 << 128;
        QTest::newRow("48000 highpass cutoff") << 48000.0 << 0.0 << 300.0 << 6000.0 << 100;
        QTest::newRow("96000 combined passband") << 96000.0 << -3.0 << 1000.0 << 18000.0 << 190;
        QTest::newRow("96000 lowpass attenuation") << 96000.0 << -3.0 << 1000.0 << 6000.0 << 225;
    }

    void filterResponseAgreesWithSteadyToneMeasurements() {
        QFETCH(double, sampleRate);
        QFETCH(double, gainDb);
        QFETCH(double, highPassHz);
        QFETCH(double, lowPassHz);
        QFETCH(int, bin);
        const auto maximum = std::min(20000.0, sampleRate * 0.45);
        const auto frequency = 20.0 * std::pow(maximum / 20.0, double(bin) / 255.0);
        squad::AudioProcessor audio(sampleRate, {gainDb, highPassHz, lowPassHz});
        constexpr int seconds = 2;
        const auto samples = int(sampleRate * seconds);
        double inputPower = 0;
        double outputPower = 0;
        for (int i = 0; i < samples; ++i) {
            const auto sample = 0.1 * std::sin(2 * std::numbers::pi * frequency * i / sampleRate);
            const auto output = audio.process(float(sample));
            if (i >= samples / 2) { inputPower += sample * sample; outputPower += output * output; }
        }
        const auto measured = 10 * std::log10(outputPower / inputPower);
        const auto response = audio.filterResponse();
        QVERIFY(std::isfinite(measured));
        QVERIFY(std::abs(measured - response[std::size_t(bin)]) < 0.35);

        const auto beforeRejected = response;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, audio.configure({0, 18000, 1000}));
        const auto afterRejected = audio.filterResponse();
        for (std::size_t i = 0; i < beforeRejected.size(); ++i)
            QVERIFY(std::abs(beforeRejected[i] - afterRejected[i]) < 1e-12);

        const squad::InputProfile replacement{-2, 200, std::min(12000.0, maximum - 1)};
        audio.configure(replacement);
        const auto expected = squad::AudioProcessor(sampleRate, replacement).filterResponse();
        const auto afterConfigure = audio.filterResponse();
        for (std::size_t i = 0; i < expected.size(); ++i)
            QVERIFY(std::abs(expected[i] - afterConfigure[i]) < 1e-12);
    }

    void filterResponseRemainsFiniteAtExtremeCutoffs() {
        const auto response = squad::AudioProcessor(48000, {24, 19900, 19999}).filterResponse();
        for (const auto value : response) QVERIFY(std::isfinite(value));
    }

    void filtersAttenuateTheConfiguredBands_data() {
        QTest::addColumn<double>("high");
        QTest::addColumn<double>("low");
        QTest::addColumn<double>("frequency");
        QTest::addColumn<bool>("attenuated");
        QTest::newRow("highpass rejects bass") << 300.0 << 0.0 << 50.0 << true;
        QTest::newRow("highpass retains speech band") << 100.0 << 0.0 << 1000.0 << false;
        QTest::newRow("lowpass rejects treble") << 0.0 << 1500.0 << 10000.0 << true;
        QTest::newRow("lowpass retains speech band") << 0.0 << 6000.0 << 1000.0 << false;
        QTest::newRow("combined passband") << 100.0 << 6000.0 << 1000.0 << false;
    }

    void filtersAttenuateTheConfiguredBands() {
        QFETCH(double, high); QFETCH(double, low); QFETCH(double, frequency); QFETCH(bool, attenuated);
        squad::AudioProcessor audio(48000, {0, high, low});
        for (int i = 0; i < 48000; ++i)
            (void)audio.process(float(0.25 * std::sin(2 * std::numbers::pi * frequency * i / 48000)));
        const auto levels = audio.levels();
        if (attenuated) QVERIFY(levels.outputDb < levels.inputDb - 25);
        else QVERIFY(std::abs(levels.outputDb - levels.inputDb) < 0.5);
    }

    void spectrumLocatesToneAndSilence() {
        squad::AudioProcessor audio(48000);
        QCOMPARE(audio.levels().strongestHz, 0.0);
        QCOMPARE(audio.levels().inputDb, -96.0);
        const auto bandFrequency = 440.0;
        for (int i = 0; i < 8192; ++i)
            (void)audio.process(float(0.25 * std::sin(2 * std::numbers::pi * bandFrequency * i / 48000)));
        QVERIFY(std::abs(audio.levels().strongestHz - bandFrequency) < 2);
        const auto bins = audio.levels().inputSpectrum;
        QVERIFY(std::abs(*std::max_element(bins.begin(), bins.end()) - 20 * std::log10(0.25)) < 1.5);
        for (int i = 0; i < 8192; ++i) (void)audio.process(0);
        QCOMPARE(audio.levels().strongestHz, 0.0);
    }

    void spectrumFindsTonesBetweenFormerMeasurementPoints_data() {
        QTest::addColumn<double>("frequency");
        for (double frequency : {110.0, 220.0, 1000.0, 5000.0, 14000.0})
            QTest::newRow(qPrintable(QString::number(frequency))) << frequency;
    }

    void spectrumFindsTonesBetweenFormerMeasurementPoints() {
        QFETCH(double, frequency);
        squad::AudioProcessor audio(48000);
        for (int i = 0; i < 12000; ++i)
            (void)audio.process(float(0.25 * std::sin(2 * std::numbers::pi * frequency * i / 48000)));
        const auto result = audio.levels();
        QVERIFY2(std::abs(result.strongestHz - frequency) < 2, qPrintable(QString::number(result.strongestHz)));
        QVERIFY(*std::max_element(result.inputSpectrum.begin(), result.inputSpectrum.end()) > -14);
    }

    void fundamentalFollowsPeriodicityRatherThanLoudestHarmonic_data() {
        QTest::addColumn<double>("sampleRate");
        QTest::addColumn<double>("frequency");
        QTest::addColumn<bool>("missingFundamental");
        for (double rate : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
            for (double frequency : {60.0, 110.0, 220.0, 440.0, 880.0}) {
                QTest::newRow(qPrintable(QString("%1 Hz at %2").arg(frequency).arg(rate))) << rate << frequency << false;
            }
            QTest::newRow(qPrintable(QString("missing fundamental at %1").arg(rate))) << rate << 220.0 << true;
        }
    }

    void fundamentalFollowsPeriodicityRatherThanLoudestHarmonic() {
        QFETCH(double, sampleRate); QFETCH(double, frequency); QFETCH(bool, missingFundamental);
        squad::AudioProcessor audio(sampleRate);
        for (int i = 0; i < sampleRate; ++i) {
            const auto phase = 2 * std::numbers::pi * frequency * i / sampleRate;
            const auto sample = missingFundamental ? 0.2 * std::sin(2 * phase) + 0.1 * std::sin(3 * phase)
                : 0.1 * std::sin(phase) + 0.25 * std::sin(2 * phase) + 0.08 * std::sin(3 * phase);
            (void)audio.process(float(sample));
        }
        const auto result = audio.levels();
        QVERIFY2(std::abs(result.fundamentalHz - frequency) / frequency < 0.01,
                 qPrintable(QString::number(result.fundamentalHz)));
        QVERIFY(std::abs(result.strongestHz - 2 * frequency) < sampleRate / 4096);
    }

    void silenceNoiseAndDcDoNotProduceAFundamental() {
        squad::AudioProcessor audio(48000);
        QCOMPARE(audio.levels().fundamentalHz, 0.0);
        std::minstd_rand random(17);
        for (int i = 0; i < 24000; ++i)
            (void)audio.process(float(0.2 * (double(random()) / random.max() - 0.5)));
        QCOMPARE(audio.levels().fundamentalHz, 0.0);
        for (int i = 0; i < 12000; ++i) (void)audio.process(0.25f);
        QCOMPARE(audio.levels().fundamentalHz, 0.0);
        QCOMPARE(audio.levels().strongestHz, 0.0);
        for (int i = 0; i < 12000; ++i) (void)audio.process(0);
        QCOMPARE(audio.levels().fundamentalHz, 0.0);
    }

    void analysisDoesNotChangeAudioOrFollowTheUsersFilters() {
        squad::AudioProcessor audio(48000, {0, 1000, 0});
        for (int i = 0; i < 12000; ++i)
            (void)audio.process(float(0.25 * std::sin(2 * std::numbers::pi * 220 * i / 48000)));
        const auto result = audio.levels();
        QVERIFY(std::abs(result.fundamentalHz - 220) < 2);
        QVERIFY(result.outputDb < result.inputDb - 20);
        QVERIFY(result.spectrumReady);
        const auto filtered = *std::max_element(result.outputSpectrum.begin(), result.outputSpectrum.end());
        const auto raw = *std::max_element(result.inputSpectrum.begin(), result.inputSpectrum.end());
        QVERIFY(filtered < raw - 20);
    }

    void analysisWindowCost() {
        squad::AudioProcessor audio(48000);
        for (int i = 0; i < 12000; ++i)
            (void)audio.process(float(0.2 * std::sin(2 * std::numbers::pi * 220 * i / 48000)));
        QBENCHMARK {
            const auto result = audio.levels();
            QVERIFY(result.fundamentalHz > 219 && result.fundamentalHz < 221);
        }
    }

    void outOfRangeTonesDoNotBecomeFalseSubharmonics_data() {
        QTest::addColumn<double>("frequency");
        for (double frequency : {25.0, 1500.0, 2000.0, 3000.0, 8000.0, 14000.0})
            QTest::newRow(qPrintable(QString::number(frequency))) << frequency;
    }

    void outOfRangeTonesDoNotBecomeFalseSubharmonics() {
        QFETCH(double, frequency);
        squad::AudioProcessor audio(48000);
        for (int i = 0; i < 24000; ++i)
            (void)audio.process(float(0.25 * std::sin(2 * std::numbers::pi * frequency * i / 48000)));
        QCOMPARE(audio.levels().fundamentalHz, 0.0);
    }

    void quietModulatedToneRemainsMeasurableAndThenClears() {
        squad::AudioProcessor audio(44100);
        for (int i = 0; i < 22050; ++i) {
            const auto time = double(i) / 44100;
            const auto envelope = 0.001 * (1 + 0.1 * std::sin(2 * std::numbers::pi * 3 * time));
            (void)audio.process(float(envelope * std::sin(2 * std::numbers::pi * 220 * time)));
        }
        QVERIFY(std::abs(audio.levels().fundamentalHz - 220) < 2);
        for (int i = 0; i < 12000; ++i) (void)audio.process(0);
        QCOMPARE(audio.levels().fundamentalHz, 0.0);
        QCOMPARE(audio.levels().strongestHz, 0.0);
    }

    void invalidValuesCannotPoisonProcessing() {
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, squad::AudioProcessor(0));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, squad::AudioProcessor(48000, {100, 0, 0}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, squad::AudioProcessor(8000, {0, 0, 6000}));
        squad::AudioProcessor audio(48000);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, audio.configure({0, 500, 100}));
        QCOMPARE(audio.process(std::numeric_limits<float>::quiet_NaN()), 0.0f);
        QCOMPARE(audio.process(std::numeric_limits<float>::infinity()), 0.0f);
        QCOMPARE(audio.process(0.5f), 0.5f);
    }

    void profilesRemainIndependentAndSurviveRestart() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto filename = directory.filePath("audio.ini");
        {
            squad::AudioProfiles profiles(filename);
            profiles.saveInput("microphone-a", {3, 80, 8000});
            profiles.saveInput("airpods", {-2, 150, 6000});
            profiles.saveOutput("airpods", 0.3);
            profiles.saveOutput("speakers", 0.8);
            profiles.select(true, {}).select(false, "airpods");
        }
        squad::AudioProfiles restored(filename);
        QVERIFY((restored.input("microphone-a") == squad::InputProfile{3, 80, 8000}));
        QVERIFY((restored.input("airpods") == squad::InputProfile{-2, 150, 6000}));
        QCOMPARE(restored.output("airpods"), 0.3);
        QCOMPARE(restored.output("speakers"), 0.8);
        QVERIFY((restored.input("unknown") == squad::InputProfile{0, 0, 0, true, true}));
        QCOMPARE(restored.output("unknown"), 0.5);
        QCOMPARE(restored.selection(true), QByteArray{});
        QCOMPARE(restored.selection(false), QByteArray("airpods"));
        restored.saveOutput("airpods", 0.1);
        QVERIFY((restored.input("airpods") == squad::InputProfile{-2, 150, 6000}));
    }

    void invalidProfilesAndUnwritableStorageAreReported() {
        QTemporaryDir directory;
        squad::AudioProfiles profiles(directory.filePath("profiles.ini"));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, profiles.saveInput({}, {}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, profiles.saveInput("a", {0, 200, 100}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, profiles.saveOutput("a", 2));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, profiles.saveNoiseSuppression("a", -0.1));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, profiles.saveNoiseSuppression("a", 1.1));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, profiles.saveNoiseSuppression("a", std::numeric_limits<double>::infinity()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ([&] {
            squad::AudioProfiles unwritable(directory.path());
            unwritable.saveOutput("a", 0.5);
        })());
    }

    void inputAutomationPersistsPerDeviceAndPreservesLegacyManualSettings() {
        QTemporaryDir directory;
        const auto path = directory.filePath("audio.ini");
        {
            QSettings legacy(path, QSettings::IniFormat);
            legacy.setValue("input/61/gainDb", 3);
            legacy.setValue("input/61/highPassHz", 80);
        }
        squad::AudioProfiles profiles(path);
        QVERIFY((profiles.input("a") == squad::InputProfile{3, 80, 0, false, false}));
        QVERIFY((profiles.input("b") == squad::InputProfile{0, 0, 0, true, true}));
        profiles.saveInput("a", {0, 20, 0, true, false});
        profiles.saveInput("b", {-2, 0, 0, false, true});
        squad::AudioProfiles restored(path);
        QVERIFY((restored.input("a") == squad::InputProfile{0, 20, 0, true, false}));
        QVERIFY((restored.input("b") == squad::InputProfile{-2, 0, 0, false, true}));
        QSettings malformed(path, QSettings::IniFormat);
        malformed.setValue("input/61/gainAutomatic", "sometimes"); malformed.sync();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, squad::AudioProfiles(path).input("a"));
    }

    void voiceActivationDefaultsOnAndPreservesExplicitDeviceChoices() {
        QTemporaryDir dir;
        const auto path = dir.filePath("audio.ini");
        {
            squad::AudioProfiles profiles(path);
            const auto initial = profiles.voiceActivation("microphone");
            QVERIFY(initial.enabled);
            QVERIFY(initial.automatic);
            profiles.saveVoiceActivation("microphone", {true, false, -38});
            profiles.saveVoiceActivation("headset", {true, true, -48});
            profiles.saveVoiceActivation("disabled", {false, false, -40});
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, profiles.saveVoiceActivation("headset", {true, true, -90}));
        }
        const squad::AudioProfiles restored(path);
        const auto manual = restored.voiceActivation("microphone");
        QVERIFY(manual.enabled); QVERIFY(!manual.automatic); QCOMPARE(manual.thresholdDb, -38);
        const auto automatic = restored.voiceActivation("headset");
        QVERIFY(automatic.enabled); QVERIFY(automatic.automatic); QCOMPARE(automatic.thresholdDb, -48);
        QVERIFY(restored.voiceActivation("new-device").enabled);
        QVERIFY(!restored.voiceActivation("disabled").enabled);
    }
    void noiseSuppressionIsOptionalPersistentAndDeviceBound() {
        QTemporaryDir directory;
        const auto filename = directory.filePath("profiles.ini");
        {
            squad::AudioProfiles profiles(filename);
            QCOMPARE(profiles.noiseSuppression("microphone"), 0.0);
            profiles.saveNoiseSuppression("microphone", 0.65).saveNoiseSuppression("headset", 0.3);
            profiles.saveInput("microphone", {6, 80, 9000}).saveOutput("microphone", 0.2);
            profiles.select(true, "headset");
        }
        squad::AudioProfiles restored(filename);
        QCOMPARE(restored.noiseSuppression("microphone"), 0.65);
        QCOMPARE(restored.noiseSuppression("headset"), 0.3);
        QCOMPARE(restored.noiseSuppression("new-device"), 0.0);
        QCOMPARE(restored.output("microphone"), 0.2);
        restored.saveNoiseSuppression("headset", 0);
        QCOMPARE(restored.noiseSuppression("headset"), 0.0);
        QCOMPARE(restored.noiseSuppression("microphone"), 0.65);
    }

    void malformedStoredValuesAreNotSilentlyUsed() {
        QTemporaryDir directory;
        const auto file = directory.filePath("profiles.ini");
        {
            QSettings settings(file, QSettings::IniFormat);
            settings.setValue("input/61/gainDb", "not-a-number");
            settings.setValue("output/61/volume", -1);
            settings.setValue("input/61/noiseSuppression", "not-a-number");
            settings.setValue("input/61/activation/automatic", "maybe");
            settings.setValue("input/62/activation/enabled", "yes");
            settings.setValue("input/63/activation/thresholdDb", "not-a-number");
            settings.sync();
        }
        squad::AudioProfiles profiles(file);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)profiles.input("a"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)profiles.output("a"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)profiles.noiseSuppression("a"));
        for (const auto& device : {"a", "b", "c"})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, (void)profiles.voiceActivation(device));
    }

    void failedSaveDoesNotChangeTheActiveProfile() {
        QTemporaryDir directory;
        const auto blocked = directory.filePath("blocked");
        squad::AudioProfiles profiles(blocked + "/audio.ini");
        QFile obstacle(blocked);
        QVERIFY(obstacle.open(QIODevice::WriteOnly));
        obstacle.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, profiles.saveVoiceActivation("a", {true, false, -20}));
        const auto activation = profiles.voiceActivation("a");
        QVERIFY(activation.enabled); QVERIFY(activation.automatic); QCOMPARE(activation.thresholdDb, -45);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, profiles.saveInput("a", {6, 80, 6000}));
        QVERIFY((profiles.input("a") == squad::InputProfile{0, 0, 0, true, true}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, profiles.saveOutput("a", 0.9));
        QCOMPARE(profiles.output("a"), 0.5);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, profiles.saveNoiseSuppression("a", 1));
        QCOMPARE(profiles.noiseSuppression("a"), 0.0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, profiles.select(true, "a"));
        QCOMPARE(profiles.selection(true), QByteArray{});
    }
};

QTEST_GUILESS_MAIN(AudioTests)
#include "audio_tests.moc"
