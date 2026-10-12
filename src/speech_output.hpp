#pragma once

#include <QString>
#include <QVariantList>
#include <memory>
#include <span>

// Generates local speech PCM. The caller mixes it into its selected output and
// echo reference. No direct system playback, recording or network transmission.
class SpeechOutput final {
public:
#if defined(Q_OS_MACOS)
    SpeechOutput();
    ~SpeechOutput();
    static constexpr bool supported = true;
    bool start(const QString& text, int sampleRate, const QString& voice = {});
    static QVariantList voices();
    void stop();
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool failed() const;
    bool mix(std::span<float> output, double gain);
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
    bool mix(std::span<float>, double) { return false; }
#endif
};
