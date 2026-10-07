#pragma once

#include <QImage>
#include <QJsonObject>
#include <memory>

class QVideoFrame;

// Bounded, low-delay video encoding/decoding. Calls to an instance are serial.
// Native encoding and destruction must stay on one thread; software decoding
// can migrate between jobs. Viewers share packets, never decoder contexts.
class VideoCodec final {
public:
    VideoCodec();
    ~VideoCodec();
    VideoCodec(const VideoCodec&) = delete;
    VideoCodec& operator=(const VideoCodec&) = delete;
    // Tiers: UHD/30, 1080/30, 720/15, 360/8. Smaller sources are not enlarged.
    // An empty packet means no frame yet; format.overloaded additionally asks
    // the caller to reduce demand after bounded encoder backlog/packet overflow.
    QPair<QJsonObject, QByteArray> encode(const QImage& image, int tier, bool keyFrame);
    // Captured packed CPU frames are read directly while mapped. Other layouts
    // retain Qt's conversion, including surface orientation and mirroring.
    QPair<QJsonObject, QByteArray> encode(const QVideoFrame& frame, int tier, bool keyFrame);
    // Rejects unsupported codecs, oversized sources and malformed packets.
    QImage decode(const QJsonObject& format, const QByteArray& packet);
    static int framePeriod(int tier);
private:
    struct State;
    std::unique_ptr<State> state_;
};
