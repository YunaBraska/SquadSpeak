#pragma once

#include <QByteArray>
#include <QString>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

// Local capture of exactly one output device. It never saves or transmits audio.
// The native worker owns all OS handles; stop joins it before destroying state.
class OutputMonitor final {
public:
    struct Block {
        std::vector<float> samples;
        int sampleRate = 0;
        bool discontinuity = false;
    };
    OutputMonitor() = default;
    ~OutputMonitor();
    // Failed or interrupted startup is not retried for the same device by
    // microphone/PTT lifecycle changes. Only an explicit user retry clears it.
    bool start(const QByteArray& deviceId, bool retry = false);
    void stop();
    [[nodiscard]] bool retryNeeded() const;
    [[nodiscard]] Block take();
    [[nodiscard]] QString error() const;
private:
    void capture(const QByteArray& deviceId);
    void append(std::span<const float> samples, int rate, bool discontinuity = false);
    void fail(QString error);
    mutable std::mutex mutex_;
    std::array<float, 19200> samples_{};
    size_t begin_ = 0, size_ = 0;
    int rate_ = 0;
    bool discontinuity_ = false;
    QString error_;
    QByteArray deviceId_;
    bool awaitingAudio_ = false;
    std::atomic<bool> stopping_{true};
    std::condition_variable wake_;
    std::thread worker_;
};
