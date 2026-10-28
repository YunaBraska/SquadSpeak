#include <QOperatingSystemVersion>
#include "audio_processor.hpp"
#include "audio_model.hpp"
#include "voice_session.hpp"
#include "local_channel.hpp"
#include "chat_content.hpp"
#include "ptt_key.hpp"
#include "license.hpp"
#include "radio_player.hpp"
#include "screen_share.hpp"
#include "radio_station.hpp"
#include <QNetworkInterface>
#include <QSignalSpy>
#include <QThread>
#include <QtQuickTest/quicktest.h>
#include <QQmlEngine>
#include <QQmlContext>
#include <QDir>
#include <QQuickWindow>
#include <QQuickItem>
#include <QWheelEvent>
#include <QPointingDevice>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QUrl>
#include <QTranslator>
#include <QAccessible>
#include <QDesktopServices>
#include <QClipboard>
#include <QUdpSocket>
#include <QJsonDocument>
#include <QSslSocket>
#include <QtEndian>
#include <QTextLayout>
#include <QGlyphRun>
#include <memory>
#include <vector>
#include <cmath>
#include <numbers>
#include <cstdio>
#include <cstdlib>
#if defined(Q_OS_IOS)
#import <UIKit/UIKit.h>
#elif defined(Q_OS_ANDROID)
#include <QJniObject>
#include <QJniEnvironment>
#endif

namespace {
QtMessageHandler previousMessageHandler = nullptr;

void failOnQuickTestStartupWarning(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const bool exposureFailure = (type == QtWarningMsg || type == QtCriticalMsg)
        && message.contains(QStringLiteral("window was never exposed! If the test case was expecting windowShown, it will hang."));
    if (exposureFailure) {
        std::fprintf(stderr, "QtQuickTest startup failure: %s\n", qPrintable(message));
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
    if (previousMessageHandler) {
        previousMessageHandler(type, context, message);
    } else {
        std::fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, context, message)));
    }
}
}

class AudioFixtures final : public QObject {
    Q_OBJECT
    QTemporaryDir folder_;
    qint64 now_ = 1700000000000;
    std::unique_ptr<VoiceSession> session_;
    std::unique_ptr<License> license_;
    std::unique_ptr<LocalChannel> channel_;
    std::unique_ptr<LocalChannel> unreadyChannel_;
    std::unique_ptr<ScreenShare> screen_;
    std::unique_ptr<PushToTalkKey> pttKey_;
    std::unique_ptr<VoiceSession> remoteSession_;
    std::unique_ptr<LocalChannel> remoteChannel_;
    std::vector<std::unique_ptr<QSslSocket>> members_;
    ChatContent chatContent_;
    std::unique_ptr<AudioModel> audio_;
    std::unique_ptr<RadioPlayer> radio_;
    std::unique_ptr<RadioStation> station_;
    std::array<VideoCodec, 4> videoEncoders_;
    QTranslator translation_;
    QString appliedLanguage_ = "en";
public slots:
    QString imageDraft(const QString& markdown) const {
        return ChatContent::embedImage(markdown, chatContent_.preparedImage());
    }
    QString reloadedBotVoice() {
        AudioModel restored(folder_.filePath("audio.ini"));
        return restored.botVoiceId();
    }
    QVariantMap participantsWithoutOutput() {
        const auto path = folder_.filePath("disconnected-output.ini");
        squad::AudioProfiles(path).select(false, "disconnected-test-output");
        AudioModel model(path);
        const QVariantList members{QVariantMap{{"id", "peer"}, {"available", true}}};
        const auto accepted = model.setParticipants(members);
        const auto repeated = model.setParticipants(members);
        return {{"outputAvailable", model.outputAvailable()}, {"accepted", accepted},
            {"repeated", repeated}, {"automatic", model.participantAutomatic("peer")},
            {"editAccepted", model.setParticipantAutomatic("peer", false)},
            {"cleared", model.setParticipants({})}, {"status", model.outputStatus()}};
    }

    bool populateMembers(int count) {
        if (count < 0 || count > 63) return false;
        members_.clear();
        const auto frame = [](const QJsonObject& object) {
            const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
            QByteArray result(4, '\0');
            qToBigEndian<quint32>(quint32(data.size()), result.data());
            return result + data;
        };
        // This fixture exercises the real roster and UI, not media capacity.
        // Prepare identities before network timers and omit unused UDP media.
        for (int i = 0; i < count; ++i) {
            const auto identity = TlsIdentity::create();
            if (!channel_->decide(identity.id(), true)) return false;
            auto socket = std::make_unique<QSslSocket>();
            socket->setSslConfiguration(identity.configuration());
            socket->setPeerVerifyMode(QSslSocket::VerifyNone);
            connect(socket.get(), &QSslSocket::readyRead, socket.get(), [peer = socket.get()] { peer->readAll(); });
            members_.push_back(std::move(socket));
        }
        for (int i = 0; i < count; ++i) {
            auto& socket = *members_[size_t(i)];
            socket.connectToHostEncrypted("127.0.0.1", channel_->port());
            if (!QTest::qWaitFor([&] { return socket.isEncrypted(); }, 5000)) return false;
            const auto hello = frame({{"type", "hello"}, {"version", 1},
                {"name", QString("Member %1").arg(i + 2, 2, 10, QChar('0'))},
                {"avatarId", session_->avatars().at(i % 10)}, {"muted", i % 3 == 0},
                {"available", true}, {"capabilities", QJsonArray{}}});
            if (socket.write(hello) != hello.size()) return false;
            if (!QTest::qWaitFor([&] { return channel_->participants().size() == i + 2; }, 5000)) {
                qWarning() << "Member admission failed" << i + 2 << socket.errorString();
                return false;
            }
            auto* heartbeat = new QTimer(&socket);
            connect(heartbeat, &QTimer::timeout, &socket, [peer = &socket, ping = frame({{"type", "ping"}})] {
                if (peer->isEncrypted()) peer->write(ping);
            });
            heartbeat->start(2000);
        }
        return true;
    }
    bool tapNativeInput(QObject* object) {
#ifdef Q_OS_ANDROID
        const auto* item = qobject_cast<QQuickItem*>(object);
        if (!item || !item->window()) return false;
        const auto point = item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
            * item->window()->devicePixelRatio();
        const QJniObject view(reinterpret_cast<jobject>(item->window()->winId()));
        QNativeInterface::QAndroidApplication::runOnAndroidMainThread([point, view] {
            const QJniObject activity = QNativeInterface::QAndroidApplication::context();
            QJniEnvironment env;
            const auto location = env->NewIntArray(2);
            view.callMethod<void>("getLocationInWindow", "([I)V", location);
            jint origin[2]{};
            env->GetIntArrayRegion(location, 0, 2, origin);
            env->DeleteLocalRef(location);
            const auto time = QJniObject::callStaticMethod<jlong>("android/os/SystemClock", "uptimeMillis");
            for (jint action : {0, 1}) {
                const auto event = QJniObject::callStaticObjectMethod("android/view/MotionEvent", "obtain",
                    "(JJIFFI)Landroid/view/MotionEvent;", time, time, action, jfloat(point.x() + origin[0]), jfloat(point.y() + origin[1]), jint(0));
                activity.callMethod<jboolean>("dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z", event.object());
                event.callMethod<void>("recycle");
            }
        });
        return true;
#else
        Q_UNUSED(object);
        return false;
#endif
    }
    bool commitNativeInput(const QString& text) {
#ifdef Q_OS_ANDROID
        QNativeInterface::QAndroidApplication::runOnAndroidMainThread([text] {
            const QJniObject activity = QNativeInterface::QAndroidApplication::context();
            const auto view = activity.callObjectMethod("getCurrentFocus", "()Landroid/view/View;");
            if (!view.isValid() || !view.callMethod<jboolean>("onCheckIsTextEditor")) return;
            const QJniObject info("android/view/inputmethod/EditorInfo");
            const auto connection = view.callObjectMethod("onCreateInputConnection",
                "(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;", info.object());
            if (connection.isValid()) connection.callMethod<jboolean>("commitText", "(Ljava/lang/CharSequence;I)Z",
                QJniObject::fromString(text).object(), jint(1));
        });
        return true;
#else
        Q_UNUSED(text);
        return false;
#endif
    }
    bool orientWindow(QObject* object, bool landscape) {
#if defined(Q_OS_IOS)
        auto* window = qobject_cast<QQuickWindow*>(object);
        if (!window) return false;
        if (@available(iOS 16.0, *)) {
            auto* view = reinterpret_cast<UIView*>(window->winId());
            auto* scene = view.window.windowScene;
            if (!scene) return false;
            auto* preferences = [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:
                landscape ? UIInterfaceOrientationMaskLandscapeLeft : UIInterfaceOrientationMaskPortrait];
            [scene requestGeometryUpdateWithPreferences:preferences errorHandler:^(NSError* error) {
                qWarning("Orientation request failed: %s", error.localizedDescription.UTF8String);
            }];
            [preferences release];
            return true;
        }
        return false;
#elif defined(Q_OS_ANDROID)
        Q_UNUSED(object);
        QNativeInterface::QAndroidApplication::runOnAndroidMainThread([landscape] {
            QNativeInterface::QAndroidApplication::context().callMethod<void>("setRequestedOrientation", jint(landscape ? 0 : 1));
        });
        return true;
#else
        Q_UNUSED(object);
        Q_UNUSED(landscape);
        return false;
#endif
    }
    bool playEvent(const QString& kind, bool deafened) {
        audio_->setDeafened(deafened);
        const bool accepted = audio_->playEvent(kind);
        audio_->setDeafened(false);
        return accepted;
    }
    QVariantMap previewOutput(double volume, bool deafened) {
        const auto previous = audio_->deafened();
        audio_->setDeafened(deafened);
        const bool accepted = audio_->previewOutput(volume);
        const bool testing = audio_->testActive();
        audio_->setDeafened(previous);
        return {{"accepted", accepted}, {"testing", testing}, {"capturing", audio_->running()}};
    }
    bool setAudioVoiceState(bool joined, bool transmit) {
        return audio_->setVoiceState(joined, transmit, true);
    }
    bool captureSurvivesCallbackPauses() {
        QSignalSpy packets(audio_.get(), &AudioModel::audioPacket);
        QElapsedTimer elapsed;
        elapsed.start();
        // Fault injection, not readiness sleeps. Record across several
        // stalls so callback grouping cannot hide missing input samples.
        for (int pause = 0; pause < 6; ++pause) {
            QThread::msleep(80);
            QTest::qWait(80);
        }
        QTest::qWait(120);
        const auto expected = elapsed.elapsed() / 20;
        qInfo() << "Captured packets across callback pauses:" << packets.size() << "expected:" << expected;
        // Allow one capture block and the bounded sender tail at either end.
        return packets.size() >= expected - 3;
    }
    bool acceptsBotMessage(const QString& text, bool joined, bool deafened) {
        const auto previous = audio_->deafened();
        audio_->setVoiceState(joined, false);
        audio_->setDeafened(deafened);
        const bool accepted = audio_->readBotMessage(text);
        audio_->cancelBotMessages();
        audio_->setVoiceState(false, false);
        audio_->setDeafened(previous);
        return accepted;
    }
    bool textHasGlyphs(const QString& text, const QFont& font) const {
        QTextLayout layout(text, font);
        layout.beginLayout();
        auto line = layout.createLine();
        if (!line.isValid()) { layout.endLayout(); return false; }
        line.setLineWidth(100000);
        layout.endLayout();
        const auto runs = layout.glyphRuns();
        if (runs.isEmpty()) return false;
        for (const auto& run : runs)
            if (run.glyphIndexes().contains(0)) return false;
        return true;
    }
    bool setTestMusic(bool active) { return remoteChannel_->setMusicState("Test station", "playing", active); }
    bool shareTestImage(bool active = true) {
        if (!remoteChannel_->setScreenSharing(active)) return false;
        if (!active) return true;
        QImage source(960, 540, QImage::Format_RGBA8888); source.fill(QColor(34, 47, 59));
        QPainter painter(&source);
        painter.fillRect(90, 80, 340, 300, QColor(82, 157, 147));
        painter.fillRect(480, 150, 340, 300, QColor(212, 164, 90));
        painter.end();
        for (int tier : remoteChannel_->screenTiers()) {
            const auto frame = videoEncoders_[size_t(tier)].encode(source, tier, true);
            if (!frame.second.isEmpty() && !remoteChannel_->sendScreenFrame(tier, frame.first, frame.second, true)) return false;
        }
        return true;
    }
    QString clipboardText() const { return QGuiApplication::clipboard()->text(); }
    void captureOpenedUrl(const QUrl& url) { emit externalUrlOpened(url.toString()); }
    QString stationUrl() const { return station_->url(); }
    bool advertiseNearbyChannels(int count, const QString& prefix = QStringLiteral("Nearby")) {
        if (!channel_->startDiscovery()) { qWarning() << "Fixture discovery could not start:" << channel_->status(); return false; }
        QUdpSocket sender;
        if (!sender.bind(QHostAddress(QHostAddress::AnyIPv4), 0)) return false;
        // Shared discovery sockets all receive multicast. TTL zero keeps these
        // synthetic announcements on this computer, even with other apps open.
        sender.setSocketOption(QAbstractSocket::MulticastTtlOption, 0);
        sender.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
        QList<QNetworkInterface> interfaces, loopback;
        for (const auto& adapter : QNetworkInterface::allInterfaces()) {
            const auto flags = adapter.flags();
            if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)) continue;
            if (!flags.testFlag(QNetworkInterface::CanMulticast) && !flags.testFlag(QNetworkInterface::IsLoopBack)) continue;
            for (const auto& entry : adapter.addressEntries())
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    (flags.testFlag(QNetworkInterface::IsLoopBack) ? loopback : interfaces).append(adapter);
                    break;
                }
        }
        if (interfaces.isEmpty()) interfaces = loopback;
        if (interfaces.isEmpty()) return false;
        for (int i = 0; i <= count; ++i) {
            const auto data = QJsonDocument(QJsonObject{{"protocol", "squadspeak/1"},
                {"id", QString::number(0x7000 + i, 16).rightJustified(64, '0')},
                {"name", QString("%1 %2").arg(prefix).arg(i, 2, 10, QChar('0'))},
                {"port", 40000 + (i == count ? 0 : i)}}).toJson(QJsonDocument::Compact);
            bool sent = false;
            for (const auto& adapter : interfaces) {
                sender.setMulticastInterface(adapter);
                if (sender.writeDatagram(data, QHostAddress("239.255.85.73"), 48762) == data.size()) sent = true;
                else qWarning() << "Fixture announcement:" << adapter.name() << sender.errorString();
            }
            if (!sent) return false;
        }
        return true;
    }
    bool pressAccessible(QObject* object) {
        auto* accessible = QAccessible::queryAccessibleInterface(object);
        auto* action = accessible ? accessible->actionInterface() : nullptr;
        if (!action || !action->actionNames().contains(QAccessibleActionInterface::pressAction())) return false;
        action->doAction(QAccessibleActionInterface::pressAction());
        return true;
    }
    bool toggleAccessible(QObject* object) {
        auto* accessible = QAccessible::queryAccessibleInterface(object);
        auto* action = accessible ? accessible->actionInterface() : nullptr;
        if (!action || !action->actionNames().contains(QAccessibleActionInterface::toggleAction())) return false;
        action->doAction(QAccessibleActionInterface::toggleAction());
        return true;
    }
    bool advanceTime() { now_ += 600001; return true; }
    bool setSupporter(bool enabled) { return session_->setSupporterEnabled(enabled); }
    bool expireChat(int days = 1) { now_ += days * ChatHistory::lifetime + 1; return true; }
    bool saveWindow(QObject* window, const QString& path) {
        auto* view = qobject_cast<QQuickWindow*>(window);
        if (!view) return false;
        const auto image = view->grabWindow();
        QImageWriter writer(path);
        if (writer.write(image)) return true;
        qWarning() << "Screenshot failed" << path << writer.errorString()
                   << "image" << image.size() << "visible" << view->isVisible() << "exposed" << view->isExposed();
        return false;
    }
    bool scrollPixels(QObject* object, int x, int y) {
        auto* item = qobject_cast<QQuickItem*>(object);
        if (!item || !item->window()) return false;
        static const QPointingDevice touchpad("Touchpad", 123, QInputDevice::DeviceType::TouchPad,
            QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position | QInputDevice::Capability::PixelScroll, 2, 0);
        const auto position = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        bool accepted = false;
        // Native Qt gestures include both deltas. An angle-free update can be
        // discarded as a compatibility duplicate of the previous event.
        for (const auto phase : {Qt::ScrollBegin, Qt::ScrollUpdate, Qt::ScrollEnd}) {
            QWheelEvent event(position, item->window()->mapToGlobal(position),
                phase == Qt::ScrollUpdate ? QPoint(x, y) : QPoint{},
                phase == Qt::ScrollUpdate ? QPoint(x * 3, y * 3) : QPoint{},
                Qt::NoButton, Qt::NoModifier, phase, false, Qt::MouseEventSynthesizedBySystem, &touchpad);
            accepted |= QCoreApplication::sendEvent(item->window(), &event);
        }
        return accepted;
    }
    QColor renderedColor(QObject* object, qreal x, qreal y) {
        auto* item = qobject_cast<QQuickItem*>(object);
        if (!item || !item->window()) return {};
        const auto logicalWidth = item->window()->width();
        const auto position = item->mapToScene(QPointF(x, y));
        const auto image = item->window()->grabWindow();
        const auto point = (position * (qreal(image.width()) / logicalWidth)).toPoint();
        return image.rect().contains(point) ? image.pixelColor(point) : QColor{};
    }
    bool portraitHasDetail(QObject* object, bool grayscale = false) {
        return portraitsHaveDetail({object}, grayscale);
    }
    bool portraitsHaveDetail(const QList<QObject*>& objects, bool grayscale = false) {
        if (objects.isEmpty()) return false;
        QQuickWindow* window = nullptr;
        QList<QRectF> areas;
        for (auto* object : objects) {
            auto* item = qobject_cast<QQuickItem*>(object);
            if (!item || !item->window() || (window && item->window() != window)) return false;
            window = item->window();
            areas.append(item->mapRectToScene(QRectF(item->width() * 0.3, item->height() * 0.35,
                item->width() * 0.4, item->height() * 0.35)));
        }
        const auto logicalWidth = window->width();
        // Grabbing can process events and recycle a PathView delegate. Capture
        // its geometry first; the returned bitmap may have no DPR metadata.
        const auto image = window->grabWindow();
        const auto ratio = qreal(image.width()) / logicalWidth;
        for (const auto& area : areas) {
            const QRect sampledArea(QPoint(int(area.left() * ratio), int(area.top() * ratio)),
                                    QPoint(int(area.right() * ratio), int(area.bottom() * ratio)));
            if (sampledArea.isEmpty() || !image.rect().contains(sampledArea)) return false;
            int darkest = 255, lightest = 0;
            for (int y = int(area.top() * ratio); y < int(area.bottom() * ratio); ++y)
                for (int x = int(area.left() * ratio); x < int(area.right() * ratio); ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    if (grayscale && (pixel.red() != pixel.green() || pixel.green() != pixel.blue())) return false;
                    const auto intensity = qGray(pixel.rgb());
                    darkest = std::min(darkest, intensity); lightest = std::max(lightest, intensity);
                }
            if (lightest - darkest <= 40) return false;
        }
        return true;
    }
    double portraitGrayscaleMismatch(QObject* colored, QObject* grayscale) {
        auto* source = qobject_cast<QQuickItem*>(colored);
        auto* target = qobject_cast<QQuickItem*>(grayscale);
        if (!source || !target || !source->window() || source->window() != target->window()) return 1;
        const auto logicalWidth = source->window()->width();
        const auto sourcePosition = source->mapToScene(QPointF{});
        const auto targetPosition = target->mapToScene(QPointF{});
        const auto size = source->size();
        const auto image = source->window()->grabWindow();
        const auto ratio = qreal(image.width()) / logicalWidth;
        const auto left = sourcePosition * ratio;
        const auto right = targetPosition * ratio;
        int sampled = 0, mismatched = 0;
        for (int y = qCeil(4 * ratio); y < int((size.height() - 4) * ratio); ++y)
            for (int x = qCeil(4 * ratio); x < int((size.width() - 4) * ratio); ++x) {
                const auto a = image.pixelColor((left + QPointF(x, y)).toPoint());
                // Exclude neutral panel backgrounds and anti-aliased outer borders.
                if (std::max({a.red(), a.green(), a.blue()}) - std::min({a.red(), a.green(), a.blue()}) < 25) continue;
                const auto b = image.pixelColor((right + QPointF(x, y)).toPoint());
                const auto expected = qRound(0.2126 * a.red() + 0.7152 * a.green() + 0.0722 * a.blue());
                ++sampled;
                if (std::abs(b.red() - expected) > 12 || std::abs(b.green() - expected) > 12
                    || std::abs(b.blue() - expected) > 12) ++mismatched;
            }
        return sampled > 30 ? double(mismatched) / sampled : 1;
    }
    bool startRemoteHost() { return remoteChannel_->listen(QHostAddress::LocalHost); }
    bool publishLevels(const QVariantMap& levels) { return channel_->publishLevels(levels); }
    bool publishSystem(const QString& text) { return channel_->sendSystemMessage(text); }
    bool setBotMusicState(const QString& name, const QString& state, bool active, const QString& host = {}) {
        auto* owner = channel_->ownChannel(host.isEmpty() ? channel_->ownId() : host);
        return owner && owner->setMusicState(name, state, active);
    }
    QString botId() const { return channel_ ? channel_->musicId() : QString{}; }
    Q_INVOKABLE bool startHost() { return channel_->listen(QHostAddress::LocalHost); }
    void qmlEngineAvailable(QQmlEngine* engine) {
        QDesktopServices::setUrlHandler("https", this, "captureOpenedUrl");
        QDesktopServices::setUrlHandler("x-apple.systempreferences", this, "captureOpenedUrl");
        QDesktopServices::setUrlHandler("app-settings", this, "captureOpenedUrl");
        QCoreApplication::setApplicationVersion(QStringLiteral(SQUADSPEAK_VERSION));
        station_ = std::make_unique<RadioStation>();
        qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1");
        engine->rootContext()->setContextProperty("fixtures", this);
        session_ = std::make_unique<VoiceSession>(folder_.filePath("session.json"));
        license_ = std::make_unique<License>(folder_.filePath("license"), QJsonObject{}, QUrl{});
        engine->rootContext()->setContextProperty("supporterLicense", license_.get());
        connect(session_.get(), &VoiceSession::preferencesChanged, engine, [this, engine] {
            if (appliedLanguage_ == session_->language()) return;
            appliedLanguage_ = session_->language();
            QCoreApplication::removeTranslator(&translation_);
            if (session_->language() != "en") {
                if (!translation_.load(":/i18n/squadspeak_" + session_->language() + ".qm")) qFatal("Translation unavailable");
                QCoreApplication::installTranslator(&translation_);
            }
            QLocale::setDefault(QLocale(appliedLanguage_ == "en" ? "en_GB" : appliedLanguage_));
            QGuiApplication::setLayoutDirection(QLocale(appliedLanguage_).textDirection());
            engine->setUiLanguage(appliedLanguage_);
            engine->retranslate();
            if (audio_) { audio_->setAppLanguage(session_->language()); audio_->devicesChanged(); }
        });
        engine->rootContext()->setContextProperty("voiceSession", session_.get());
        // Endpoints stay closed until a network UI scenario explicitly starts
        // one. No microphone or native global keyboard observer is started.
        channel_ = std::make_unique<LocalChannel>(*session_, folder_.filePath("channel.json"), TlsIdentity::create(), [this] { return now_; });
        unreadyChannel_ = std::make_unique<LocalChannel>(*session_, folder_.filePath("unready-channel.json"));
        QQmlEngine::setObjectOwnership(unreadyChannel_.get(), QQmlEngine::CppOwnership);
        engine->rootContext()->setContextProperty("unreadyChannel", unreadyChannel_.get());
        screen_ = std::make_unique<ScreenShare>(*channel_);
        engine->rootContext()->setContextProperty("screenShare", screen_.get());
        radio_ = std::make_unique<RadioPlayer>(folder_.filePath("radio.json"));
        QQmlEngine::setObjectOwnership(channel_.get(), QQmlEngine::CppOwnership);
        QQmlEngine::setObjectOwnership(radio_.get(), QQmlEngine::CppOwnership);
        if (!radio_->bind(*channel_)) throw std::runtime_error("Radio fixture could not bind channels.");
        engine->rootContext()->setContextProperty("radio", radio_.get());
        engine->rootContext()->setContextProperty("session", session_.get());
        engine->rootContext()->setContextProperty("channel", channel_.get());
        engine->rootContext()->setContextProperty("chatContent", &chatContent_);
        QImage chatImage(80, 50, QImage::Format_ARGB32);
        chatImage.fill(QColor(5, 150, 105, 128));
        if (!chatImage.save(folder_.filePath("chat.png"))) qFatal("Chat fixture could not be created");
        engine->rootContext()->setContextProperty("chatImageFile", QUrl::fromLocalFile(folder_.filePath("chat.png")));
        remoteSession_ = std::make_unique<VoiceSession>(folder_.filePath("remote-session.json"));
        remoteSession_->setUserName("Mira");
        remoteSession_->setAvatar("courier");
        remoteChannel_ = std::make_unique<LocalChannel>(*remoteSession_, folder_.filePath("remote-channel.json"), TlsIdentity::create(), [this] { return now_; });
        QQmlEngine::setObjectOwnership(remoteChannel_.get(), QQmlEngine::CppOwnership);
        engine->rootContext()->setContextProperty("remoteChannel", remoteChannel_.get());
        engine->rootContext()->setContextProperty("remoteSession", remoteSession_.get());
        pttKey_ = std::make_unique<PushToTalkKey>(*session_, false);
        engine->rootContext()->setContextProperty("pttKey", pttKey_.get());
        // Use the real settings API without starting capture, playback or
        // connecting it to the test channels.
        audio_ = std::make_unique<AudioModel>(folder_.filePath("audio.ini"));
        const auto testInput = qEnvironmentVariable("SQUADSPEAK_TEST_INPUT_DEVICE");
        if (!testInput.isEmpty() && !audio_->selectInput(testInput))
            qFatal("The requested virtual test input is unavailable.");
        audio_->setAppLanguage(session_->language());
        const auto expectedInput = qgetenv("SQUADSPEAK_TEST_INPUT");
        if ((expectedInput == "present" && !audio_->inputAvailable())
                || (expectedInput == "absent" && audio_->inputAvailable()))
            qFatal("The test input device does not match SQUADSPEAK_TEST_INPUT.");
        engine->rootContext()->setContextProperty("audio", audio_.get());
        engine->rootContext()->setContextProperty("nativeCaptureAcceptance",
            qEnvironmentVariable("SQUADSPEAK_TEST_INPUT") == "present");
        squad::AudioProcessor processor(48000, {0, 0, 300});
        squad::VoiceMixer preview;
        preview.setNoiseSuppression(0.5);
        for (int offset = 0; offset < 24000; offset += 960) {
            std::array<float, 960> samples{};
            for (size_t i = 0; i < samples.size(); ++i) {
                const auto phase = 2 * std::numbers::pi * 220 * (offset + i) / 48000;
                samples[i] = float(0.1 * std::sin(phase) + 0.25 * std::sin(2 * phase) + 0.08 * std::sin(3 * phase));
            }
            (void)preview.encode(samples, 48000, false,
                [&](std::span<float> frame) { for (auto& sample : frame) sample = processor.filter(sample); },
                [&](std::span<const float> raw, std::span<const float> filtered) {
                    for (size_t i = 0; i < raw.size(); ++i)
                        processor.observeInput(raw[i]).observeOutput(filtered[i]);
                });
        }
        const auto levels = processor.levels();
        QVariantList raw, filtered;
        for (const auto db : levels.inputSpectrum) raw.push_back(db);
        for (const auto db : levels.outputSpectrum) filtered.push_back(db);
        engine->rootContext()->setContextProperty("referenceTone", QVariantMap{
            {"running", true}, {"spectrumReady", levels.spectrumReady},
            {"inputDb", levels.inputDb}, {"outputDb", levels.outputDb},
            {"maximumFrequency", processor.maximumFrequency()},
            {"filterResponse", [&] { QVariantList values; for (const auto db : processor.filterResponse()) values.push_back(db); return values; }()},
            {"strongestHz", levels.strongestHz}, {"fundamentalHz", levels.fundamentalHz},
            {"inputSpectrum", raw}, {"outputSpectrum", filtered}});
        auto imageDirectory = qEnvironmentVariable("SQUAD_TEST_ARTIFACTS");
#ifdef Q_OS_ANDROID
        if (imageDirectory.isEmpty()) imageDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/artifacts";
#endif
        if (!imageDirectory.isEmpty()) QDir().mkpath(imageDirectory);
        engine->rootContext()->setContextProperty("imageDirectory", imageDirectory);
        engine->rootContext()->setContextProperty("nativeTranslationExpected",
            LocalLanguage::supported() && QGuiApplication::platformName() != "offscreen"
                && QOperatingSystemVersion::current().majorVersion() >= 26);
    }
signals:
    void externalUrlOpened(const QString& url);
};

int main(int argc, char** argv) {
    QTEST_SET_MAIN_SOURCE_PATH
    QGuiApplication app(argc, argv);
    previousMessageHandler = qInstallMessageHandler(failOnQuickTestStartupWarning);
    // Audio and network fixtures must be destroyed before Qt's application services.
    AudioFixtures setup;
    return quick_test_main_with_setup(argc, argv, "audio_controls", nullptr, &setup);
}
#include "controls_tests.moc"
