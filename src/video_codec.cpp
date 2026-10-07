#include "video_codec.hpp"
#include <QCoreApplication>
#include <QScopeGuard>
#include <QVideoFrame>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include <array>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <objbase.h>
#endif

namespace {
constexpr qsizetype maximumPacket = 2 * 1024 * 1024;
constexpr std::array<int, 4> heights{2160, 1080, 720, 360};
constexpr std::array<int, 4> rates{30, 30, 15, 8};
constexpr std::array<int, 4> bitrates{12000000, 4000000, 1500000, 350000};
void check(int status) {
    if (status >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE]; av_strerror(status, text, sizeof(text));
    throw std::runtime_error(QCoreApplication::translate("VideoCodec", "Video codec: %1")
        .arg(QString::fromUtf8(text)).toStdString());
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(QCoreApplication::translate("VideoCodec", message).toStdString());
}
}

struct VideoCodec::State final {
    AVCodecContext* encoder = nullptr;
    AVCodecContext* decoder = nullptr;
    AVFrame* input = av_frame_alloc();
    AVFrame* output = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    SwsContext* scaleIn = nullptr;
    SwsContext* scaleOut = nullptr;
    QJsonObject encodedFormat, decodedFormat;
    qint64 sequence = 0;
    int tier = -1, pending = 0;
    ~State() {
        avcodec_free_context(&encoder); avcodec_free_context(&decoder);
        av_frame_free(&input); av_frame_free(&output); av_packet_free(&packet);
        sws_freeContext(scaleIn); sws_freeContext(scaleOut);
    }
};
VideoCodec::VideoCodec() : state_(std::make_unique<State>()) {
    require((avcodec_version() >> 16) == LIBAVCODEC_VERSION_MAJOR, QT_TRANSLATE_NOOP("VideoCodec", "Video codec ABI does not match its headers."));
    require(state_->input && state_->output && state_->packet, QT_TRANSLATE_NOOP("VideoCodec", "Video buffers could not be allocated."));
}
VideoCodec::~VideoCodec() = default;
int VideoCodec::framePeriod(int tier) {
    require(tier >= 0 && tier < 4, QT_TRANSLATE_NOOP("VideoCodec", "Invalid video quality tier."));
    return (1000 + rates[size_t(tier)] - 1) / rates[size_t(tier)];
}
QPair<QJsonObject, QByteArray> VideoCodec::encode(const QVideoFrame& frame, int tier, bool keyFrame) {
    const auto surface = frame.surfaceFormat();
    const auto format = QVideoFrameFormat::imageFormatFromPixelFormat(frame.pixelFormat());
    auto mapped = frame;
    if (frame.handleType() == QVideoFrame::NoHandle && format != QImage::Format_Invalid
        && surface.rotation() == QtVideo::Rotation::None && !surface.isMirrored()
        && surface.scanLineDirection() == QVideoFrameFormat::TopToBottom
        && surface.colorSpace() == QVideoFrameFormat::ColorSpace_Undefined
        && surface.colorTransfer() == QVideoFrameFormat::ColorTransfer_Unknown
        && surface.colorRange() == QVideoFrameFormat::ColorRange_Unknown
        && surface.viewport() == QRect(QPoint{}, frame.size()) && mapped.map(QVideoFrame::ReadOnly)) {
        const auto unmap = qScopeGuard([&] { mapped.unmap(); });
        const auto* pixels = mapped.bits(0);
        // Keep the frame mapped until swscale has copied its pixels. Avoid
        // Qt's per-thread GPU conversion context for already usable CPU data.
        return encode(QImage(pixels, mapped.width(), mapped.height(), mapped.bytesPerLine(0), format), tier, keyFrame);
    }
    return encode(frame.toImage(), tier, keyFrame);
}
QPair<QJsonObject, QByteArray> VideoCodec::encode(const QImage& image, int tier, bool keyFrame) {
    framePeriod(tier);
    require(!image.isNull() && image.width() >= 2 && image.height() >= 2
        && image.width() <= 16384 && image.height() <= 16384 && qint64(image.width()) * image.height() <= 67108864,
        QT_TRANSLATE_NOOP("VideoCodec", "The screen source exceeds the supported capture size."));
    auto& s = *state_;
    if (s.pending >= 6) {
        avcodec_free_context(&s.encoder); s.pending = 0;
        return {{{"overloaded", true}}, {}};
    }
    const QSize target = image.size().scaled(QSize(heights[size_t(tier)] * 16 / 9, heights[size_t(tier)]), Qt::KeepAspectRatio);
    const int width = std::min(image.width(), target.width()) & ~1;
    const int height = std::min(image.height(), target.height()) & ~1;
    require(width >= 2 && height >= 2, QT_TRANSLATE_NOOP("VideoCodec", "The screen source is too narrow."));
    if (!s.encoder || s.encoder->width != width || s.encoder->height != height || s.tier != tier) {
        avcodec_free_context(&s.encoder); av_frame_unref(s.input); s.sequence = 0; s.pending = 0;
        // Prefer native H.264. MPEG-4 is the existing FFmpeg software fallback,
        // so a missing hardware encoder does not require a second codec runtime.
        const AVCodec* native = nullptr;
#if defined(Q_OS_MACOS)
        native = avcodec_find_encoder_by_name("h264_videotoolbox");
#elif defined(Q_OS_WIN)
        APTTYPE apartment;
        APTTYPEQUALIFIER qualifier;
        const auto com = CoGetApartmentType(&apartment, &qualifier);
        // FFmpeg 7.1's failed MF initialization can uninitialize a caller's STA.
        // The retained capture worker is MTA-compatible; GUI callers use the
        // existing software encoder without entering that broken error path.
        if (com == CO_E_NOTINITIALIZED || (SUCCEEDED(com) && apartment == APTTYPE_MTA))
            native = avcodec_find_encoder_by_name("h264_mf");
#endif
        const std::array<const AVCodec*, 2> candidates{native, avcodec_find_encoder(AV_CODEC_ID_MPEG4)};
        for (const auto* codec : candidates) {
            if (!codec) continue;
            s.encoder = avcodec_alloc_context3(codec);
            require(s.encoder, QT_TRANSLATE_NOOP("VideoCodec", "The video encoder could not be allocated."));
            auto& e = *s.encoder;
            e.width = width; e.height = height; e.time_base = {1, rates[size_t(tier)]}; e.framerate = {rates[size_t(tier)], 1};
            e.pix_fmt = codec == native ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P; e.bit_rate = bitrates[size_t(tier)];
            e.gop_size = rates[size_t(tier)]; e.max_b_frames = 0; e.thread_count = 2;
            e.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (codec == native) e.flags |= AV_CODEC_FLAG_LOW_DELAY;
            e.color_range = AVCOL_RANGE_MPEG; e.colorspace = AVCOL_SPC_BT709;
            e.color_primaries = AVCOL_PRI_BT709; e.color_trc = AVCOL_TRC_BT709;
            if (codec == native) av_opt_set(e.priv_data, "realtime", "1", 0);
            if (avcodec_open2(s.encoder, codec, nullptr) >= 0) break;
            avcodec_free_context(&s.encoder);
        }
        require(s.encoder, QT_TRANSLATE_NOOP("VideoCodec", "No supported video encoder is available."));
        require(s.encoder->extradata_size <= 12288, QT_TRANSLATE_NOOP("VideoCodec", "Video encoder configuration is too large."));
        s.encodedFormat = {{"codec", s.encoder->codec_id == AV_CODEC_ID_H264 ? "h264" : "mpeg4"},
            {"width", width}, {"height", height}, {"extra", QString::fromLatin1(QByteArray(reinterpret_cast<char*>(s.encoder->extradata), s.encoder->extradata_size).toBase64())}};
        s.input->width = width; s.input->height = height; s.input->format = s.encoder->pix_fmt;
        s.input->color_range = AVCOL_RANGE_MPEG; s.input->colorspace = AVCOL_SPC_BT709;
        check(av_frame_get_buffer(s.input, 32)); s.tier = tier;
        keyFrame = true;
    }
    check(av_frame_make_writable(s.input));
    // Qt's native RGB32 layout matches FFmpeg's endian-aware RGB32 format.
    // Keep premultiplied and other layouts on the existing conversion path.
    const bool nativeRgb = image.format() == QImage::Format_ARGB32 || image.format() == QImage::Format_RGB32;
    const auto source = nativeRgb ? image : image.convertToFormat(QImage::Format_RGBA8888);
    s.scaleIn = sws_getCachedContext(s.scaleIn, source.width(), source.height(), nativeRgb ? AV_PIX_FMT_RGB32 : AV_PIX_FMT_RGBA,
        width, height, s.encoder->pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    require(s.scaleIn, QT_TRANSLATE_NOOP("VideoCodec", "Video scaling could not be initialized."));
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    check(sws_setColorspaceDetails(s.scaleIn, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16));
    const uint8_t* planes[]{source.constBits(), nullptr, nullptr, nullptr};
    const int strides[]{int(source.bytesPerLine()), 0, 0, 0};
    check(sws_scale(s.scaleIn, planes, strides, 0, source.height(), s.input->data, s.input->linesize));
    s.input->pts = s.sequence++; s.input->pict_type = keyFrame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    check(avcodec_send_frame(s.encoder, s.input)); ++s.pending;
    av_packet_unref(s.packet);
    const int status = avcodec_receive_packet(s.encoder, s.packet);
    if (status == AVERROR(EAGAIN)) return {};
    check(status);
    --s.pending;
    if (s.packet->size > maximumPacket) {
        av_packet_unref(s.packet); avcodec_free_context(&s.encoder); s.pending = 0;
        return {{{"overloaded", true}}, {}};
    }
    require(s.packet->size > 0, QT_TRANSLATE_NOOP("VideoCodec", "The encoder returned an empty video packet."));
    auto format = s.encodedFormat;
    format.insert("key", bool(s.packet->flags & AV_PKT_FLAG_KEY));
    return {format, QByteArray(reinterpret_cast<char*>(s.packet->data), s.packet->size)};
}
QImage VideoCodec::decode(const QJsonObject& format, const QByteArray& packet) {
    const auto codec = format.value("codec").toString();
    const auto width = format.value("width").toInt(), height = format.value("height").toInt();
    require((codec == "h264" || codec == "mpeg4") && width >= 2 && width <= 3840 && height >= 2 && height <= 2160
        && format.value("width").toDouble() == width && format.value("height").toDouble() == height
        && width % 2 == 0 && height % 2 == 0 && !packet.isEmpty() && packet.size() <= maximumPacket
        && format.value("extra").isString() && format.value("extra").toString().size() <= 16384, QT_TRANSLATE_NOOP("VideoCodec", "Invalid video frame format."));
    auto& s = *state_;
    const QJsonObject stableFormat{{"codec", codec}, {"width", width}, {"height", height},
        {"extra", format.value("extra")}};
    if (!s.decoder || s.decodedFormat != stableFormat) {
        const auto extra = QByteArray::fromBase64Encoding(format.value("extra").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        require(bool(extra), QT_TRANSLATE_NOOP("VideoCodec", "Invalid video decoder configuration."));
        avcodec_free_context(&s.decoder);
        s.decodedFormat = {};
        const auto* implementation = avcodec_find_decoder(codec == "h264" ? AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG4);
        require(implementation, QT_TRANSLATE_NOOP("VideoCodec", "This video codec cannot be decoded."));
        s.decoder = avcodec_alloc_context3(implementation);
        require(s.decoder, QT_TRANSLATE_NOOP("VideoCodec", "Video decoder could not be allocated."));
        s.decoder->max_pixels = 3840LL * 2160; s.decoder->thread_count = 2;
        s.decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
        s.decoder->thread_type = FF_THREAD_SLICE; s.decoder->err_recognition = AV_EF_EXPLODE;
        s.decoder->extradata_size = int(extra.decoded.size());
        s.decoder->extradata = static_cast<uint8_t*>(av_mallocz(extra.decoded.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        require(s.decoder->extradata, QT_TRANSLATE_NOOP("VideoCodec", "Video decoder configuration could not be allocated."));
        std::copy(extra.decoded.begin(), extra.decoded.end(), s.decoder->extradata);
        check(avcodec_open2(s.decoder, implementation, nullptr)); s.decodedFormat = stableFormat;
    }
    av_packet_unref(s.packet); check(av_new_packet(s.packet, int(packet.size())));
    std::copy(packet.begin(), packet.end(), s.packet->data);
    check(avcodec_send_packet(s.decoder, s.packet));
    av_frame_unref(s.output);
    const int status = avcodec_receive_frame(s.decoder, s.output);
    if (status == AVERROR(EAGAIN)) return {};
    check(status);
    require(s.output->width == width && s.output->height == height, QT_TRANSLATE_NOOP("VideoCodec", "Decoded dimensions do not match the screen stream."));
    // swscale's SIMD output needs aligned scanlines and tail padding. Let
    // FFmpeg allocate them; the returned image owns that allocation without
    // copying pixels or depending on the decoder's lifetime.
    uint8_t* planes[4]{};
    int strides[4]{};
    check(av_image_alloc(planes, strides, width, height, AV_PIX_FMT_RGBA, 64));
    auto release = qScopeGuard([&] { av_free(planes[0]); });
    QImage image(planes[0], width, height, strides[0], QImage::Format_RGBA8888, av_free, planes[0]);
    require(!image.isNull(), QT_TRANSLATE_NOOP("VideoCodec", "Decoded image could not be allocated."));
    release.dismiss();
    s.scaleOut = sws_getCachedContext(s.scaleOut, width, height, AVPixelFormat(s.output->format),
        width, height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    require(s.scaleOut, QT_TRANSLATE_NOOP("VideoCodec", "Video color conversion could not be initialized."));
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
    check(sws_setColorspaceDetails(s.scaleOut, coefficients, 0, coefficients, 1, 0, 1 << 16, 1 << 16));
    check(sws_scale(s.scaleOut, s.output->data, s.output->linesize, 0, height, planes, strides));
    return image;
}
