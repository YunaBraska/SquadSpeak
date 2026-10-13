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
bool targetLocale(const QString& value) { return value == "system" || locale(value); }
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
                const auto members = object.value("members").toObject();
                const auto channels = object.value("channels").toObject();
                bool valid = members.size() <= 512 && channels.size() <= 512;
                for (auto it = members.begin(); it != members.end(); ++it) {
                    const auto value = it.value().toObject();
                    valid &= identity(it.key()) && locale(value.value("source").toString())
                        && targetLocale(value.value("target").toString()) && value.value("voice").isString()
                        && value.value("voice").toString().size() <= 256 && value.value("incoming").isBool();
                }
                for (auto it = channels.begin(); it != channels.end(); ++it)
                    valid &= identity(it.key()) && locale(it.value().toString());
                if (valid && object.value("version").toInt() == 1) preferences_ = object;
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
    systemRefresh_.setInterval(5000);
    connect(&systemRefresh_, &QTimer::timeout, this, &LocalLanguage::refresh);
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) refresh();
        });
    const auto storedMembers = preferences_.value("members").toObject();
    for (const auto& value : storedMembers)
        if (value.toObject().value("target") == "system" || value.toObject().value("voice") == "system") { systemRefresh_.start(); break; }
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
    preferences.insert("version", 1);
    if (!settingsFile_.isEmpty()) {
        QSaveFile file(settingsFile_);
        const auto bytes = QJsonDocument(preferences).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            error_ = tr("Language settings could not be saved."); notify(); return false;
        }
    }
    preferences_ = std::move(preferences);
    systemRefresh_.stop();
    const auto storedMembers = preferences_.value("members").toObject();
    for (const auto& value : storedMembers)
        if (value.toObject().value("target") == "system" || value.toObject().value("voice") == "system") { systemRefresh_.start(); break; }
    error_.clear(); notify(); return true;
}
QVariantMap LocalLanguage::member(const QString& id) const {
    auto result = preferences_.value("members").toObject().value(id).toObject().toVariantMap();
    if (!result.isEmpty()) {
        result.insert("targetSelection", result.value("target"));
        if (result.value("target") == "system" || result.value("voice") == "system")
            result.insert("target", systemVoice().value("language"));
    }
    if (const auto it = active_.find(id); it != active_.end()) {
        result.insert("transcript", it->second->transcript);
        result.insert("status", it->second->status);
    }
    return result;
}
bool LocalLanguage::configure(const QString& id, const QString& source, const QString& target,
                              const QString& voice, bool incoming) {
    if (!identity(id) || !locale(source) || !targetLocale(target) || voice.size() > 256) return false;
    QString actualTarget = target;
    if (voice == "system") actualTarget = "system";
    else if (!voice.isEmpty()) {
        const auto options = voices();
        const auto found = std::find_if(options.begin(), options.end(), [&](const auto& value) { return value.toMap().value("id") == voice; });
        if (found == options.end()) return false;
        actualTarget = found->toMap().value("language").toString();
    }
    auto preferences = preferences_;
    auto members = preferences.value("members").toObject();
    if (!members.contains(id) && members.size() >= 512) return false;
    members.insert(id, QJsonObject{{"source", source}, {"target", actualTarget}, {"voice", voice}, {"incoming", incoming}});
    preferences.insert("members", members);
    if (!save(preferences)) return false;
    stopMember(id); clearResults();
    if (members_.contains(id) && !suspended_) startMember(id);
    return true;
}
QString LocalLanguage::channelTarget(const QString& id) const { return preferences_.value("channels").toObject().value(id).toString(); }
bool LocalLanguage::setChannelTarget(const QString& id, const QString& target) {
    if (!identity(id) || (!target.isEmpty() && !locale(target))) return false;
    auto preferences = preferences_;
    auto channels = preferences.value("channels").toObject();
    if (!channels.contains(id) && channels.size() >= 512) return false;
    if (target.isEmpty()) channels.remove(id); else channels.insert(id, target);
    preferences.insert("channels", channels);
    return save(preferences);
}
void LocalLanguage::refresh() {
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    squad_language_refresh(native_);
#endif
}
bool LocalLanguage::openSettings(bool speech) {
#if defined(Q_OS_MACOS)
    return QDesktopServices::openUrl(QUrl(speech
        ? "x-apple.systempreferences:com.apple.preference.universalaccess?SpokenContent"
        : "x-apple.systempreferences:com.apple.Localization-Settings.extension?translation"));
#else
    Q_UNUSED(speech)
    return false;
#endif
}
bool LocalLanguage::prepare(const QString& id) {
    refresh();
    const auto preference = member(id);
    if (!available() || preference.isEmpty() || preparing_.size() >= 1
        || (!preference.value("voice").toString().isEmpty() && !speechAvailable())) return false;
    const auto request = ++nextRequest_;
    preparing_.insert(request, id); notify();
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    squad_language_prepare(native_, request, qUtf8Printable(preference.value("source").toString()),
        qUtf8Printable(preference.value("target").toString()), !preference.value("voice").toString().isEmpty());
#endif
    return true;
}
void LocalLanguage::cancelPreparation() {
    const auto requests = preparing_.keys();
    preparing_.clear();
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    for (const auto request : requests) squad_language_cancel(native_, request);
#endif
    notify();
}
bool LocalLanguage::prepareTranslation(const QString& key) {
    const auto state = result(key);
    const auto source = state.value("detectedSource", state.value("source")).toString();
    const auto target = state.value("target").toString();
    if (!available() || !locale(source) || !locale(target) || !preparing_.isEmpty()) return false;
    const auto request = ++nextRequest_;
    preparing_.insert(request, QString{}); notify();
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    squad_language_prepare(native_, request, qUtf8Printable(source), qUtf8Printable(target), false);
#endif
    return true;
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
        qUtf8Printable(job.parts[job.replacements.size()].text));
#endif
}
void LocalLanguage::event(qint64 request, const QJsonObject& event) {
    const auto kind = event.value("kind").toString();
    if (kind == "voicesChanged") { voices_ = SpeechOutput::voices(); emit capabilitiesChanged(); notify(); return; }
    if (kind == "capabilities") {
        const bool systemChanged = capabilities_.value("systemVoice") != event.value("systemVoice");
        capabilities_ = event;
        emit capabilitiesChanged();
        if (systemChanged) {
            clearResults();
            for (const auto& id : members_) if (member(id).value("voice") == "system") {
                stopMember(id);
                if (!suspended_) startMember(id);
            }
        }
        notify(); return;
    }
    if (preparing_.contains(request)) {
        const auto id = preparing_.take(request);
        if (kind == "ready") { error_.clear(); clearResults(); stopMember(id); if (members_.contains(id) && !suspended_) startMember(id); emit prepared(id); }
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
                    if (!voice->second->output.start(text, 48000, member(peer).value("voice") == "system" ? QString{} : member(peer).value("voice").toString())) {
                        translations_.erase(it); failMember(peer); return;
                    }
                    voice->second->status.clear();
                }
                translations_.erase(it); notify(); return;
            }
            job.replacements.append(text); next(request); return;
        }
        const auto peer = job.peer;
        if (peer.isEmpty()) {
            auto state = results_.value(job.key);
            if (event.contains("source")) state.insert("detectedSource", event.value("source").toString());
            state.insert("pending", false);
            state.insert("needsPreparation", event.value("code") == "modelsRequired");
            state.insert("error", event.value("code") == "modelsRequired" ? tr("Prepare languages") : nativeError(event));
            results_.insert(job.key, state);
        }
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
        squad_language_cancel(native_, request);
#endif
        translations_.erase(it);
        if (!peer.isEmpty()) failMember(peer);
        notify(); return;
    }
    for (auto& [id, voice] : active_) if (voice->request == request) {
        if (kind == "ready") { voice->ready = true; voice->waitingSince = -1; voice->status.clear(); notify(); return; }
        if (kind != "text") { failMember(id); return; }
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
            qUtf8Printable(preference.value("target").toString()), qUtf8Printable(text));
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
void LocalLanguage::setMembers(const QStringList& members, const QString& channel) {
    const bool switched = channel_ != channel;
    channel_ = channel;
    members_ = members;
    QStringList removed;
    for (const auto& [id, voice] : active_) if (switched || !members.contains(id)) removed.append(id);
    for (const auto& id : removed) stopMember(id);
    if (!suspended_) for (const auto& id : members) startMember(id);
    notify();
}
void LocalLanguage::suspend(bool suspended) {
    if (suspended_ == suspended) return;
    suspended_ = suspended;
    if (suspended) {
        QStringList ids;
        for (const auto& [id, voice] : active_) ids.append(id);
        for (const auto& id : ids) stopMember(id);
    } else for (const auto& id : members_) startMember(id);
    notify();
}
bool LocalLanguage::voiceProcessing() const {
    return !suspended_ && std::any_of(active_.begin(), active_.end(), [](const auto& item) { return item.second->ready; });
}
void LocalLanguage::processVoice(const QString& id, std::span<float> samples, qint64) {
    const auto it = active_.find(id);
    if (it == active_.end() || !it->second->ready || suspended_) return;
    const auto request = it->second->request;
#if defined(SQUAD_LOCAL_LANGUAGE_NATIVE)
    squad_language_audio(native_, request, samples.data(), int(samples.size()));
#endif
    // Native ingress may synchronously cancel and replace the stream on failure.
    const auto current = active_.find(id);
    if (current == active_.end() || current->second->request != request || !current->second->ready) return;
    auto& voice = *current->second;
    double energy = 0;
    for (auto sample : samples) energy += double(sample) * sample;
    if (energy > 0.00001 * samples.size() && voice.waitingSince < 0 && !voice.output.active()) voice.waitingSince = clock_.elapsed();
    std::fill(samples.begin(), samples.end(), 0);
    if (voice.output.mix(samples, 1)) voice.waitingSince = -1;
}
