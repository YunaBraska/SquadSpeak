#include "local_language.hpp"
#include <QFile>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextCursor>
#include <QRegularExpression>
#include <QDesktopServices>
#include <QGuiApplication>
#include <algorithm>
#include <cmath>
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
#include "local_language_native.h"
#endif

namespace {
class TextDocument final : public QTextDocument {
    QVariant loadResource(int, const QUrl&) override { return {}; }
};
bool locale(const QString& value) {
    static const QRegularExpression pattern("^[a-z]{2,3}(-[A-Za-z0-9]{2,8}){0,3}$");
    return pattern.match(value).hasMatch();
}
bool optionalLocale(const QString& value) { return value.isEmpty() || locale(value); }
QString nativeError(const QJsonObject& event) {
    const auto detail = event.value("error").toString().trimmed().left(512);
    return detail.isEmpty() ? QCoreApplication::translate("LocalLanguage", "Local language processing is unavailable.") : detail;
}
bool identity(const QString& value) { return !value.isEmpty() && value.size() <= 256 && !value.contains(QChar::Null); }
}

bool LocalLanguage::supported() {
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    return true;
#else
    return false;
#endif
}
LocalLanguage::LocalLanguage(QString settingsFile, QObject* parent)
    : QObject(parent), settingsFile_(std::move(settingsFile)) {
    clock_.start();
    if (!settingsFile_.isEmpty()) {
        QFile file(settingsFile_);
        if (file.exists()) {
            if (file.open(QIODevice::ReadOnly) && file.size() <= 512 * 1024) {
                const auto object = QJsonDocument::fromJson(file.readAll()).object();
                auto channels = object.value("channels").toObject();
                const auto version = object.value("version").toInt();
                bool valid = (object.value("channels").isObject() || (version == 1 && !object.contains("channels")))
                    && channels.size() <= 512 && (version == 1 || version == 2);
                for (auto it = channels.begin(); it != channels.end(); ++it) {
                    valid &= identity(it.key());
                    if (version == 1) {
                        valid &= it.value().isString() && locale(it.value().toString());
                        it.value() = QJsonObject{{"messages", false}, {"outgoing", it.value().toString()}, {"voice", false}, {"voiceTarget", ""}};
                    }
                    const auto value = it.value().toObject();
                    valid &= value.value("messages").isBool() && value.value("voice").isBool()
                        && value.value("outgoing").isString() && optionalLocale(value.value("outgoing").toString())
                        && value.value("voiceTarget").isString() && optionalLocale(value.value("voiceTarget").toString());
                }
                if (valid) preferences_ = {{"version", 2}, {"channels", channels}};
                else error_ = tr("Language settings could not be read.");
            } else error_ = tr("Language settings could not be read.");
        }
    }
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    native_ = squad_language_create([](void* context, int64_t request, const char* json) {
        static_cast<LocalLanguage*>(context)->event(request, QJsonDocument::fromJson(QByteArray(json)).object());
    }, this);
    capabilities_ = QJsonDocument::fromJson(QByteArray(squad_language_capabilities(native_))).object();
#endif
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) { refresh(); refreshVoices(); }
        });
    deadline_.setInterval(250);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        QList<qint64> expired;
        for (const auto& [id, job] : translations_) if (clock_.elapsed() - job.started > 10000) expired.append(id);
        for (auto id : expired) event(id, {{"kind", "error"}});
        QStringList failed;
        for (const auto& [id, voice] : active_)
            if (voice->request && ((voice->waitingSince >= 0 && clock_.elapsed() - voice->waitingSince > 10000)
                    || voice->output.failed())) failed.append(id);
        for (const auto& id : failed) failMember(id);
        if (translations_.empty() && std::none_of(active_.begin(), active_.end(), [](const auto& item) { return item.second->request != 0; })) deadline_.stop();
    });
}
LocalLanguage::~LocalLanguage() {
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    squad_language_destroy(native_);
#endif
}
void LocalLanguage::notify() { ++revision_; emit changed(); }
bool LocalLanguage::save(QJsonObject preferences) {
    preferences.insert("version", 2);
    if (!settingsFile_.isEmpty()) {
        QSaveFile file(settingsFile_);
        const auto bytes = QJsonDocument(preferences).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            error_ = tr("Language settings could not be saved."); notify(); return false;
        }
    }
    preferences_ = std::move(preferences);
    error_.clear(); notify(); return true;
}
QVariantMap LocalLanguage::channelPreferences(const QString& id) const {
    auto value = preferences_.value("channels").toObject().value(id).toObject().toVariantMap();
    value.insert("incomingTarget", appLanguage_);
    value.insert("listenTarget", value.value("voiceTarget").toString().isEmpty() ? appLanguage_ : value.value("voiceTarget"));
    return value;
}
QVariantMap LocalLanguage::member(const QString& id) const {
    const auto settings = channelPreferences(channel_);
    const auto target = settings.value("listenTarget").toString();
    const auto source = members_.value(id);
    QVariantMap result{{"source", source}, {"target", target}};
    if (settings.value("voice").toBool() && (source.isEmpty() || QLocale(source).language() != QLocale(target).language())) {
        if (!source.isEmpty()) result.insert("voice", SpeechOutput::preferredVoice(voices_, target).value("id"));
        if (result.value("voice").toString().isEmpty())
            result.insert("status", tr("Original audio") + ". " + tr("Local language processing is unavailable."));
    }
    if (const auto it = active_.find(id); it != active_.end()) {
        result.insert("transcript", it->second->transcript);
        result.insert("status", it->second->status);
    }
    return result;
}
bool LocalLanguage::configureChannel(const QString& id, bool messages, const QString& outgoing,
                                     bool voice, const QString& voiceTarget) {
    if (!identity(id) || !optionalLocale(outgoing) || !optionalLocale(voiceTarget)) return false;
    auto preferences = preferences_;
    auto channels = preferences.value("channels").toObject();
    if (!channels.contains(id) && channels.size() >= 512) return false;
    channels.insert(id, QJsonObject{{"messages", messages}, {"outgoing", outgoing}, {"voice", voice}, {"voiceTarget", voiceTarget}});
    preferences.insert("channels", channels);
    if (!save(preferences)) return false;
    cancelPreparation(); offeredPreparations_.clear(); clearResults();
    if (id == channel_) restartMembers();
    return true;
}
void LocalLanguage::setAppLanguage(const QString& language) {
    if (!locale(language) || appLanguage_ == language) return;
    appLanguage_ = language;
    cancelPreparation(); offeredPreparations_.clear(); clearResults(); restartMembers();
}
void LocalLanguage::refresh() {
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    squad_language_refresh(native_);
#endif
}
void LocalLanguage::refreshVoices() {
    const auto voices = SpeechOutput::voices();
    if (voices == voices_) return;
    voices_ = voices;
    restartMembers();
    emit capabilitiesChanged();
    notify();
}
bool LocalLanguage::openSettings(bool speech) {
#if defined(Q_OS_MACOS)
    return QDesktopServices::openUrl(QUrl(speech
        ? "x-apple.systempreferences:com.apple.preference.universalaccess?SpokenContent"
        : "x-apple.systempreferences:com.apple.Localization-Settings.extension?translation"));
#elif defined(Q_OS_IOS)
    return speech && QDesktopServices::openUrl(QUrl("https://support.apple.com/111798"));
#else
    Q_UNUSED(speech)
    return false;
#endif
}
void LocalLanguage::offerPreparation(const QString& source, const QString& target, bool speech, const QString& peer) {
    const auto key = source + ":" + target + (speech ? ":speech" : ":text");
    if (!locale(source) || !locale(target) || preparing() || !preparation_.isEmpty() || offeredPreparations_.contains(key)) return;
    offeredPreparations_.insert(key);
    preparation_ = {{"source", source}, {"target", target}, {"speech", speech}, {"peer", peer}};
    notify();
}
bool LocalLanguage::prepare(const QString& source, const QString& target, bool speech, const QString& peer) {
    if (!available() || !locale(source) || !locale(target) || preparing() || (speech && !speechAvailable())) return false;
    const auto request = ++nextRequest_;
    preparing_.insert(request, peer); notify();
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    squad_language_prepare(native_, request, qUtf8Printable(source), qUtf8Printable(target), speech);
#endif
    return true;
}
bool LocalLanguage::acceptPreparation() {
    const auto choice = preparation_;
    preparation_.clear(); notify();
    return prepare(choice.value("source").toString(), choice.value("target").toString(), choice.value("speech").toBool(), choice.value("peer").toString());
}
void LocalLanguage::cancelPreparation() {
    const auto requests = preparing_.keys();
    preparing_.clear(); preparation_.clear();
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    for (const auto request : requests) squad_language_cancel(native_, request);
#endif
    notify();
}
bool LocalLanguage::prepareTranslation(const QString& key) {
    const auto state = result(key);
    return prepare(state.value("detectedSource", state.value("source")).toString(), state.value("target").toString(), false);
}
void LocalLanguage::cancel(const QString& key) {
    for (auto it = translations_.begin(); it != translations_.end();) {
        if (it->second.key != key) { ++it; continue; }
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
        squad_language_cancel(native_, it->first);
#endif
        it = translations_.erase(it);
    }
    results_.remove(key);
}
void LocalLanguage::clearResults() {
    const auto keys = results_.keys();
    for (const auto& key : keys) cancel(key);
    notify();
}
QVariantMap LocalLanguage::result(const QString& key) const { return results_.value(key); }
bool LocalLanguage::request(const QString& key, const QString& markdown, const QString& source, const QString& target) {
    if (!identity(key) || markdown.isEmpty() || markdown.size() > 16384 || (!source.isEmpty() && !locale(source)) || !locale(target)) return false;
    const auto old = result(key);
    if (old.value("original").toString() == markdown && old.value("source").toString() == source && old.value("target").toString() == target) return true;
    cancel(key);
    if (results_.size() >= 160) {
        auto keys = results_.keys();
        // Completed results can be recomputed when a bounded history page returns.
        for (const auto& cached : keys) if (!results_.value(cached).value("pending").toBool()) { results_.remove(cached); break; }
    }
    if (results_.size() >= 160) return false;
    QVariantMap state{{"original", markdown}, {"source", source}, {"target", target}, {"pending", false}};
    if (!available() || busy()) {
        state.insert("error", tr("Local language processing is unavailable."));
        results_.insert(key, state); notify(); return false;
    }
    Translation job;
    job.key = key; job.original = markdown; job.source = source; job.target = target; job.started = clock_.elapsed();
    job.document = std::make_unique<TextDocument>();
    job.document->setMarkdown(markdown, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
    for (auto block = job.document->begin(); block.isValid(); block = block.next()) {
        const auto format = block.blockFormat();
        if (format.hasProperty(QTextFormat::BlockCodeFence) || format.hasProperty(QTextFormat::BlockCodeLanguage) || format.nonBreakableLines()) continue;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            const auto style = fragment.charFormat();
            const auto text = fragment.text();
            if (style.isImageFormat() || style.fontFixedPitch() || style.fontFamilies().toStringList().contains("monospace")
                || text.trimmed().isEmpty() || (style.isAnchor() && text == style.anchorHref())) continue;
            job.parts.append({fragment.position(), fragment.length(), text});
            job.detectionText += text + " ";
        }
    }
    if (job.parts.size() > 128) {
        state.insert("error", tr("Local language processing is unavailable."));
        results_.insert(key, state); notify(); return false;
    }
    state.insert("pending", true); results_.insert(key, state);
    const auto request = ++nextRequest_;
    translations_.emplace(request, std::move(job)); deadline_.start(); next(request); notify(); return true;
}
void LocalLanguage::next(qint64 request) {
    auto& job = translations_.at(request);
    if (job.replacements.size() == job.parts.size()) {
        for (int i = job.parts.size() - 1; i >= 0; --i) {
            const auto& part = job.parts[i];
            QTextCursor cursor(job.document.get());
            cursor.setPosition(part.position); cursor.setPosition(part.position + part.length, QTextCursor::KeepAnchor);
            cursor.insertText(job.replacements[i], cursor.charFormat());
        }
        auto state = results_.value(job.key);
        state.insert("text", job.document->toMarkdown(QTextDocument::MarkdownDialectGitHub));
        state.insert("pending", false); results_.insert(job.key, state);
        translations_.erase(request); notify(); return;
    }
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    squad_language_translate(native_, request, qUtf8Printable(job.source), qUtf8Printable(job.target),
        qUtf8Printable(job.parts[job.replacements.size()].text), qUtf8Printable(job.detectionText));
#endif
}
void LocalLanguage::event(qint64 request, const QJsonObject& event) {
    const auto kind = event.value("kind").toString();
    if (kind == "voicesChanged") { refreshVoices(); return; }
    if (kind == "capabilities") {
        const bool changed = available() != event.value("available").toBool() || speechAvailable() != event.value("speechAvailable").toBool();
        capabilities_ = event;
        emit capabilitiesChanged();
        if (changed) restartMembers();
        notify(); return;
    }
    if (preparing_.contains(request)) {
        preparing_.remove(request);
        if (kind == "ready") { error_.clear(); clearResults(); restartMembers(); }
        else error_ = nativeError(event);
        notify(); return;
    }
    if (auto it = translations_.find(request); it != translations_.end()) {
        auto& job = it->second;
        const auto text = event.value("text").toString();
        if (kind == "text" && !text.trimmed().isEmpty() && text.size() <= 16384) {
            if (!job.peer.isEmpty()) {
                const auto peer = job.peer;
                auto voice = active_.find(peer);
                if (voice != active_.end()) {
                    if (!voice->second->output.start(text, 48000, member(peer).value("voice").toString())) {
                        translations_.erase(it); failMember(peer); return;
                    }
                    voice->second->status.clear();
                }
                translations_.erase(it); notify(); return;
            }
            if (job.source.isEmpty()) job.source = event.value("source").toString();
            const auto& original = job.parts[job.replacements.size()].text;
            qsizetype begin = 0, end = original.size();
            while (begin < end && original[begin].isSpace()) ++begin;
            while (end > begin && original[end - 1].isSpace()) --end;
            job.replacements.append(original.first(begin) + text.trimmed() + original.mid(end));
            next(request); return;
        }
        const auto peer = job.peer;
        if (peer.isEmpty()) {
            auto state = results_.value(job.key);
            if (event.contains("source")) state.insert("detectedSource", event.value("source").toString());
            state.insert("pending", false);
            state.insert("needsPreparation", event.value("code") == "modelsRequired");
            state.insert("error", event.value("code") == "modelsRequired" ? tr("Prepare languages") : nativeError(event));
            results_.insert(job.key, state);
            if (state.value("needsPreparation").toBool()) offerPreparation(state.value("detectedSource").toString(), job.target, false);
        }
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
        squad_language_cancel(native_, request);
#endif
        translations_.erase(it);
        if (!peer.isEmpty()) {
            const auto preference = member(peer);
            failMember(peer);
            if (event.value("code") == "modelsRequired")
                offerPreparation(preference.value("source").toString(), preference.value("target").toString(), true, peer);
        }
        notify(); return;
    }
    for (auto& [id, voice] : active_) if (voice->request == request) {
        if (kind == "ready") { voice->ready = true; voice->waitingSince = -1; voice->status.clear(); notify(); return; }
        if (kind != "text") {
            const auto preference = member(id);
            const auto peer = id;
            failMember(peer);
            if (event.value("code") == "modelsRequired")
                offerPreparation(preference.value("source").toString(), preference.value("target").toString(), true, peer);
            return;
        }
        if (!event.value("final").toBool()) return;
        const auto text = event.value("text").toString().trimmed();
        if (text.isEmpty()) return;
        if (text.size() > 400 || voice->output.active() || translations_.size() >= 4
            || std::any_of(translations_.begin(), translations_.end(), [&](const auto& item) { return item.second.peer == id; })) {
            failMember(id); return;
        }
        voice->transcript = text;
        voice->waitingSince = clock_.elapsed();
        const auto preference = member(id);
        Translation job;
        job.peer = id; job.started = clock_.elapsed();
        const auto translation = ++nextRequest_;
        translations_.emplace(translation, std::move(job));
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
        squad_language_translate(native_, translation, qUtf8Printable(preference.value("source").toString()),
            qUtf8Printable(preference.value("target").toString()), qUtf8Printable(text), qUtf8Printable(text));
#endif
        notify(); return;
    }
}
void LocalLanguage::startMember(const QString& id) {
    if (active_.contains(id) || member(id).value("voice").toString().isEmpty()) return;
    auto voice = std::make_unique<Voice>();
    const auto count = std::count_if(active_.begin(), active_.end(), [](const auto& item) { return item.second->request != 0; });
    voice->status = tr("Original audio") + ". " + tr("Local language processing is unavailable.");
    if (available() && speechAvailable() && count < 4) { voice->request = ++nextRequest_; voice->waitingSince = clock_.elapsed(); }
    const auto request = voice->request;
    active_.emplace(id, std::move(voice));
    if (request) {
        deadline_.start();
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
        squad_language_listen(native_, request, qUtf8Printable(member(id).value("source").toString()));
#endif
    }
    notify();
}
void LocalLanguage::stopMember(const QString& id) {
    if (const auto it = active_.find(id); it != active_.end()) {
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
        if (it->second->request) squad_language_cancel(native_, it->second->request);
#endif
        active_.erase(it);
    }
    for (auto it = translations_.begin(); it != translations_.end();) {
        if (it->second.peer != id) { ++it; continue; }
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
        squad_language_cancel(native_, it->first);
#endif
        it = translations_.erase(it);
    }
}
void LocalLanguage::failMember(QString id) {
    const auto transcript = member(id).value("transcript").toString();
    stopMember(id);
    auto voice = std::make_unique<Voice>();
    voice->transcript = transcript; voice->status = tr("Original audio") + ". " + tr("Local language processing is unavailable.");
    active_.emplace(id, std::move(voice)); notify();
}
void LocalLanguage::restartMembers() {
    QStringList active;
    for (const auto& [id, voice] : active_) active.append(id);
    for (const auto& id : active) stopMember(id);
    if (!suspended_) for (auto it = members_.begin(); it != members_.end(); ++it) startMember(it.key());
    notify();
}
void LocalLanguage::setMembers(const QVariantList& members, const QString& channel) {
    QMap<QString, QString> next;
    for (const auto& entry : members) {
        const auto member = entry.toMap();
        const auto id = member.value("id").toString(), language = member.value("language").toString();
        if (identity(id)) next.insert(id, locale(language) ? language : QString{});
    }
    const bool switched = channel_ != channel;
    if (!switched && members_ == next) return;
    if (switched) { cancelPreparation(); offeredPreparations_.clear(); }
    QStringList removed;
    for (const auto& [id, voice] : active_)
        if (switched || !next.contains(id) || next.value(id) != members_.value(id)) removed.append(id);
    for (const auto& id : removed) stopMember(id);
    if (removed.contains(preparation_.value("peer").toString())
        || std::any_of(preparing_.cbegin(), preparing_.cend(), [&](const auto& peer) { return removed.contains(peer); }))
        cancelPreparation();
    channel_ = channel; members_ = std::move(next);
    if (!suspended_) for (auto it = members_.begin(); it != members_.end(); ++it) startMember(it.key());
    notify();
}
void LocalLanguage::suspend(bool suspended) {
    if (suspended_ == suspended) return;
    suspended_ = suspended;
    if (suspended) cancelPreparation();
    restartMembers();
}
bool LocalLanguage::voiceProcessing() const {
    return !suspended_ && std::any_of(active_.begin(), active_.end(), [](const auto& item) { return item.second->ready; });
}
void LocalLanguage::processVoice(const QString& id, std::span<float> samples, qint64, int channels) {
    if ((channels != 1 && channels != 2) || samples.size() % size_t(channels)) return;
    const auto it = active_.find(id);
    if (it == active_.end() || !it->second->ready || suspended_) return;
    const auto request = it->second->request;
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    if (channels == 1) squad_language_audio(native_, request, samples.data(), int(samples.size()));
    else {
        std::vector<float> mono(samples.size() / 2);
        for (size_t i = 0; i < mono.size(); ++i) mono[i] = (samples[i * 2] + samples[i * 2 + 1]) * .5f;
        squad_language_audio(native_, request, mono.data(), int(mono.size()));
    }
#endif
    // Native ingress may synchronously cancel and replace the stream on failure.
    const auto current = active_.find(id);
    if (current == active_.end() || current->second->request != request || !current->second->ready) return;
    auto& voice = *current->second;
    double energy = 0;
    for (auto sample : samples) energy += double(sample) * sample;
    if (energy > 0.00001 * samples.size() && voice.waitingSince < 0 && !voice.output.active()) voice.waitingSince = clock_.elapsed();
    std::fill(samples.begin(), samples.end(), 0);
    if (voice.output.mix(samples, 1, channels)) voice.waitingSince = -1;
}
