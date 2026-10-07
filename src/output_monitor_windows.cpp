#include "output_monitor.hpp"

#ifdef Q_OS_WIN

#include <QScopeGuard>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {
struct NativeFormat final {
    int channels = 0;
    int rate = 0;
    int bits = 0;
    bool floating = false;
    bool pcm = false;
};

bool describeFormat(const WAVEFORMATEX* format, NativeFormat& result) {
    if (!format || format->nChannels == 0 || format->nSamplesPerSec == 0) return false;
    result.channels = format->nChannels;
    result.rate = int(format->nSamplesPerSec);
    result.bits = int(format->wBitsPerSample);
    result.floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    result.pcm = format->wFormatTag == WAVE_FORMAT_PCM;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        result.floating = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        result.pcm = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }
    return result.channels <= 32 && result.rate >= 8000 && result.rate <= 192000
        && result.rate % 100 == 0 && ((result.floating && result.bits == 32)
        || (result.pcm && (result.bits == 16 || result.bits == 32)));
}
}

void OutputMonitor::capture(const QByteArray& deviceId) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        fail(QStringLiteral("Could not initialize Windows audio monitoring.")); return;
    }
    const auto uninitialize = qScopeGuard([] { CoUninitialize(); });
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        fail(QStringLiteral("Could not enumerate Windows audio devices.")); return;
    }
    const auto id = QString::fromUtf8(deviceId).toStdWString();
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(id.c_str(), &device))) {
        fail(QStringLiteral("The selected output device is no longer available.")); return;
    }
    ComPtr<IMMEndpoint> endpoint;
    EDataFlow flow = eAll;
    if (FAILED(device.As(&endpoint)) || FAILED(endpoint->GetDataFlow(&flow)) || flow != eRender) {
        fail(QStringLiteral("The selected device is not an audio output.")); return;
    }
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf())))) {
        fail(QStringLiteral("The selected output device could not be opened.")); return;
    }
    WAVEFORMATEX* rawFormat = nullptr;
    if (FAILED(client->GetMixFormat(&rawFormat))) {
        fail(QStringLiteral("The selected output format could not be read.")); return;
    }
    const auto releaseFormat = qScopeGuard([&] { CoTaskMemFree(rawFormat); });
    NativeFormat format;
    if (!describeFormat(rawFormat, format)) {
        fail(QStringLiteral("The selected output format is unsupported.")); return;
    }
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  1000000, 0, rawFormat, nullptr))) {
        fail(QStringLiteral("The selected output loopback could not be opened.")); return;
    }
    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) {
        fail(QStringLiteral("The selected output loopback could not be started.")); return;
    }
    const auto stopClient = qScopeGuard([&] { client->Stop(); });
    std::vector<float> mono;
    while (!stopping_.load()) {
        UINT32 packets = 0;
        if (FAILED(capture->GetNextPacketSize(&packets))) {
            fail(QStringLiteral("The selected output loopback stopped.")); return;
        }
        while (packets && !stopping_.load()) {
            BYTE* bytes = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&bytes, &frames, &flags, nullptr, nullptr))) {
                fail(QStringLiteral("The selected output loopback could not be read.")); return;
            }
            auto release = qScopeGuard([&] { capture->ReleaseBuffer(frames); });
            if (frames > 19200) {
                fail(QStringLiteral("The selected output loopback packet is too large.")); return;
            }
            mono.resize(frames);
            const auto discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(mono.begin(), mono.end(), 0.0f);
            } else if (!bytes) {
                std::fill(mono.begin(), mono.end(), 0.0f);
            } else if (format.floating) {
                const auto* values = reinterpret_cast<const float*>(bytes);
                for (UINT32 frame = 0; frame < frames; ++frame) {
                    float sum = 0;
                    for (int channel = 0; channel < format.channels; ++channel)
                        sum += values[frame * format.channels + channel];
                    mono[frame] = sum / float(format.channels);
                }
            } else {
                if (format.bits == 16) {
                    const auto* values = reinterpret_cast<const qint16*>(bytes);
                    for (UINT32 frame = 0; frame < frames; ++frame) {
                        int sum = 0;
                        for (int channel = 0; channel < format.channels; ++channel)
                            sum += values[frame * format.channels + channel];
                        mono[frame] = float(sum) / float(format.channels * 32768.0);
                    }
                } else {
                    const auto* values = reinterpret_cast<const qint32*>(bytes);
                    for (UINT32 frame = 0; frame < frames; ++frame) {
                        qint64 sum = 0;
                        for (int channel = 0; channel < format.channels; ++channel)
                            sum += values[frame * format.channels + channel];
                        mono[frame] = float(double(sum) / (double(format.channels) * 2147483648.0));
                    }
                }
            }
            append(mono, format.rate, discontinuity);
            release.dismiss();
            if (FAILED(capture->ReleaseBuffer(frames))) {
                fail(QStringLiteral("The selected output loopback buffer could not be released.")); return;
            }
            if (FAILED(capture->GetNextPacketSize(&packets))) {
                fail(QStringLiteral("The selected output loopback stopped.")); return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

#endif
