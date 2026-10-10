#include "video_codec.hpp"
#include "screen_share.hpp"
#include <QTest>
#include <QPainter>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QVideoFrame>
#include <QTranslator>
#ifdef Q_OS_WIN
#include <objbase.h>
#endif

#ifdef SQUADSPEAK_DECODE_PROBE
#include <QSemaphore>
#include <atomic>

namespace {
std::atomic<bool> holdDecode{false};
std::atomic<int> decodeCalls{0};
QSemaphore decodePermits;
}
struct AVCodecContext;
struct AVFrame;
extern "C" int __real_avcodec_receive_frame(AVCodecContext*, AVFrame*);
extern "C" int __wrap_avcodec_receive_frame(AVCodecContext* context, AVFrame* frame) {
    if (holdDecode.load()) {
        ++decodeCalls;
        decodePermits.acquire();
    }
    return __real_avcodec_receive_frame(context, frame);
}
#endif

class VideoTests final : public QObject {
    Q_OBJECT
private slots:
#ifdef Q_OS_WIN
    void encodingPreservesWindowsApartment() {
        QVERIFY(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
        const auto apartment = qScopeGuard([] { CoUninitialize(); });
        APTTYPE before, after;
        APTTYPEQUALIFIER beforeQualifier, afterQualifier;
        QCOMPARE(CoGetApartmentType(&before, &beforeQualifier), S_OK);
        {
            VideoCodec encoder;
            QImage source(640, 360, QImage::Format_RGB32); source.fill(Qt::darkGreen);
            const auto packet = encoder.encode(source, 1, true);
            QVERIFY(!packet.second.isEmpty());
            VideoCodec decoder; QVERIFY(!decoder.decode(packet.first, packet.second).isNull());
        }
        QCOMPARE(CoGetApartmentType(&after, &afterQualifier), S_OK);
        QCOMPARE(after, before); QCOMPARE(afterQualifier, beforeQualifier);
        QWindowCapture capture;
        QVERIFY(!capture.isActive());
    }
#endif
    void codecFailuresUseSelectedLanguage_data() {
        QTest::addColumn<QString>("language");
        for (const auto* language : {"en", "de", "ar"}) QTest::newRow(language) << QString(language);
    }
    void codecFailuresUseSelectedLanguage() {
        QFETCH(QString, language);
        QTemporaryDir directory;
        VoiceSession profile(directory.filePath("profile"));
        QTranslator catalog;
        if (language != "en") {
            QVERIFY(catalog.load(":/i18n/squadspeak_" + language + ".qm"));
            QVERIFY(QCoreApplication::installTranslator(&catalog));
        }
        const auto remove = qScopeGuard([&] { QCoreApplication::removeTranslator(&catalog); });
        const auto failure = [](auto operation) {
            try { operation(); }
            catch (const std::runtime_error& error) { return QString::fromUtf8(error.what()); }
            return QString{};
        };
        const auto translated = [](const char* source) { return QCoreApplication::translate("VideoCodec", source); };
        const auto qualityError = translated("Invalid video quality tier.");
        if (language != "en") QVERIFY(qualityError != "Invalid video quality tier.");
        QCOMPARE(failure([] { VideoCodec::framePeriod(4); }), qualityError);
        VideoCodec codec;
        QCOMPARE(failure([&] { codec.encode(QImage{}, 0, false); }), translated("The screen source exceeds the supported capture size."));
        QCOMPARE(failure([&] { codec.decode({}, "junk"); }), translated("Invalid video frame format."));
        const auto detail = failure([&] {
            codec.decode({{"codec", "mpeg4"}, {"width", 640}, {"height", 360}, {"extra", ""}}, "junk");
        });
        const auto prefix = translated("Video codec: %1").arg("");
        QVERIFY2(detail.startsWith(prefix), qPrintable(detail));
        QVERIFY(detail.size() > prefix.size());
    }
    void roundTripAndQualityChanges_data() {
        QTest::addColumn<int>("format");
        QTest::newRow("rgba") << int(QImage::Format_RGBA8888);
        QTest::newRow("native-rgb") << int(QImage::Format_RGB32);
    }
    void roundTripAndQualityChanges() {
        QFETCH(int, format);
        bool validDuration = true;
        const int soakSeconds = qEnvironmentVariableIsSet("SQUAD_VIDEO_SOAK_SECONDS")
            ? qEnvironmentVariableIntValue("SQUAD_VIDEO_SOAK_SECONDS", &validDuration) : 0;
        QVERIFY2(validDuration && soakSeconds >= 0 && soakSeconds <= 1800,
            "SQUAD_VIDEO_SOAK_SECONDS must be 0..1800");
        VideoCodec encoder, decoder;
        QImage source(3840, 2160, QImage::Format(format));
        source.fill(QColor(24, 100, 180));
        const std::array<QColor, 8> colors{Qt::black, Qt::white, Qt::red, Qt::green, Qt::blue, Qt::cyan, Qt::magenta, Qt::yellow};
        QPainter paint(&source);
        for (int n = 0; n < int(colors.size()); ++n) paint.fillRect(QRect(240 * n, 400, 240, 480), colors[size_t(n)]);
        paint.end();
        bool soaked = false;
        for (int tier : {0, 1, 2, 3, 2, 0}) {
            const int height = std::array{2160, 1080, 720, 360}[size_t(tier)];
            const int frames = tier == 0 && !soaked && soakSeconds ? soakSeconds * 30 : 30;
            if (tier == 0) soaked = true;
            int decoded = 0;
            qint64 bytes = 0, encodingNs = 0, decodingNs = 0;
            QElapsedTimer elapsed; elapsed.start();
            for (int frame = 0; frame < frames; ++frame) {
                QPainter motion(&source);
                motion.fillRect(QRect(0, 40, source.width(), 240), QColor(24, 100, 180));
                motion.fillRect(QRect((frame * 37) % (source.width() - 240), 40, 240, 240), Qt::white);
                motion.end();
                QElapsedTimer processing; processing.start();
                const auto encoded = encoder.encode(source, tier, frame == 0);
                encodingNs += processing.nsecsElapsed();
                const auto remaining = qint64(frame + 1) * VideoCodec::framePeriod(tier) - elapsed.elapsed();
                if (remaining > 0) QTest::qWait(int(remaining));
                if (encoded.second.isEmpty()) continue;
                QVERIFY(encoded.second.size() <= 2 * 1024 * 1024);
                bytes += encoded.second.size();
                QCOMPARE(encoded.first.value("height").toInt(), height);
                processing.restart();
                const auto image = decoder.decode(encoded.first, encoded.second);
                decodingNs += processing.nsecsElapsed();
                if (image.isNull()) continue;
                ++decoded;
                if (decoded == 1) qInfo() << "Video codec" << encoded.first.value("codec").toString();
                QCOMPARE(image.size(), QSize(height * 16 / 9, height));
                const auto color = image.pixelColor(image.width() - 30, image.height() - 30);
                QVERIFY(std::abs(color.red() - 24) < 12);
                QVERIFY(std::abs(color.green() - 100) < 12);
                QVERIFY(std::abs(color.blue() - 180) < 12);
                for (int n = 0; n < int(colors.size()); ++n) {
                    const auto pixel = image.pixelColor((240 * n + 120) * image.width() / source.width(), 640 * image.height() / source.height());
                    const auto expected = colors[size_t(n)];
                    QVERIFY(std::abs(pixel.red() - expected.red()) < 12);
                    QVERIFY(std::abs(pixel.green() - expected.green()) < 12);
                    QVERIFY(std::abs(pixel.blue() - expected.blue()) < 12);
                    QCOMPARE(pixel.alpha(), 255);
                }
            }
            qInfo() << "Tier" << tier << "decoded frames" << decoded << "of" << frames
                << "elapsed ms" << elapsed.elapsed() << "encoded bytes" << bytes
                << "encode ms/frame" << encodingNs / (1000000.0 * frames)
                << "decode ms/frame" << decodingNs / (1000000.0 * std::max(decoded, 1));
            QVERIFY(decoded >= frames * 4 / 5);
        }
    }
    void nativePixelFormatsKeepColorsAcrossChanges_data() {
        QTest::addColumn<bool>("captured");
        QTest::newRow("image") << false;
        QTest::newRow("captured-frame") << true;
    }
    void nativePixelFormatsKeepColorsAcrossChanges() {
        QFETCH(bool, captured);
        VideoCodec encoder, decoder;
        const std::array<QColor, 4> colors{QColor(220, 40, 30), QColor(30, 180, 70),
            QColor(30, 60, 210), QColor(130, 90, 40)};
        for (const auto format : {QImage::Format_RGBA8888, QImage::Format_ARGB32,
            QImage::Format_RGB32, QImage::Format_ARGB32_Premultiplied,
            QImage::Format_RGB888, QImage::Format_ARGB32, QImage::Format_RGBA8888}) {
            QImage source(802, 602, format);
            QPainter paint(&source);
            paint.setCompositionMode(QPainter::CompositionMode_Source);
            for (int i = 0; i < 4; ++i) {
                auto color = colors[size_t(i)]; if (source.hasAlphaChannel()) color.setAlpha(128);
                paint.fillRect(QRect(i % 2 * 401, i / 2 * 301, 401, 301), color);
            }
            paint.end();
            int decoded = 0;
            for (int frame = 0; frame < 12; ++frame) {
                const auto encoded = captured ? encoder.encode(QVideoFrame(source), 0, frame == 0)
                                              : encoder.encode(source, 0, frame == 0);
                QTest::qWait(VideoCodec::framePeriod(0));
                if (encoded.second.isEmpty()) continue;
                const auto image = decoder.decode(encoded.first, encoded.second);
                if (image.isNull()) continue;
                ++decoded;
                QCOMPARE(image.size(), source.size());
                for (int i = 0; i < 4; ++i) {
                    const auto actual = image.pixelColor(i % 2 * 401 + 200, i / 2 * 301 + 150);
                    const auto expected = colors[size_t(i)];
                    QVERIFY2(std::abs(actual.red() - expected.red()) < 12
                        && std::abs(actual.green() - expected.green()) < 12
                        && std::abs(actual.blue() - expected.blue()) < 12,
                        qPrintable(QString("format=%1 frame=%2 color=%3 actual=%4 expected=%5 source=%6")
                            .arg(int(format)).arg(frame).arg(i).arg(actual.name()).arg(expected.name())
                            .arg(source.pixelColor(i % 2 * 401 + 200, i / 2 * 301 + 150).name())));
                }
            }
            QVERIFY(decoded >= 6);
        }
    }
    void capturedFrameKeepsQtSurfaceConversion_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto* scenario : {"rotated", "mirrored", "bottom-up", "yuv"})
            QTest::newRow(scenario) << QString(scenario);
    }
    void capturedFrameKeepsQtSurfaceConversion() {
        QFETCH(QString, scenario);
        QImage source(640, 360, QImage::Format_RGBA8888);
        QPainter paint(&source);
        const std::array<QColor, 4> colors{QColor(220, 40, 30), QColor(30, 180, 70),
            QColor(30, 60, 210), QColor(130, 90, 40)};
        for (int i = 0; i < 4; ++i) paint.fillRect(QRect(i % 2 * 320, i / 2 * 180, 320, 180), colors[size_t(i)]);
        paint.end();
        const bool yuv = scenario == "yuv";
        QVideoFrameFormat surface(source.size(), yuv ? QVideoFrameFormat::Format_NV12 : QVideoFrameFormat::Format_RGBA8888);
        if (scenario == "rotated") surface.setRotation(QtVideo::Rotation::Clockwise90);
        if (scenario == "mirrored") surface.setMirrored(true);
        if (scenario == "bottom-up") surface.setScanLineDirection(QVideoFrameFormat::BottomToTop);
        QVideoFrame frame(surface);
        QVERIFY(frame.map(QVideoFrame::WriteOnly));
        if (yuv) {
            std::fill_n(frame.bits(0), frame.mappedBytes(0), uchar(160));
            std::fill_n(frame.bits(1), frame.mappedBytes(1), uchar(128));
        } else {
            for (int row = 0; row < source.height(); ++row)
                std::copy_n(source.constScanLine(row), source.width() * 4, frame.bits(0) + row * frame.bytesPerLine(0));
        }
        frame.unmap();
        // Qt's public conversion defines surface transforms; presentation-only
        // frame rotation/mirroring deliberately does not change encoded pixels.
        const auto expected = frame.toImage();
        QVERIFY(!expected.isNull());
        QCOMPARE(expected.size(), scenario == "rotated" ? QSize(360, 640) : source.size());
        VideoCodec encoder, decoder;
        int decoded = 0;
        for (int index = 0; index < 12; ++index) {
            const auto packet = encoder.encode(frame, 1, index == 0);
            QTest::qWait(VideoCodec::framePeriod(1));
            if (packet.second.isEmpty()) continue;
            const auto image = decoder.decode(packet.first, packet.second);
            if (image.isNull()) continue;
            ++decoded; QCOMPARE(image.size(), expected.size());
            for (int i = 0; i < 4; ++i) {
                const QPoint point((i % 2 * 2 + 1) * image.width() / 4, (i / 2 * 2 + 1) * image.height() / 4);
                const auto actual = image.pixelColor(point), color = expected.pixelColor(point);
                QVERIFY(std::abs(actual.red() - color.red()) < 14);
                QVERIFY(std::abs(actual.green() - color.green()) < 14);
                QVERIFY(std::abs(actual.blue() - color.blue()) < 14);
                QCOMPARE(actual.alpha(), 255);
            }
        }
        QVERIFY(decoded >= 6);
    }
    void decodedImagesKeepIndependentStorage() {
        QList<QImage> images;
        const QList<QSize> sizes{{18, 66}, {360, 640}, {642, 362}, {638, 358}};
        const auto color = QColor(30, 100, 180);
        {
            VideoCodec encoder, decoder;
            for (const auto size : sizes) {
                QImage source(size, QImage::Format_RGBA8888); source.fill(color);
                QImage decoded;
                for (int frame = 0; frame < 12 && decoded.isNull(); ++frame) {
                    const auto packet = encoder.encode(source, 1, true);
                    QTest::qWait(VideoCodec::framePeriod(1));
                    if (!packet.second.isEmpty()) decoded = decoder.decode(packet.first, packet.second);
                }
                QCOMPARE(decoded.size(), size);
                images.append(decoded);
            }
        }
        // Decoding another size and destroying the codec cannot invalidate a
        // displayed frame. Qt copies must also retain normal detach semantics.
        for (qsizetype index = 0; index < images.size(); ++index) {
            const auto& image = images[index];
            QCOMPARE(image.size(), sizes[index]);
            for (const auto point : {QPoint{}, QPoint(image.width() - 1, image.height() - 1)}) {
                const auto actual = image.pixelColor(point);
                QVERIFY(std::abs(actual.red() - color.red()) < 14);
                QVERIFY(std::abs(actual.green() - color.green()) < 14);
                QVERIFY(std::abs(actual.blue() - color.blue()) < 14);
                QCOMPARE(actual.alpha(), 255);
            }
            auto copy = image;
            copy.setPixelColor(0, 0, Qt::red);
            QVERIFY(copy.pixelColor(0, 0) != image.pixelColor(0, 0));
        }
    }
    void smallerSourcesAndLateViewers() {
        VideoCodec encoder;
        QImage source(802, 602, QImage::Format_RGBA8888); source.fill(Qt::darkGreen);
        QPair<QJsonObject, QByteArray> encoded;
        for (int frame = 0; frame < 4; ++frame) encoded = encoder.encode(source, 0, false);
        VideoCodec newcomer;
        encoded = encoder.encode(source, 0, true);
        QVERIFY(!encoded.second.isEmpty());
        QCOMPARE(encoded.first.value("key").toBool(), true);
        const auto decoded = newcomer.decode(encoded.first, encoded.second);
        QCOMPARE(decoded.size(), source.size());
    }
    void unknownFrameMetadataDoesNotResetDecoderState() {
        VideoCodec encoder, decoder;
        QImage source(640, 360, QImage::Format_RGBA8888); source.fill(QColor(30, 100, 180));
        int decoded = 0;
        for (int frame = 0; frame < 12; ++frame) {
            auto packet = encoder.encode(source, 1, frame == 0);
            QTest::qWait(VideoCodec::framePeriod(1));
            if (packet.second.isEmpty()) continue;
            packet.first.insert("futureTimestamp", frame * 34);
            try {
                const auto image = decoder.decode(packet.first, packet.second);
                if (image.isNull()) continue;
                ++decoded;
                QCOMPARE(image.size(), source.size());
                const auto color = image.pixelColor(100, 100);
                QVERIFY(std::abs(color.blue() - 180) < 12);
            } catch (const std::exception& error) { QFAIL(error.what()); }
        }
        QVERIFY(decoded >= 8);
    }
    void invalidFramesAndFormatsFailBoundedly() {
        VideoCodec codec;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codec.encode(QImage{}, 0, false));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codec.encode(QVideoFrame{}, 0, false));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codec.encode(QVideoFrame{}, 4, false));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, VideoCodec::framePeriod(4));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codec.decode({}, "junk"));
        QJsonObject format{{"codec", "h264"}, {"width", 3842}, {"height", 2160}, {"extra", ""}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codec.decode(format, "junk"));
        format.insert("width", 1920);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codec.decode(format, QByteArray(2 * 1024 * 1024 + 1, 'x')));
        format.insert("extra", "not base64!");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codec.decode(format, "junk"));
        format.insert("extra", "");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, codec.decode(format, "junk"));
    }
    void rejectedConfigurationDoesNotPoisonPreviousStream() {
        VideoCodec encoder, decoder;
        QImage source(640, 360, QImage::Format_RGBA8888); source.fill(Qt::darkGreen);
        QPair<QJsonObject, QByteArray> frame;
        for (int attempt = 0; attempt < 5 && frame.second.isEmpty(); ++attempt)
            frame = encoder.encode(source, 1, true);
        QVERIFY(!frame.second.isEmpty());
        QCOMPARE(decoder.decode(frame.first, frame.second).size(), source.size());
        auto invalid = frame.first;
        invalid.insert("codec", "h264");
        invalid.insert("extra", QString::fromLatin1(QByteArray::fromHex("0100000000").toBase64()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decoder.decode(invalid, "junk"));
        try { QCOMPARE(decoder.decode(frame.first, frame.second).size(), source.size()); }
        catch (const std::exception& error) { QFAIL(error.what()); }
    }
    void encryptedStreamReachesTheViewerWithoutCapturePermissions() {
        QTemporaryDir dir;
        VoiceSession hostProfile(dir.filePath("owner")), viewerProfile(dir.filePath("viewer"));
        LocalChannel host(hostProfile, dir.filePath("host"), TlsIdentity::create());
        LocalChannel viewer(viewerProfile, dir.filePath("client"), TlsIdentity::create());
        ScreenShare display(viewer);
        QVideoSink sink;
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.setScreenSharing(true));
        QVERIFY(host.decide(viewer.ownId(), true));
        QVERIFY(viewer.openChat(host.ownId(), "127.0.0.1", host.port()));
        QTRY_VERIFY(viewer.screenInfo(host.ownId()).value("available").toBool());
        QVERIFY(display.attach(host.ownId(), &sink));
        QTRY_COMPARE(viewer.screenInfo(host.ownId()).value("tier").toInt(), 1);
        VideoCodec encoder;
        QImage source(1920, 1080, QImage::Format_RGBA8888); source.fill(QColor(30, 100, 180));
        // Native encoders may buffer initial frames; keep producing a real stream.
        for (int n = 0; n < 30 && !sink.videoFrame().isValid(); ++n) {
            const auto frame = encoder.encode(source, 1, true);
            if (!frame.second.isEmpty()) QVERIFY(host.sendScreenFrame(1, frame.first, frame.second, true));
            QTest::qWait(VideoCodec::framePeriod(1));
        }
        QTRY_VERIFY_WITH_TIMEOUT(sink.videoFrame().isValid(), 5000);
        QCOMPARE(sink.videoSize(), source.size());
        const auto pixel = sink.videoFrame().toImage().pixelColor(100, 100);
        QVERIFY(std::abs(pixel.red() - 30) < 12);
        QVERIFY(std::abs(pixel.green() - 100) < 12);
        QVERIFY(std::abs(pixel.blue() - 180) < 12);
        QCOMPARE(pixel.alpha(), 255);
        QVideoSink preview;
        QVERIFY(display.attach(host.ownId(), &preview, true));
        QVERIFY(preview.videoFrame().isValid());
        QVERIFY(display.detach(&preview));
        QVERIFY(!preview.videoFrame().isValid());
        QVERIFY(sink.videoFrame().isValid());
        QVERIFY(display.watching());
        QVERIFY(!display.detach(&preview));
        QVERIFY(display.attach(host.ownId(), &preview, true));
        QVERIFY(preview.videoFrame().isValid());
        QVERIFY(display.detach(&preview));
        {
            QVideoSink temporary;
            QVERIFY(display.attach(host.ownId(), &temporary, true));
            QVERIFY(temporary.videoFrame().isValid());
        }
        QVERIFY(display.watching());
        QVERIFY(sink.videoFrame().isValid());
        QVERIFY(host.setBlocked(viewer.ownId(), true));
        QTRY_VERIFY(!sink.videoFrame().isValid());
        QVERIFY(host.screenSharing());
        QVERIFY(display.detach(&sink));
    }
    void udpScreenRoundTripDecodesRealFrames() {
        QTemporaryDir dir;
        VoiceSession hostProfile(dir.filePath("owner")), viewerProfile(dir.filePath("viewer"));
        LocalChannel host(hostProfile, dir.filePath("host"), TlsIdentity::create());
        LocalChannel viewer(viewerProfile, dir.filePath("client"), TlsIdentity::create());
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.setScreenSharing(true));
        QVERIFY(host.decide(viewer.ownId(), true));
        QVERIFY(viewer.openChat(host.ownId(), "127.0.0.1", host.port()));
        QTRY_VERIFY_WITH_TIMEOUT(viewer.screenInfo(host.ownId()).value("available").toBool(), 5000);
        QVERIFY(viewer.watchScreen(host.ownId(), true, 1));
        QTRY_COMPARE_WITH_TIMEOUT(viewer.screenInfo(host.ownId()).value("tier").toInt(), 1, 5000);
        QTest::qWait(500);

        VideoCodec encoder, decoder;
        QImage source(1920, 1080, QImage::Format_RGBA8888);
        source.fill(QColor(24, 100, 180));
        QPainter paint(&source);
        paint.fillRect(QRect(120, 120, 480, 360), QColor(220, 40, 30));
        paint.fillRect(QRect(960, 540, 480, 360), QColor(30, 180, 70));
        paint.end();
        QSignalSpy received(&viewer, &LocalChannel::screenFrameReceived);
        QString callbackError;
        connect(&viewer, &LocalChannel::screenFrameReceived, &viewer,
            [&](const QString& id, qint64 serial, const QJsonObject& format, const QByteArray& packet) {
                if (id != host.ownId()) return;
                try {
                    QElapsedTimer decoding; decoding.start();
                    const auto image = decoder.decode(format, packet);
                    const auto expected = QSize(format.value("height").toInt() * 16 / 9, format.value("height").toInt());
                    if (image.size() != expected) callbackError = QString("decoded size %1x%2, expected %3x%4")
                        .arg(image.width()).arg(image.height()).arg(expected.width()).arg(expected.height());
                    const auto blue = image.pixelColor(image.width() - 20, image.height() - 20);
                    if (std::abs(blue.red() - 24) >= 15 || std::abs(blue.green() - 100) >= 15 || std::abs(blue.blue() - 180) >= 15)
                        callbackError = "background color mismatch";
                    const auto red = image.pixelColor(image.width() * 360 / 1920, image.height() * 300 / 1080);
                    const auto green = image.pixelColor(image.width() * 1200 / 1920, image.height() * 700 / 1080);
                    if (std::abs(red.red() - 220) >= 20 || std::abs(red.green() - 40) >= 20 || std::abs(red.blue() - 30) >= 20
                        || std::abs(green.red() - 30) >= 20 || std::abs(green.green() - 180) >= 20 || std::abs(green.blue() - 70) >= 20)
                        callbackError = "marker color mismatch";
                    if (!viewer.acknowledgeScreen(id, serial, int(decoding.elapsed()))) callbackError = "screen acknowledgement failed";
                } catch (const std::exception& error) { callbackError = QString::fromUtf8(error.what()); }
            });

        for (const int tier : {1, 2, 3}) {
            QVERIFY(viewer.watchScreen(host.ownId(), true, tier));
            QTRY_COMPARE_WITH_TIMEOUT(viewer.screenInfo(host.ownId()).value("tier").toInt(), tier, 5000);
            const int expectedHeight = std::array{2160, 1080, 720, 360}[size_t(tier)];
            const auto before = received.size();
            QPair<QJsonObject, QByteArray> encoded;
            for (int attempt = 0; attempt < 12 && encoded.second.isEmpty(); ++attempt) {
                encoded = encoder.encode(source, tier, attempt == 0);
                QTest::qWait(VideoCodec::framePeriod(tier));
            }
            QVERIFY(!encoded.second.isEmpty());
            QCOMPARE(encoded.first.value("height").toInt(), expectedHeight);
            QVERIFY(host.sendScreenFrame(tier, encoded.first, encoded.second, true));
            QTRY_VERIFY_WITH_TIMEOUT(received.size() > before, 5000);
            QVERIFY2(callbackError.isEmpty(), qPrintable(callbackError));
        }
        QCOMPARE(received.size(), 3);
        QVERIFY(viewer.chatReady());
    }
    void repeatedViewerDetachDuringDecodeIsSafe() {
        QTemporaryDir dir;
        VoiceSession hostProfile(dir.filePath("owner")), viewerProfile(dir.filePath("viewer"));
        LocalChannel host(hostProfile, dir.filePath("host"), TlsIdentity::create());
        LocalChannel viewer(viewerProfile, dir.filePath("client"), TlsIdentity::create());
        ScreenShare display(viewer);
        QVideoSink sink;
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.setScreenSharing(true));
        QVERIFY(host.decide(viewer.ownId(), true));
        QVERIFY(viewer.openChat(host.ownId(), "127.0.0.1", host.port()));
        QTRY_VERIFY_WITH_TIMEOUT(viewer.screenInfo(host.ownId()).value("available").toBool(), 5000);
        VideoCodec encoder;
        QImage source(1920, 1080, QImage::Format_RGBA8888); source.fill(QColor(30, 100, 180));
        for (int attempt = 0; attempt < 5; ++attempt) {
            QVERIFY(display.attach(host.ownId(), &sink));
            QTRY_COMPARE(viewer.screenView().value("tier").toInt(), 1);
            QPair<QJsonObject, QByteArray> frame;
            for (int attempt = 0; attempt < 5 && frame.second.isEmpty(); ++attempt)
                frame = encoder.encode(source, 1, true);
            QVERIFY(!frame.second.isEmpty());
            QVERIFY(host.sendScreenFrame(1, frame.first, frame.second, true));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QVERIFY(display.detach(&sink));
            QTest::qWait(20);
        }
        QVERIFY(display.attach(host.ownId(), &sink));
        QTRY_COMPARE(viewer.screenView().value("tier").toInt(), 1);
        QPair<QJsonObject, QByteArray> recovery;
        for (int attempt = 0; attempt < 5 && recovery.second.isEmpty(); ++attempt)
            recovery = encoder.encode(source, 1, true);
        QVERIFY(!recovery.second.isEmpty());
        QVERIFY(host.sendScreenFrame(1, recovery.first, recovery.second, true));
        QTRY_VERIFY_WITH_TIMEOUT(sink.videoFrame().isValid(), 5000);
        QCOMPARE(sink.videoFrame().width(), source.width());
        QVERIFY(display.detach(&sink));
        QVERIFY(viewer.chatReady());
        QVERIFY(host.setScreenSharing(false));
    }
#if defined(SQUADSPEAK_DECODE_PROBE)
    void sourceChangesKeepOnlyTheLatestPendingDecode_data() {
        QTest::addColumn<QString>("finish");
        for (const auto* finish : {"show", "detach", "invalid"}) QTest::newRow(finish) << QString(finish);
    }
    void sourceChangesKeepOnlyTheLatestPendingDecode() {
        QFETCH(QString, finish);
        QTemporaryDir dir;
        VoiceSession ownerProfile(dir.filePath("owner")), viewerProfile(dir.filePath("viewer"));
        LocalChannel host(ownerProfile, dir.filePath("host"), TlsIdentity::create());
        LocalChannel viewer(viewerProfile, dir.filePath("client"), TlsIdentity::create());
        ScreenShare display(viewer);
        QVideoSink sink;
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.setScreenSharing(true));
        QVERIFY(host.decide(viewer.ownId(), true));
        QVERIFY(viewer.openChat(host.ownId(), "127.0.0.1", host.port()));
        QTRY_VERIFY(viewer.screenInfo(host.ownId()).value("available").toBool());
        VideoCodec encoder;
        QImage source(640, 360, QImage::Format_RGBA8888); source.fill(QColor(30, 100, 180));
        const auto frame = encoder.encode(source, 1, true);
        source.fill(QColor(30, 180, 100));
        const auto latest = encoder.encode(source, 1, true);
        QVERIFY(!frame.second.isEmpty()); QVERIFY(!latest.second.isEmpty());
        QSignalSpy received(&viewer, &LocalChannel::screenFrameReceived);
        while (decodePermits.tryAcquire()) {}
        decodeCalls = 0; holdDecode = true;
        const auto release = qScopeGuard([] {
            holdDecode = false;
            decodePermits.release(64);
        });
        // Hold the real codec boundary, not the GUI or TLS event loop. Each new
        // subscription receives an actual keyframe while older decode work waits.
        for (int attempt = 0; attempt < 12; ++attempt) {
            QVERIFY(display.attach(host.ownId(), &sink));
            QTRY_COMPARE(viewer.screenInfo(host.ownId()).value("tier").toInt(), 1);
            const auto& selected = attempt == 11 ? latest : frame;
            const auto payload = attempt == 11 && finish == "invalid" ? QByteArray("junk") : selected.second;
            QVERIFY(host.sendScreenFrame(1, selected.first, payload, true));
            QTRY_COMPARE(received.size(), attempt + 1);
            if (attempt == 0) QTRY_COMPARE(decodeCalls.load(), 1);
            QVERIFY(!sink.videoFrame().isValid());
            if (attempt != 11) QVERIFY(display.detach(&sink));
        }
        if (finish == "detach") QVERIFY(display.detach(&sink));
        decodePermits.release(12);
        if (finish == "invalid") {
            QTRY_VERIFY(!display.error().isEmpty());
            QVERIFY(!sink.videoFrame().isValid());
            QVERIFY(display.detach(&sink));
        }
        if (finish != "show") {
            QVERIFY(display.attach(host.ownId(), &sink));
            QTRY_COMPARE(viewer.screenInfo(host.ownId()).value("tier").toInt(), 1);
            QVERIFY(host.sendScreenFrame(1, latest.first, latest.second, true));
        }
        QTRY_VERIFY(sink.videoFrame().isValid());
        QCOMPARE(sink.videoSize(), source.size());
        const auto color = sink.videoFrame().toImage().pixelColor(source.width() / 2, source.height() / 2);
        QVERIFY(color.green() > color.blue() + 50);
        QCOMPARE(decodeCalls.load(), 2);
        if (finish != "invalid") QVERIFY(display.error().isEmpty());
        QVERIFY(display.detach(&sink));
        QVERIFY(viewer.chatReady());
    }
#endif
    void publisherPreviewDoesNotSubscribeToItsOwnMedia() {
        QTemporaryDir dir;
        VoiceSession profile(dir.filePath("profile"));
        LocalChannel host(profile, dir.filePath("host"), TlsIdentity::create());
        ScreenShare share(host); QVideoSink thumbnail, viewer;
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.setScreenSharing(true));
        QVERIFY(share.attach(host.ownId(), &thumbnail, true));
        QVERIFY(share.attach(host.ownId(), &viewer));
        QVERIFY(!share.watching()); QVERIFY(host.screenTiers().isEmpty());
        QVERIFY(share.detach(&thumbnail)); QVERIFY(share.detach(&viewer));
        QVERIFY(share.stop()); QVERIFY(!thumbnail.videoFrame().isValid());
    }

    void screenPipelineStartsSilentAndNeedsExplicitSource() {
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("profile"));
        LocalChannel channel(session, dir.filePath("host"), TlsIdentity::create());
        ScreenShare share(channel);
        QVERIFY(!share.active());
        QVERIFY(!share.start(-1));
        QVERIFY(!share.start(0));
        QVERIFY(!share.attach("unknown", nullptr));
        QVERIFY(share.stop()); QVERIFY(share.stop());
        QVERIFY(share.sources().isEmpty());
    }
    void oneReceivedStreamExcludesPublishingAcrossOwnedChannels() {
#if SQUADSPEAK_STORE_BUILD
        return;
#else
        QTemporaryDir dir;
        VoiceSession profile(dir.filePath("viewer"));
        LocalChannel viewer(profile, dir.filePath("client"), TlsIdentity::create());
        ScreenShare display(viewer);
        QVideoSink preview, window, third;
        std::array<std::unique_ptr<VoiceSession>, 2> profiles;
        std::array<std::unique_ptr<LocalChannel>, 2> hosts;
        for (size_t i = 0; i < hosts.size(); ++i) {
            profiles[i] = std::make_unique<VoiceSession>(dir.filePath(QString("profile%1").arg(i)));
            hosts[i] = std::make_unique<LocalChannel>(*profiles[i], dir.filePath(QString("host%1").arg(i)), TlsIdentity::create());
            QVERIFY(hosts[i]->listen(QHostAddress::LocalHost));
            QVERIFY(profiles[i]->setSupporterEnabled(true)); QVERIFY(hosts[i]->setScreenSharing(true));
            QVERIFY(hosts[i]->decide(viewer.ownId(), true));
            QVERIFY(viewer.openChat(hosts[i]->ownId(), "127.0.0.1", hosts[i]->port()));
            QTRY_VERIFY(viewer.chatReady());
            QTRY_VERIFY(viewer.screenInfo(hosts[i]->ownId()).value("available").toBool());
        }
        QVERIFY(display.attach(hosts[0]->ownId(), &preview, true));
        QVERIFY(display.attach(hosts[0]->ownId(), &window));
        QVERIFY(!display.attach(hosts[1]->ownId(), &third));
        QVERIFY(!viewer.watchScreen(hosts[1]->ownId(), true));
        QVERIFY(!viewer.screenInfo(hosts[1]->ownId()).value("watching").toBool());
        QVERIFY(display.detach(&preview));
        QVERIFY(viewer.screenInfo(hosts[0]->ownId()).value("watching").toBool());
        QVERIFY(!display.attach(hosts[1]->ownId(), &third, true));
        QVERIFY(display.detach(&window));
        QVERIFY(!viewer.screenInfo(hosts[0]->ownId()).value("watching").toBool());
        QVERIFY(display.attach(hosts[1]->ownId(), &third, true));
        QVERIFY(viewer.listen(QHostAddress::LocalHost));
        QVERIFY(profile.setSupporterEnabled(true));
        const auto ownedId = viewer.addOwnedChannel("Another own channel");
        auto* owned = viewer.ownChannel(ownedId); QVERIFY(owned);
        QVERIFY(!viewer.setScreenSharing(true));
        QVERIFY(!owned->setScreenSharing(true));
        QVERIFY(viewer.screenInfo(hosts[1]->ownId()).value("watching").toBool());
        QVERIFY(display.detach(&third));
        QVERIFY(owned->setScreenSharing(true));
        QVERIFY(!display.attach(hosts[0]->ownId(), &preview, true));
        QVERIFY(!viewer.watchScreen(hosts[0]->ownId(), true));
        QVERIFY(owned->screenSharing());
        QVERIFY(owned->setScreenSharing(false));
        QVERIFY(viewer.setScreenSharing(true));
        QVERIFY(!viewer.watchScreen(hosts[0]->ownId(), true));
        QVERIFY(viewer.setScreenSharing(false));
        QVERIFY(display.attach(hosts[0]->ownId(), &window));
        QVERIFY(display.detach(&window));
#endif
    }
};
QTEST_MAIN(VideoTests)
#include "video_tests.moc"
