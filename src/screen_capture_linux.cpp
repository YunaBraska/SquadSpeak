#include "screen_share.hpp"
#include "voice_mixer.hpp"
#include <QFile>
#include <QGuiApplication>
#include <QScopeGuard>
#include <QSysInfo>
#include <QWindow>
#include <algorithm>
#include <deque>
#include <map>
#include <pulse/pulseaudio.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

namespace {
// Linux PIDs alone are reusable. Retain /proc's birth time for the selected
// process; only that process and its current descendants may supply audio.
QList<QByteArray> processInfo(qint64 pid) {
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto data = file.read(4096);
    const auto end = data.lastIndexOf(')');
    return end >= 0 ? data.mid(end + 2).simplified().split(' ') : QList<QByteArray>{};
}

struct LinuxCapture final {
    Display* display = nullptr;
    QList<Window> windows;
    Window selected = 0;
    qint64 pid = 0;
    QByteArray birth;
    pa_mainloop* loop = nullptr;
    pa_context* context = nullptr;
    QString error;
    bool scanning = false, rescan = false, ready = false;
    QElapsedTimer startup;
    struct Track final {
        pa_stream* stream = nullptr;
        uint32_t sink = PA_INVALID_INDEX;
        qint64 pid = 0;
        bool ready = false;
        std::deque<float> samples;
        ~Track() { if (stream) { pa_stream_disconnect(stream); pa_stream_unref(stream); } }
    };
    std::map<uint32_t, std::unique_ptr<Track>> tracks;
    std::map<uint32_t, QByteArray> monitors;
    std::map<uint32_t, qint64> clients;
    std::map<uint32_t, std::pair<uint32_t, qint64>> desired;
    squad::VoiceMixer encoder;

    ~LinuxCapture() { stop(); if (display) XCloseDisplay(display); }
    void stop() {
        tracks.clear(); monitors.clear(); clients.clear(); desired.clear();
        if (context) { pa_context_disconnect(context); pa_context_unref(context); context = nullptr; }
        if (loop) { pa_mainloop_free(loop); loop = nullptr; }
        scanning = rescan = ready = false; error.clear(); encoder.resetCapture();
    }
    void report(int code) { if (error.isEmpty()) error = QString::fromUtf8(pa_strerror(code)); }
    void operation(pa_operation* value) {
        if (value) pa_operation_unref(value);
        else report(pa_context_errno(context));
    }
    qint64 windowPid(Window window) const {
        XTextProperty machine{};
        if (!XGetWMClientMachine(display, window, &machine) || !machine.value) return 0;
        const auto releaseMachine = qScopeGuard([&] { XFree(machine.value); });
        if (machine.format != 8 || machine.nitems > 255
            || QString::fromUtf8(reinterpret_cast<const char*>(machine.value), qsizetype(machine.nitems))
                != QSysInfo::machineHostName()) return 0;
        const auto atom = XInternAtom(display, "_NET_WM_PID", True);
        if (!atom) return 0;
        Atom type = 0; int format = 0; unsigned long count = 0, after = 0; unsigned char* data = nullptr;
        const auto status = XGetWindowProperty(display, window, atom, 0, 1, False, XA_CARDINAL,
            &type, &format, &count, &after, &data);
        const auto release = qScopeGuard([&] { if (data) XFree(data); });
        return status == Success && type == XA_CARDINAL && format == 32 && count == 1
            ? qint64(*reinterpret_cast<unsigned long*>(data)) : 0;
    }
    bool belongs(qint64 candidate) const {
        for (int depth = 0; candidate > 1 && depth < 64; ++depth) {
            const auto info = processInfo(candidate);
            if (info.size() <= 19) return false;
            if (candidate == pid) return info[19] == birth;
            const auto parent = info[1].toLongLong();
            if (parent == candidate) return false;
            candidate = parent;
        }
        return false;
    }
    void reconcile() {
        std::erase_if(tracks, [&](const auto& entry) {
            const auto match = desired.find(entry.first);
            return match == desired.end() || match->second != std::pair(entry.second->sink, entry.second->pid);
        });
        for (const auto& [id, target] : desired) {
            if (tracks.contains(id)) continue;
            const auto monitor = monitors.find(target.first);
            if (monitor == monitors.end()) { rescan = true; continue; }
            if (tracks.size() == 32) { report(PA_ERR_TOOLARGE); break; }
            auto track = std::make_unique<Track>();
            track->sink = target.first; track->pid = target.second;
            const pa_sample_spec format{PA_SAMPLE_FLOAT32NE, 48000, 1};
            track->stream = pa_stream_new(context, "SquadSpeak", &format, nullptr);
            const pa_buffer_attr buffers{4800 * sizeof(float), uint32_t(-1), uint32_t(-1), uint32_t(-1), 960 * sizeof(float)};
            if (!track->stream || pa_stream_set_monitor_stream(track->stream, id) < 0
                || pa_stream_connect_record(track->stream, monitor->second.constData(), &buffers,
                    pa_stream_flags_t(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE)) < 0) {
                report(pa_context_errno(context)); break;
            }
            tracks.emplace(id, std::move(track));
        }
        scanning = false; ready = error.isEmpty();
    }
    static void inputs(pa_context*, const pa_sink_input_info* info, int end, void* data) {
        auto& state = *static_cast<LinuxCapture*>(data);
        if (end < 0) { state.report(pa_context_errno(state.context)); state.scanning = false; return; }
        if (end) { state.reconcile(); return; }
        // Native PipeWire streams need not repeat their owner's process ID.
        const auto owner = state.clients.find(info->client);
        // A missing identity is never permission to monitor the entire sink.
        if (owner == state.clients.end()) return;
        const auto pid = owner->second;
        if (state.selected ? !state.belongs(pid) : state.belongs(pid)) return;
        if (state.desired.size() == 32) { state.report(PA_ERR_TOOLARGE); return; }
        state.desired[info->index] = {info->sink, pid};
    }
    static void sinks(pa_context*, const pa_sink_info* info, int end, void* data) {
        auto& state = *static_cast<LinuxCapture*>(data);
        if (end < 0) { state.report(pa_context_errno(state.context)); state.scanning = false; return; }
        if (end) { state.operation(pa_context_get_sink_input_info_list(state.context, inputs, data)); return; }
        if (state.monitors.size() == 256) { state.report(PA_ERR_TOOLARGE); return; }
        if (info->monitor_source_name) state.monitors[info->index] = info->monitor_source_name;
    }
    static void owners(pa_context*, const pa_client_info* info, int end, void* data) {
        auto& state = *static_cast<LinuxCapture*>(data);
        if (end < 0) { state.report(pa_context_errno(state.context)); state.scanning = false; return; }
        if (end) { state.operation(pa_context_get_sink_info_list(state.context, sinks, data)); return; }
        if (state.clients.size() == 4096) { state.report(PA_ERR_TOOLARGE); return; }
        const auto* property = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_ID);
        const auto pid = property ? QByteArray(property).toLongLong() : 0;
        if (pid > 1) state.clients[info->index] = pid;
    }
    bool start() {
        stop(); startup.start();
        loop = pa_mainloop_new();
        if (!loop) { report(PA_ERR_INTERNAL); return false; }
        context = pa_context_new(pa_mainloop_get_api(loop), "SquadSpeak");
        if (!context) { report(PA_ERR_INTERNAL); return false; }
        pa_context_set_state_callback(context, [](pa_context* context, void* data) {
            auto& state = *static_cast<LinuxCapture*>(data);
            if (pa_context_get_state(context) == PA_CONTEXT_READY) {
                if (pa_context_is_local(context) != 1) { state.report(PA_ERR_NOTSUPPORTED); return; }
                pa_context_set_subscribe_callback(context, [](pa_context*, pa_subscription_event_type_t, uint32_t, void* data) {
                    static_cast<LinuxCapture*>(data)->rescan = true;
                }, data);
                state.operation(pa_context_subscribe(context,
                    pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_CLIENT),
                    [](pa_context* context, int success, void* data) {
                        if (!success) static_cast<LinuxCapture*>(data)->report(pa_context_errno(context));
                    }, data));
                state.rescan = true;
            } else if (pa_context_get_state(context) == PA_CONTEXT_FAILED)
                state.report(pa_context_errno(context));
        }, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) report(pa_context_errno(context));
        return error.isEmpty();
    }
    bool read(std::array<float, 960>& mixed) {
        if (!ready && startup.elapsed() >= 5000) { report(PA_ERR_TIMEOUT); return false; }
        // All Pulse callbacks run on this bounded, nonblocking mainloop pump.
        // No capture thread or queued Qt callback survives audio disable.
        for (int i = 0; i < 8; ++i) {
            const auto result = pa_mainloop_iterate(loop, 0, nullptr);
            if (result < 0) { report(PA_ERR_CONNECTIONTERMINATED); return false; }
            if (result == 0) break;
        }
        if (rescan && !scanning && pa_context_get_state(context) == PA_CONTEXT_READY) {
            rescan = false; scanning = true; monitors.clear(); clients.clear(); desired.clear();
            operation(pa_context_get_client_info_list(context, owners, this));
        }
        bool hasAudio = false;
        for (auto it = tracks.begin(); it != tracks.end();) {
            auto& track = *it->second;
            const auto state = pa_stream_get_state(track.stream);
            if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
                if (state == PA_STREAM_FAILED && !track.ready) report(pa_context_errno(context));
                it = tracks.erase(it); rescan = true; continue;
            }
            ++it;
            if (state != PA_STREAM_READY) continue;
            track.ready = true;
            while (pa_stream_readable_size(track.stream) > 0) {
                const void* bytes = nullptr; size_t size = 0;
                if (pa_stream_peek(track.stream, &bytes, &size) < 0) { report(pa_context_errno(context)); break; }
                if (size == 0) break;
                const auto count = size / sizeof(float), keep = std::min<size_t>(count, 4800);
                if (bytes) {
                    const auto* values = static_cast<const float*>(bytes);
                    track.samples.insert(track.samples.end(), values + count - keep, values + count);
                } else track.samples.insert(track.samples.end(), keep, 0);
                if (pa_stream_drop(track.stream) < 0) { report(pa_context_errno(context)); break; }
                if (track.samples.size() > 4800) track.samples.erase(track.samples.begin(), track.samples.end() - 4800);
            }
            if (track.samples.size() >= mixed.size()) {
                for (auto& sample : mixed) { sample += track.samples.front(); track.samples.pop_front(); }
                hasAudio = true;
            }
        }
        for (auto& sample : mixed) sample = std::clamp(sample, -1.0f, 1.0f);
        return hasAudio;
    }
};
}

bool ScreenShare::refreshNativeSources() {
    if (!native_) native_ = new LinuxCapture;
    auto& state = *static_cast<LinuxCapture*>(native_);
    screens_.clear(); sources_.clear(); windows_ = QWindowCapture::capturableWindows();
    for (auto* screen : QGuiApplication::screens()) {
        screens_.append(screen);
        sources_.append(QVariantMap{{"index", sources_.size()}, {"name", screen->name()}, {"kind", "screen"}});
    }
    for (const auto& window : windows_)
        sources_.append(QVariantMap{{"index", sources_.size()}, {"name", window.description()}, {"kind", "window"}});
    state.windows.fill(0, windows_.size());
    if (QGuiApplication::platformName() == "xcb") {
        if (!state.display) state.display = XOpenDisplay(nullptr);
        if (state.display) {
            const auto atom = XInternAtom(state.display, "_NET_CLIENT_LIST", True);
            Atom type = 0; int format = 0; unsigned long count = 0, after = 0; unsigned char* data = nullptr;
            const auto release = qScopeGuard([&] { if (data) XFree(data); });
            if (atom && XGetWindowProperty(state.display, DefaultRootWindow(state.display), atom, 0, 4096,
                    False, XA_WINDOW, &type, &format, &count, &after, &data) == Success
                && type == XA_WINDOW && format == 32 && data) {
                const auto* ids = reinterpret_cast<const Window*>(data);
                for (unsigned long i = 0; i < count; ++i) {
                    const auto window = std::unique_ptr<QWindow>(QWindow::fromWinId(ids[i]));
                    if (!window) continue;
                    const auto index = windows_.indexOf(QCapturableWindow(window.get()));
                    if (index >= 0) state.windows[index] = ids[i];
                }
            }
        }
    }
    error_.clear(); emit sourcesChanged(); emit changed(); return true;
}

bool ScreenShare::startNative(int index) {
    auto& state = *static_cast<LinuxCapture*>(native_);
    state.selected = index >= screens_.size() ? state.windows.value(index - screens_.size()) : 0;
    state.pid = state.selected ? state.windowPid(state.selected) : QCoreApplication::applicationPid();
    state.birth = processInfo(state.pid).value(19);
    // Decide scope before presenting the audio toggle. A missing app identity
    // permits only an explicitly labelled computer-audio selection.
    if (!state.selected || state.birth.isEmpty()) {
        state.selected = 0;
        state.pid = QCoreApplication::applicationPid();
        state.birth = processInfo(state.pid).value(19);
        computerAudio_ = true;
    }
    if (audioEnabled_ && !updateNativeAudio()) return false;
    return true;
}
bool ScreenShare::updateNativeAudio() {
    auto& state = *static_cast<LinuxCapture*>(native_);
    captureTick_.stop(); state.stop();
    if (captureHost_) captureHost_->setScreenAudio(false);
    if (!audioEnabled_) return true;
    if (state.birth.isEmpty())
        return fail(QString::fromUtf8(pa_strerror(PA_ERR_NOTSUPPORTED)));
    if (state.selected && state.pid == QCoreApplication::applicationPid()) {
        audioEnabled_ = false; emit changed(); return true;
    }
    if (!state.start()) { const auto error = state.error; return fail(error); }
    captureTick_.start(); return true;
}
void ScreenShare::pollNative() {
    if (!active()) { stop(); return; }
    auto& state = *static_cast<LinuxCapture*>(native_);
    std::array<float, 960> samples{};
    const auto hasAudio = state.read(samples);
    if (!state.error.isEmpty()) { const auto error = state.error; fail(error); return; }
    if (state.ready) captureHost_->setScreenAudio(true);
    if (hasAudio) {
        try { for (const auto& packet : state.encoder.encode(samples, 48000)) captureHost_->sendScreenAudio(packet); }
        catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
    }
}
void ScreenShare::checkNativeSource() {
    if (!native_ || !active()) return;
    const auto& state = *static_cast<LinuxCapture*>(native_);
    if (capture_.windowCapture() && (!window_.window().isValid()
            || (state.selected && (state.windowPid(state.selected) != state.pid || processInfo(state.pid).value(19) != state.birth))))
        fail(tr("The shared window was closed. Select a source to start again."));
}
void ScreenShare::stopNative() {
    captureTick_.stop();
    if (native_) static_cast<LinuxCapture*>(native_)->stop();
}
void ScreenShare::disposeNative() { delete static_cast<LinuxCapture*>(native_); native_ = nullptr; }
