#include "ptt_key.hpp"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QTest>
#include <memory>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

class LinuxKeyTests final : public QObject {
    Q_OBJECT
private slots:
    void anotherApplicationKeepsReceivingGlobalChords() {
        std::unique_ptr<Display, decltype(&XCloseDisplay)> display(XOpenDisplay(nullptr), XCloseDisplay);
        QVERIFY(display);
        const auto window = XCreateSimpleWindow(display.get(), DefaultRootWindow(display.get()), 0, 0, 100, 100, 0, 0, 0);
        XSelectInput(display.get(), window, KeyPressMask | KeyReleaseMask);
        XMapWindow(display.get(), window);
        XSetInputFocus(display.get(), window, RevertToParent, CurrentTime);
        XSync(display.get(), false);
        const auto key = [&](KeySym symbol, bool down) {
            XTestFakeKeyEvent(display.get(), XKeysymToKeycode(display.get(), symbol), down, CurrentTime);
            XFlush(display.get());
        };

        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        QVERIFY(session.setPttShortcut({XK_Control_L, XK_F8}, {"Ctrl", "F8"}));
        QVERIFY(session.setPttShortcut({XK_F9}, {"F9"}, true));
        QVERIFY(session.setPushToTalk(true));
        QVERIFY(session.setMuted(false));
        PushToTalkKey input(session, true);
        QTRY_VERIFY_WITH_TIMEOUT(input.globalAvailable(), 1000);

        key(XK_Control_L, true);
        QTest::qWait(20);
        QVERIFY(!session.transmissionAllowed());
        key(XK_F8, true);
        QTRY_VERIFY(session.transmissionAllowed());
        const auto revision = session.pttInputRevision();
        key(XK_F8, true);
        QTest::qWait(20);
        QCOMPARE(session.pttInputRevision(), revision);
        key(XK_Control_L, false);
        QTRY_VERIFY(!session.transmissionAllowed());
        key(XK_F8, false);
        key(XK_F9, true);
        QTRY_VERIFY(session.remotePttKeyHeld());
        QVERIFY(!session.transmissionAllowed());
        key(XK_F9, false);
        QTRY_VERIFY(!session.remotePttKeyHeld());

        int presses = 0, releases = 0;
        XSync(display.get(), false);
        while (XPending(display.get())) {
            XEvent event;
            XNextEvent(display.get(), &event);
            if (event.type == KeyPress) ++presses;
            if (event.type == KeyRelease) ++releases;
        }
        QVERIFY(presses >= 3);
        QVERIFY(releases >= 3);
        QVERIFY(input.clear());
        QVERIFY(input.clear(true));
        QVERIFY(session.setPushToTalk(false));
        QVERIFY(!input.globalAvailable());
        QVERIFY(session.setPttShortcut({XK_F10}, {"F10"}));
        QTRY_VERIFY(input.globalAvailable());
        key(XK_F8, true);
        key(XK_F8, false);
        QTest::qWait(20);
        QVERIFY(!session.pttInputHeld());
        key(XK_F10, true);
        QTRY_VERIFY(session.pttInputHeld());
        key(XK_F10, false);
        QTRY_VERIFY(!session.pttInputHeld());
    }

    void bindingCanBeRemovedWhileTheGlobalKeyIsPressed() {
        std::unique_ptr<Display, decltype(&XCloseDisplay)> display(XOpenDisplay(nullptr), XCloseDisplay);
        QVERIFY(display);
        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        QVERIFY(session.setPttShortcut({XK_F8}, {"F8"}));
        PushToTalkKey input(session, true);
        QTRY_VERIFY(input.globalAvailable());
        bool removed = false;
        connect(&session, &VoiceSession::presenceChanged, &input, [&] {
            if (!session.pttInputHeld() || removed) return;
            removed = true;
            QVERIFY(input.clear());
        });
        XTestFakeKeyEvent(display.get(), XKeysymToKeycode(display.get(), XK_F8), true, CurrentTime);
        XFlush(display.get());
        QTRY_VERIFY(removed);
        QVERIFY(!input.globalAvailable());
        QVERIFY(!session.pttInputHeld());
        XTestFakeKeyEvent(display.get(), XKeysymToKeycode(display.get(), XK_F8), false, CurrentTime);
        XFlush(display.get());
        QTest::qWait(20);
    }

    void captureAcceptsReleasesAndRestartRetainsTheLastState() {
        std::unique_ptr<Display, decltype(&XCloseDisplay)> display(XOpenDisplay(nullptr), XCloseDisplay);
        QVERIFY(display);
        const auto key = [&](bool down) {
            XTestFakeKeyEvent(display.get(), XKeysymToKeycode(display.get(), XK_F8), down, CurrentTime);
            XFlush(display.get());
        };
        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        QVERIFY(session.setPttShortcut({XK_F8}, {"F8"}));
        auto input = std::make_unique<PushToTalkKey>(session, true);
        QTRY_VERIFY(input->globalAvailable());
        key(true);
        QTRY_VERIFY(session.pttInputHeld());
        QVERIFY(input->capture(true));
        key(false);
        QTRY_VERIFY(!session.pttInputHeld());
        key(true);
        QTest::qWait(20);
        QVERIFY(!session.pttInputHeld());
        key(false);
        QTest::qWait(20);
        QVERIFY(input->capture(true));
        key(true);
        QTRY_VERIFY(session.pttInputHeld());
        input.reset();
        QVERIFY(session.pttInputHeld());
        input = std::make_unique<PushToTalkKey>(session, true);
        QTRY_VERIFY(input->globalAvailable());
        QVERIFY(session.pttInputHeld());
        key(false);
        QTRY_VERIFY(!session.pttInputHeld());
    }

    void clearingAChordKeepsTheSharedModifierAvailableToTheOtherChord() {
        std::unique_ptr<Display, decltype(&XCloseDisplay)> display(XOpenDisplay(nullptr), XCloseDisplay);
        QVERIFY(display);
        const auto key = [&](KeySym symbol, bool down) {
            XTestFakeKeyEvent(display.get(), XKeysymToKeycode(display.get(), symbol), down, CurrentTime);
            XFlush(display.get());
        };
        VoiceSession session;
        QVERIFY(session.setPttShortcut({XK_Control_L, XK_F8}, {"Ctrl", "F8"}));
        QVERIFY(session.setPttShortcut({XK_Control_L, XK_F9}, {"Ctrl", "F9"}, true));
        PushToTalkKey input(session, true);
        QTRY_VERIFY(input.globalAvailable());
        key(XK_Control_L, true);
        key(XK_F9, true);
        QTRY_VERIFY(session.remotePttKeyHeld());
        QVERIFY(input.clear(false));
        key(XK_F9, false);
        QTRY_VERIFY(!session.remotePttKeyHeld());
        key(XK_F9, true);
        QTRY_VERIFY(session.remotePttKeyHeld());
        key(XK_Control_L, false);
        QTRY_VERIFY(!session.remotePttKeyHeld());
        key(XK_F9, false);
        QTest::qWait(20);
    }

    void xwaylandDoesNotClaimDesktopWideAccess() {
        qputenv("WAYLAND_DISPLAY", "test-wayland");
        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        QVERIFY(session.setPttShortcut({XK_F8}, {"F8"}));
        PushToTalkKey input(session, true);
        QCoreApplication::processEvents();
        QVERIFY(!input.globalAvailable());
        QVERIFY(input.status().contains("only in the app window"));
        qunsetenv("WAYLAND_DISPLAY");
    }
};

QTEST_MAIN(LinuxKeyTests)
#include "linux_ptt_tests.moc"
