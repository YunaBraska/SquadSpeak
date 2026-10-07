#include "screen_share.hpp"
#include "voice_mixer.hpp"
#include "output_monitor.hpp"
#include "echo_canceller.hpp"
#include <QApplication>
#include <QAudioSink>
#include <QMediaDevices>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QVideoFrame>
#include <QWidget>
#ifdef Q_OS_LINUX
#include <signal.h>
#endif
#include <cmath>
#include <numbers>
#include <QtEndian>
#ifdef Q_OS_LINUX
#include <X11/Xlib.h>
#endif

static QPair<double, double> amplitudes(const QSignalSpy& packets, const QString& musicId, double wanted, double unrelated) {
    squad::VoiceMixer decoder;
    decoder.setAutomatic(false);
    std::vector<float> rendered;
    qint64 received = 0;
    for (const auto& packet : packets) {
        if (packet.at(0).toString() != musicId
            || !decoder.receive(musicId, packet.at(1).toByteArray(), received)) return {};
        const auto frame = decoder.render(48000, received);
        // Ignore codec startup and measure several complete periods.
        if (received >= 100) rendered.insert(rendered.end(), frame.begin(), frame.end());
        received += 20;
    }
    if (std::none_of(rendered.begin(), rendered.end(), [](float value) { return std::abs(value) > 0.00001; })) return {};
    const auto amplitude = [&](double frequency) {
        double sine = 0, cosine = 0;
        for (size_t i = 0; i < rendered.size(); ++i) {
            const auto phase = 2 * std::numbers::pi * frequency * double(i) / 48000;
            sine += rendered[i] * std::sin(phase); cosine += rendered[i] * std::cos(phase);
        }
        return std::hypot(sine, cosine);
    };
    return {amplitude(wanted), amplitude(unrelated)};
}

static bool receivedFixture(const QVideoSink& sink) {
    const auto image = sink.videoFrame().toImage();
    if (image.isNull()) return false;
    int colored = 0;
    // A captured mouse pointer may cover the exact center. Require the
    // fixture color across the image, independently of the user's cursor.
    for (int x = 1; x <= 3; ++x) for (int y = 1; y <= 3; ++y) {
        const auto pixel = image.pixelColor(image.width() * x / 4, image.height() * y / 4);
        if (pixel.saturation() > 100 && pixel.value() > 100) ++colored;
    }
    return colored >= 6;
}

// Captures fixture windows, or the isolated CI compositor under Wayland.
// Competing tones verify the selected audio scope at the receiving client.
class CaptureTests final : public QObject {
    Q_OBJECT
    QProcess producer_, otherProducer_;
    QString otherControl_;
#ifdef Q_OS_LINUX
    QProcess windowManager_;
    void computerScreenReachesAnEncryptedViewer();
#endif
private slots:
    void outputMonitorRejectsMissingDevices() {
        OutputMonitor monitor;
        QVERIFY(!monitor.start({}));
        for (int repeat = 0; repeat < 3; ++repeat) {
            QVERIFY(monitor.start("squadspeak-missing-output-device"));
            QTRY_VERIFY_WITH_TIMEOUT(!monitor.error().isEmpty(), 6000);
            QVERIFY(monitor.take().samples.empty());
            monitor.stop();
            QVERIFY(monitor.take().samples.empty());
        }
    }
    void outputReferenceReducesExternalSpeechEcho() {
        const auto speech = QFINDTESTDATA("audio/speech1.wav");
        QVERIFY(!speech.isEmpty());
        producer_.start(QCoreApplication::applicationFilePath(), {"--producer", "Echo reference fixture", speech});
        QVERIFY(producer_.waitForStarted());
        OutputMonitor monitor;
        QVERIFY(monitor.start(QMediaDevices::defaultAudioOutput().id()));
        std::vector<float> reference;
        int rate = 0;
        size_t received = 0, longest = 0;
        int discontinuities = 0;
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 12000 && (rate == 0 || reference.size() < size_t(rate * 8))) {
            QTest::qWait(10);
            auto block = monitor.take();
            QVERIFY2(monitor.error().isEmpty(), qPrintable(monitor.error()));
            if (block.discontinuity) { ++discontinuities; reference.clear(); }
            if (block.samples.empty()) continue;
            rate = block.sampleRate;
            received += block.samples.size();
            reference.insert(reference.end(), block.samples.begin(), block.samples.end());
            longest = std::max(longest, reference.size());
        }
        monitor.stop();
        QVERIFY2(rate > 0 && reference.size() >= size_t(rate * 8), qPrintable(QStringLiteral(
            "Reference rate %1, received %2, longest contiguous %3, discontinuities %4; producer state %5, exit %6: %7")
            .arg(rate).arg(received).arg(longest).arg(discontinuities).arg(producer_.state()).arg(producer_.exitCode())
            .arg(QString::fromUtf8(producer_.readAllStandardError()))));
        // Actual OS playback is the reference; only the microphone reflection
        // is simulated, with known delay/gain and no physical microphone.
        squad::EchoCanceller echo;
        const auto frames = size_t(rate / 100);
        std::vector<float> microphone(frames);
        double before = 0, after = 0;
        for (size_t offset = 0; offset + frames <= reference.size(); offset += frames) {
            for (size_t i = 0; i < frames; ++i)
                microphone[i] = offset + i >= frames * 8 ? reference[offset + i - frames * 8] * .4f : 0;
            const auto now = qint64(offset / frames) * 10;
            echo.render(std::span(reference).subspan(offset, frames), rate, now, 0);
            const auto cleaned = echo.capture(microphone, rate, now, 0);
            if (now < 3000) continue;
            for (float value : microphone) before += value * value;
            for (float value : cleaned) after += value * value;
        }
        QVERIFY(before > .001);
        qInfo() << "External speech echo residual energy" << after / before;
        QVERIFY(after < before * .1);
    }
    void selectedWindowReachesAnEncryptedViewer() {
#ifdef Q_OS_LINUX
        if (QGuiApplication::platformName().startsWith("wayland")) {
            computerScreenReachesAnEncryptedViewer(); return;
        }
#endif
        QTemporaryDir dir;
        const auto otherTitle = "Unrelated capture fixture " + QUuid::createUuid().toString(QUuid::WithoutBraces);
#ifdef Q_OS_LINUX
        windowManager_.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        windowManager_.setProgram(QStringLiteral("openbox"));
        windowManager_.setArguments({QStringLiteral("--sm-disable"), QStringLiteral("--startup"),
            QStringLiteral("printf squadspeak-wm-ready")});
        windowManager_.start();
        QVERIFY(windowManager_.waitForStarted(5000));
        // Process startup precedes the X11 client-list and event-loop setup.
        QByteArray managerOutput;
        const bool managerReady = QTest::qWaitFor([&] {
            managerOutput += windowManager_.readAllStandardOutput();
            return managerOutput.contains("squadspeak-wm-ready") || windowManager_.state() == QProcess::NotRunning;
        }, 5000);
        QVERIFY2(managerReady && managerOutput.contains("squadspeak-wm-ready"),
            qPrintable(QStringLiteral("Openbox did not finish startup: %1 (state %2)")
                .arg(QString::fromUtf8(managerOutput)).arg(windowManager_.state())));
#endif
        const QString tone = "500";
        const QString otherTone = "1000";
        const auto otherLog = dir.filePath("other.log");
        // Stop the LaunchServices application before its temporary bundle and
        // control file disappear, including on an assertion failure.
        const auto stopOther = qScopeGuard([&] {
            if (!otherControl_.isEmpty()) {
                QFile control(otherControl_ + ".stop");
                if (control.open(QIODevice::WriteOnly)) { control.write("stop"); control.close(); }
                otherProducer_.waitForFinished(3000);
                otherControl_.clear();
            }
            if (QTest::currentTestFailed()) {
                QFile log(otherLog);
                if (log.open(QIODevice::ReadOnly)) qWarning().noquote() << log.read(65536);
            }
        });
        auto otherApplication = QCoreApplication::applicationFilePath();
        otherControl_ = dir.filePath("other-control");
#ifdef Q_OS_MACOS
        // ScreenCaptureKit selects an application, including its other
        // windows/processes. Give the unrelated fixture a distinct bundle ID.
        const auto bundle = dir.filePath("Other.app");
        QVERIFY(QDir().mkpath(bundle + "/Contents/MacOS"));
        otherApplication = bundle + "/Contents/MacOS/other";
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), otherApplication));
        QFile plist(bundle + "/Contents/Info.plist"); QVERIFY(plist.open(QIODevice::WriteOnly));
        plist.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict>"
            "<key>CFBundleIdentifier</key><string>app.squadspeak.unrelated-audio-fixture</string>"
            "<key>CFBundleExecutable</key><string>other</string><key>CFBundlePackageType</key><string>APPL</string>"
            "<key>LSUIElement</key><true/></dict></plist>");
        plist.close();
        QCOMPARE(QProcess::execute("/usr/bin/codesign", {"--force", "--sign", "-", bundle}), 0);
        otherProducer_.start("/usr/bin/open", {"-n", "-W", "--stdout", otherLog, "--stderr", otherLog, bundle, "--args", "--producer",
            otherTitle, otherTone, otherControl_});
#else
        otherProducer_.setProcessChannelMode(QProcess::MergedChannels);
        otherProducer_.setStandardOutputFile(otherLog);
        otherProducer_.start(otherApplication, {"--producer", otherTitle, otherTone, otherControl_});
#endif
        QVERIFY(otherProducer_.waitForStarted());
        const bool otherReady = QTest::qWaitFor([&] {
            return QFile::exists(otherControl_) || otherProducer_.state() == QProcess::NotRunning;
        }, 10000);
        QVERIFY2(otherReady && QFile::exists(otherControl_), qPrintable(QStringLiteral(
            "Unrelated audio producer did not become ready (state %1, exit %2, status %3): %4")
            .arg(otherProducer_.state()).arg(otherProducer_.exitCode()).arg(otherProducer_.exitStatus())
            .arg(otherProducer_.errorString())));
        const auto title = "SquadSpeak capture fixture " + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QByteArray output, diagnostic;
        const auto ready = connect(&producer_, &QProcess::readyReadStandardOutput, this, [&] { output += producer_.readAllStandardOutput(); });
        const auto errors = connect(&producer_, &QProcess::readyReadStandardError, this, [&] { diagnostic += producer_.readAllStandardError(); });
        const auto disconnectOutput = qScopeGuard([&] { disconnect(ready); disconnect(errors); });
        producer_.start(QCoreApplication::applicationFilePath(), {"--producer", title, tone});
        QVERIFY(producer_.waitForStarted());
        const bool readyWindow = QTest::qWaitFor([&] {
            return output.contains("ready") || producer_.state() == QProcess::NotRunning;
        }, 10000);
        QVERIFY2(readyWindow && output.contains("ready"), diagnostic.constData());

        // Unlike app-only sharing, the echo reference must contain both
        // independent processes playing through the selected output device.
        OutputMonitor monitor;
        for (int repeat = 0; repeat < 2; ++repeat) {
            QVERIFY(monitor.start(QMediaDevices::defaultAudioOutput().id()));
            std::vector<float> mixed;
            int rate = 0;
            bool bounded = true;
            const bool received = QTest::qWaitFor([&] {
                auto block = monitor.take();
                if (block.discontinuity) mixed.clear();
                if (!block.samples.empty()) {
                    rate = block.sampleRate;
                    bounded = bounded && block.samples.size() <= size_t(rate / 10);
                    mixed.insert(mixed.end(), block.samples.begin(), block.samples.end());
                }
                return !monitor.error().isEmpty() || (rate && mixed.size() >= size_t(rate / 2));
            }, 10000);
            QVERIFY(bounded);
            QVERIFY2(received && monitor.error().isEmpty() && !mixed.empty(), qPrintable(monitor.error()));
            const auto amplitude = [&](double frequency) {
                double sine = 0, cosine = 0;
                for (size_t i = 0; i < mixed.size(); ++i) {
                    const auto phase = 2 * std::numbers::pi * frequency * double(i) / rate;
                    sine += mixed[i] * std::sin(phase); cosine += mixed[i] * std::cos(phase);
                }
                return 2 * std::hypot(sine, cosine) / mixed.size();
            };
            qInfo() << "Output monitor tones" << amplitude(500) << amplitude(1000) << "rate" << rate;
            QVERIFY(amplitude(500) > 0.0001);
            QVERIFY(amplitude(1000) > 0.0001);
            QElapsedTimer stopping; stopping.start();
            monitor.stop();
            QVERIFY(stopping.elapsed() < 1000);
            QTest::qWait(50);
            QVERIFY(monitor.take().samples.empty());
        }

        VoiceSession ownerProfile(dir.filePath("owner")), guestProfile(dir.filePath("guest"));
        LocalChannel host(ownerProfile, dir.filePath("host"), TlsIdentity::create());
        LocalChannel guest(guestProfile, dir.filePath("guest-channel"), TlsIdentity::create());
        ScreenShare source(host), viewer(guest);
        QVERIFY(!source.setAudioEnabled(true));
#ifdef Q_OS_LINUX
        const auto audioDiagnostic = qScopeGuard([&] {
            if (!QTest::currentTestFailed() || !qEnvironmentVariableIsSet("SQUADSPEAK_CAPTURE_DIAGNOSTICS")) return;
            qWarning() << "Capture producer PIDs" << producer_.processId() << otherProducer_.processId();
            for (const auto* kind : {"clients", "sink-inputs", "sinks", "source-outputs"}) {
                QProcess probe;
                probe.setProcessChannelMode(QProcess::MergedChannels);
                probe.start("pactl", {"list", kind});
                if (!probe.waitForFinished(2000)) { probe.kill(); probe.waitForFinished(); }
                qWarning().noquote() << kind << probe.readAll().first(65536);
            }
        });
#endif
        QVERIFY(source.audioAvailable());
        QVERIFY(host.listen(QHostAddress::LocalHost)); QVERIFY(!ownerProfile.supporterEnabled());
        QVERIFY(host.decide(guest.ownId(), true));
        QVERIFY(guest.openChat(host.ownId(), "127.0.0.1", host.port())); QTRY_VERIFY(guest.chatReady());
        int selected = -1, otherSelected = -1;
        const bool enumerated = QTest::qWaitFor([&] {
            selected = -1; otherSelected = -1;
            for (const auto& value : source.sources()) {
                const auto entry = value.toMap();
                if (entry.value("kind") == "window" && entry.value("name").toString().contains(title))
                    selected = entry.value("index").toInt();
                if (entry.value("kind") == "window" && entry.value("name").toString().contains(otherTitle))
                    otherSelected = entry.value("index").toInt();
            }
            if ((selected >= 0 && otherSelected >= 0) || !source.error().isEmpty()) return true;
            source.refreshSources(); return false;
        }, 60000);
        QByteArray enumerationDetail = QJsonDocument::fromVariant(source.sources()).toJson(QJsonDocument::Compact) + diagnostic;
#ifdef Q_OS_LINUX
        enumerationDetail += " DISPLAY=" + qgetenv("DISPLAY") + " openbox-state="
            + QByteArray::number(windowManager_.state());
#endif
        QVERIFY2(enumerated, enumerationDetail.constData());
        QVERIFY2(source.error().isEmpty(), qPrintable(source.error()));
        QVERIFY2(selected >= 0, "The fixture window was not enumerated");
        QVERIFY(source.start(selected));
        QVERIFY(!source.computerAudio());
        QVERIFY(!source.audioEnabled());
        QTRY_VERIFY(guest.screenInfo(host.ownId()).value("available").toBool());
        QVideoSink sink; QVERIFY(viewer.attach(host.ownId(), &sink));
        // Capture can initially deliver a blank frame before the producer is
        // painted. Require actual content within the same startup deadline.
        QTRY_VERIFY_WITH_TIMEOUT(receivedFixture(sink), 15000);
        QVERIFY(sink.videoSize().width() > 0);
        QVERIFY(otherSelected >= 0);
        for (const auto index : {selected, otherSelected}) {
            if (index != selected) { QVERIFY(source.stop()); QVERIFY(source.start(index)); }
            QSignalSpy packets(&guest, &LocalChannel::audioReceived);
            QTest::qWait(200); QCOMPARE(packets.size(), 0);
            QVERIFY(source.setAudioEnabled(true));
            // Recreating an application's playback stream must preserve its
            // selection, without admitting the other application's output.
            for (int playback = 0; playback < (index == otherSelected ? 2 : 1); ++playback) {
                if (playback) {
                    QFile restart(otherControl_ + ".restart-audio");
                    QVERIFY(restart.open(QIODevice::WriteOnly)); restart.close();
                    QTRY_VERIFY(!QFile::exists(restart.fileName()));
                    QTest::qWait(200); packets.clear();
                }
                const bool receivedAudio = QTest::qWaitFor([&] { return packets.size() >= 25 || !source.error().isEmpty(); }, 10000);
                QVERIFY2(receivedAudio && packets.size() >= 25, qPrintable(QStringLiteral("source=%1 active=%2 audio=%3 error=%4 viewer=%5")
                    .arg(index).arg(source.active()).arg(source.audioEnabled()).arg(source.error())
                    .arg(QString::fromUtf8(QJsonDocument::fromVariant(guest.screenInfo(host.ownId())).toJson(QJsonDocument::Compact)))));
                const auto [wanted, unrelated] = amplitudes(packets, host.musicId(), index == selected ? 500 : 1000, index == selected ? 1000 : 500);
                QVERIFY2(wanted > 10 * unrelated,
                    qPrintable(QStringLiteral("Selected tone %1; unrelated tone %2").arg(wanted).arg(unrelated)));
            }
            QVERIFY(source.setAudioEnabled(false));
            QTest::qWait(100); packets.clear(); QTest::qWait(100); QCOMPARE(packets.size(), 0);
        }
        QVERIFY(source.stop()); QVERIFY(source.start(selected));
        QVERIFY(source.active());
        QVERIFY(ownerProfile.setSupporterEnabled(false));
        QVERIFY(source.active());
        QVERIFY(source.stop());
        QTRY_VERIFY(!source.active());
        QVERIFY(!source.audioEnabled());
        QTRY_VERIFY(!sink.videoFrame().isValid());
        QVERIFY(!ownerProfile.supporterEnabled());
        QVERIFY(source.start(selected));
        QTRY_VERIFY_WITH_TIMEOUT(sink.videoFrame().isValid(), 15000);
        producer_.terminate(); QVERIFY(producer_.waitForFinished(3000));
        QTRY_VERIFY_WITH_TIMEOUT(!source.active(), 5000);
        QVERIFY(!source.audioEnabled());
        QVERIFY(!host.screenSharing());
#ifdef Q_OS_LINUX
        // Computer audio intentionally excludes this test process and
        // its descendants. Use a detached producer for the explicitly labelled
        // computer-audio scope; a QProcess child would be filtered by design.
        const auto computerControl = dir.filePath("computer-control");
        qint64 computerPid = 0;
        QVERIFY(QProcess::startDetached(QCoreApplication::applicationFilePath(),
            {"--producer", "Detached computer audio fixture", "750", computerControl}, {}, &computerPid));
        QVERIFY(computerPid > 1);
        const auto stopComputer = qScopeGuard([&] {
            QFile stop(computerControl + ".stop");
            if (stop.open(QIODevice::WriteOnly)) stop.close();
            const auto deadline = QDeadlineTimer(3000);
            while (deadline.remainingTime() > 0 && QFile::exists(QStringLiteral("/proc/%1").arg(computerPid)))
                QTest::qWait(50);
            if (QFile::exists(QStringLiteral("/proc/%1").arg(computerPid))) ::kill(pid_t(computerPid), SIGTERM);
        });
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(computerControl), 10000);
        // Missing app identity changes the displayed scope, but cannot start
        // computer audio without a new explicit enable action.
        QFile anonymous(otherControl_ + ".anonymous");
        QVERIFY(anonymous.open(QIODevice::WriteOnly)); anonymous.close();
        QTRY_VERIFY(!QFile::exists(anonymous.fileName()));
        QVERIFY(source.refreshSources());
        otherSelected = -1;
        for (const auto& value : source.sources()) {
            const auto entry = value.toMap();
            if (entry.value("kind") == "window" && entry.value("name").toString().contains(otherTitle))
                otherSelected = entry.value("index").toInt();
        }
        QVERIFY(otherSelected >= 0); QVERIFY(source.start(otherSelected));
        QVERIFY(source.computerAudio());
        QVERIFY(!source.audioEnabled());
        QSignalSpy computerPackets(&guest, &LocalChannel::audioReceived);
        QTest::qWait(300); QCOMPARE(computerPackets.size(), 0);
        QVERIFY(source.setAudioEnabled(true));
        QTRY_VERIFY_WITH_TIMEOUT(computerPackets.size() >= 25, 10000);
        const auto [computerTone, ownTreeTone] = amplitudes(computerPackets, host.musicId(), 750, 1000);
        QVERIFY2(computerTone > 10 * ownTreeTone,
            qPrintable(QStringLiteral("Computer tone %1; own process-tree tone %2").arg(computerTone).arg(ownTreeTone)));
        QVERIFY(source.error().isEmpty()); QVERIFY(source.active());
        QVERIFY(source.stop());
        QVERIFY(source.start(otherSelected));
        QVERIFY(!source.audioEnabled());
        QTest::qWait(200); computerPackets.clear();
        QTest::qWait(200); QCOMPARE(computerPackets.size(), 0);
#endif
    }
    void cleanup() {
        for (auto* producer : {&producer_, &otherProducer_}) if (producer->state() != QProcess::NotRunning) {
            producer->terminate();
            if (!producer->waitForFinished(3000)) { producer->kill(); producer->waitForFinished(); }
        }
#ifdef Q_OS_LINUX
        if (windowManager_.state() != QProcess::NotRunning) {
            windowManager_.terminate();
            if (!windowManager_.waitForFinished(3000)) { windowManager_.kill(); windowManager_.waitForFinished(); }
        }
#endif
    }
};

#ifdef Q_OS_LINUX
void CaptureTests::computerScreenReachesAnEncryptedViewer() {
    // Only the isolated CI compositor may be captured in this mode.
    QCOMPARE(qEnvironmentVariable("SQUADSPEAK_TEST_WAYLAND"), QStringLiteral("isolated"));
    QCOMPARE(QGuiApplication::screens().size(), 1);
    QTemporaryDir dir;
    const auto controlPath = dir.filePath("external-control");
    qint64 externalPid = 0;
    QVERIFY(QProcess::startDetached(QCoreApplication::applicationFilePath(),
        {"--producer", "External Wayland fixture", "750", controlPath}, {}, &externalPid));
    QVERIFY(externalPid > 1);
    const auto stopExternal = qScopeGuard([&] {
        QFile stop(controlPath + ".stop");
        if (stop.open(QIODevice::WriteOnly)) stop.close();
        QDeadlineTimer deadline(3000);
        const auto process = QStringLiteral("/proc/%1").arg(externalPid);
        while (deadline.remainingTime() > 0 && QFile::exists(process)) QTest::qWait(50);
        if (QFile::exists(process)) ::kill(pid_t(externalPid), SIGTERM);
    });
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(controlPath), 10000);
    const auto ownControl = dir.filePath("own-control");
    producer_.start(QCoreApplication::applicationFilePath(),
        {"--producer", "Own Wayland fixture", "1000", ownControl});
    QVERIFY(producer_.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(ownControl), 10000);

    VoiceSession ownerProfile(dir.filePath("owner")), guestProfile(dir.filePath("guest"));
    LocalChannel host(ownerProfile, dir.filePath("host"), TlsIdentity::create());
    LocalChannel guest(guestProfile, dir.filePath("guest-channel"), TlsIdentity::create());
    ScreenShare source(host), viewer(guest);
    QVERIFY(host.listen(QHostAddress::LocalHost));
    QVERIFY(!ownerProfile.supporterEnabled());
    QVERIFY(host.decide(guest.ownId(), true));
    QVERIFY(guest.openChat(host.ownId(), "127.0.0.1", host.port())); QTRY_VERIFY(guest.chatReady());
    QVERIFY(source.refreshSources());
    int selected = -1;
    for (const auto& entry : source.sources()) if (entry.toMap().value("kind") == "screen") {
        selected = entry.toMap().value("index").toInt(); break;
    }
    QVERIFY(selected >= 0);
    // Cancelling before the portal responds must not leave a live request or
    // route its delayed response into the next capture's handshake.
    QVERIFY(source.start(selected));
    QVERIFY(source.stop());
    QVERIFY(!source.active());
    QVERIFY(source.start(selected));
    QVERIFY(source.computerAudio());
    QVERIFY(!source.audioEnabled());
    QTRY_VERIFY(guest.screenInfo(host.ownId()).value("available").toBool() || !source.error().isEmpty());
    QVERIFY2(source.error().isEmpty(), qPrintable(source.error()));
    QVERIFY(guest.screenInfo(host.ownId()).value("available").toBool());
    QVideoSink sink; QVERIFY(viewer.attach(host.ownId(), &sink));
    QTRY_VERIFY_WITH_TIMEOUT(receivedFixture(sink), 15000);
    QSignalSpy packets(&guest, &LocalChannel::audioReceived);
    QTest::qWait(300); QCOMPARE(packets.size(), 0);
    QVERIFY(source.setAudioEnabled(true));
    QTRY_VERIFY_WITH_TIMEOUT(packets.size() >= 25 || !source.error().isEmpty(), 10000);
    QVERIFY2(source.error().isEmpty(), qPrintable(source.error()));
    QVERIFY(packets.size() >= 25);
    const auto [external, own] = amplitudes(packets, host.musicId(), 750, 1000);
    QVERIFY2(external > 10 * own, qPrintable(QStringLiteral("External tone %1; own tree tone %2").arg(external).arg(own)));
    QVERIFY(source.setAudioEnabled(false));
    QTest::qWait(200); packets.clear(); QTest::qWait(200); QCOMPARE(packets.size(), 0);
    for (int repeat = 0; repeat < 3; ++repeat) {
        QVERIFY(source.stop());
        QTRY_VERIFY(!sink.videoFrame().isValid());
        QVERIFY(source.start(selected));
        QVERIFY(!source.audioEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(sink.videoFrame().isValid() || !source.error().isEmpty(), 15000);
        QVERIFY2(source.error().isEmpty(), qPrintable(source.error()));
        QVERIFY(sink.videoFrame().isValid());
    }
    QVERIFY(ownerProfile.setSupporterEnabled(false));
    QVERIFY(source.active());
    QVERIFY(source.stop());
    QTRY_VERIFY(!source.active());
    QTRY_VERIFY(!sink.videoFrame().isValid());
}
#endif

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().value(1) == "--producer") {
        QWidget window; window.setWindowTitle(app.arguments().value(2)); window.resize(480, 320);
        auto palette = window.palette(); palette.setColor(QPalette::Window, QColor(190, 50, 50)); window.setPalette(palette);
        window.setAutoFillBackground(true); window.show();
        QTimer animation;
        QObject::connect(&animation, &QTimer::timeout, &window, [&] {
            auto palette = window.palette();
            palette.setColor(QPalette::Window, QColor::fromHsv(int(QDateTime::currentMSecsSinceEpoch() / 20 % 360), 180, 190));
            window.setPalette(palette);
        });
        animation.start(33);
        QTimer control;
        const auto controlPath = app.arguments().value(4);
        std::unique_ptr<QAudioSink> output;
        QIODevice* audioSink = nullptr;
        QTimer audioTick;
        if (!controlPath.isEmpty()) {
            QObject::connect(&control, &QTimer::timeout, &app, [&] {
                if (QFile::exists(controlPath + ".stop")) app.quit();
                if (output && QFile::exists(controlPath + ".restart-audio")) {
                    audioTick.stop(); output->reset(); audioSink = output->start();
                    if (!audioSink) { app.exit(3); return; }
                    audioTick.start(); QFile::remove(controlPath + ".restart-audio");
                }
#ifdef Q_OS_LINUX
                if (QFile::exists(controlPath + ".anonymous")) {
                    if (auto* display = XOpenDisplay(nullptr)) {
                        XDeleteProperty(display, window.winId(), XInternAtom(display, "_NET_WM_PID", False));
                        XSync(display, False); XCloseDisplay(display);
                        QFile::remove(controlPath + ".anonymous");
                    }
                }
#endif
            });
            control.start(100);
            QTimer::singleShot(120000, &app, &QCoreApplication::quit);
        }
        quint64 sample = 0;
        const auto frequency = app.arguments().value(3).toDouble();
        QByteArray speech;
        if (frequency == 0 && app.arguments().value(3).endsWith(".wav")) {
            QFile file(app.arguments().value(3));
            if (!file.open(QIODevice::ReadOnly)) return 5;
            speech = file.readAll();
            if (speech.size() != 44 + 8 * 48000 * 2 || speech.mid(36, 4) != "data"
                || qFromLittleEndian<quint32>(speech.constData() + 24) != 48000) return 5;
        }
        if (frequency > 0 || !speech.isEmpty()) {
            QAudioFormat format; format.setSampleRate(48000); format.setChannelCount(1);
            format.setSampleFormat(QAudioFormat::Float);
            const auto device = QMediaDevices::defaultAudioOutput();
            if (device.isNull() || !device.isFormatSupported(format)) {
                qCritical() << "Capture fixture needs a 48 kHz floating-point audio output; available outputs"
                            << QMediaDevices::audioOutputs().size() << "default" << device.description()
                            << "preferred format" << device.preferredFormat();
                return 2;
            }
            output = std::make_unique<QAudioSink>(device, format);
            output->setBufferSize(4800 * sizeof(float));
            audioSink = output->start();
            if (!audioSink) { qCritical() << "Capture fixture audio output could not start"; return 3; }
            QObject::connect(&audioTick, &QTimer::timeout, &app, [&] {
                const auto count = std::min<qsizetype>(output->bytesFree() / sizeof(float), 960);
                std::array<float, 960> samples{};
                for (qsizetype i = 0; i < count; ++i) {
                    samples[size_t(i)] = speech.isEmpty()
                        ? float((frequency == 500 ? 0.015 : 0.06) * std::sin(2 * std::numbers::pi * frequency * double(sample) / 48000))
                        : qFromLittleEndian<qint16>(speech.constData() + 44 + (sample % (8 * 48000)) * 2) / 32768.0f * .2f;
                    ++sample;
                }
                const auto bytes = count * sizeof(float);
                if (audioSink->write(reinterpret_cast<const char*>(samples.data()), bytes) != qint64(bytes)) app.exit(4);
            });
            audioTick.setTimerType(Qt::PreciseTimer); audioTick.start(10);
        }
        window.winId();
        QTimer::singleShot(0, &window, [&] {
            if (!controlPath.isEmpty()) {
                QFile ready(controlPath);
                if (ready.open(QIODevice::WriteOnly)) ready.write("ready");
            }
            qInfo() << "Capture fixture platform" << QGuiApplication::platformName() << "DISPLAY" << qgetenv("DISPLAY");
            QTextStream(stdout) << "ready\n" << Qt::flush;
        });
        return app.exec();
    }
    CaptureTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "capture_tests.moc"
