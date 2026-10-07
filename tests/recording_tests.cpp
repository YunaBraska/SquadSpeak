#include "audio_recording.hpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <bit>

class RecordingTests final : public QObject {
    Q_OBJECT
    static QAudioFormat format(QAudioFormat::SampleFormat type = QAudioFormat::Float, int rate = 8000, int channels = 1) {
        QAudioFormat value;
        value.setSampleFormat(type); value.setSampleRate(rate); value.setChannelCount(channels);
        return value;
    }
private slots:
    void wavPreservesDeviceBytesAndDescribesPhases_data() {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("channels"); QTest::addColumn<int>("rate");
        QTest::newRow("float mono") << int(QAudioFormat::Float) << 1 << 8000;
        QTest::newRow("signed16 stereo") << int(QAudioFormat::Int16) << 2 << 44100;
        QTest::newRow("signed32") << int(QAudioFormat::Int32) << 1 << 48000;
        QTest::newRow("unsigned8 odd length") << int(QAudioFormat::UInt8) << 1 << 8001;
    }
    void wavPreservesDeviceBytesAndDescribesPhases() {
        QFETCH(int, type); QFETCH(int, channels); QFETCH(int, rate);
        QTemporaryDir directory;
        AudioRecording recording(directory.path());
        const auto fmt = format(QAudioFormat::SampleFormat(type), rate, channels);
        QVERIFY(recording.start(fmt, "device-a", "Test microphone"));
        QVERIFY(!recording.start(fmt, "device-b", "Other microphone"));
        const qsizetype total = qsizetype(rate) * 5 * fmt.bytesPerFrame();
        QByteArray expected(total, '\0');
        for (qsizetype i = 0; i < total; ++i) expected[i] = char(i % 251);
        // Irregular blocks exercise writes across phase boundaries and exact final truncation.
        qsizetype offset = 0;
        while (recording.active()) {
            const auto size = std::min(qsizetype(997 * fmt.bytesPerFrame()), total - offset);
            auto block = expected.mid(offset, size);
            if (offset + size == total) block.append(QByteArray(32 * fmt.bytesPerFrame(), 'x'));
            QVERIFY(recording.append(block));
            offset += size;
        }
        QVERIFY(recording.error().isEmpty()); QCOMPARE(recording.progress(), 1.0);
        QVERIFY(recording.reviewing()); QVERIFY(recording.accept());
        QFile wave(recording.resultPath()); QVERIFY(wave.open(QIODevice::ReadOnly));
        const auto bytes = wave.readAll();
        const int headSize = type == QAudioFormat::Float ? 58 : 44;
        QCOMPARE(bytes.first(4), QByteArray("RIFF"));
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 4), bytes.size() - 8);
        QCOMPARE(bytes.mid(8, 8), QByteArray("WAVEfmt "));
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 20), type == QAudioFormat::Float ? 3 : 1);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 22), channels);
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 24), rate);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 32), fmt.bytesPerFrame());
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 34), fmt.bytesPerSample() * 8);
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + headSize - 4), total);
        if (type == QAudioFormat::Float) QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 46), rate * 5);
        if constexpr (std::endian::native == std::endian::big) {
            for (qsizetype i = 0; i < expected.size(); i += fmt.bytesPerSample())
                std::reverse(expected.begin() + i, expected.begin() + i + fmt.bytesPerSample());
        }
        QCOMPARE(bytes.mid(headSize, total), expected);
        QCOMPARE(bytes.size(), headSize + total + total % 2);
        QFile metadata(recording.resultPath().chopped(4) + ".json"); QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto json = QJsonDocument::fromJson(metadata.readAll()).object();
        QCOMPARE(json["sampleRate"].toInt(), rate); QCOMPARE(json["channels"].toInt(), channels);
        QCOMPARE(json["deviceName"].toString(), "Test microphone");
        QCOMPARE(json["appProcessing"].toString(), "none");
        QCOMPARE(json["frames"].toInteger(), qint64(rate) * 5);
        const auto steps = json["phases"].toArray(); QCOMPARE(steps.size(), 1);
        qint64 end = 0;
        for (const auto step : steps) {
            QCOMPARE(step.toObject()["startFrame"].toInteger(), end);
            end = step.toObject()["endFrame"].toInteger();
        }
        QCOMPARE(end, qint64(rate) * 5);
        QVERIFY(!recording.append("unexpected"));
    }
    void aCompletedStepWaitsForTheUser() {
        QTemporaryDir directory;
        AudioRecording recording(directory.path());
        QVERIFY(recording.start(format(), "a", "Mic"));
        QVERIFY(recording.append(QByteArray(8000 * 4 * 5, '\0')));
        QVERIFY(!recording.active());
        QVERIFY(recording.resultPath().isEmpty());
        QCOMPARE(QDir(directory.path()).entryList({"*.wav"}, QDir::Files).size(), 0);
    }
    void eachStepCanBeRetriedAndAcceptedIndependently() {
        QTemporaryDir directory;
        AudioRecording recording(directory.path());
        QVERIFY(!recording.finish()); QVERIFY(!recording.accept());
        for (int step = 0; step < recording.stepCount(); ++step) {
            QCOMPARE(recording.step(), step);
            QVERIFY(recording.start(format(), "a", "Mic"));
            QVERIFY(recording.append(QByteArray(8000 * 4, 'a')));
            QVERIFY(recording.finish()); QVERIFY(recording.reviewing());
            QCOMPARE(recording.step(), step);
            QVERIFY(recording.start(format(), "a", "Mic"));
            QVERIFY(!recording.reviewing());
            QVERIFY(recording.append(QByteArray(8000 * 4 * 2, 'b')));
            QVERIFY(recording.finish()); QVERIFY(recording.accept());
            QCOMPARE(QDir(directory.path()).entryList({"*.wav"}, QDir::Files).size(), step + 1);
            QFile file(recording.resultPath()); QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll().mid(58), QByteArray(8000 * 4 * 2, 'b'));
        }
        QVERIFY(recording.complete()); QVERIFY(!recording.start(format(), "a", "Mic"));
        QVERIFY(recording.reset()); QCOMPARE(recording.step(), 0); QVERIFY(!recording.complete());
        QVERIFY(recording.start(format(), "a", "Mic"));
        QVERIFY(recording.append(QByteArray(4, 'x')));
        QVERIFY(!recording.finish()); QVERIFY(!recording.reviewing()); QVERIFY(!recording.error().isEmpty());
        QCOMPARE(QDir(directory.path()).entryList({"*.wav"}, QDir::Files).size(), 5);
    }
    void invalidFormatAndPartialFrameNeverProduceACompletedRecording() {
        QTemporaryDir directory;
        AudioRecording recording(directory.path());
        QVERIFY(!recording.start(QAudioFormat{}, "a", "Mic")); QVERIFY(!recording.error().isEmpty());
        QVERIFY(recording.start(format(), "a", "Mic"));
        QVERIFY(!recording.append(QByteArray("xyz"))); QVERIFY(!recording.active());
        QVERIFY(recording.resultPath().isEmpty()); QVERIFY(!recording.error().isEmpty());
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 0);
        QVERIFY(recording.start(format(), "a", "Mic")); QVERIFY(recording.error().isEmpty());
    }
    void unwritableDestinationFailsWithoutOverwriting() {
        QTemporaryDir directory;
        QFile existing(directory.filePath("keep")); QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("unchanged"); existing.close();
        AudioRecording recording(existing.fileName() + "/recordings");
        QVERIFY(!recording.start(format(), "a", "Mic")); QVERIFY(!recording.error().isEmpty());
        QVERIFY(existing.open(QIODevice::ReadOnly)); QCOMPARE(existing.readAll(), QByteArray("unchanged"));
    }
};
QTEST_GUILESS_MAIN(RecordingTests)
#include "recording_tests.moc"
