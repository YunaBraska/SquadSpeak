#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QLocale>
#include <memory>
#include <span>

// Generates local speech PCM. The caller mixes it into its selected output and
// echo reference. No direct system playback, recording or network transmission.
class SpeechOutput final {
public:
    // Prefer quality, then the requested region, with a stable tie-breaker.
    // Automatic selection stays within the language. An explicit missing voice
    // stays unavailable. Personal voices require a separate authorization flow.
    static QVariantMap preferredVoice(const QVariantList& voices, QString language, const QString& selectedId = {}) {
        if (!selectedId.isEmpty()) {
            for (const auto& entry : voices) {
                const auto voice = entry.toMap();
                if (voice.value("id").toString() == selectedId && !voice.value("personal").toBool()) return voice;
            }
            return {};
        }
        language.replace('_', '-');
        const auto base = language.section('-', 0, 0);
        QVariantMap best;
        for (const auto& entry : voices) {
            const auto voice = entry.toMap();
            const auto id = voice.value("id").toString();
            const auto tag = voice.value("language").toString();
            if (id.isEmpty() || base.isEmpty() || tag.section('-', 0, 0).compare(base, Qt::CaseInsensitive)
                || voice.value("personal").toBool() || voice.value("novelty").toBool()) continue;
            // Chinese script variants require matching pronunciation and text.
            if (base == "zh" && QLocale(tag).script() != QLocale(language).script()) continue;
            const auto quality = voice.value("quality").toInt();
            const bool exact = tag.compare(language, Qt::CaseInsensitive) == 0;
            const auto bestQuality = best.value("quality").toInt();
            const bool bestExact = best.value("language").toString().compare(language, Qt::CaseInsensitive) == 0;
            if (best.isEmpty() || quality > bestQuality || (quality == bestQuality
                    && (exact > bestExact || (exact == bestExact && id < best.value("id").toString())))) best = voice;
        }
        return best;
    }
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    SpeechOutput();
    ~SpeechOutput();
    static constexpr bool supported = true;
    bool start(const QString& text, int sampleRate, const QString& voice = {});
    static QVariantList voices();
    void stop();
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool failed() const;
    bool mix(std::span<float> output, double gain, int channels = 1);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
#else
    static constexpr bool supported = false;
    bool start(const QString&, int, const QString& = {}) { return false; }
    static QVariantList voices() { return {}; }
    void stop() {}
    [[nodiscard]] bool active() const { return false; }
    [[nodiscard]] bool failed() const { return false; }
    bool mix(std::span<float>, double, int = 1) { return false; }
#endif
};
