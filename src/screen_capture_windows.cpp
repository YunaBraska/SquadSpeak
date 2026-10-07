#include "screen_share.hpp"
#include "voice_mixer.hpp"
#include <QGuiApplication>
#include <QScopeGuard>
#include <QWindow>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;

// The OS retains this COM callback until activation completes. It owns its
// event and result, never a pointer into the screen-sharing QObject.
class AudioActivation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IActivateAudioInterfaceCompletionHandler, Microsoft::WRL::FtmBase> {
public:
    HANDLE done = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    PROPVARIANT argument{};
    ~AudioActivation() { if (done) CloseHandle(done); }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        ComPtr<IUnknown> activated;
        HRESULT status = E_FAIL;
        result = operation->GetActivateResult(&status, &activated);
        if (SUCCEEDED(result)) result = status;
        if (SUCCEEDED(result)) result = activated.As(&client);
        SetEvent(done);
        return S_OK;
    }
};

struct WindowsCapture final {
    std::vector<std::unique_ptr<QWindow>> windows;
    QList<DWORD> pids;
    HWND selected = nullptr;
    HANDLE process = nullptr;
    DWORD pid = 0;
    std::mutex mutex;
    std::deque<float> audio;
    QString error;
    bool ready = false;
    squad::VoiceMixer encoder;
    std::jthread worker;

    void stop() {
        worker.request_stop();
        if (worker.joinable()) worker.join();
        std::lock_guard lock(mutex);
        audio.clear(); error.clear(); ready = false; encoder.resetCapture();
    }
    ~WindowsCapture() { stop(); if (process) CloseHandle(process); }

    void capture(std::stop_token stop, DWORD target, bool wholeScreen) {
        const auto report = [this](HRESULT result) {
            if (FAILED(result)) {
                std::lock_guard lock(mutex);
                wchar_t text[1024]{};
                const auto size = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    nullptr, DWORD(result), 0, text, DWORD(std::size(text)), nullptr);
                error = QString::fromWCharArray(text, int(size)).trimmed();
                error += QStringLiteral(" (WASAPI 0x%1)").arg(quint32(result), 8, 16, QLatin1Char('0'));
            }
            return SUCCEEDED(result);
        };
        if (!report(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
        const auto activation = Microsoft::WRL::Make<AudioActivation>();
        if (!activation || !activation->done) { report(E_OUTOFMEMORY); return; }
        auto& parameters = activation->parameters;
        parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        parameters.ProcessLoopbackParams.TargetProcessId = target;
        parameters.ProcessLoopbackParams.ProcessLoopbackMode = wholeScreen
            ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        auto& argument = activation->argument;
        argument.vt = VT_BLOB; argument.blob.cbSize = sizeof(parameters);
        argument.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        if (!report(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                &argument, activation.Get(), &operation))) return;
        QElapsedTimer deadline; deadline.start();
        while (WaitForSingleObject(activation->done, 20) != WAIT_OBJECT_0) {
            if (stop.stop_requested()) return;
            if (deadline.elapsed() >= 5000) { report(HRESULT_FROM_WIN32(ERROR_TIMEOUT)); return; }
        }
        if (stop.stop_requested() || !report(activation->result)) return;
        const auto client = activation->client;
        // Request one canonical format from WASAPI. No endpoint-wide capture
        // path is used if process-loopback activation fails.
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; format.nChannels = 1;
        format.nSamplesPerSec = 48000; format.wBitsPerSample = 32;
        format.nBlockAlign = 4; format.nAvgBytesPerSec = 192000;
        if (!report(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                1000000, 0, &format, nullptr))) return;
        ComPtr<IAudioCaptureClient> capture;
        if (!report(client->GetService(IID_PPV_ARGS(&capture))) || !report(client->Start())) return;
        const auto stopClient = qScopeGuard([&] { client->Stop(); });
        { std::lock_guard lock(mutex); ready = true; }
        while (!stop.stop_requested()) {
            UINT32 available = 0;
            if (!report(capture->GetNextPacketSize(&available))) return;
            while (available && !stop.stop_requested()) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                if (!report(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return;
                {
                    const auto release = qScopeGuard([&] { capture->ReleaseBuffer(frames); });
                    const auto keep = std::min<UINT32>(frames, 4800);
                    const auto* values = reinterpret_cast<const float*>(data);
                    std::lock_guard lock(mutex);
                    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) audio.clear();
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) audio.insert(audio.end(), keep, 0);
                    else if (values) audio.insert(audio.end(), values + frames - keep, values + frames);
                    if (audio.size() > 4800) audio.erase(audio.begin(), audio.end() - 4800);
                }
                if (!report(capture->GetNextPacketSize(&available))) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};
}

bool ScreenShare::refreshNativeSources() {
    if (!native_) native_ = new WindowsCapture;
    auto& state = *static_cast<WindowsCapture*>(native_);
    state.windows.clear(); state.pids.clear(); windows_.clear(); screens_.clear(); sources_.clear();
    for (auto* screen : QGuiApplication::screens()) {
        screens_.append(screen);
        sources_.append(QVariantMap{{"index", sources_.size()}, {"name", screen->name()}, {"kind", "screen"}});
    }
    EnumWindows([](HWND handle, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<WindowsCapture*>(data);
        DWORD pid = 0;
        GetWindowThreadProcessId(handle, &pid);
        if (!pid || !IsWindowVisible(handle) || GetWindowTextLengthW(handle) == 0) return TRUE;
        auto window = std::unique_ptr<QWindow>(QWindow::fromWinId(reinterpret_cast<WId>(handle)));
        if (!window || !QCapturableWindow(window.get()).isValid()) return TRUE;
        state.windows.push_back(std::move(window)); state.pids.append(pid);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    for (const auto& window : state.windows) {
        const QCapturableWindow capture(window.get());
        windows_.append(capture);
        sources_.append(QVariantMap{{"index", sources_.size()}, {"name", capture.description()}, {"kind", "window"}});
    }
    error_.clear(); emit sourcesChanged(); emit changed(); return true;
}

bool ScreenShare::startNative(int index) {
    auto& state = *static_cast<WindowsCapture*>(native_);
    if (state.process) { CloseHandle(state.process); state.process = nullptr; }
    state.selected = nullptr; state.pid = GetCurrentProcessId();
    if (index >= screens_.size()) {
        const auto window = size_t(index - screens_.size());
        state.selected = reinterpret_cast<HWND>(state.windows[window]->winId());
        DWORD pid = 0; GetWindowThreadProcessId(state.selected, &pid);
        if (!pid || pid != state.pids[index - screens_.size()])
            return fail(tr("The selected window is no longer available."));
        state.pid = pid;
        state.process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!state.process) return fail(tr("The selected window is no longer available."));
    }
    if (audioEnabled_ && !updateNativeAudio()) return false;
    captureTick_.start(); return true;
}

bool ScreenShare::updateNativeAudio() {
    auto& state = *static_cast<WindowsCapture*>(native_);
    state.stop();
    if (captureHost_) captureHost_->setScreenAudio(false);
    if (!audioEnabled_) return true;
    // Capturing our own playback would feed received speech back into music.
    if (state.selected && state.pid == GetCurrentProcessId()) {
        audioEnabled_ = false; emit changed(); return true;
    }
    state.worker = std::jthread([&state, target = state.pid, wholeScreen = !state.selected](std::stop_token stop) {
        state.capture(stop, target, wholeScreen);
    });
    return true;
}

void ScreenShare::pollNative() {
    auto& state = *static_cast<WindowsCapture*>(native_);
    if (!active()) { stop(); return; }
    std::array<float, 960> samples{};
    bool ready = false, hasAudio = false;
    QString error;
    {
        std::lock_guard lock(state.mutex);
        ready = state.ready; error = std::move(state.error);
        if (audioEnabled_ && state.audio.size() >= samples.size()) {
            std::copy_n(state.audio.begin(), samples.size(), samples.begin());
            state.audio.erase(state.audio.begin(), state.audio.begin() + samples.size()); hasAudio = true;
        }
    }
    if (!error.isEmpty()) { fail(error); return; }
    if (ready && audioEnabled_) captureHost_->setScreenAudio(true);
    if (hasAudio) {
        try { for (const auto& packet : state.encoder.encode(samples, 48000)) captureHost_->sendScreenAudio(packet); }
        catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
    }
}

void ScreenShare::checkNativeSource() {
    if (!native_ || !active()) return;
    const auto& state = *static_cast<WindowsCapture*>(native_);
    DWORD pid = 0;
    if (state.selected) GetWindowThreadProcessId(state.selected, &pid);
    if (state.selected && (pid != state.pid || WaitForSingleObject(state.process, 0) != WAIT_TIMEOUT))
        fail(tr("The shared window was closed. Select a source to start again."));
}

void ScreenShare::stopNative() {
    captureTick_.stop();
    if (!native_) return;
    auto& state = *static_cast<WindowsCapture*>(native_);
    state.stop();
    if (state.process) { CloseHandle(state.process); state.process = nullptr; }
    state.selected = nullptr;
}
void ScreenShare::disposeNative() { delete static_cast<WindowsCapture*>(native_); native_ = nullptr; }
