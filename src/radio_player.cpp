#include "radio_player.hpp"
#include "voice_session.hpp"
#include "local_channel.hpp"
#include <QFile>
#include <QAudioBuffer>
#include <QSet>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
QString searchText(const QString& value) {
    QString result;
    for (const auto c : value.toCaseFolded().normalized(QString::NormalizationForm_KD)) {
        if (c.category() == QChar::Mark_NonSpacing || c.category() == QChar::Mark_SpacingCombining) continue;
        result += c.isLetterOrNumber() ? c : QChar(' ');
    }
    return result.simplified();
}

bool oneEditApart(QStringView a, QStringView b) {
    if (std::abs(a.size() - b.size()) > 1) return false;
    qsizetype i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    if (i == std::min(a.size(), b.size())) return true;
    if (a.size() == b.size()) {
        if (a.sliced(i + 1) == b.sliced(i + 1)) return true;
        return i + 1 < a.size() && a[i] == b[i + 1] && a[i + 1] == b[i]
            && a.sliced(i + 2) == b.sliced(i + 2);
    }
    return a.size() > b.size() ? a.sliced(i + 1) == b.sliced(i) : a.sliced(i) == b.sliced(i + 1);
}

int searchScore(const QStringList& query, const QString& name, const QString& metadata) {
    const auto nameWords = name.split(' ', Qt::SkipEmptyParts);
    const auto words = (name + ' ' + metadata).split(' ', Qt::SkipEmptyParts);
    QString initials;
    for (const auto& word : nameWords) initials += word.front();
    int total = 0;
    for (const auto& term : query) {
        int best = 1000;
        if (term.size() >= 2 && initials.startsWith(term)) best = 12;
        for (qsizetype i = 0; i < words.size(); ++i) {
            const auto& word = words[i];
            const int field = i < nameWords.size() ? 0 : 20;
            if (word == term) best = std::min(best, field);
            else if (word.startsWith(term)) best = std::min(best, field + 3);
            else if (term.size() >= 3 && word.contains(term)) best = std::min(best, field + 8);
            else if (term.size() >= 4 && oneEditApart(term, word)) best = std::min(best, field + 30);
            else if (term.size() >= 3) {
                qsizetype next = 0;
                for (const auto c : word) if (next < term.size() && c == term[next]) ++next;
                if (next == term.size()) best = std::min(best, field + 60);
            }
        }
        if (best == 1000) return -1;
        total += best;
    }
    return total;
}

QAudioFormat streamFormat() {
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Float);
    return format;
}
}

RadioPlayer::RadioPlayer(QString storageFile, QObject* parent)
    : QObject(parent), storageFile_(std::move(storageFile)), output_(streamFormat()), coordinator_(this) {
    QFile file(storageFile_);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly) || file.size() > 384 * 1024)
            throw std::runtime_error(tr("Radio stations could not be read.").toStdString());
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        const auto data = document.object();
        if (error.error != QJsonParseError::NoError || !document.isObject() || data.value("version").toInt() != 1
            || !data.value("stations").isArray() || data.value("stations").toArray().size() > 128
            || !data.value("playing").isBool() || !data.value("selected").isString())
            throw std::runtime_error(tr("Saved radio stations are invalid.").toStdString());
        QSet<QString> ids;
        for (const auto entry : data.value("stations").toArray()) {
            const auto item = entry.toObject();
            const auto canonical = station(item.value("id").toString(), item.value("name").toString(), item.value("url").toString());
            const auto id = canonical.value("id").toString();
            if (item != canonical || ids.contains(id)) throw std::runtime_error(tr("Saved radio station is invalid.").toStdString());
            ids.insert(id);
        }
        if (data.contains("catalogSelection")) {
            const auto item = data.value("catalogSelection").toObject();
            if (item != station(item.value("id").toString(), item.value("name").toString(), item.value("url").toString()))
                throw std::runtime_error(tr("Saved catalog selection is invalid.").toStdString());
            ids.insert(item.value("id").toString());
        }
        if ((!data.value("selected").toString().isEmpty() && !ids.contains(data.value("selected").toString()))
            || (data.value("playing").toBool() && data.value("selected").toString().isEmpty()))
            throw std::runtime_error(tr("Saved radio selection is invalid.").toStdString());
        saved_ = data;
    }
    player_.setAudioBufferOutput(&output_);
    retry_.setSingleShot(true);
    tick_.setInterval(20);
    tick_.setTimerType(Qt::PreciseTimer);
    connect(&retry_, &QTimer::timeout, this, &RadioPlayer::startStream);
    connect(&output_, &QAudioBufferOutput::audioBufferReceived, this, &RadioPlayer::receive);
    connect(&player_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString& error) { reconnect(error); });
    connect(&player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) reconnect(tr("The stream ended."));
    });
    connect(&tick_, &QTimer::timeout, this, [this] {
        if (received_.isValid() && received_.elapsed() > 15000) { reconnect(tr("The stream stopped responding.")); return; }
        if (packets_.empty()) return;
        // A suspended event loop must never deliver a burst of stale music.
        if (delivered_.isValid() && delivered_.elapsed() > 120) packets_.clear();
        delivered_.restart();
        if (packets_.empty()) return;
        auto packet = std::move(packets_.front()); packets_.pop_front();
        emit audioPacket(packet);
    });
}

RadioPlayer::~RadioPlayer() {
    if (auto* probe = std::exchange(probe_, nullptr)) {
        probe->setSource({});
        delete probe;
    }
    halt();
    player_.setAudioBufferOutput(nullptr);
}

RadioPlayer* RadioPlayer::rootPlayer() const {
    return coordinator_ ? coordinator_ : const_cast<RadioPlayer*>(this);
}

bool RadioPlayer::slotHeld() const {
    return active() && available_ && state_ != "stopped" && state_ != "unavailable";
}

int RadioPlayer::runningPlayers() const {
    const auto* root = rootPlayer();
    int count = 0;
    const auto countPlayer = [&count](const RadioPlayer* player) {
        if (player && player->slotHeld()) ++count;
    };
    countPlayer(root);
    for (const auto& child : root->children_) countPlayer(child);
    return count;
}

void RadioPlayer::notifySlotChanged() {
    auto* root = rootPlayer();
    const auto wake = [root](RadioPlayer* player) {
        if (!player || player->coordinator_ != root || !player->active() || !player->available_
            || player->state_ != "unavailable") return;
        player->startStream();
    };
    wake(root);
    for (const auto& child : root->children_) wake(child);
}

bool RadioPlayer::bind(LocalChannel& channel) {
    if (channel_ && channel_ != &channel) return false;
    if (channel_ == &channel) { syncBoundAvailability(); syncBoundState(); return true; }
    channel_ = &channel;
    if (!coordinator_) coordinator_ = this;
    connect(&channel, &LocalChannel::stateChanged, this, [this] { syncBoundAvailability(); });
    connect(&channel, &LocalChannel::screenChanged, this, [this] { syncBoundAvailability(); });
    connect(&channel, &QObject::destroyed, this, [this] {
        channel_ = nullptr;
        setAvailable(false);
        for (const auto& pointer : children_) {
            auto* child = pointer.data();
            if (!child) continue;
            child->channel_ = nullptr;
            delete child;
        }
        children_.clear();
    });
    connect(&channel, &LocalChannel::ownedChannelsChanged, this, [this] {
        if (!channel_) return;
        for (auto it = children_.begin(); it != children_.end();) {
            if (!channel_->ownChannel(it.key())) {
                auto* child = it.value().data();
                it = children_.erase(it);
                delete child;
            } else ++it;
        }
        syncChildren();
        notifySlotChanged();
    });
    connect(this, &RadioPlayer::changed, &channel, [this] {
        channel_->setMusicState(stationName(), state_, active());
    });
    connect(this, &RadioPlayer::audioPacket, &channel, &LocalChannel::sendMusic);
    syncBoundAvailability(); syncBoundState(); syncChildren();
    return true;
}

void RadioPlayer::syncChildren() {
    if (!channel_ || rootPlayer() != this) return;
    for (const auto& value : channel_->ownedChannels()) {
        const auto id = value.toMap().value("id").toString();
        if (id == channel_->channelId()) continue;
        if (auto* channel = channel_->ownChannel(id)) childFor(*channel);
    }
}

RadioPlayer* RadioPlayer::childFor(LocalChannel& channel) {
    const auto id = channel.channelId();
    if (id == channel_->channelId()) return this;
    if (auto child = children_.value(id)) return child;
    RadioPlayer* child = nullptr;
    try { child = new RadioPlayer(channel.radioStorageFile(), rootPlayer()); }
    catch (const std::exception& error) {
        error_ = QString::fromUtf8(error.what()); emit changed(); return nullptr;
    }
    child->coordinator_ = rootPlayer();
    children_.insert(id, child);
    if (!child->bind(channel)) {
        children_.remove(id);
        delete child;
        return nullptr;
    }
    return child;
}

RadioPlayer* RadioPlayer::forChannel(const QString& id) {
    if (!channel_) return nullptr;
    auto* root = rootPlayer();
    auto* channel = root->channel_->ownChannel(id);
    if (!channel) return nullptr;
    return root->childFor(*channel);
}

void RadioPlayer::syncBoundAvailability() {
    if (!channel_) return;
    setAvailable(channel_->hosting() && !channel_->screenSharing());
}

void RadioPlayer::syncBoundState() {
    if (channel_) channel_->setMusicState(stationName(), state_, active());
}

QJsonObject RadioPlayer::station(const QString& id, const QString& name, const QString& url) {
    if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id)
        throw std::invalid_argument(tr("Invalid radio station identity.").toStdString());
    const auto label = VoiceSession::validatedName(name);
    const QUrl address(url.trimmed(), QUrl::StrictMode);
    if (url.size() > 2048 || !address.isValid() || address.host().isEmpty() || !address.userInfo().isEmpty()
        || (address.scheme() != "http" && address.scheme() != "https") || address.hasFragment())
        throw std::invalid_argument(tr("Enter a direct HTTP or HTTPS audio stream URL without credentials.").toStdString());
    return {{"id", id}, {"name", label}, {"url", address.toString(QUrl::FullyEncoded)}};
}

QVariantList RadioPlayer::stations() const { return saved_.value("stations").toArray().toVariantList(); }
QJsonObject RadioPlayer::selectedStation() const {
    for (const auto entry : saved_.value("stations").toArray())
        if (entry.toObject().value("id").toString() == selectedId()) return entry.toObject();
    if (saved_.value("catalogSelection").toObject().value("id").toString() == selectedId())
        return saved_.value("catalogSelection").toObject();
    return {};
}
QString RadioPlayer::stationName() const { return selectedStation().value("name").toString(); }

const QJsonObject& RadioPlayer::catalog() {
    static const auto data = [] {
        Q_INIT_RESOURCE(radio_catalog);
        QFile file(":/radio/radio_catalog.json");
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(tr("The radio catalog is missing.").toStdString());
        const auto result = QJsonDocument::fromJson(file.readAll()).object();
        if (result.value("version").toInt() != 1 || !result.value("stations").isArray())
            throw std::runtime_error(tr("The radio catalog is invalid.").toStdString());
        return result;
    }();
    return data;
}

QVariantMap RadioPlayer::catalogInfo() const {
    auto info = catalog(); info.remove("stations");
    info.insert("count", catalog().value("stations").toArray().size());
    return info.toVariantMap();
}

QVariantMap RadioPlayer::searchStations(const QString& query, int offset, int limit) const {
    if (query.size() > 256 || offset < 0 || limit < 1 || limit > 128)
        return {{"items", QVariantList{}}, {"total", 0}};
    const auto terms = searchText(query).split(' ', Qt::SkipEmptyParts);
    const auto manual = saved_.value("stations").toArray();
    const auto directory = catalog().value("stations").toArray();
    QList<std::pair<int, int>> matches;
    QSet<QString> urls, ids;
    for (int i = 0; i < manual.size() + directory.size(); ++i) {
        const auto item = (i < manual.size() ? manual[i] : directory[i - manual.size()]).toObject();
        const auto url = QUrl(item.value("url").toString()).toString(QUrl::FullyEncoded);
        const auto id = item.value("id").toString();
        if (urls.contains(url) || ids.contains(id)) continue;
        urls.insert(url); ids.insert(id);
        const auto metadata = item.value("country").toString() + ' ' + item.value("countryCode").toString()
            + ' ' + item.value("language").toString() + ' ' + item.value("tags").toString();
        const auto score = searchScore(terms, searchText(item.value("name").toString()), searchText(metadata));
        if (score >= 0) matches.append({score + (i < manual.size() ? 0 : 100000), i});
    }
    std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QVariantList items;
    for (qsizetype n = offset; n < matches.size() && items.size() < limit; ++n) {
        const auto i = matches[n].second;
        auto item = (i < manual.size() ? manual[i] : directory[i - manual.size()]).toObject().toVariantMap();
        item.insert("manual", i < manual.size()); items.append(item);
    }
    return {{"items", items}, {"total", matches.size()}};
}

bool RadioPlayer::persist(QJsonObject next) {
    QSaveFile file(storageFile_);
    const auto bytes = QJsonDocument(next).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error_ = tr("Radio settings could not be saved."); emit changed(); return false;
    }
    saved_ = std::move(next); error_.clear();
    return true;
}

bool RadioPlayer::saveStation(const QString& id, const QString& name, const QString& url) {
    if (checking()) { error_ = tr("A station is already being checked."); emit changed(); return false; }
    try {
        auto item = station(id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id, name, url);
        auto entries = saved_.value("stations").toArray();
        qsizetype index = -1;
        for (qsizetype i = 0; i < entries.size(); ++i) {
            const auto existing = entries[i].toObject();
            if (existing.value("id").toString() == id) index = i;
            if (existing.value("url") != item.value("url")) continue;
            if (!id.isEmpty() && existing.value("id").toString() != id) {
                error_ = tr("This stream is already saved as another station."); emit changed(); return false;
            }
            if (id.isEmpty()) { item.insert("id", existing.value("id")); index = i; }
        }
        if (!id.isEmpty() && index < 0) { error_ = tr("This station no longer exists."); emit changed(); return false; }
        if (index < 0 && entries.size() == 128) { error_ = tr("The station list is full."); emit changed(); return false; }
        pendingStation_ = item; pendingUpdate_ = index >= 0; error_.clear();
        auto* probe = new QMediaPlayer(this);
        auto* output = new QAudioBufferOutput(streamFormat(), probe);
        auto* deadline = new QTimer(probe);
        probe_ = probe;
        probe->setAudioBufferOutput(output);
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, probe, [this, probe] {
            if (probe_ == probe) finishStationCheck(false, tr("The stream did not provide audio within 15 seconds."));
        });
        connect(probe, &QMediaPlayer::errorOccurred, probe, [this, probe](QMediaPlayer::Error, const QString& error) {
            if (probe_ == probe) finishStationCheck(false, error);
        });
        connect(probe, &QMediaPlayer::mediaStatusChanged, probe, [this, probe](QMediaPlayer::MediaStatus status) {
            if (probe_ == probe && status == QMediaPlayer::EndOfMedia)
                finishStationCheck(false, tr("The stream ended before providing audio."));
        });
        connect(output, &QAudioBufferOutput::audioBufferReceived, probe, [this, probe](const QAudioBuffer& buffer) {
            if (probe_ != probe || !buffer.isValid()) return;
            if (buffer.format() != streamFormat() || buffer.sampleCount() <= 0 || buffer.sampleCount() > 48000
                || !std::all_of(buffer.constData<float>(), buffer.constData<float>() + buffer.sampleCount(),
                                [](float sample) { return std::isfinite(sample); })) {
                finishStationCheck(false, tr("The stream decoder returned an unsupported audio format.")); return;
            }
            finishStationCheck(true);
        });
        // Start after the caller can subscribe to the completion and update its UI.
        QTimer::singleShot(0, probe, [this, probe, deadline, item] {
            if (probe_ != probe) return;
            deadline->start(15000);
            probe->setSource(QUrl(item.value("url").toString())); probe->play();
        });
        emit changed(); return true;
    } catch (const std::invalid_argument& error) { error_ = QString::fromUtf8(error.what()); emit changed(); return false; }
}

void RadioPlayer::finishStationCheck(bool valid, QString error) {
    auto* probe = std::exchange(probe_, nullptr);
    if (!probe) return;
    const auto item = std::exchange(pendingStation_, {});
    const auto update = std::exchange(pendingUpdate_, false);
    // Unload the source before destroying its decoder. Stopping alone leaves
    // FFmpeg's demuxer waiting for network data while teardown joins its thread.
    probe->setSource({}); probe->setAudioBufferOutput(nullptr); probe->deleteLater();
    error_ = error;
    if (valid) {
        auto entries = saved_.value("stations").toArray();
        if (update) {
            for (qsizetype i = 0; i < entries.size(); ++i)
                if (entries[i].toObject().value("id") == item.value("id")) { entries[i] = item; break; }
        } else entries.prepend(item);
        auto next = saved_; next.insert("stations", entries);
        valid = persist(next);
        if (valid) emit stationsChanged();
        if (valid && active() && selectedId() == item.value("id").toString()) startStream();
    }
    emit changed();
    emit stationSaved(valid ? item.toVariantMap() : QVariantMap{}, valid);
}

bool RadioPlayer::cancelStationCheck() {
    finishStationCheck(false);
    return true;
}

bool RadioPlayer::removeStation(const QString& id) {
    if (checking() && pendingStation_.value("id").toString() == id) cancelStationCheck();
    auto entries = saved_.value("stations").toArray();
    qsizetype index = -1;
    for (qsizetype i = 0; i < entries.size(); ++i) if (entries[i].toObject().value("id").toString() == id) { index = i; break; }
    if (index < 0) { error_ = tr("This station no longer exists."); emit changed(); return false; }
    entries.removeAt(index);
    auto next = saved_; next.insert("stations", entries);
    const bool selected = selectedId() == id;
    if (selected) { next.insert("selected", ""); next.insert("playing", false); }
    if (!persist(next)) return false;
    if (selected) { halt(); state_ = "stopped"; notifySlotChanged(); }
    emit stationsChanged();
    emit changed(); return true;
}

bool RadioPlayer::play(const QString& id) {
    QJsonObject chosen;
    for (const auto entry : saved_.value("stations").toArray())
        if (entry.toObject().value("id").toString() == id) { chosen = entry.toObject(); break; }
    const bool manual = !chosen.isEmpty();
    if (!manual) {
        for (const auto entry : catalog().value("stations").toArray()) {
            const auto item = entry.toObject();
            if (item.value("id").toString() != id) continue;
            chosen = station(id, item.value("name").toString(), item.value("url").toString()); break;
        }
    }
    if (chosen.isEmpty()) {
        error_ = tr("Choose a saved station."); emit changed(); return false;
    }
    if (available_ && runningPlayers() - (slotHeld() ? 1 : 0) >= 2) {
        error_ = tr("Two radio streams are already playing."); emit changed(); return false;
    }
    auto next = saved_; next.insert("selected", id); next.insert("playing", true);
    if (manual) next.remove("catalogSelection"); else next.insert("catalogSelection", chosen);
    if (!persist(next)) return false;
    attempts_ = 0; startStream(); emit changed(); return true;
}
bool RadioPlayer::stop() {
    auto next = saved_; next.insert("playing", false);
    if (!persist(next)) return false;
    halt(); attempts_ = 0; state_ = "stopped"; emit changed(); notifySlotChanged(); return true;
}
bool RadioPlayer::setAvailable(bool available) {
    if (available_ == available) return true;
    available_ = available;
    if (available_ && active()) startStream();
    else { halt(); state_ = active() ? "unavailable" : "stopped"; emit changed(); notifySlotChanged(); }
    return true;
}
void RadioPlayer::halt() {
    stopping_ = true; retry_.stop(); tick_.stop(); player_.stop(); player_.setSource({}); stopping_ = false;
    packets_.clear(); encoder_.resetCapture(); received_.invalidate(); delivered_.invalidate(); stable_.invalidate();
}
void RadioPlayer::startStream() {
    halt();
    if (!active() || !available_) { state_ = active() ? "unavailable" : "stopped"; emit changed(); return; }
    if (runningPlayers() - (slotHeld() ? 1 : 0) >= 2) {
        state_ = "unavailable"; error_ = tr("Two radio streams are already playing."); emit changed(); return;
    }
    state_ = attempts_ == 0 ? "connecting" : "reconnecting";
    received_.start(); tick_.start(); emit changed();
    player_.setSource(QUrl(selectedStation().value("url").toString())); player_.play();
}
void RadioPlayer::reconnect(const QString& error) {
    if (stopping_ || !active() || !available_ || retry_.isActive()) return;
    halt(); error_ = error; state_ = "reconnecting";
    retry_.start(std::min(60000, 1000 * (1 << attempts_)));
    attempts_ = std::min(attempts_ + 1, 6);
    emit changed();
}
void RadioPlayer::receive(const QAudioBuffer& buffer) {
    if (!active() || !available_ || stopping_ || retry_.isActive() || !buffer.isValid()) return;
    if (buffer.format() != streamFormat() || buffer.sampleCount() > 48000) {
        reconnect(tr("The stream decoder returned an unsupported audio format.")); return;
    }
    try {
        const auto encoded = encoder_.encode(std::span(buffer.constData<float>(), size_t(buffer.sampleCount())), 48000);
        for (const auto& packet : encoded) {
            if (packets_.size() == 25) packets_.pop_front();
            packets_.push_back(packet);
        }
        received_.restart();
        if (!stable_.isValid()) stable_.start();
        if (stable_.elapsed() >= 30000) attempts_ = 0;
        if (state_ != "playing") { state_ = "playing"; error_.clear(); emit changed(); }
    } catch (const std::exception& error) { reconnect(QString::fromUtf8(error.what())); }
}
