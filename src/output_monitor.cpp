#include "output_monitor.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

OutputMonitor::~OutputMonitor() { stop(); }

bool OutputMonitor::start(const QByteArray& deviceId) {
    stop();
    if (deviceId.isEmpty()) return false;
    stopping_ = false;
    worker_ = std::thread([this, deviceId] { if (!stopping_) capture(deviceId); });
    return true;
}

void OutputMonitor::stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    begin_ = size_ = 0; rate_ = 0; discontinuity_ = false; error_.clear();
}

void OutputMonitor::append(std::span<const float> samples, int rate, bool discontinuity) {
    if (rate < 8000 || rate > 192000 || rate % 100) { fail(QStringLiteral("Unsupported output monitor sample rate")); return; }
    std::lock_guard lock(mutex_);
    if (!error_.isEmpty()) return;
    if (rate_ != rate || discontinuity) { begin_ = size_ = 0; discontinuity_ = true; }
    rate_ = rate;
    const auto limit = size_t(rate / 10);
    if (samples.size() > limit) { samples = samples.last(limit); begin_ = size_ = 0; discontinuity_ = true; }
    for (float sample : samples) {
        if (size_ == limit) { begin_ = (begin_ + 1) % samples_.size(); --size_; discontinuity_ = true; }
        samples_[(begin_ + size_++) % samples_.size()] = std::isfinite(sample) ? std::clamp(sample, -1.f, 1.f) : 0;
    }
}

OutputMonitor::Block OutputMonitor::take() {
    std::lock_guard lock(mutex_);
    Block block;
    block.sampleRate = rate_; block.discontinuity = std::exchange(discontinuity_, false);
    block.samples.reserve(size_);
    for (size_t i = 0; i < size_; ++i) block.samples.push_back(samples_[(begin_ + i) % samples_.size()]);
    begin_ = size_ = 0;
    return block;
}

QString OutputMonitor::error() const { std::lock_guard lock(mutex_); return error_; }
void OutputMonitor::fail(QString error) {
    std::lock_guard lock(mutex_);
    error_ = std::move(error); begin_ = size_ = 0; discontinuity_ = true;
}

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN) && (!defined(Q_OS_LINUX) || defined(Q_OS_ANDROID))
void OutputMonitor::capture(const QByteArray&) { fail(QStringLiteral("Output monitor unavailable on this platform")); }
#endif
