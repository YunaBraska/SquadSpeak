#include "voice_mixer.hpp"
#include "echo_canceller.hpp"
#include "audio_processor.hpp"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTest>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <numbers>
#include <stdexcept>

namespace {
constexpr int rate = 48000, length = 12 * rate, beginSpeech = 2 * rate, endSpeech = 10 * rate;
using Samples = std::vector<float>;
double energy(std::span<const float> samples) {
    return std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
}
double rms(std::span<const float> samples) { return std::sqrt(energy(samples) / samples.size()); }
std::span<const float> body(const Samples& samples) {
    return std::span(samples).subspan(beginSpeech, endSpeech - beginSpeech);
}
double peak(const Samples& samples) {
    return std::abs(*std::max_element(samples.begin(), samples.end(),
        [](float a, float b) { return std::abs(a) < std::abs(b); }));
}
double levelChange(std::span<const float> output, std::span<const float> input) {
    return 20 * std::log10(std::max(rms(output), 1e-12) / std::max(rms(input), 1e-12));
}
double correlation(std::span<const float> a, std::span<const float> b) {
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0)
        / std::sqrt(std::max(energy(a) * energy(b), 1e-30));
}
// This metric is only applied before the lossy codec. A separate clean-speech
// retention check prevents interpreting complete suppression as an improvement.
double siSdr(std::span<const float> estimate, std::span<const float> reference) {
    const double meanX = std::accumulate(estimate.begin(), estimate.end(), 0.0) / estimate.size();
    const double meanS = std::accumulate(reference.begin(), reference.end(), 0.0) / reference.size();
    double dot = 0, referenceEnergy = 0;
    for (size_t i = 0; i < estimate.size(); ++i) {
        dot += (estimate[i] - meanX) * (reference[i] - meanS);
        referenceEnergy += std::pow(reference[i] - meanS, 2);
    }
    const double scale = dot / referenceEnergy;
    double targetEnergy = 0, errorEnergy = 0;
    for (size_t i = 0; i < estimate.size(); ++i) {
        const double target = scale * (reference[i] - meanS);
        targetEnergy += target * target;
        errorEnergy += std::pow(estimate[i] - meanX - target, 2);
    }
    return 10 * std::log10(std::max(targetEnergy, 1e-30) / std::max(errorEnergy, 1e-30));
}
QByteArray read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(qPrintable(file.errorString()));
    return file.readAll();
}
QByteArray hash(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
Samples loadPcm(const QByteArray& bytes, int frames) {
    // The importer deliberately writes canonical, metadata-free PCM16 WAV.
    if (bytes.size() != 44 + frames * 2 || bytes.mid(0, 4) != "RIFF" || bytes.mid(8, 8) != "WAVEfmt "
        || qFromLittleEndian<quint32>(bytes.constData() + 4) != quint32(bytes.size() - 8)
        || qFromLittleEndian<quint32>(bytes.constData() + 16) != 16
        || qFromLittleEndian<quint16>(bytes.constData() + 20) != 1
        || qFromLittleEndian<quint16>(bytes.constData() + 22) != 1
        || qFromLittleEndian<quint32>(bytes.constData() + 24) != rate
        || qFromLittleEndian<quint32>(bytes.constData() + 28) != rate * 2
        || qFromLittleEndian<quint16>(bytes.constData() + 32) != 2
        || qFromLittleEndian<quint16>(bytes.constData() + 34) != 16
        || bytes.mid(36, 4) != "data" || qFromLittleEndian<quint32>(bytes.constData() + 40) != quint32(frames * 2))
        throw std::runtime_error("Corpus fixture is not canonical mono PCM16/48000 WAV");
    Samples samples(size_t(frames), 0);
    for (int i = 0; i < frames; ++i) samples[size_t(i)] = qFromLittleEndian<qint16>(bytes.constData() + 44 + 2 * i) / 32768.0f;
    return samples;
}
bool writeWav(const QString& path, const Samples& samples) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4); stream << quint32(36 + samples.size() * 2);
    stream.writeRawData("WAVEfmt ", 8); stream << quint32(16) << quint16(1) << quint16(1)
        << quint32(rate) << quint32(rate * 2) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4); stream << quint32(samples.size() * 2);
    for (float sample : samples) stream << qint16(std::clamp(std::lround(sample * 32768.0), -32768L, 32767L));
    return stream.status() == QDataStream::Ok && file.commit();
}
// Independent library-level measurement, not a replacement for the app path.
// The wet signal has 960 samples of history; flush before compensating it.
Samples rawSuppression(const Samples& input) {
    std::unique_ptr<DenoiseState, decltype(&std::free)> state(
        static_cast<DenoiseState*>(std::malloc(rnnoise_get_size())), std::free);
    if (!state || rnnoise_init(state.get(), nullptr) != 0) throw std::runtime_error("RNNoise initialization failed");
    Samples output(input.size() + 960, 0);
    for (size_t offset = 0; offset < output.size(); offset += 480) {
        std::array<float, 480> frame{};
        if (offset < input.size()) for (size_t i = 0; i < frame.size(); ++i) frame[i] = input[offset + i] * 32768;
        rnnoise_process_frame(state.get(), frame.data(), frame.data());
        for (size_t i = 0; i < frame.size(); ++i) output[offset + i] = std::clamp(frame[i] / 32768.0f, -1.0f, 1.0f);
    }
    return {output.begin() + 960, output.end()};
}
}

class CorpusTests final : public QObject {
    Q_OBJECT
    QJsonObject manifest_;
    QHash<QString, Samples> sources_;
    QHash<QString, QString> categories_;
    QStringList speech_, interference_;
    QJsonArray results_;
    QJsonArray echoResults_;
    QJsonArray inputResults_;
    int codecDelay_ = 0;
    QString outputDir_;
    QByteArray manifestHash_;
    bool extended_ = false;
    int shard_ = 0, shards_ = 1, totalCases_ = 0, selectedCases_ = 0;
private slots:
    void initTestCase() {
        const QDir directory(QStringLiteral(SQUAD_CORPUS_DIR));
        const auto bytes = read(directory.filePath("manifest.json"));
        QJsonParseError error;
        manifest_ = QJsonDocument::fromJson(bytes, &error).object();
        QCOMPARE(error.error, QJsonParseError::NoError);
        manifestHash_ = hash(bytes);
        QCOMPARE(manifest_.value("version").toInt(), 2);
        const auto suite = qEnvironmentVariable("SQUAD_CORPUS_SUITE", "smoke");
        QVERIFY2(suite == "smoke" || suite == "extended", "SQUAD_CORPUS_SUITE must be smoke or extended");
        extended_ = suite == "extended";
        bool validShard = false, validCount = false;
        shard_ = qEnvironmentVariable("SQUAD_CORPUS_SHARD", "0").toInt(&validShard);
        shards_ = qEnvironmentVariable("SQUAD_CORPUS_SHARDS", "1").toInt(&validCount);
        QVERIFY2(validShard && validCount && shards_ >= 1 && shards_ <= 256 && shard_ >= 0 && shard_ < shards_, "Invalid corpus shard/count");
        QCOMPARE(manifest_.value("sample_rate").toInt(), rate);
        QCOMPARE(manifest_.value("duration_seconds").toInt(), 12);
        QCOMPARE(manifest_.value("speech_interval_seconds").toArray(), QJsonArray({2, 10}));
        QCOMPARE(manifest_.value("snr_db").toArray(), QJsonArray({-5, 0, 10}));
        for (const auto value : manifest_.value("sources").toArray()) {
            const auto item = value.toObject();
            const auto id = item.value("id").toString();
            QVERIFY(QRegularExpression("^[a-z0-9-]+$").match(id).hasMatch() && !sources_.contains(id));
            const auto pcm = read(directory.filePath(id + ".wav"));
            QCOMPARE(hash(pcm), item.value("sha256").toString().toLatin1());
            QCOMPARE(hash(read(directory.filePath("licenses/" + id + ".txt"))),
                item.value("license").toObject().value("sha256").toString().toLatin1());
            auto samples = loadPcm(pcm, item.value("frames").toInt());
            QVERIFY(rms(samples) > 0.0001);
            const bool selected = extended_ || manifest_.value("smoke_sources").toArray().contains(id);
            if (item.value("kind").toString() == "speech") {
                QCOMPARE(samples.size(), size_t(8 * rate));
                if (selected) speech_.append(id);
            } else {
                QCOMPARE(item.value("kind").toString(), QString("interference"));
                if (selected) interference_.append(id);
            }
            categories_.insert(id, item.value("category").toString());
            QVERIFY(!categories_[id].isEmpty());
            sources_.insert(id, std::move(samples));
        }
        QVERIFY(speech_.size() >= 3 && interference_.size() >= 4);
        if (!extended_) { QCOMPARE(speech_.size(), 3); QCOMPARE(interference_.size(), 4); }
        // Reproducible broadband clicks, not recordings or a keyboard classifier.
        // Exercise short impulses alongside the same licensed speech fixtures.
        const auto durations = manifest_.value("synthetic_transients_ms").toArray();
        QCOMPARE(durations, QJsonArray({1, 5, 20}));
        for (const auto duration : durations) {
            const int milliseconds = duration.toInt();
            const auto id = "clicks-" + QString::number(milliseconds) + "ms";
            Samples clicks(length, 0);
            quint32 random = 0x51a7;
            for (int start = rate / 5; start + milliseconds * 48 < length; start += rate / 5 + int(random % (rate / 10))) {
                for (int i = 0; i < milliseconds * 48; ++i) {
                    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                    clicks[start + i] = float((double(random) / 2147483648.0 - 1) * std::exp(-5.0 * i / (milliseconds * 48)));
                }
            }
            sources_.insert(id, std::move(clicks));
            categories_.insert(id, "synthetic-transient");
            interference_.append(id);
        }
        int opusError = 0;
        std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder(
            opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &opusError), opus_encoder_destroy);
        QCOMPARE(opusError, OPUS_OK);
        QCOMPARE(opus_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&codecDelay_)), OPUS_OK);
        QVERIFY(codecDelay_ > 0 && codecDelay_ < 960);
        outputDir_ = qEnvironmentVariable("SQUAD_CORPUS_OUTPUT", QStringLiteral(SQUAD_CORPUS_OUTPUT));
        QVERIFY(QDir().mkpath(outputDir_));
    }
    void automaticInput_data() {
        QTest::addColumn<QString>("speaker"); QTest::addColumn<int>("rumbleHz");
        for (const auto value : manifest_.value("sources").toArray()) {
            const auto item = value.toObject();
            if (item.value("kind") != "speech") continue;
            const auto speaker = item.value("id").toString();
            for (const auto hz : {0, 25, 40, 60})
                QTest::newRow(qPrintable(speaker + '-' + QString::number(hz))) << speaker << hz;
        }
    }
    void automaticInput() {
        QFETCH(QString, speaker); QFETCH(int, rumbleHz);
        auto clean = sources_.value(speaker);
        const auto scale = 0.5 / peak(clean);
        for (auto& sample : clean) sample *= scale;
        auto input = clean, output = clean;
        squad::AudioProcessor processor(rate, {0, 0, 0, true, true});
        // Compare adaptation with the fixed gentle 20 Hz baseline used by Auto,
        // so source DC/rumble is not mistaken for speech that must be preserved.
        squad::AudioProcessor cleanBaseline(rate, {0, 20, 0}), inputBaseline(rate, {0, 20, 0});
        double maximumCut = 0;
        QElapsedTimer elapsed; elapsed.start();
        for (size_t i = 0; i < input.size(); ++i) {
            if (rumbleHz) input[i] += float(0.4 * std::sin(2 * std::numbers::pi * rumbleHz * i / rate));
            output[i] = processor.process(input[i]);
            input[i] = inputBaseline.process(input[i]);
            clean[i] = cleanBaseline.process(clean[i]);
            QVERIFY(std::isfinite(output[i]));
            maximumCut = std::max(maximumCut, processor.highPassHz());
        }
        const auto processingMs = elapsed.elapsed();
        QVERIFY(peak(output) <= 0.890001);
        const auto reference = std::span(clean).subspan(rate);
        const auto result = std::span(output).subspan(rate);
        const auto improvement = siSdr(result, reference) - siSdr(std::span(input).subspan(rate), reference);
        inputResults_.append(QJsonObject{{"source", speaker}, {"rumble_hz", rumbleHz}, {"maximum_cut_hz", maximumCut},
            {"level_change_db", levelChange(result, reference)}, {"correlation", correlation(result, reference)},
            {"si_sdr_improvement_db", improvement}, {"processing_and_two_baselines_ms", processingMs}});
        if (!rumbleHz) {
            QVERIFY2(maximumCut < 40, qPrintable(QString("Clean voice low cut %1 Hz").arg(maximumCut)));
            QVERIFY(levelChange(result, reference) > -1);
            QVERIFY(correlation(result, reference) > 0.99);
        } else {
            QVERIFY2(improvement > 3, qPrintable(QString("Rumble improvement %1 dB, cut %2 Hz").arg(improvement).arg(processor.highPassHz())));
        }
    }
    void playbackEcho_data() {
        QTest::addColumn<QString>("speaker"); QTest::addColumn<QString>("playback");
        QTest::addColumn<int>("delayMs"); QTest::addColumn<int>("driftPpm");
        int index = 0;
        for (const auto* speaker : {"speech1", "speech3", "speech-ls61", "speech-ls6930"}) {
            const QString playback = index++ % 2 ? "instrumental" : "song";
            for (int delay : {5, 30, 120}) for (int drift : {-100, 0, 100}) {
                if (drift && delay != 30) continue;
                QTest::newRow(qPrintable(QString("%1-%2-%3ms-%4ppm").arg(speaker).arg(playback).arg(delay).arg(drift)))
                    << QString(speaker) << playback << delay << drift;
            }
        }
        for (const auto& pair : {std::pair{"speech1", "speech-ls6930"}, std::pair{"speech3", "speech-ls61"}})
            for (int drift : {-100, 0, 100})
                QTest::newRow(qPrintable(QString("%1-%2-30ms-%3ppm").arg(pair.first).arg(pair.second).arg(drift)))
                    << QString(pair.first) << QString(pair.second) << 30 << drift;
    }

    void playbackEcho() {
        QFETCH(QString, speaker); QFETCH(QString, playback); QFETCH(int, delayMs); QFETCH(int, driftPpm);
        const auto& voice = sources_[speaker]; const auto& reference = sources_[playback];
        const double voiceGain = 0.04 / rms(voice), playbackGain = 0.07 / rms(reference);
        Samples near(length), far(length), microphone(length), clean, processed;
        for (int i = 0; i < length; ++i) {
            near[i] = float(voice[size_t(i) % voice.size()] * voiceGain);
            far[i] = float(reference[size_t(i) % reference.size()] * playbackGain);
        }
        for (int i = 0; i < length; ++i) {
            microphone[i] = near[i];
            for (const auto [offset, gain] : {std::pair{0, 0.4}, {480, 0.16}, {1392, 0.08}}) {
                const double position = (i - delayMs * 48 - offset) * (1 + driftPpm / 1000000.0);
                if (position < 0 || position >= length - 1) continue;
                const auto sample = size_t(position);
                microphone[i] += float(gain * (far[sample] + (far[sample + 1] - far[sample]) * (position - sample)));
            }
        }
        QVERIFY(peak(microphone) < 1);
        squad::EchoCanceller quietReference, playbackReference;
        const std::array<float, 960> silence{};
        for (int offset = 0; offset < length; offset += 960) {
            const auto now = offset / 48;
            quietReference.render(silence, rate, now, 20);
            playbackReference.render(std::span(far).subspan(offset, 960), rate, now, 20);
            int used = 0;
            for (const auto chunk : {101, 379, 17, 463}) {
                const auto cleaned = quietReference.capture(std::span(near).subspan(offset + used, chunk), rate, now, 10);
                const auto mixed = playbackReference.capture(std::span(microphone).subspan(offset + used, chunk), rate, now, 10);
                clean.insert(clean.end(), cleaned.begin(), cleaned.end());
                processed.insert(processed.end(), mixed.begin(), mixed.end());
                used += chunk;
            }
        }
        QCOMPARE(clean.size(), near.size()); QCOMPARE(processed.size(), near.size());
        QVERIFY(std::all_of(processed.begin(), processed.end(), [](float value) { return std::isfinite(value); }));
        // Calibrate latency using clean speech, never by aligning the echo or
        // choosing a delay that maximizes the mixed result's reported score.
        const auto target = std::span(near).subspan(5 * rate, 4 * rate);
        int delay = 0; double best = -1;
        const auto score = [&](int candidate, int step) {
            double dot = 0, a = 0, b = 0;
            for (int i = 0; i < int(target.size()); i += step) {
                const double x = target[size_t(i)], y = clean[size_t(5 * rate + i + candidate)];
                dot += x * y; a += x * x; b += y * y;
            }
            return dot / std::sqrt(std::max(a * b, 1e-30));
        };
        for (int i = 0; i <= 960; i += 8) if (score(i, 8) > best) { best = score(i, 8); delay = i; }
        const int approximate = delay;
        best = -1;
        for (int i = std::max(0, approximate - 8); i <= std::min(960, approximate + 8); ++i)
            if (score(i, 1) > best) { best = score(i, 1); delay = i; }
        const auto clear = std::span(clean).subspan(5 * rate + delay, target.size());
        const auto output = std::span(processed).subspan(5 * rate + delay, target.size());
        const auto input = std::span(microphone).subspan(5 * rate, target.size());
        const auto improvement = siSdr(output, target) - siSdr(input, target);
        echoResults_.append(QJsonObject{{"case", QString::fromLatin1(QTest::currentDataTag())}, {"delay_ms", delayMs},
            {"drift_ppm", driftPpm}, {"aligned_delay_samples", delay}, {"si_sdr_gain_db", improvement},
            {"clean_correlation", correlation(clear, target)}, {"clean_level_change_db", levelChange(clear, target)}});
        if (qEnvironmentVariableIntValue("SQUAD_CORPUS_EXPORT") == 1) {
            const auto path = outputDir_ + "/echo-" + QString::fromLatin1(QTest::currentDataTag());
            QVERIFY(writeWav(path + "-wanted.wav", near));
            QVERIFY(writeWav(path + "-microphone.wav", microphone));
            QVERIFY(writeWav(path + "-processed.wav", processed));
        }
        QVERIFY(correlation(clear, target) > 0.95);
        QVERIFY(std::abs(levelChange(clear, target)) < 1.5);
        QVERIFY(levelChange(output, target) > -6);
        QVERIFY(std::isfinite(improvement));
        // Drift cases measure the known limitation without disguising a
        // negative result as proven improvement on every microphone pair.
        if (!driftPpm) QVERIFY2(improvement > 0, qPrintable(QString::number(improvement)));
    }

    void duplexEchoDecays_data() {
        QTest::addColumn<QString>("speaker"); QTest::addColumn<int>("delayMs");
        QTest::addColumn<bool>("activation");
        for (const auto* speaker : {"speech1", "speech3", "speech-ls61"})
            for (const auto delay : {20, 80})
                for (const bool activation : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2ms-%3").arg(speaker).arg(delay).arg(activation ? "voice" : "ptt")))
                        << QString(speaker) << delay << activation;
    }
    void duplexEchoDecays() {
        QFETCH(QString, speaker); QFETCH(int, delayMs); QFETCH(bool, activation);
        const auto& voice = sources_[speaker];
        const double voiceGain = 0.04 / rms(voice);
        std::array<double, 2> tail{};
        for (const bool enabled : {false, true}) {
            std::array<squad::EchoCanceller, 2> echoes;
            std::array<squad::VoiceMixer, 2> mixers;
            std::array<squad::AudioProcessor, 2> processors{
                squad::AudioProcessor(rate, {0, 0, 0, true, true}), squad::AudioProcessor(rate, {0, 0, 0, true, true})};
            std::array<Samples, 2> playback{Samples(length), Samples(length)};
            for (auto& echo : echoes) echo.setEnabled(enabled);
            for (auto& mixer : mixers) mixer.setVoiceActivation({activation, true, -45});
            // Two separate rooms. Each microphone hears its own loudspeaker,
            // including audio previously reflected and returned by the peer.
            // Receiver gain stays automatic; disabling AEC is the failure control.
            for (int offset = 0; offset < length; offset += 960) {
                const auto now = offset / 48;
                for (int peer = 0; peer < 2; ++peer) {
                    const auto output = mixers[peer].render(rate, now);
                    QCOMPARE(output.size(), size_t(960));
                    std::copy(output.begin(), output.end(), playback[peer].begin() + offset);
                    echoes[peer].render(output, rate, now, 20);
                }
                for (int peer = 0; peer < 2; ++peer) {
                    std::array<float, 960> microphone{};
                    for (int i = 0; i < 960; ++i) {
                        const int position = offset + i;
                        const int speaking = position - peer * 3 * rate;
                        double sample = speaking >= 0 && speaking < 3 * rate ? voice[speaking] * voiceGain : 0;
                        for (const auto [tap, gain] : {std::pair{0, 0.6}, {480, 0.2}, {1392, 0.1}}) {
                            const int reflected = position - delayMs * 48 - tap;
                            if (reflected >= 0) sample += playback[peer][reflected] * gain;
                        }
                        microphone[i] = float(std::clamp(sample, -1.0, 1.0));
                        processors[peer].observeInput(microphone[i]);
                    }
                    auto clean = echoes[peer].capture(microphone, rate, now, 0);
                    for (auto& sample : clean) sample = processors[peer].filter(sample);
                    for (const auto& packet : mixers[peer].encode(clean, rate))
                        QVERIFY(mixers[1 - peer].receive("peer", packet, now));
                }
            }
            for (const auto& output : playback) {
                QVERIFY(std::all_of(output.begin(), output.end(), [](float value) { return std::isfinite(value); }));
                tail[enabled] += energy(std::span(output).subspan(10 * rate));
            }
            QVERIFY(rms(std::span(playback[1]).subspan(rate, 2 * rate)) > 0.01);
            QVERIFY(rms(std::span(playback[0]).subspan(4 * rate, 2 * rate)) > 0.01);
        }
        qInfo() << "duplex echo tail energy, disabled/enabled" << tail[false] << tail[true];
        QVERIFY(tail[false] > 1);
        QVERIFY(tail[true] < tail[false] * 0.01);
    }

    void mixtures_data() {
        QTest::addColumn<QString>("speaker"); QTest::addColumn<QString>("interference"); QTest::addColumn<int>("snr");
        QTest::addColumn<int>("peakDb");
        const auto add = [&](QString id, const QString& speaker, const QString& noise, int snr, int level) {
            if (extended_ && id != "silence") id += "-peak" + QString::number(level);
            if (totalCases_++ % shards_ != shard_) return;
            ++selectedCases_;
            QTest::newRow(qPrintable(id)) << speaker << noise << snr << level;
        };
        add("silence", {}, {}, 0, 0);
        const auto config = manifest_.value("extended").toObject();
        const auto levels = extended_ ? config.value("mixture_peak_dbfs").toArray() : QJsonArray{QJsonValue(0)};
        const auto snrs = extended_ ? config.value("snr_db").toArray() : manifest_.value("snr_db").toArray();
        QVERIFY(!levels.isEmpty() && !snrs.isEmpty());
        QSet<int> uniqueLevels, uniqueSnrs;
        for (const auto value : levels) {
            QVERIFY(value.isDouble() && value.toDouble() == value.toInt() && value.toInt() >= -60 && value.toInt() <= 0);
            QVERIFY(!uniqueLevels.contains(value.toInt())); uniqueLevels.insert(value.toInt());
        }
        for (const auto value : snrs) {
            QVERIFY(value.isDouble() && value.toDouble() == value.toInt() && std::abs(value.toInt()) <= 60);
            QVERIFY(!uniqueSnrs.contains(value.toInt())); uniqueSnrs.insert(value.toInt());
        }
        for (const auto level : levels) {
            for (const auto& speaker : speech_) add(speaker + "-clean", speaker, {}, 0, level.toInt());
            for (const auto& noise : interference_) {
                add(noise + "-only", {}, noise, 0, level.toInt());
                for (const auto& speaker : speech_) for (const auto snr : snrs)
                    add(speaker + "-" + noise + "-snr" + QString::number(snr.toInt()), speaker, noise, snr.toInt(), level.toInt());
            }
        }
        QVERIFY2(selectedCases_ > 0, "Selected corpus shard has no cases");
    }
    void mixtures() {
        QFETCH(QString, speaker); QFETCH(QString, interference); QFETCH(int, snr);
        QFETCH(int, peakDb);
        Samples reference(length, 0), noise(length, 0), input(length, 0);
        if (!speaker.isEmpty()) {
            const auto& speech = sources_[speaker];
            const double gain = 0.04 / rms(speech);
            for (size_t i = 0; i < speech.size(); ++i) reference[beginSpeech + i] = float(speech[i] * gain);
        }
        if (!interference.isEmpty()) {
            const auto& source = sources_[interference];
            for (size_t i = 0; i < noise.size(); ++i) noise[i] = source[i % source.size()];
            const double gain = 0.04 * std::pow(10, -snr / 20.0) / rms(body(noise));
            for (auto& sample : noise) sample *= float(gain);
        }
        for (size_t i = 0; i < input.size(); ++i) input[i] = reference[i] + noise[i];
        // Apply the same headroom to all stems. This never changes the SNR.
        const double targetPeak = extended_ ? std::pow(10, peakDb / 20.0) : 0.8;
        const double headroom = extended_ && peak(input) > 0 ? targetPeak / peak(input)
            : (peak(input) > 0.8 ? 0.8 / peak(input) : 1);
        for (size_t i = 0; i < input.size(); ++i) { input[i] *= headroom; reference[i] *= headroom; noise[i] *= headroom; }
        if (!speaker.isEmpty() && !interference.isEmpty()) QVERIFY(std::abs(levelChange(body(reference), body(noise)) - snr) < 0.001);
        QVERIFY(peak(input) <= targetPeak + 0.00001);
        if (extended_ && (!speaker.isEmpty() || !interference.isEmpty())) QVERIFY(std::abs(peak(input) - targetPeak) < 0.00001);
        const QString id = QTest::currentDataTag();
        const bool exportAudio = qEnvironmentVariableIntValue("SQUAD_CORPUS_EXPORT") == 1;
        const auto path = [&](const QString& suffix) { return outputDir_ + "/" + id + "-" + suffix + ".wav"; };
        if (exportAudio) {
            QVERIFY(writeWav(path("input"), input)); QVERIFY(writeWav(path("speech"), reference)); QVERIFY(writeWav(path("interference"), noise));
        }
        QJsonObject row{{"case", id}, {"speaker", speaker}, {"interference", interference}, {"common_headroom_gain", headroom}};
        row.insert("category", interference.isEmpty() ? (speaker.isEmpty() ? "silence" : "clean-speech") : categories_[interference]);
        row.insert("input_peak_dbfs", 20 * std::log10(std::max(peak(input), 1e-12)));
        row.insert("speech_rms_dbfs", 20 * std::log10(std::max(rms(body(reference)), 1e-12)));
        row.insert("interference_rms_dbfs", 20 * std::log10(std::max(rms(body(noise)), 1e-12)));
        if (extended_ && id != "silence") row.insert("target_peak_dbfs", peakDb);
        if (!speaker.isEmpty() && !interference.isEmpty()) row.insert("input_snr_db", levelChange(body(reference), body(noise)));
        const auto wet = rawSuppression(input);
        Samples baseline;
        QJsonArray variants;
        const double inputSiSdr = speaker.isEmpty() ? 0 : siSdr(body(input), body(reference));
        for (int strength : {0, 50, 100}) {
            squad::VoiceMixer sender, receiver;
            sender.setNoiseSuppression(strength / 100.0);
            receiver.setAutomatic(false);
            Samples padded = input; padded.resize(input.size() + 3 * 960, 0);
            Samples decoded; decoded.reserve(padded.size());
            for (size_t offset = 0; offset < padded.size(); offset += 960) {
                const auto packets = sender.encode(std::span(padded).subspan(offset, 960), rate);
                QCOMPARE(packets.size(), 1);
                const qint64 now = qint64(offset / 48);
                QVERIFY(receiver.receive("speaker", packets.first(), now));
                const auto frame = receiver.render(rate, now);
                QCOMPARE(frame.size(), size_t(960));
                decoded.insert(decoded.end(), frame.begin(), frame.end());
            }
            QVERIFY(sender.encode(std::span(input).first(960), rate, false).isEmpty());
            const auto delay = codecDelay_ + (strength ? 960 : 0);
            Samples aligned(decoded.begin() + delay, decoded.begin() + delay + length);
            QVERIFY(std::all_of(aligned.begin(), aligned.end(), [](float value) { return std::isfinite(value) && std::abs(value) <= 0.95001; }));
            if (!strength) baseline = aligned;
            QJsonObject variant{{"suppression_percent", strength}, {"pipeline_peak", peak(aligned)},
                {"pipeline_level_vs_bypass_db", levelChange(body(aligned), body(baseline))}};
            Samples raw(length);
            for (size_t i = 0; i < raw.size(); ++i) raw[i] = float(input[i] * (1 - strength / 100.0) + wet[i] * (strength / 100.0));
            if (!speaker.isEmpty()) {
                variant.insert("precodec_si_sdr_db", siSdr(body(raw), body(reference)));
                if (!interference.isEmpty()) {
                    const auto improvement = siSdr(body(raw), body(reference)) - inputSiSdr;
                    variant.insert("precodec_si_sdr_gain_db", improvement);
                    if (!extended_ && categories_[interference] == "synthetic-transient" && strength == 100)
                        QVERIFY2(improvement >= 0, "Full suppression degraded a speech/click regression fixture");
                }
                if (interference.isEmpty()) {
                    const auto retained = levelChange(body(aligned), body(baseline));
                    const auto similarity = correlation(body(aligned), body(baseline));
                    variant.insert("clean_pipeline_correlation", similarity);
                    const bool retainedSpeech = retained > -12 && retained < 6 && similarity > 0.6;
                    variant.insert("clean_retention_guard_passed", retainedSpeech);
                    // Preserve the original regression gate. Uncalibrated quiet/new
                    // recordings in the stress matrix report degradation explicitly.
                    if (!extended_) {
                        QVERIFY2(retained > -12 && retained < 6, qPrintable(QString("Clean speech level changed by %1 dB").arg(retained)));
                        QVERIFY2(similarity > 0.6, qPrintable(QString("Clean speech correlation %1").arg(similarity)));
                    }
                }
            } else if (!interference.isEmpty()) {
                variant.insert("precodec_level_vs_input_db", levelChange(body(raw), body(input)));
                QVERIFY(levelChange(body(aligned), body(baseline)) < 6);
                if (!extended_ && categories_[interference] == "synthetic-transient" && strength == 100)
                    QVERIFY2(levelChange(body(aligned), body(baseline)) < -10, "Synthetic clicks must be attenuated by at least 10 dB");
            } else {
                QVERIFY(rms(aligned) < 0.0001);
            }
            if (exportAudio) {
                QVERIFY(writeWav(path("app-" + QString::number(strength)), aligned));
                QVERIFY(writeWav(path("precodec-" + QString::number(strength)), raw));
            }
            variants.append(variant);
        }
        row.insert("variants", variants); results_.append(row);
    }
    void cleanupTestCase() {
        if (outputDir_.isEmpty()) return;
        QJsonObject report{{"version", 2}, {"fixture_manifest_sha256", QString::fromLatin1(manifestHash_)},
            {"suite", extended_ ? "extended" : "smoke"}, {"shard", shard_}, {"shards", shards_},
            {"total_case_count", totalCases_}, {"selected_case_count", selectedCases_},
            {"sample_rate", rate}, {"opus_version", opus_get_version_string()}, {"opus_delay_samples", codecDelay_},
            {"rnnoise_delay_samples", 960}, {"measurement_interval_seconds", QJsonArray({2, 10})},
            {"echo_measurement_interval_seconds", QJsonArray({5, 9})},
            {"scope", "Artificial mixtures. Precodec SI-SDR is library-level RNNoise, not perceived quality or speaker identity. Pipeline levels use the actual app codec/mixer with output normalization off. Passed guards are not product audio acceptance."},
            {"cases", results_}, {"echo_cases", echoResults_}, {"input_cases", inputResults_}};
        QSaveFile output(outputDir_ + "/report.json");
        QVERIFY(output.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(report).toJson();
        QCOMPARE(output.write(bytes), bytes.size());
        QVERIFY(output.commit());
    }
};

QTEST_GUILESS_MAIN(CorpusTests)
#include "corpus_tests.moc"
