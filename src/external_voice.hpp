#pragma once

#include "teamspeak_bridge/teamspeak_bridge.h"
#include <QStringView>
#include <QByteArrayView>
#include <QtEndian>

// The shared channel model owns lifecycle and presentation. Only connection
// identity, defaults and the wire adapter vary by external voice provider.
struct ExternalVoiceProvider final {
    const char* id;
    const char* name;
    const char* scheme;
    const char* pins;
    quint16 port;
    int maximumAudioPacket;
    void* (*open)(const uint8_t*, size_t);
};

inline const ExternalVoiceProvider* externalVoiceProvider(QStringView id) {
    static const ExternalVoiceProvider providers[] = {
        {"teamspeak", "TeamSpeak", "ts3", "teamSpeakPins", 9987, SS_TS_MAX_AUDIO, ss_ts_open},
        {"mumble", "Mumble", "mumble", "mumblePins", 64738, 1024 - 32, ss_mumble_open}
    };
    for (const auto& provider : providers)
        if (id == QLatin1StringView(provider.id)) return &provider;
    return nullptr;
}

inline bool mumbleGreeting(QByteArrayView header) {
    // The server speaks first with Version (type 0). This only selects an
    // adapter. Its TLS identity and protocol still have to authenticate there.
    return header.size() >= 6 && qFromBigEndian<quint16>(header.data()) == 0
        && qFromBigEndian<quint32>(header.data() + 2) > 0
        && qFromBigEndian<quint32>(header.data() + 2) <= 65536;
}
