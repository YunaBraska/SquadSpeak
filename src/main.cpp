#include "audio_model.hpp"
#include "voice_session.hpp"
#include "local_channel.hpp"
#include "chat_content.hpp"
#include "ptt_key.hpp"
#include "radio_player.hpp"
#include "headless.hpp"
#include "license.hpp"
#include "screen_share.hpp"
#ifdef SQUADSPEAK_UPDATES
#include "app_updates.hpp"
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
#include "apple_notifications.hpp"
#include "system_appearance.hpp"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#if !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
#include <QMenu>
#include <QSystemTrayIcon>
#endif
#ifdef Q_OS_IOS
#include <QImageReader>
#endif
#include <QPainter>
#include <QPainterPath>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlPropertyMap>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QStyleHints>
#include <QLockFile>
#include <QTranslator>
#include <QLocale>

#include <cstdio>
#ifdef Q_OS_MACOS
#include <QKeySequence>
#include <QMenuBar>
#import <AppKit/AppKit.h>
#endif

int main(int argc, char** argv) {
#ifdef Q_OS_ANDROID
    // QApplication and its post-routines finish before Android ends the process.
    // Qt's additional native exit races Android HWUI's detached worker shutdown.
    qputenv("QT_ANDROID_NO_EXIT_CALL", "1");
    qputenv("QT_ANDROID_NO_FULLSCREEN_KEYBOARD", "1");
#endif
#ifdef Q_OS_MACOS
    // SecureTransport needs a temporary import for TLS. Durable device secrets
    // remain owned by QtKeychain; handshake imports do not enter the login keychain.
    qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1");
#endif
    for (int i = 1; i < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == "--headless") return Headless::run(argc, argv);
    QApplication app(argc, argv);
#ifdef Q_OS_IOS
    QImageReader::setAllocationLimit(128);
#endif
    app.setApplicationName("SquadSpeak");
    app.setOrganizationName("SquadSpeak");
    app.setApplicationVersion(QStringLiteral(SQUADSPEAK_VERSION));
    app.setWindowIcon(QIcon(":/qt/qml/SquadSpeak/ui/icons/app.png"));
    app.setDesktopFileName("app.squadspeak");
    app.setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle("Basic");
    const auto addOptions = [](QCommandLineParser& parser) {
        parser.setApplicationDescription("SquadSpeak - " + QCoreApplication::translate("Tray", "Channels"));
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addOption({"headless", QCoreApplication::translate("Headless", "Run without GUI, microphone capture, or local audio output.") + " (--headless --help)"});
        parser.addOption({"settings", QCoreApplication::translate("Desktop", "Open settings at startup.")});
        parser.addOption({"settings-file", QCoreApplication::translate("Desktop", "Alternative file for audio profiles."), "path"});
        parser.addOption({"smoke-test", QCoreApplication::translate("Desktop", "Load the interface without microphone capture and exit after one second.")});
        parser.addOption({"screenshot", QCoreApplication::translate("Desktop", "Save an image of the app window during the UI test."), "path"});
    };
    QCommandLineParser bootstrap;
    addOptions(bootstrap);
    const bool parsed = bootstrap.parse(app.arguments());
    auto settingsFile = bootstrap.value("settings-file");
    if (settingsFile.isEmpty()) settingsFile = qEnvironmentVariable("SQUADSPEAK_SETTINGS_FILE");
    if (settingsFile.isEmpty())
        settingsFile = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/audio.ini";

    // Take the profile lock before reading preferences for a normal start.
    // Informational CLI calls do not create a directory or acquire that lock.
    const bool profileRequired = parsed && !bootstrap.isSet("help")
        && !bootstrap.isSet("help-all") && !bootstrap.isSet("version");
    const bool directoryReady = !profileRequired || QDir().mkpath(QFileInfo(settingsFile).absolutePath());
    QLockFile instanceLock(settingsFile + ".instance.lock");
    const bool locked = profileRequired && directoryReady && instanceLock.tryLock(0);
    QTranslator translation, baseTranslation, controlsTranslation;
    try {
        VoiceSession session(settingsFile + ".session.json");
        QString appliedLanguage;
        const auto applyLanguage = [&] {
            if (appliedLanguage == session.language()) return false;
            for (auto* catalog : {&translation, &baseTranslation, &controlsTranslation}) app.removeTranslator(catalog);
            appliedLanguage = session.language();
            if (appliedLanguage != "en") {
                for (const auto& entry : {std::pair{&translation, "squadspeak"}, std::pair{&baseTranslation, "qtbase"}, std::pair{&controlsTranslation, "qtdeclarative"}}) {
                    const auto path = ":/i18n/" + QString::fromLatin1(entry.second) + "_" + appliedLanguage + ".qm";
                    if (entry.first != &translation && !QFile::exists(path)) continue;
                    if (!entry.first->load(path))
                        throw std::runtime_error(qUtf8Printable(QCoreApplication::translate("Headless", "Could not load language: %1").arg(path)));
                    app.installTranslator(entry.first);
                }
            }
            QLocale::setDefault(QLocale(appliedLanguage == "en" ? "en_GB" : appliedLanguage));
            app.setLayoutDirection(QLocale(appliedLanguage).textDirection());
            return true;
        };
        applyLanguage();
        QCommandLineParser arguments;
        addOptions(arguments);
        arguments.process(app);
        if (!directoryReady) {
            qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "The settings directory could not be created.")));
            return 1;
        }
        if (!locked) {
            qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "This SquadSpeak profile is already running or cannot be locked.")));
            return 1;
        }
        QElapsedTimer startup;
        startup.start();
        const auto startupPhase = [&](const char* phase) {
            if (arguments.isSet("smoke-test")) qInfo("UI smoke: %s (%lld ms)", phase, startup.elapsed());
        };
        startupPhase("initializing audio");
        AudioModel audio(settingsFile);
        audio.setAppLanguage(session.language());
        startupPhase("initializing supporter access");
        License license(License::storageDirectory(),
            License::distributionProduct(), QUrl("https://api.github.com/graphql"));
        QObject::connect(&license, &License::changed, &session, [&] { session.setSupporterEnabled(license.active()); });
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, &license, [&](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && !arguments.isSet("smoke-test")) license.refreshIfDue();
        });
        const auto applyAppearance = [&session] {
            QGuiApplication::styleHints()->setColorScheme(session.theme() == "dark" ? Qt::ColorScheme::Dark
                : session.theme() == "light" ? Qt::ColorScheme::Light : Qt::ColorScheme::Unknown);
        };
        QObject::connect(&session, &VoiceSession::preferencesChanged, &app, applyAppearance);
        applyAppearance();
        startupPhase("initializing channels");
        LocalChannel channel(session, settingsFile + ".channel.json");
        startupPhase("initializing screen capture");
        ScreenShare screenShare(channel);
        ChatContent chatContent;
        startupPhase("initializing radio");
        RadioPlayer radio(settingsFile + ".radio.json");
        // Channel/radio accessors lend these objects to QML; C++ owns their lifetime.
        QQmlEngine::setObjectOwnership(&channel, QQmlEngine::CppOwnership);
        QQmlEngine::setObjectOwnership(&radio, QQmlEngine::CppOwnership);
        if (!radio.bind(channel)) throw std::runtime_error(qUtf8Printable(QCoreApplication::translate("Headless", "Radio channels could not be initialized.")));
        // Persist the first suggested name only after startup settings validate.
        if (!session.setUserName(session.userName())) throw std::runtime_error(qUtf8Printable(session.error()));
        startupPhase("initializing shortcuts and updates");
        PushToTalkKey pttKey(session, !arguments.isSet("smoke-test"));
#ifdef SQUADSPEAK_UPDATES
        AppUpdates updates(!arguments.isSet("smoke-test"));
#endif
        QQmlPropertyMap systemAppearance;
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
        observeSystemAppearance(systemAppearance);
#else
        systemAppearance.insert("reducedMotion", false);
        systemAppearance.insert("textScale", 1.0);
#endif
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("systemAppearance", &systemAppearance);
        engine.setUiLanguage(session.language());
        QObject::connect(&session, &VoiceSession::preferencesChanged, &engine, [&] {
            if (applyLanguage()) {
                engine.setUiLanguage(session.language());
                audio.setAppLanguage(session.language());
                audio.devicesChanged();
                engine.retranslate();
            }
        });
        engine.rootContext()->setContextProperty("screenShare", &screenShare);
        engine.rootContext()->setContextProperty("radio", &radio);
        engine.rootContext()->setContextProperty("supporterLicense", &license);
#ifdef SQUADSPEAK_UPDATES
        engine.rootContext()->setContextProperty("appUpdates", &updates);
#endif
        engine.rootContext()->setContextProperty("audio", &audio);
        engine.rootContext()->setContextProperty("session", &session);
        engine.rootContext()->setContextProperty("channel", &channel);
        engine.rootContext()->setContextProperty("chatContent", &chatContent);
        engine.rootContext()->setContextProperty("pttKey", &pttKey);
        startupPhase("loading interface");
        engine.loadFromModule("SquadSpeak", "Channels");
        startupPhase("interface loaded");
        if (engine.rootObjects().size() != 1) return 1;
        auto* channels = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        if (!channels) return 1;
        if (license.configured() && !arguments.isSet("smoke-test")) license.refresh();
        if (!arguments.isSet("smoke-test") && !QObject::connect(channels, SIGNAL(previewRequested(bool)),
                &audio, SLOT(setPreviewActive(bool)))) return 1;
        QObject::connect(&audio, &AudioModel::runningChanged, &session, [&session, &audio] {
            session.setAudioTestActive(audio.testActive());
        });
        const auto updateReadiness = [&] { session.setAudioReadiness(audio.inputReady(), audio.outputReady()); };
        QObject::connect(&audio, &AudioModel::readinessChanged, &session, updateReadiness);
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, &session, updateReadiness);
        updateReadiness();
        const auto updateVoice = [&audio, &channel, &session] {
            audio.setVoiceState(channel.joined() || !channel.audioSources().isEmpty(),
                channel.joined() && session.transmissionAllowed(), session.pushToTalk(), channel.audioPacketLimit());
            audio.setDeafened(session.deafened());
        };
        QObject::connect(&channel, &LocalChannel::stateChanged, &audio, updateVoice);
        QObject::connect(&session, &VoiceSession::presenceChanged, &audio, updateVoice);
        QObject::connect(&channel, &LocalChannel::participantsChanged, &audio, [&audio, &channel, updateVoice] {
            updateVoice(); audio.setParticipants(channel.audioSources(), channel.joinedHostId());
        });
        QObject::connect(&channel, &LocalChannel::audioReceived, &audio, &AudioModel::receiveAudio);
        QObject::connect(&channel, &LocalChannel::contactsObserved, &audio, &AudioModel::observeContacts);
        QObject::connect(&audio, &AudioModel::audioPacket, &channel, &LocalChannel::sendAudio);
        QObject::connect(&audio, &AudioModel::activityChanged, &channel, [&] {
            if (!audio.voiceSending()) channel.sendAudio({});
        });
        QObject::connect(&channel, &LocalChannel::channelEvent, &audio, [&](const QString& kind) {
            const bool spoken = audio.botSpeechReady() && channel.joined() && kind != "leave" && kind != "request";
            if (session.eventSounds() && !spoken) audio.playEvent(kind);
        });
        QObject::connect(&session, &VoiceSession::preferencesChanged, &audio, [&] {
            if (!session.eventSounds()) audio.cancelNotifications();
        });
        QObject::connect(&channel, &LocalChannel::stateChanged, &audio,
            [&, host = QString{}]() mutable {
                const auto next = channel.joinedHostId();
                if (host != next || channel.remoteMode()) audio.cancelBotMessages();
                host = next;
            });
        QObject::connect(&channel, &LocalChannel::botNotification, &audio, [&](const QVariantMap& message) {
            if (!session.eventSounds()) return;
            audio.readBotMessage(chatContent.plainText(message));
        });
        QObject::connect(&channel, &LocalChannel::chatNotification, &app, [&](const QString& hostId, const QVariantMap& message) {
            const bool announcement = message.value("event").toMap().value("kind") == "announcement";
            if (session.eventSounds() && !(announcement && audio.botSpeechReady()))
                audio.playEvent(announcement ? "announcement" : "message");
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
            QString name;
            for (const auto& entry : channel.savedChannels())
                if (entry.toMap().value("id").toString() == hostId) name = entry.toMap().value("name").toString();
            const auto title = name + " - " + message.value("name").toString();
            const auto text = message.value("text").toString().simplified();
            const auto body = text.isEmpty() ? QCoreApplication::translate("Tray", "New image") : text.left(160);
            AppleNotifications::show(hostId + '/' + message.value("sequence").toString(), title, body);
#else
            Q_UNUSED(hostId);
#endif
        });
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
        QObject::connect(&channel, &LocalChannel::stateChanged, &app, [&channel, notificationsPrepared = false]() mutable {
            if (!notificationsPrepared && channel.joined()) {
                notificationsPrepared = true;
                AppleNotifications::prepare();
            }
        });
#endif
        QObject::connect(&audio, &AudioModel::activityChanged, &channel, [&] {
            auto levels = audio.playbackLevels(); levels.insert(channel.ownId(), audio.transmitLevel());
            channel.publishLevels(levels);
        });
        updateVoice();
        const auto showChannels = [channels] {
            channels->show();
            channels->raise();
            channels->requestActivate();
        };
        const auto showSettings = [channels, showChannels] {
            showChannels();
            QMetaObject::invokeMethod(channels, "openSettings", Q_ARG(QVariant, 0));
        };
#ifdef Q_OS_MACOS
        const auto applyDock = [&session] {
            const auto policy = session.showInDock() ? NSApplicationActivationPolicyRegular : NSApplicationActivationPolicyAccessory;
            if (NSApp.activationPolicy != policy && ![NSApp setActivationPolicy:policy])
                qWarning("Could not change the application activation policy");
        };
        applyDock();
        QObject::connect(&session, &VoiceSession::preferencesChanged, &app, applyDock);
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, channels, [&, showChannels](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && session.showInDock() && !channels->isVisible()) showChannels();
        });
        if (session.showInDock() && !arguments.isSet("smoke-test")) showChannels();
#endif

#if !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
        const auto speechIcon = [&app](bool active) {
            QPixmap mark(64, 64);
            mark.fill(Qt::transparent);
            {
                QPainter painter(&mark);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.scale(64.0 / 18, 64.0 / 18);
                const auto color = app.palette().color(QPalette::WindowText);
                painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.setBrush(active ? QBrush(color) : QBrush(Qt::NoBrush));
                QPainterPath bubble;
                bubble.moveTo(5, 3); bubble.lineTo(13, 3); bubble.quadTo(16, 3, 16, 6);
                bubble.lineTo(16, 10); bubble.quadTo(16, 13, 13, 13);
                bubble.lineTo(8, 13); bubble.lineTo(4, 16); bubble.lineTo(4, 12.8);
                bubble.quadTo(2, 12, 2, 10); bubble.lineTo(2, 6); bubble.quadTo(2, 3, 5, 3);
                bubble.closeSubpath();
                painter.drawPath(bubble);
            }
            QIcon icon(mark);
#ifdef Q_OS_MACOS
            icon.setIsMask(true);
#endif
            return icon;
        };
        const auto idleIcon = speechIcon(false), speakingIcon = speechIcon(true);
        QMenu menu;
        auto* channelsAction = menu.addAction(QCoreApplication::translate("Tray", "Channels"), &app, showChannels);
        auto* settingsAction = menu.addAction(QCoreApplication::translate("Tray", "Settings"), &app, showSettings);
        menu.addSeparator();
        auto* quitAction = menu.addAction(QCoreApplication::translate("Tray", "Quit"), &app, &QCoreApplication::quit);
#ifdef Q_OS_MACOS
        // A parentless menu bar supplies the native app menu to Qt Quick windows.
        QMenuBar applicationMenuBar;
        auto* applicationMenu = applicationMenuBar.addMenu(app.applicationName());
        auto* aboutAction = applicationMenu->addAction(QCoreApplication::translate("Channels", "About"), channels,
            [channels, showChannels] {
                showChannels();
                QMetaObject::invokeMethod(channels, "openSettings", Q_ARG(QVariant, 3));
            });
        aboutAction->setMenuRole(QAction::AboutRole);
        settingsAction->setMenuRole(QAction::PreferencesRole);
        settingsAction->setShortcut(QKeySequence::Preferences);
        settingsAction->setShortcutContext(Qt::ApplicationShortcut);
        quitAction->setMenuRole(QAction::QuitRole);
        applicationMenu->addAction(settingsAction);
        applicationMenu->addSeparator();
        applicationMenu->addAction(quitAction);
        const auto updateLocalMenu = [&channel, settingsAction, aboutAction] {
            settingsAction->setEnabled(!channel.remoteMode());
            aboutAction->setEnabled(!channel.remoteMode());
        };
        QObject::connect(&channel, &LocalChannel::remoteChanged, &applicationMenuBar, updateLocalMenu);
        updateLocalMenu();
        QObject::connect(&session, &VoiceSession::preferencesChanged, &applicationMenuBar, [aboutAction] {
            aboutAction->setText(QCoreApplication::translate("Channels", "About"));
        });
#endif
        QObject::connect(&session, &VoiceSession::preferencesChanged, &menu, [&] {
            channelsAction->setText(QCoreApplication::translate("Tray", "Channels"));
            settingsAction->setText(QCoreApplication::translate("Tray", "Settings"));
            quitAction->setText(QCoreApplication::translate("Tray", "Quit"));
        });
        QSystemTrayIcon tray(idleIcon, &app);
        bool speaking = false;
        QObject::connect(&audio, &AudioModel::activityChanged, &tray, [&] {
            const bool active = audio.transmitLevel() > 0.001;
            if (active == speaking) return;
            speaking = active;
            tray.setIcon(active ? speakingIcon : idleIcon);
            tray.setToolTip(active ? QCoreApplication::translate("Tray", "SquadSpeak - speaking") : "SquadSpeak");
        });
        tray.setToolTip("SquadSpeak");
#ifndef Q_OS_MACOS
        tray.setContextMenu(&menu);
#endif
        QObject::connect(&tray, &QSystemTrayIcon::activated, &app, [&](QSystemTrayIcon::ActivationReason reason) {
            // Qt 6.9 queries clickCount on a non-mouse AppKit event when a
            // status-item menu opens on macOS 27. Keep activation on the button;
            // the separate context menu does not install that tracking observer.
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showChannels();
#ifdef Q_OS_MACOS
            else if (reason == QSystemTrayIcon::Context) menu.popup(QCursor::pos());
#endif
        });
        if (QSystemTrayIcon::isSystemTrayAvailable()) tray.show();
        else if (!arguments.isSet("smoke-test")) {
            qWarning("%s", qUtf8Printable(QCoreApplication::translate("Desktop", "The system tray is unavailable; SquadSpeak remains accessible as a window.")));
            app.setQuitOnLastWindowClosed(true);
            showChannels();
        }
#else
        channels->showMaximized();
#endif
        if (!arguments.isSet("smoke-test") && arguments.isSet("settings")) showSettings();
        if (!arguments.isSet("smoke-test")) channel.initialize();
        if (arguments.isSet("smoke-test")) {
            qInfo("UI smoke: checking profile and settings");
            if (app.windowIcon().isNull() || app.windowIcon().pixmap(32, 32).isNull()) return 1;
            // Exercise settings persistence while the application instance lock
            // is held, even on test hosts without an audio input device.
            if (!audio.selectInput("")) return 1;
            showChannels();
            if (!session.available()) return 1;
#ifdef Q_OS_MACOS
            qInfo("UI smoke: checking native Settings and About actions");
            auto* settingsDialog = channels->findChild<QObject*>("settingsDialog");
            settingsAction->trigger();
            if ((QGuiApplication::platformName() == "cocoa" && !applicationMenuBar.isNativeMenuBar())
                    || settingsAction->shortcut() != QKeySequence(QKeySequence::Preferences)
                    || !settingsDialog || !settingsDialog->property("visible").toBool()
                    || channels->property("settingsPage").toInt() != 0 || !session.available()) {
                qCritical("UI smoke: native Settings must open the profile settings (platform=%s native=%d shortcut=%d dialog=%d visible=%d page=%d voice=%d)",
                    qPrintable(QGuiApplication::platformName()), applicationMenuBar.isNativeMenuBar(),
                    settingsAction->shortcut() == QKeySequence(QKeySequence::Preferences), bool(settingsDialog),
                    settingsDialog && settingsDialog->property("visible").toBool(),
                    channels->property("settingsPage").toInt(), session.available());
                return 1;
            }
            aboutAction->trigger();
            if (!settingsDialog->property("visible").toBool() || channels->property("settingsPage").toInt() != 3
                    || !session.available()) {
                qCritical("UI smoke: native About must open the About settings");
                return 1;
            }
#endif
            if (!QMetaObject::invokeMethod(channels, "openSettings", Q_ARG(QVariant, 1)) || !session.available()) {
                qCritical("UI smoke: audio settings must preserve voice availability");
                return 1;
            }
            QTimer::singleShot(1000, &app, [&app, &arguments, &session, channels] {
                qInfo("UI smoke: capturing settings");
                const auto output = arguments.value("screenshot");
                if (!output.isEmpty()) {
                    const auto directory = QFileInfo(output).absolutePath();
                    if (!QDir().mkpath(directory) || !channels->grabWindow().save(output)) {
                        qCritical("%s", qUtf8Printable(QCoreApplication::translate("Desktop", "The window image could not be saved.")));
                        app.exit(1);
                        return;
                    }
                }
                QMetaObject::invokeMethod(channels, "closeSettings");
                if (!session.available()) { app.exit(1); return; }
                QTimer::singleShot(100, &app, [&app, channels, output] {
                    qInfo("UI smoke: capturing channels");
                    if (!output.isEmpty() && !channels->grabWindow().save(QFileInfo(output).absolutePath() + "/channels.png")) { app.exit(1); return; }
                    qInfo("UI smoke: complete");
                    app.quit();
                });
            });
        }
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &audio, [&] {
            for (auto* window : QGuiApplication::allWindows()) {
                if (auto* quick = qobject_cast<QQuickWindow*>(window)) {
                    quick->close();
                    quick->releaseResources();
                }
            }
            channel.leave(); audio.stop();
            // Drain render-thread notifications while their QML targets still
            // exist. After aboutToQuit, Qt only processes deferred deletions.
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
            channels->deleteLater();
        });
        return app.exec();
    } catch (const std::exception& error) {
        QFile stderrFile;
        if (stderrFile.open(stderr, QIODevice::WriteOnly)) {
            stderrFile.write((QStringLiteral("SquadSpeak: ")
                + QString::fromUtf8(error.what()) + QLatin1Char('\n')).toUtf8());
            stderrFile.flush();
        }
        return 1;
    }
}
