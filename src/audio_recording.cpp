#include "audio_recording.hpp"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>
#include <array>
#include <bit>

namespace {
struct Phase { int seconds; const char* title; const char* instruction; bool speaking; };
constexpr std::array phases{
    Phase{5, QT_TRANSLATE_NOOP("AudioRecording", "Room sound"), QT_TRANSLATE_NOOP("AudioRecording", "Keep music off and stay quiet. Start when ready."), false},
    Phase{30, QT_TRANSLATE_NOOP("AudioRecording", "Your voice"), QT_TRANSLATE_NOOP("AudioRecording", "Sit at your usual distance. Read the sentence, then stop recording."), true},
    Phase{10, QT_TRANSLATE_NOOP("AudioRecording", "Background sound"), QT_TRANSLATE_NOOP("AudioRecording", "Play music or another background sound at a normal volume. Stay quiet while recording."), false},
    Phase{30, QT_TRANSLATE_NOOP("AudioRecording", "Voice with background sound"), QT_TRANSLATE_NOOP("AudioRecording", "Keep the background volume unchanged. Read the sentence, then stop recording."), true},
    Phase{30, QT_TRANSLATE_NOOP("AudioRecording", "Voice farther away"), QT_TRANSLATE_NOOP("AudioRecording", "Move about one meter from the microphone. Keep the background sound playing and read the sentence."), true}
};
}

AudioRecording::AudioRecording(QString directory, QObject* parent)
    : QObject(parent), directory_(QDir(directory).absolutePath()) {}

QString AudioRecording::sentence() const {
    return tr("The window is open. I can hear the rain outside. One, two, three, four, five.");
}

QString AudioRecording::title() const {
    return complete_ ? tr("Recordings saved") : tr(phases[phase_].title);
}
QString AudioRecording::instruction() const { return complete_ ? tr("You can stop the background sound now.") : tr(phases[phase_].instruction); }
bool AudioRecording::speaking() const { return !complete_ && phases[phase_].speaking; }
int AudioRecording::remainingSeconds() const {
    return active() ? phases[phase_].seconds - int(frames_ / format_.sampleRate()) : 0;
}
double AudioRecording::progress() const {
    return format_.sampleRate() > 0 ? double(frames_) / (qint64(format_.sampleRate()) * phases[phase_].seconds) : 0;
}

QByteArray AudioRecording::header() const {
    const bool floating = format_.sampleFormat() == QAudioFormat::Float;
    const auto dataBytes = quint32(frames_ * format_.bytesPerFrame());
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(dataBytes + (dataBytes % 2) + (floating ? 50 : 36));
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(floating ? 18 : 16) << quint16(floating ? 3 : 1)
        << quint16(format_.channelCount()) << quint32(format_.sampleRate())
        << quint32(format_.sampleRate() * format_.bytesPerFrame())
        << quint16(format_.bytesPerFrame()) << quint16(format_.bytesPerSample() * 8);
    if (floating) {
        out << quint16(0);
        out.writeRawData("fact", 4);
        out << quint32(4) << quint32(frames_);
    }
    out.writeRawData("data", 4);
    out << dataBytes;
    return bytes;
}

bool AudioRecording::start(const QAudioFormat& format, const QByteArray& deviceId, const QString& deviceName) {
    if (active() || complete_) return false;
    if (file_) { file_->cancelWriting(); file_.reset(); }
    resultPath_.clear(); error_.clear(); frames_ = 0; startedAt_ = 0;
    if (!format.isValid() || format.sampleRate() < 8000 || format.sampleRate() > 192000
        || format.channelCount() < 1 || format.channelCount() > 8
        || format.sampleFormat() < QAudioFormat::UInt8 || format.sampleFormat() > QAudioFormat::Float)
        return cancel(tr("This microphone format is not supported for test recordings."));
    format_ = format; deviceId_ = deviceId; deviceName_ = deviceName;
    if (!QDir().mkpath(directory_)) return cancel(tr("The recordings folder could not be created."));
    basePath_ = directory_ + "/audio-test-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")
        + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    file_ = std::make_unique<QSaveFile>(basePath_ + ".wav");
    if (!file_->open(QIODevice::WriteOnly) || !file_->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return cancel(tr("The test recording could not be created."));
    const auto bytes = header();
    if (file_->write(bytes) != bytes.size()) return cancel(tr("The WAV header could not be written."));
    capturing_ = true;
    emit changed();
    return true;
}

bool AudioRecording::append(QByteArrayView frames) {
    if (!active()) return false;
    if (frames.size() % format_.bytesPerFrame() != 0) return cancel(tr("Incomplete audio block. Recording discarded."));
    if (frames.empty()) return true;
    if (startedAt_ == 0) startedAt_ = QDateTime::currentMSecsSinceEpoch();
    const auto previousSecond = frames_ / format_.sampleRate();
    const auto remaining = (qint64(phases[phase_].seconds) * format_.sampleRate() - frames_) * format_.bytesPerFrame();
    const auto count = std::min(qint64(frames.size()), remaining);
    QByteArray littleEndian;
    const char* data = frames.data();
    if constexpr (std::endian::native == std::endian::big) {
        littleEndian = QByteArray(data, count);
        const auto width = format_.bytesPerSample();
        for (qsizetype i = 0; i < count; i += width) std::reverse(littleEndian.begin() + i, littleEndian.begin() + i + width);
        data = littleEndian.constData();
    }
    if (file_->write(data, count) != count) return cancel(tr("The test recording could not be saved completely."));
    frames_ += count / format_.bytesPerFrame();
    if (frames_ == qint64(phases[phase_].seconds) * format_.sampleRate()) return finish();
    if (previousSecond != frames_ / format_.sampleRate()) emit changed();
    return true;
}

bool AudioRecording::finish() {
    if (!active()) return false;
    if (frames_ < format_.sampleRate() / 2) return cancel(tr("Record at least half a second, then try again."));
    capturing_ = false;
    emit changed();
    return true;
}

bool AudioRecording::reset() {
    cancel(); phase_ = 0; complete_ = false; resultPath_.clear();
    emit changed(); return true;
}

bool AudioRecording::accept() {
    if (!reviewing()) return false;
    if ((frames_ * format_.bytesPerFrame()) % 2 != 0 && file_->write("\0", 1) != 1)
        return cancel(tr("The WAV file could not be finalized."));
    const auto& p = phases[phase_];
    const QJsonArray steps{QJsonObject{{"title", tr(p.title)}, {"instruction", tr(p.instruction)},
        {"startFrame", 0}, {"endFrame", frames_}, {"speaking", p.speaking}}};
    const auto json = QJsonDocument(QJsonObject{{"version", 2}, {"step", phase_}, {"startedAtUtcMs", startedAt_},
        {"deviceId", QString::fromLatin1(deviceId_.toHex())}, {"deviceName", deviceName_},
        {"sampleRate", format_.sampleRate()}, {"channels", format_.channelCount()},
        {"bitsPerSample", format_.bytesPerSample() * 8}, {"floatingPoint", format_.sampleFormat() == QAudioFormat::Float},
        {"frames", frames_}, {"appProcessing", "none"}, {"upstreamProcessing", "device/OS processing may be present"},
        {"sentence", sentence()}, {"phases", steps}}).toJson();
    QSaveFile metadata(basePath_ + ".json");
    if (!metadata.open(QIODevice::WriteOnly) || !metadata.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || metadata.write(json) != json.size() || !metadata.commit())
        return cancel(tr("The recording description could not be saved."));
    const auto bytes = header();
    if (!file_->seek(0) || file_->write(bytes) != bytes.size() || !file_->commit()) {
        QFile::remove(basePath_ + ".json");
        return cancel(tr("The WAV file could not be finalized."));
    }
    file_.reset();
    resultPath_ = basePath_ + ".wav";
    if (phase_ + 1 == stepCount()) complete_ = true; else ++phase_;
    frames_ = 0;
    emit changed();
    return true;
}

bool AudioRecording::cancel(const QString& reason) {
    if (file_) { file_->cancelWriting(); file_.reset(); }
    error_ = reason;
    capturing_ = false;
    frames_ = 0;
    emit changed();
    return reason.isEmpty();
}
