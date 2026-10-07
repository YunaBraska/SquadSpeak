#include "output_monitor.hpp"

#ifdef Q_OS_LINUX

#include <QElapsedTimer>
#include <pulse/pulseaudio.h>
#include <algorithm>

namespace {
struct PulseState final {
    pa_mainloop* loop = nullptr;
    pa_context* context = nullptr;
    pa_stream* stream = nullptr;
    QByteArray requested;
    QByteArray monitor;
    QString error;
    bool sinkDone = false;
    bool streamReady = false;
    bool iterate() {
        // Dispatch pending protocol/deferred events immediately. Only the
        // idle poll waits, bounded to 10 ms so stop can join promptly.
        return pa_mainloop_prepare(loop, 10000) >= 0
            && pa_mainloop_poll(loop) >= 0 && pa_mainloop_dispatch(loop) >= 0;
    }
};

void sinkInfo(pa_context* context, const pa_sink_info* info, int end, void* data) {
    auto& state = *static_cast<PulseState*>(data);
    if (end < 0) state.error = QStringLiteral("Could not enumerate output devices.");
    else if (end) state.sinkDone = true;
    else if (info && info->name && QByteArray(info->name) == state.requested
             && info->monitor_source_name)
        state.monitor = info->monitor_source_name;
    Q_UNUSED(context);
}

void streamState(pa_stream*, void* data) {
    auto& state = *static_cast<PulseState*>(data);
    if (!state.stream) return;
    const auto streamState = pa_stream_get_state(state.stream);
    if (streamState == PA_STREAM_READY) state.streamReady = true;
    else if (streamState == PA_STREAM_FAILED || streamState == PA_STREAM_TERMINATED)
        state.error = QStringLiteral("The selected output monitor stopped.");
}

}

void OutputMonitor::capture(const QByteArray& deviceId) {
    PulseState state;
    state.requested = deviceId;
    state.loop = pa_mainloop_new();
    if (!state.loop) { fail(QStringLiteral("Could not initialize the Linux output monitor.")); return; }
    state.context = pa_context_new(pa_mainloop_get_api(state.loop), "SquadSpeak output monitor");
    if (!state.context || pa_context_connect(state.context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
        fail(QStringLiteral("Could not connect to the local audio server."));
        if (state.context) pa_context_unref(state.context);
        pa_mainloop_free(state.loop); return;
    }
    QElapsedTimer timer; timer.start();
    while (pa_context_get_state(state.context) != PA_CONTEXT_READY && timer.elapsed() < 5000
           && !stopping_.load()) {
        if (!state.iterate()) break;
    }
    if (stopping_.load()) { pa_context_disconnect(state.context); pa_context_unref(state.context); pa_mainloop_free(state.loop); return; }
    if (pa_context_get_state(state.context) != PA_CONTEXT_READY) state.error = QStringLiteral("The local audio server is unavailable.");
    if (state.error.isEmpty()) {
        auto* operation = pa_context_get_sink_info_list(state.context, sinkInfo, &state);
        if (!operation) state.error = QStringLiteral("Could not enumerate output devices.");
        else pa_operation_unref(operation);
    }
    while (!state.sinkDone && state.error.isEmpty() && timer.elapsed() < 5000 && !stopping_.load()) {
        if (!state.iterate()) state.error = QStringLiteral("The local audio server stopped.");
    }
    if (state.monitor.isEmpty() && state.error.isEmpty()) state.error = QStringLiteral("The selected output device is unavailable.");
    if (state.error.isEmpty() && !stopping_.load()) {
        const pa_sample_spec format{PA_SAMPLE_FLOAT32NE, 48000, 1};
        state.stream = pa_stream_new(state.context, "SquadSpeak output monitor", &format, nullptr);
        if (state.stream) pa_stream_set_state_callback(state.stream, streamState, &state);
        const pa_buffer_attr buffers{4800 * sizeof(float), uint32_t(-1), uint32_t(-1), uint32_t(-1), 480 * sizeof(float)};
        if (!state.stream || pa_stream_connect_record(state.stream, state.monitor.constData(), &buffers,
                pa_stream_flags_t(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE)) < 0)
            state.error = QStringLiteral("The selected output monitor could not be opened.");
    }
    while (!state.streamReady && state.error.isEmpty() && timer.elapsed() < 5000 && !stopping_.load()) {
        if (!state.iterate()) state.error = QStringLiteral("The output monitor stopped.");
    }
    if (!state.streamReady && state.error.isEmpty() && !stopping_.load())
        state.error = QStringLiteral("Timed out opening the selected output monitor.");
    while (state.error.isEmpty() && !stopping_.load()) {
        if (!state.iterate()) { state.error = QStringLiteral("The output monitor stopped."); break; }
        while (state.stream && state.error.isEmpty() && !stopping_.load()) {
            const auto pending = pa_stream_readable_size(state.stream);
            if (pending == size_t(-1)) { state.error = QStringLiteral("The output monitor could not be read."); break; }
            if (pending == 0) break;
            const void* bytes = nullptr; size_t size = 0;
            if (pa_stream_peek(state.stream, &bytes, &size) < 0) { state.error = QStringLiteral("The output monitor could not be read."); break; }
            if (size) {
                const auto count = size / sizeof(float);
                if (bytes) append(std::span(static_cast<const float*>(bytes), count), 48000);
                else {
                    static const std::array<float, 4800> silence{};
                    for (size_t offset = 0; offset < count; offset += silence.size())
                        append(std::span(silence).first(std::min(silence.size(), count - offset)), 48000);
                }
            }
            if (pa_stream_drop(state.stream) < 0) { state.error = QStringLiteral("The output monitor could not be released."); break; }
        }
    }
    if (!state.error.isEmpty()) fail(state.error);
    if (state.stream) { pa_stream_disconnect(state.stream); pa_stream_unref(state.stream); }
    pa_context_disconnect(state.context); pa_context_unref(state.context); pa_mainloop_free(state.loop);
}

#endif
