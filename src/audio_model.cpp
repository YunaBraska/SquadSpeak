#include "audio_model.hpp"
#ifdef Q_OS_IOS
#include "ios_audio_session.hpp"
#endif

#include <QCoreApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QPermission>
#include <QPointer>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {
QVariantList deviceList(const QList<QAudioDevice>& devices) {
    QVariantList result{QVariantMap{{"label", QCoreApplication::translate("AudioModel", "System default")}, {"deviceId", ""}}};
    for (const auto& device : devices)
        result.push_back(QVariantMap{{"label", device.description()}, {"deviceId", QString::fromLatin1(device.id().toHex())}});
    return result;
}
int selectedIndex(const QList<QAudioDevice>& devices, const QByteArray& selected) {
    if (selected.isEmpty()) return 0;
    const auto match = std::find_if(devices.begin(), devices.end(), [&](const auto& device) { return device.id() == selected; });
    return match == devices.end() ? -1 : int(std::distance(devices.begin(), match)) + 1;
}
QVariantList spectrum(const std::array<double, squad::AudioLevels::spectrumSize>& values) {
    QVariantList result;
    result.reserve(values.size());
    for (const auto value : values) result.push_back(value);
    return result;
}
}

AudioModel::AudioModel(const QString& settingsFile, QObject* parent) try
    : QObject(parent), profiles_(settingsFile),
      language_(settingsFile.isEmpty() ? QString{} : settingsFile + ".languages.json", this) {
    connect(&language_, &LocalLanguage::capabilitiesChanged, this, [this] { setAppLanguage(botLanguage_); });
    setAppLanguage("en");
    connect(&devices_, &QMediaDevices::audioInputsChanged, this, &AudioModel::refreshDevices);
    connect(&devices_, &QMediaDevices::audioOutputsChanged, this, &AudioModel::refreshDevices);
    connect(this, &AudioModel::statusChanged, this, &AudioModel::readinessChanged);
    connect(this, &AudioModel::devicesChanged, this, &AudioModel::readinessChanged);
    meter_.setInterval(50);
    connect(&meter_, &QTimer::timeout, this, [this] {
        if (!processor_ || !wantsCapture_) return;
        levels_ = processor_->levels();
        filterResponse_ = processor_->filterResponse();
        emit levelsChanged();
    });
    levels_.inputSpectrum.fill(-96);
    levels_.outputSpectrum.fill(-96);
    clock_.start();
    playbackTimer_.setInterval(10);
    playbackTimer_.setTimerType(Qt::PreciseTimer);
    connect(&playbackTimer_, &QTimer::timeout, this, [this] {
        playVoice();
        if (clock_.elapsed() - activityPublishedAt_ >= 50) {
            activityPublishedAt_ = clock_.elapsed(); emit activityChanged();
        }
    });
    refreshDevices();
} catch (const std::exception& error) {
    throw std::runtime_error(tr(error.what()).toUtf8().constData());
}

QVariantList AudioModel::inputs() const { return deviceList(QMediaDevices::audioInputs()); }
QVariantList AudioModel::outputs() const { return deviceList(QMediaDevices::audioOutputs()); }
int AudioModel::inputIndex() const { return selectedIndex(QMediaDevices::audioInputs(), profiles_.selection(true)); }
int AudioModel::outputIndex() const { return selectedIndex(QMediaDevices::audioOutputs(), profiles_.selection(false)); }
QString AudioModel::inputName() const { return input_.isNull() ? tr("No selected microphone is available") : input_.description(); }
QString AudioModel::outputName() const { return output_.isNull() ? tr("No selected output is available") : output_.description(); }
QVariantList AudioModel::inputSpectrum() const { return spectrum(levels_.inputSpectrum); }
QVariantList AudioModel::outputSpectrum() const { return spectrum(levels_.outputSpectrum); }
QVariantList AudioModel::filterResponse() const { return spectrum(filterResponse_); }
double AudioModel::maximumFrequency() const { return input_.isNull() ? 20000 : std::min(20000.0, captureFormat(input_).sampleRate() * 0.45); }
bool AudioModel::inputReady() const {
    return inputAvailable() && !inputFailed_ && qApp->checkPermission(QMicrophonePermission{}) == Qt::PermissionStatus::Granted;
}

QAudioDevice AudioModel::resolve(const QList<QAudioDevice>& devices, const QByteArray& selected, bool input) {
    if (selected.isEmpty()) return input ? QMediaDevices::defaultAudioInput() : QMediaDevices::defaultAudioOutput();
    const auto match = std::find_if(devices.begin(), devices.end(), [&](const auto& device) { return device.id() == selected; });
    return match == devices.end() ? QAudioDevice{} : *match;
}

QAudioFormat AudioModel::captureFormat(const QAudioDevice& device) {
    QAudioFormat preferred;
    preferred.setSampleRate(48000);
    preferred.setChannelCount(2);
    preferred.setSampleFormat(QAudioFormat::Float);
    return device.isFormatSupported(preferred) ? preferred : device.preferredFormat();
}

void AudioModel::setStatus(QString status, bool input) {
    (input ? inputStatus_ : outputStatus_) = std::move(status);
    emit statusChanged();
}

void AudioModel::refreshDevices() {
    const auto input = resolve(QMediaDevices::audioInputs(), profiles_.selection(true), true);
    const auto output = resolve(QMediaDevices::audioOutputs(), profiles_.selection(false), false);
    const bool inputChanged = input.id() != input_.id()
        || (!input.isNull() && captureFormat(input) != captureFormat(input_));
    const bool outputChanged = output.id() != output_.id()
        || (!output.isNull() && captureFormat(output) != captureFormat(output_));
    const bool resume = wantsCapture_;
    if (inputChanged) {
        stopCapture();
        wantsCapture_ = resume;
        input_ = input;
        inputFailed_ = false;
        profile_ = {};
        noiseSuppression_ = 0;
        echoSuppression_.reset();
        echo_.setSuppression(echoSuppression_).reset();
        mixer_.setNoiseSuppression(0);
        activation_ = {};
        applyActivation();
        filterResponse_.fill(0.0);
    }
    if (outputChanged) {
        outputMonitor_.stop();
        outputReferenceAt_ = -1; deviceReference_ = false;
        echo_.reset();
        sink_.reset();
        tone_.close();
        testingOutput_ = false;
        voiceSink_.reset(); playback_ = nullptr;
        mixer_.resetPlayback();
        participantVolumes_.clear();
        cancelNotifications();
        output_ = output;
        outputFailed_ = false;
        outputVolume_ = 0.5;
        musicVolume_ = 0.175;
        notificationVolume_ = 0.5;
        readBotMessages_ = false;
        botVoiceId_.clear();
    }
    try {
        if (inputChanged && !input_.isNull()) {
            profile_ = profiles_.input(input_.id());
            filterResponse_ = squad::AudioProcessor(captureFormat(input_).sampleRate(), profile_).filterResponse();
            noiseSuppression_ = profiles_.noiseSuppression(input_.id());
            mixer_.setNoiseSuppression(profile_.noiseEnabled ? noiseSuppression_ : 0);
            echoSuppression_ = profiles_.echoSuppression(input_.id());
            echo_.setSuppression(profile_.echoEnabled ? echoSuppression_ : std::optional(0.0));
            activation_ = profiles_.voiceActivation(input_.id());
            applyActivation();
        }
    } catch (const std::exception& error) {
        setStatus(tr(error.what()));
    }
    try {
        if (outputChanged && !output_.isNull()) {
            setStatus({}, false);
            outputVolume_ = profiles_.output(output_.id());
            musicVolume_ = profiles_.musicVolume(output_.id());
            notificationVolume_ = profiles_.notificationVolume(output_.id());
            readBotMessages_ = profiles_.readBotMessages(output_.id());
            botVoiceId_ = profiles_.botVoice(output_.id());
            automaticVolume_ = profiles_.automaticVolume(output_.id());
            mixer_.setAutomatic(automaticVolume_);
            setParticipants(participants_, language_.channel());
            beginPlayback();
            monitorOutput();
        }
    } catch (const std::exception& error) {
        setStatus(tr(error.what()), false);
    }
    if (outputChanged) setAppLanguage(botLanguage_);
    if (inputChanged && (wantsCapture_ || (voiceActive_ && transmitEnabled_))) {
        if (input_.isNull()) setStatus(tr("Microphone disconnected. Recording paused until the device returns."));
        else ensureCapture();
    }
    emit runningChanged();
    emit devicesChanged();
    emit profileChanged();
    emit levelsChanged();
    emit activityChanged();
}

bool AudioModel::select(bool input, const QString& deviceId) {
    const auto devices = input ? QMediaDevices::audioInputs() : QMediaDevices::audioOutputs();
    const auto match = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
        return QString::fromLatin1(device.id().toHex()) == deviceId;
    });
    if (!deviceId.isEmpty() && match == devices.end()) {
        setStatus(tr("The selected audio device is no longer available."), input);
        return false;
    }
    try {
        profiles_.select(input, deviceId.isEmpty() ? QByteArray{} : match->id());
        setStatus({}, input);
        refreshDevices();
        return true;
    } catch (const std::exception& error) {
        setStatus(tr(error.what()), input);
        return false;
    }
}

bool AudioModel::selectInput(const QString& deviceId) { return select(true, deviceId); }
bool AudioModel::selectOutput(const QString& deviceId) { return select(false, deviceId); }
QVariantMap AudioModel::filtersEnabled() const {
    return {{"gain", profile_.gainEnabled}, {"highPass", profile_.highPassEnabled}, {"lowPass", profile_.lowPassEnabled}, {"noise", profile_.noiseEnabled}, {"echo", profile_.echoEnabled}};
}

bool AudioModel::setFilterEnabled(const QString& filter, bool enabled) {
    auto next = profile_;
    if (filter == "gain") next.gainEnabled = enabled;
    else if (filter == "highPass") next.highPassEnabled = enabled;
    else if (filter == "lowPass") next.lowPassEnabled = enabled;
    else if (filter == "noise") next.noiseEnabled = enabled;
    else if (filter == "echo") next.echoEnabled = enabled;
    else return false;
    if (next == profile_) return true;
    return updateProfile(next);
}

bool AudioModel::setGainDb(double value) { auto next = profile_; next.gainDb = value; next.gainAutomatic = false; return updateProfile(next); }
bool AudioModel::setHighPassHz(double value) { auto next = profile_; next.highPassHz = value; next.highPassAutomatic = false; return updateProfile(next); }
bool AudioModel::setLowPassHz(double value) { auto next = profile_; next.lowPassHz = value; return updateProfile(next); }
bool AudioModel::setGainAutomatic(bool value) { auto next = profile_; next.gainDb = effectiveGainDb(); next.gainAutomatic = value; return updateProfile(next); }
bool AudioModel::setHighPassAutomatic(bool value) { auto next = profile_; next.highPassHz = effectiveHighPassHz(); next.highPassAutomatic = value; return updateProfile(next); }

bool AudioModel::setNoiseSuppression(double value) {
    if (input_.isNull()) return false;
    try {
        profiles_.saveNoiseSuppression(input_.id(), value);
        mixer_.setNoiseSuppression(profile_.noiseEnabled ? value : 0);
        noiseSuppression_ = value;
        emit profileChanged();
        emit activityChanged();
        return true;
    } catch (const std::exception& error) {
        setStatus(tr(error.what()));
        emit profileChanged();
        return false;
    }
}

bool AudioModel::setActivationAutomatic(bool automatic) {
    auto next = activation_;
    next.automatic = automatic;
    next.thresholdDb = mixer_.activationThresholdDb();
    return updateActivation(next);
}

bool AudioModel::setActivationThresholdDb(double value) {
    auto next = activation_;
    next.thresholdDb = value;
    next.automatic = false;
    return updateActivation(next);
}

bool AudioModel::updateActivation(squad::VoiceActivation activation) {
    if (input_.isNull()) return false;
    try {
        profiles_.saveVoiceActivation(input_.id(), activation);
        activation_ = activation;
        applyActivation();
        emit profileChanged();
        emit activityChanged();
        return true;
    } catch (const std::exception& error) {
        setStatus(tr(error.what()));
        emit profileChanged();
        emit activityChanged();
        return false;
    }
}

bool AudioModel::updateProfile(squad::InputProfile profile) {
    if (input_.isNull()) return false;
    try {
        // Validate at the boundary before persistence or changing the live path.
        const squad::AudioProcessor validated(captureFormat(input_).sampleRate(), profile);
        profiles_.saveInput(input_.id(), profile);
        const bool echoChanged = profile_.echoEnabled != profile.echoEnabled;
        profile_ = profile;
        mixer_.setNoiseSuppression(profile_.noiseEnabled ? noiseSuppression_ : 0);
        if (echoChanged) {
            echo_.setSuppression(profile_.echoEnabled ? echoSuppression_ : std::optional(0.0));
            mixer_.resetCapture();
            monitorOutput();
        }
        filterResponse_ = validated.filterResponse();
        if (processor_) processor_->configure(profile);
        emit profileChanged();
        emit levelsChanged();
        return true;
    } catch (const std::exception& error) {
        setStatus(tr(error.what()));
        emit profileChanged();
        return false;
    }
}

bool AudioModel::setDeafened(bool deafened) {
    if (deafened_ == deafened) return true;
    deafened_ = deafened;
    echo_.reset();
    if (sink_) sink_->setVolume(deafened_ ? 0 : outputVolume_);
    if (voiceSink_) voiceSink_->setVolume(deafened_ ? 0 : 1);
    if (deafened_) { cancelNotifications(); language_.suspend(true); }
    emit profileChanged();
    return true;
}

bool AudioModel::setEchoAutomatic(bool automatic) {
    return updateEcho(automatic ? std::nullopt : std::optional(echoStrength()));
}

bool AudioModel::setEchoStrength(double strength) { return updateEcho(strength); }

bool AudioModel::updateEcho(std::optional<double> strength) {
    if (input_.isNull()) return false;
    if (strength == echoSuppression_) return true;
    try {
        profiles_.saveEchoSuppression(input_.id(), strength);
        const bool wasEnabled = echoCancellation();
        echoSuppression_ = strength;
        echo_.setSuppression(profile_.echoEnabled ? strength : std::optional(0.0));
        if (wasEnabled != echoCancellation()) monitorOutput();
        mixer_.resetCapture();
        emit profileChanged();
        return true;
    } catch (const std::exception& error) { setStatus(tr(error.what())); return false; }
}

bool AudioModel::playEvent(const QString& kind) {
    if (!eventAudioEnabled()) return false;
    QString sound;
    if (kind == "join" || kind == "memberJoin") sound = "join";
    else if (kind == "leave" || kind == "memberLeave") sound = "leave";
    else if (kind == "request") sound = "message";
    else if (kind == "kick" || kind == "ban" || kind == "message" || kind == "announcement") sound = kind;
    else return false;
    return playSound(sound, notificationVolume_);
}

bool AudioModel::previewOutput(double volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1) return false;
    return playSound("message", volume);
}

bool AudioModel::playSound(const QString& sound, double volume) {
    eventSound_.stop();
    if (deafened_ || !outputReady() || volume == 0) return false;
    eventSound_.setAudioDevice(output_);
    eventSound_.setVolume(volume);
    eventSound_.setSource(QUrl("qrc:/qt/qml/SquadSpeak/ui/sounds/" + sound + ".wav"));
    eventSound_.play();
    return true;
}

bool AudioModel::setOutputVolume(double value) {
    if (output_.isNull()) return false;
    try {
        profiles_.saveOutput(output_.id(), value);
        echo_.reset();
        outputVolume_ = value;
        if (sink_) sink_->setVolume(deafened_ ? 0 : value);
        applyOutputVolumes();
        return true;
    } catch (const std::exception& error) {
        setStatus(tr(error.what()), false);
        return false;
    }
}

bool AudioModel::start() {
    cancelNotifications();
    wantsCapture_ = true;
    emit runningChanged();
    return ensureCapture();
}

void AudioModel::cancelNotifications() {
    eventSound_.stop();
    cancelBotMessages();
}

void AudioModel::cancelBotMessages() {
    speech_.stop();
    announcements_.clear();
}

bool AudioModel::setNotificationVolume(double value) {
    if (output_.isNull()) return false;
    try {
        profiles_.saveNotificationVolume(output_.id(), value);
        notificationVolume_ = value;
        eventSound_.setVolume(deafened_ ? 0 : value);
        if (value == 0) cancelNotifications();
        emit profileChanged();
        return true;
    } catch (const std::exception& error) { setStatus(tr(error.what()), false); return false; }
}

bool AudioModel::setReadBotMessages(bool enabled) {
    if (!speechAvailable() || output_.isNull()) return false;
    try {
        profiles_.saveReadBotMessages(output_.id(), enabled);
        readBotMessages_ = enabled;
        if (enabled) language_.refreshVoices();
        cancelNotifications();
        emit profileChanged();
        return true;
    } catch (const std::exception& error) { setStatus(tr(error.what()), false); return false; }
}

void AudioModel::setAppLanguage(const QString& language) {
    language_.setAppLanguage(language);
    const auto voice = SpeechOutput::preferredVoice(language_.voices(), language == "en" ? "en-GB" : language, botVoiceId_);
    if (botLanguage_ == language && botVoice_ == voice) return;
    cancelBotMessages();
    botLanguage_ = language;
    botVoice_ = voice;
    emit profileChanged();
}

bool AudioModel::setBotVoice(const QString& id) {
    if (!speechAvailable() || output_.isNull()) return false;
    if (!id.isEmpty() && SpeechOutput::preferredVoice(language_.voices(), botLanguage_, id).isEmpty()) return false;
    if (botVoiceId_ == id) return true;
    try {
        profiles_.saveBotVoice(output_.id(), id);
        botVoiceId_ = id;
        cancelBotMessages();
        setAppLanguage(botLanguage_);
        emit profileChanged();
        return true;
    } catch (const std::exception& error) { setStatus(tr(error.what()), false); return false; }
}

bool AudioModel::readBotMessage(const QString& text) {
    if (!botSpeechReady() || !eventAudioEnabled() || !voiceActive_ || testActive()
            || text.trimmed().isEmpty()) return false;
    // Bound both announcement bursts and long free-form messages. Expired queued
    // announcements are discarded during playback, never replayed after unmute.
    if (announcements_.size() == 3) announcements_.pop_front();
    auto plain = text.simplified();
    if (plain.size() > 400) {
        plain.truncate(397);
        if (plain.back().isHighSurrogate()) plain.chop(1);
        plain += "...";
    }
    announcements_.emplace_back(std::move(plain), clock_.elapsed());
    return true;
}

bool AudioModel::ensureCapture() {
    if (!wantsCapture_ && !(voiceActive_ && transmitEnabled_)) { stopCapture(); return true; }
    if (source_) return true;
    QMicrophonePermission permission;
    switch (qApp->checkPermission(permission)) {
    case Qt::PermissionStatus::Undetermined:
        if (permissionPending_) return true;
        permissionPending_ = true;
        setStatus(tr("Allow microphone access in the system dialog."));
        qApp->requestPermission(permission, this, [this](const QPermission& granted) {
            permissionPending_ = false;
            if (granted.status() != Qt::PermissionStatus::Granted) {
                wantsCapture_ = false; emit runningChanged(); setStatus(tr("Microphone access was denied."));
                emit microphoneAccessDenied();
            } else if (wantsCapture_ || (voiceActive_ && transmitEnabled_)) ensureCapture();
        });
        return true;
    case Qt::PermissionStatus::Denied:
        wantsCapture_ = false;
        emit runningChanged();
        setStatus(tr("Microphone access is missing. Allow it in System Settings."));
        emit microphoneAccessDenied();
        return false;
    case Qt::PermissionStatus::Granted:
        if (input_.isNull()) {
            wantsCapture_ = false; emit runningChanged();
            setStatus(tr("No microphone available.")); return false;
        }
        beginCapture();
        return source_ != nullptr;
    }
    return false;
}

void AudioModel::beginCapture() {
    if (input_.isNull() || source_ || (!wantsCapture_ && !(voiceActive_ && transmitEnabled_))) return;
    try {
        format_ = captureFormat(input_);
        if (!format_.isValid()) throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "The microphone has no supported audio format"));
#ifdef Q_OS_IOS
        QString sessionError;
        if (!activateIosAudioSession(sessionError))
            throw std::runtime_error(sessionError.isEmpty()
                ? QT_TRANSLATE_NOOP("AudioModel", "Microphone could not be opened") : sessionError.toUtf8().constData());
#endif
        processor_ = std::make_unique<squad::AudioProcessor>(format_.sampleRate(), profile_);
        source_ = std::make_unique<QAudioSource>(input_, format_);
        source_->setBufferSize(format_.bytesForDuration(50000));
        connect(source_.get(), &QAudioSource::stateChanged, this, [this](QtAudio::State state) {
            if (source_ && state == QtAudio::StoppedState && source_->error() != QtAudio::NoError) {
                const QPointer<QAudioSource> failed = source_.get();
                QTimer::singleShot(0, this, [this, failed] {
                    if (!failed || failed != source_.get()) return;
                    inputFailed_ = true;
                    stopCapture();
                    setStatus(tr("Microphone capture was stopped by the audio device."));
                });
            }
        });
        capture_ = source_->start();
        if (!capture_ || source_->error() != QtAudio::NoError)
            throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Microphone could not be opened"));
        connect(capture_, &QIODevice::readyRead, this, &AudioModel::consume);
        inputFailed_ = false;
        monitorOutput();
        meter_.start();
        setStatus({});
        emit runningChanged();
    } catch (const std::exception& error) {
        inputFailed_ = true;
        stopCapture();
        setStatus(tr(error.what()));
    }
}

void AudioModel::consume() {
    if (!capture_ || !processor_) return;
    pending_.append(capture_->readAll());
    const auto frameBytes = format_.bytesPerFrame();
    const auto usable = (pending_.size() / frameBytes) * frameBytes;
    const auto sampleBytes = format_.bytesPerSample();
    std::vector<float> processed;
    processed.reserve(size_t(usable / frameBytes) * 2);
    for (qsizetype offset = 0; offset < usable; offset += frameBytes) {
        std::array<float, 2> frame{};
        for (int channel = 0; channel < 2; ++channel) {
            const int sourceChannel = format_.channelCount() == 1 ? 0 : channel;
            const auto value = format_.normalizedSampleValue(pending_.constData() + offset + sourceChannel * sampleBytes);
            frame[channel] = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
        }
        processor_->observeInput(std::abs(frame[0]) >= std::abs(frame[1]) ? frame[0] : frame[1]);
        processed.insert(processed.end(), frame.begin(), frame.end());
    }
    pending_.remove(0, usable);
    try {
        auto reference = outputMonitor_.take();
        const auto now = clock_.elapsed();
        if (!reference.samples.empty()) {
            if (!deviceReference_ || reference.discontinuity) echo_.reset();
            deviceReference_ = true; outputReferenceAt_ = now;
            echo_.render(reference.samples, reference.sampleRate, now, 0);
        } else if (deviceReference_ && now - outputReferenceAt_ > 250) {
            deviceReference_ = false; echo_.reset();
        }
        // Qt provides no capture timestamp. The callback size is not a
        // hardware latency measurement; AEC estimates the remaining delay.
        processed = echo_.capture(processed, format_.sampleRate(), clock_.elapsed(), 0);
        for (size_t frame = 0; frame < processed.size(); frame += 2) {
            const auto filtered = processor_->filter({processed[frame], processed[frame + 1]});
            processed[frame] = filtered[0]; processed[frame + 1] = filtered[1];
        }
        const auto packets = mixer_.encode(processed, format_.sampleRate(), voiceActive_ && transmitEnabled_ && !testActive());
        if (!packets.isEmpty()) lastPacketAt_ = clock_.elapsed();
        for (const auto& packet : packets) emit audioPacket(packet);
        emit activityChanged();
    } catch (const std::exception& error) {
        echo_.reset();
        mixer_.resetCapture();
        setStatus(tr(error.what()));
        emit activityChanged();
    }
}

void AudioModel::monitorOutput(bool retry) {
    outputMonitor_.stop();
    outputReferenceAt_ = -1; deviceReference_ = false;
    echo_.reset();
    if (source_ && echoCancellation() && !output_.isNull())
        outputMonitor_.start(output_.id(), retry);
    emit activityChanged();
}

bool AudioModel::retryEchoReference() {
    if (!echoRetryAvailable()) return false;
    monitorOutput(true);
    return true;
}

QString AudioModel::echoReference() const {
    if (!source_ || !echoCancellation()) return {};
    return deviceReference_ ? tr("Speaker audio") : tr("App audio only");
}

bool AudioModel::microphonePermissionDenied() const {
    return qApp->checkPermission(QMicrophonePermission{}) == Qt::PermissionStatus::Denied;
}

bool AudioModel::openMicrophoneSettings() {
#if defined(Q_OS_MACOS)
    const QUrl url("x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone");
#elif defined(Q_OS_IOS)
    const QUrl url("app-settings:");
#elif defined(Q_OS_WIN)
    const QUrl url("ms-settings:privacy-microphone");
#else
    setStatus(tr("Allow microphone access in your system's privacy or app settings."));
    return false;
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS) || defined(Q_OS_WIN)
    if (QDesktopServices::openUrl(url)) return true;
    setStatus(tr("Open your system's privacy settings and allow microphone access for SquadSpeak."));
    return false;
#endif
}

bool AudioModel::stop() {
    wantsCapture_ = false;
    testingOutput_ = false;
    sink_.reset(); tone_.close();
    emit runningChanged();
    return ensureCapture();
}

void AudioModel::stopCapture() {
    outputMonitor_.stop();
    outputReferenceAt_ = -1; deviceReference_ = false;
    echo_.reset();
    meter_.stop();
    capture_ = nullptr;
    if (source_) {
        source_->disconnect(this);
        source_->stop();
        source_.reset();
    }
    processor_.reset();
    if (!input_.isNull()) filterResponse_ = squad::AudioProcessor(captureFormat(input_).sampleRate(), profile_).filterResponse();
    pending_.clear();
    mixer_.resetCapture();
    levels_ = {};
    levels_.inputSpectrum.fill(-96);
    levels_.outputSpectrum.fill(-96);
    setStatus(tr("Microphone preview is off."));
    emit runningChanged();
    emit levelsChanged();
    emit activityChanged();
}

bool AudioModel::testOutput() {
    if (output_.isNull()) return false;
    cancelNotifications();
    sink_.reset();
    tone_.close();
    const auto format = output_.preferredFormat();
    if (!format.isValid()) { setStatus(tr("The output provides no usable audio format."), false); return false; }
    const auto frames = format.sampleRate() / 3;
    QByteArray data(frames * format.bytesPerFrame(), '\0');
    for (int frame = 0; frame < frames; ++frame) {
        const auto edge = std::min({1.0, double(frame) / (format.sampleRate() * 0.02), double(frames - frame - 1) / (format.sampleRate() * 0.02)});
        const auto value = 0.15 * edge * std::sin(2 * std::numbers::pi * 440 * frame / format.sampleRate());
        for (int channel = 0; channel < format.channelCount(); ++channel) {
            auto* dest = data.data() + frame * format.bytesPerFrame() + channel * format.bytesPerSample();
            switch (format.sampleFormat()) {
            case QAudioFormat::Float: { const float v = value; std::memcpy(dest, &v, sizeof(v)); break; }
            case QAudioFormat::Int16: { const qint16 v = qRound(value * 32767); std::memcpy(dest, &v, sizeof(v)); break; }
            case QAudioFormat::Int32: { const qint32 v = qRound64(value * 2147483647); std::memcpy(dest, &v, sizeof(v)); break; }
            case QAudioFormat::UInt8: *dest = char(qRound((value + 1) * 127.5)); break;
            default: setStatus(tr("The output format is not supported."), false); return false;
            }
        }
    }
    tone_.setData(data);
    tone_.open(QIODevice::ReadOnly);
    sink_ = std::make_unique<QAudioSink>(output_, format);
    sink_->setVolume(deafened_ ? 0 : outputVolume_);
    connect(sink_.get(), &QAudioSink::stateChanged, this, [this](QtAudio::State state) {
        if (sink_ && state == QtAudio::StoppedState && sink_->error() != QtAudio::NoError)
            setStatus(tr("The test tone could not be played."), false);
    });
    testingOutput_ = true;
    emit runningChanged();
    sink_->start(&tone_);
    connect(sink_.get(), &QAudioSink::stateChanged, this, [this](QtAudio::State state) {
        if (state == QtAudio::IdleState || state == QtAudio::StoppedState) {
            testingOutput_ = false;
            emit runningChanged();
        }
    });
    return true;
}

void AudioModel::applyActivation() {
    auto effective = activation_;
    effective.enabled = !pushToTalk_;
    mixer_.setVoiceActivation(effective);
}

bool AudioModel::setVoiceState(bool joined, bool transmit, bool pushToTalk) {
    if (voiceActive_ == joined && transmitEnabled_ == transmit && pushToTalk_ == pushToTalk) return true;
    const bool changedChannelState = voiceActive_ != joined;
    voiceActive_ = joined;
    transmitEnabled_ = transmit;
    pushToTalk_ = pushToTalk;
    applyActivation();
    mixer_.resetCapture();
    if (changedChannelState) {
        language_.suspend(!joined || deafened_ || testActive());
        cancelBotMessages();
        echo_.reset();
        mixer_.resetPlayback();
        voiceSink_.reset(); playback_ = nullptr;
        if (joined) { beginPlayback(); playbackTimer_.start(); }
        else playbackTimer_.stop();
    }
    emit activityChanged();
    return ensureCapture();
}

bool AudioModel::setParticipants(const QVariantList& participants, const QString& channel) {
    for (const auto& previous : participants_) {
        const auto id = previous.toMap().value("id").toString();
        const auto current = std::find_if(participants.begin(), participants.end(), [&](const auto& value) {
            return value.toMap().value("id").toString() == id;
        });
        if (current == participants.end() || current->toMap().value("muted").toBool()
            || !current->toMap().value("available").toBool()) mixer_.remove(id);
    }
    participants_ = participants;
    QVariantList translatedMembers;
    for (const auto& entry : participants) {
        const auto member = entry.toMap();
        if (!member.value("music").toBool() && member.value("available").toBool() && !member.value("muted").toBool())
            translatedMembers.append(member);
    }
    language_.setMembers(translatedMembers, channel);
    emit activityChanged();
    try {
        QHash<QString, double> volumes;
        if (!output_.isNull()) for (const auto& value : participants_) {
            const auto id = value.toMap().value("id").toString();
            volumes.insert(id, profiles_.participant(output_.id(), id));
        }
        participantVolumes_ = volumes;
        applyOutputVolumes();
        return true;
    } catch (const std::exception& error) { setStatus(tr(error.what()), false); return false; }
}

bool AudioModel::receiveAudio(const QString& peer, const QByteArray& packet, int missing) {
    if (!voiceActive_) return false;
    const bool permitted = std::any_of(participants_.begin(), participants_.end(), [&](const auto& value) {
        const auto member = value.toMap();
        return member.value("id").toString() == peer && member.value("available").toBool() && !member.value("muted").toBool();
    });
    if (!permitted) return false;
    try { return mixer_.receive(peer, packet, clock_.elapsed(), missing); }
    catch (const std::exception& error) { setStatus(tr(error.what()), false); return false; }
}

QVariantMap AudioModel::playbackLevels() const {
    QVariantMap levels;
    if (!voiceActive_ || testingOutput_) return levels;
    for (const auto& entry : participants_) {
        const auto peer = entry.toMap();
        if (peer.value("muted").toBool() || !peer.value("available").toBool()) continue;
        const auto id = peer.value("id").toString();
        levels.insert(id, mixer_.playbackLevel(id, clock_.elapsed()));
    }
    return levels;
}

double AudioModel::transmitLevel() const {
    return voiceSending() && lastPacketAt_ >= 0 && clock_.elapsed() - lastPacketAt_ <= 120 ? mixer_.transmitLevel() : 0;
}

double AudioModel::participantVolume(const QString& peer) const {
    return participantVolumes_.value(peer, 1.0);
}

bool AudioModel::setParticipantVolume(const QString& peer, double value) {
    if (output_.isNull()) return false;
    try {
        profiles_.saveParticipant(output_.id(), peer, value);
        participantVolumes_.insert(peer, value);
        applyOutputVolumes();
        return true;
    } catch (const std::exception& error) { setStatus(tr(error.what()), false); return false; }
}

bool AudioModel::setMusicVolume(double value) {
    if (output_.isNull()) return false;
    try {
        profiles_.saveMusicVolume(output_.id(), value);
        musicVolume_ = value;
        applyOutputVolumes();
        return true;
    } catch (const std::exception& error) { setStatus(tr(error.what()), false); return false; }
}

double AudioModel::channelMusicVolume(const QString& source) {
    if (output_.isNull() || source.isEmpty()) return musicVolume_;
    try { return profiles_.musicVolume(output_.id(), source); }
    catch (const std::exception& error) { setStatus(tr(error.what()), false); return 0; }
}

bool AudioModel::setChannelMusicVolume(const QString& source, double value) {
    return saveChannelMusicVolume(source, value);
}

bool AudioModel::resetChannelMusicVolume(const QString& source) {
    return saveChannelMusicVolume(source, std::nullopt);
}

bool AudioModel::saveChannelMusicVolume(const QString& source, std::optional<double> value) {
    if (output_.isNull() || source.isEmpty()) return false;
    try {
        profiles_.saveChannelMusicVolume(output_.id(), source, value);
        applyOutputVolumes();
        return true;
    } catch (const std::exception& error) { setStatus(tr(error.what()), false); return false; }
}

void AudioModel::applyOutputVolumes() {
    for (const auto& value : participants_) {
        const auto member = value.toMap();
        const auto id = member.value("id").toString();
        mixer_.setGain(id, participantVolumes_.value(id, 1.0)
            * (member.value("music").toBool() ? channelMusicVolume(id) : outputVolume_));
    }
    emit profileChanged();
}

bool AudioModel::setAutomaticVolume(bool value) {
    if (output_.isNull()) return false;
    try {
        profiles_.saveAutomaticVolume(output_.id(), value);
        automaticVolume_ = value;
        mixer_.setAutomatic(value);
        emit profileChanged();
        return true;
    } catch (const std::exception& error) { setStatus(tr(error.what()), false); return false; }
}

void AudioModel::beginPlayback() {
    if (!voiceActive_ || voiceSink_ || output_.isNull()) return;
    playbackFormat_ = captureFormat(output_);
    if (!playbackFormat_.isValid()) { outputFailed_ = true; setStatus(tr("No usable format for voice playback."), false); return; }
    voiceSink_ = std::make_unique<QAudioSink>(output_, playbackFormat_);
    voiceSink_->setBufferSize(playbackFormat_.bytesForDuration(80000));
    voiceSink_->setVolume(deafened_ ? 0 : 1);
    connect(voiceSink_.get(), &QAudioSink::stateChanged, this, [this](QtAudio::State state) {
        if (voiceSink_ && state == QtAudio::StoppedState && voiceSink_->error() != QtAudio::NoError) {
            const QPointer<QAudioSink> failed = voiceSink_.get();
            QTimer::singleShot(0, this, [this, failed] {
                if (!failed || failed != voiceSink_.get()) return;
                outputFailed_ = true;
                cancelNotifications();
                echo_.reset();
                voiceSink_.reset(); playback_ = nullptr;
                setStatus(tr("Voice playback could not be opened."), false);
            });
        }
    });
    playback_ = voiceSink_->start();
    outputFailed_ = !playback_ || voiceSink_->error() != QtAudio::NoError;
    if (!playback_ || voiceSink_->error() != QtAudio::NoError) {
        cancelNotifications();
        voiceSink_.reset(); playback_ = nullptr;
        setStatus(tr("Voice playback could not be opened."), false);
    }
    emit readinessChanged();
}

void AudioModel::playVoice() {
    if (!voiceSink_ || !playback_) return;
    const auto required = playbackFormat_.bytesForDuration(20000);
    if (voiceSink_->bytesFree() < required) return;
    try {
        language_.suspend(deafened_ || testActive() || !voiceActive_);
        squad::VoiceMixer::PlaybackFilter filter;
        if (language_.voiceProcessing()) filter = [this](const QString& peer, std::span<float> frame, qint64 now) {
            language_.processVoice(peer, frame, now, 2);
        };
        auto samples = mixer_.render(playbackFormat_.sampleRate(), clock_.elapsed(), filter);
        if (!speech_.active()) {
            if (speech_.failed()) setStatus(tr("Speech output is unavailable."), false);
            speech_.stop();
            while (!announcements_.empty() && clock_.elapsed() - announcements_.front().second > 10000)
                announcements_.pop_front();
            if (!announcements_.empty()) {
                if (!speech_.start(announcements_.front().first, playbackFormat_.sampleRate(), botVoice_.value("id").toString()))
                    setStatus(tr("Speech output is unavailable."), false);
                announcements_.pop_front();
            }
        }
        speech_.mix(samples, notificationVolume_, 2);
        if (testingOutput_) std::fill(samples.begin(), samples.end(), 0);
        QByteArray data(qsizetype(samples.size() / 2) * playbackFormat_.bytesPerFrame(), '\0');
        for (size_t frame = 0; frame < samples.size() / 2; ++frame) {
            for (int channel = 0; channel < playbackFormat_.channelCount(); ++channel) {
                const auto value = std::clamp(playbackFormat_.channelCount() == 1
                    ? (samples[frame * 2] + samples[frame * 2 + 1]) * 0.5f
                    : channel < 2 ? samples[frame * 2 + channel] : 0.0f, -1.0f, 1.0f);
                auto* dest = data.data() + frame * playbackFormat_.bytesPerFrame() + channel * playbackFormat_.bytesPerSample();
                switch (playbackFormat_.sampleFormat()) {
                case QAudioFormat::Float: std::memcpy(dest, &value, sizeof(value)); break;
                case QAudioFormat::Int16: { const qint16 v = qRound(value * 32767); std::memcpy(dest, &v, sizeof(v)); break; }
                case QAudioFormat::Int32: { const qint32 v = qRound64(double(value) * 2147483647); std::memcpy(dest, &v, sizeof(v)); break; }
                case QAudioFormat::UInt8: *dest = char(qRound((value + 1) * 127.5)); break;
                default: throw std::runtime_error(QT_TRANSLATE_NOOP("AudioModel", "Unsupported voice output sample format"));
                }
            }
        }
        const auto queuedMs = int(std::clamp<qint64>(playbackFormat_.durationForBytes(
            voiceSink_->bufferSize() - voiceSink_->bytesFree()) / 1000, 0, 500));
        const auto written = playback_->write(data);
        if (written != data.size()) {
            echo_.reset();
            setStatus(tr("Voice playback could not play the audio block completely."), false);
        } else {
            // Read back quantized PCM, then apply the same linear volume as
            // QAudioSink. Unplayed/partial blocks never become a reference.
            for (size_t frame = 0; frame < samples.size() / 2; ++frame)
                for (int channel = 0; channel < 2; ++channel)
                    samples[frame * 2 + channel] = playbackFormat_.normalizedSampleValue(data.constData()
                        + frame * playbackFormat_.bytesPerFrame()
                        + (playbackFormat_.channelCount() == 1 ? 0 : channel) * playbackFormat_.bytesPerSample()) * (deafened_ ? 0 : 1);
            if (source_ && !deviceReference_) echo_.render(samples, playbackFormat_.sampleRate(), clock_.elapsed(), queuedMs, 2);
            else if (!source_) echo_.reset();
        }
    } catch (const std::exception& error) {
        echo_.reset();
        outputFailed_ = true;
        cancelNotifications();
        voiceSink_.reset(); playback_ = nullptr; playbackTimer_.stop();
        setStatus(tr(error.what()), false);
    }
}
