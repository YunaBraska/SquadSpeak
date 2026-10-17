#include "ptt_key.hpp"
#include <QSignalSpy>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QScopeGuard>
#include <algorithm>
#include <memory>

namespace {
bool key(QObject& target, QEvent::Type type, int qtKey, quint32 nativeKey,
         Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool repeat = false) {
    QKeyEvent event(type, qtKey, modifiers, 0, nativeKey, 0, {}, repeat);
    return QCoreApplication::sendEvent(&target, &event);
}
}

class KeyTests final : public QObject {
    Q_OBJECT
private slots:
    void modifierNamesMatchTheKeyboard_data() {
        QTest::addColumn<int>("qtKey");
        QTest::addColumn<QString>("name");
#ifdef Q_OS_MACOS
        QTest::newRow("command") << int(Qt::Key_Control) << QString("Cmd");
        QTest::newRow("control") << int(Qt::Key_Meta) << QString("Ctrl");
        QTest::newRow("option") << int(Qt::Key_Alt) << QString("Option");
#else
        QTest::newRow("control") << int(Qt::Key_Control) << QString("Ctrl");
        QTest::newRow("meta") << int(Qt::Key_Meta) << QString("Meta");
        QTest::newRow("alt") << int(Qt::Key_Alt) << QString("Alt");
#endif
        QTest::newRow("shift") << int(Qt::Key_Shift) << QString("Shift");
    }
    void modifierNamesMatchTheKeyboard() {
        QFETCH(int, qtKey);
        QFETCH(QString, name);
        VoiceSession session;
        PushToTalkKey input(session, false);
        QObject window;
        QVERIFY(input.capture());
        QVERIFY(key(window, QEvent::KeyPress, qtKey, 59));
        QVERIFY(key(window, QEvent::KeyRelease, qtKey, 59));
        QCOMPARE(session.pttKeyName(), name);
    }
    void oneShortcutFollowsLocalOrRemoteMode() {
        VoiceSession session;
        PushToTalkKey input(session, false);
        QObject window;
        QVERIFY(session.setPttShortcut({59, 0}, {"Ctrl", "A"}));
        QVERIFY(session.setPushToTalk(true));
        QVERIFY(session.setMuted(false));
        for (const bool local : {true, false, true}) {
            QVERIFY(session.setPttLocal(local));
            (void)key(window, QEvent::KeyPress, Qt::Key_Control, 59);
            QVERIFY(!session.pttInputHeld());
            (void)key(window, QEvent::KeyPress, Qt::Key_A, 0);
            QVERIFY(session.pttInputHeld());
            QCOMPARE(session.transmissionAllowed(), local);
            QVERIFY(session.setPttButtonHeld(true));
            (void)key(window, QEvent::KeyRelease, Qt::Key_Control, 59);
            QVERIFY(session.pttInputHeld());
            QVERIFY(session.setPttButtonHeld(false));
            QVERIFY(!session.pttInputHeld());
            QVERIFY(!session.transmissionAllowed());
            (void)key(window, QEvent::KeyRelease, Qt::Key_A, 0);
        }
    }
    void enablingPttRequestsGlobalInputWithoutASeparateAction() {
        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        PushToTalkKey input(session, false);
        QSignalSpy changes(&input, &PushToTalkKey::changed);
        QCoreApplication::processEvents();
        QCOMPARE(changes.count(), 0);
        QVERIFY(session.setPushToTalk(true));
        QCOMPARE(changes.count(), 1);
        QVERIFY(input.status().contains("disabled in this test run"));
        QVERIFY(session.setUserName("Another name"));
        QCOMPARE(changes.count(), 1);
        QVERIFY(session.setPttShortcut({59, 0}, {"Ctrl", "A"}));
        QCOMPARE(changes.count(), 2);
        QVERIFY(session.setPttShortcut({59, 11}, {"Ctrl", "B"}));
        QCOMPARE(changes.count(), 3);
    }
    void restoredPttWithoutAKeyStillChecksPermissionAfterStartup() {
        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        QVERIFY(session.setPushToTalk(true));
        PushToTalkKey input(session, false);
        QSignalSpy changes(&input, &PushToTalkKey::changed);
        QTRY_COMPARE(changes.count(), 1);
        QVERIFY(input.status().contains("disabled in this test run"));
    }
    void captureCancelRepeatAndReleaseUseRealApplicationEvents() {
        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        PushToTalkKey input(session, false);
        QObject window;
        QVERIFY(input.capture());
        QVERIFY(key(window, QEvent::KeyPress, Qt::Key_Escape, 53));
        QVERIFY(!input.capturing()); QCOMPARE(session.pttKeyCode(), -1);
        QVERIFY(input.capture());
        QVERIFY(key(window, QEvent::KeyPress, Qt::Key_A, 0));
        QVERIFY(key(window, QEvent::KeyRelease, Qt::Key_A, 0));
        QCOMPARE(session.pttKeyCode(), 0); QVERIFY(!session.pttInputHeld());
        QVERIFY(!input.globalAvailable());
        QVERIFY(session.setPushToTalk(true)); QVERIFY(session.setMuted(false));
        const auto revision = session.pttInputRevision();
        (void)key(window, QEvent::KeyPress, Qt::Key_A, 0);
        QVERIFY(session.transmissionAllowed());
        QCOMPARE(session.pttInputRevision(), revision + 1);
        (void)key(window, QEvent::KeyPress, Qt::Key_A, 0, Qt::NoModifier, true);
        (void)key(window, QEvent::KeyRelease, Qt::Key_A, 0, Qt::NoModifier, true);
        QCOMPARE(session.pttInputRevision(), revision + 1); QVERIFY(session.pttInputHeld());
        QVERIFY(input.capture());
        QVERIFY(key(window, QEvent::KeyPress, Qt::Key_Escape, 53));
        QVERIFY(!session.pttInputHeld()); QVERIFY(!session.transmissionAllowed());
        QVERIFY(session.pttInputRevision() >= revision + 1);
        QVERIFY(!input.globalAvailable());
    }
    void restoringAndLosingTheObserverNeverInventsARelease() {
        QTemporaryDir folder;
        const auto path = folder.filePath("session.json");
        {
            VoiceSession session(path);
            QVERIFY(session.setPttShortcut(100, "F8"));
            QVERIFY(session.setPttKeyHeld(true));
            QVERIFY(session.setPushToTalk(true)); QVERIFY(session.setMuted(false));
            { PushToTalkKey observer(session, false); QVERIFY(session.transmissionAllowed()); }
            QVERIFY(session.transmissionAllowed());
        }
        VoiceSession restored(path);
        PushToTalkKey observer(restored, false);
        QVERIFY(restored.transmissionAllowed());
        QObject window;
        (void)key(window, QEvent::KeyPress, Qt::Key_B, 11);
        (void)key(window, QEvent::KeyRelease, Qt::Key_B, 11);
        QVERIFY(restored.transmissionAllowed());
        const auto revision = restored.pttInputRevision();
        QVERIFY(restored.releasePttInput());
        QVERIFY(!restored.transmissionAllowed()); QCOMPARE(restored.pttInputRevision(), revision + 1);
        QVERIFY(observer.capture());
        QVERIFY(key(window, QEvent::KeyPress, Qt::Key_MediaPlay, 0));
        QCOMPARE(restored.pttKeyCode(), 100);
    }

    void combinationsCommitOnFinalReleaseAndRequireEveryKey() {
        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        PushToTalkKey input(session, false);
        QObject window;
        QVERIFY(input.capture());
        (void)key(window, QEvent::KeyPress, Qt::Key_Control, 59, Qt::ControlModifier);
        (void)key(window, QEvent::KeyPress, Qt::Key_A, 0, Qt::ControlModifier);
        QVERIFY(key(window, QEvent::KeyRelease, Qt::Key_A, 0, Qt::ControlModifier));
        QVERIFY(key(window, QEvent::KeyRelease, Qt::Key_Control, 59, Qt::NoModifier));
        QCOMPARE(session.pttKeyCodes(), QList<int>({59, 0}));
        QVERIFY(!input.capturing());
        QVERIFY(session.setMuted(false)); QVERIFY(session.setPushToTalk(true));
        (void)key(window, QEvent::KeyPress, Qt::Key_Control, 59, Qt::ControlModifier);
        QVERIFY(!session.transmissionAllowed());
        (void)key(window, QEvent::KeyPress, Qt::Key_A, 0, Qt::ControlModifier);
        QVERIFY(session.transmissionAllowed());
        (void)key(window, QEvent::KeyRelease, Qt::Key_Control, 59, Qt::ControlModifier);
        QVERIFY(!session.transmissionAllowed());
        (void)key(window, QEvent::KeyRelease, Qt::Key_A, 0, Qt::NoModifier);
    }

    void explicitClearRecoversAStuckKeyWithoutClearingAnotherControllersHold() {
        VoiceSession session;
        PushToTalkKey input(session, false);
        QVERIFY(session.setPttShortcut(10, "A"));
        QVERIFY(session.setPushToTalk(true));
        QVERIFY(session.setMuted(false));
        QVERIFY(session.setPttKeyHeld(true));
        QVERIFY(session.setRemotePttHeld(true));
        QVERIFY(input.clear());
        QCOMPARE(session.pttKeyCode(), -1);
        QVERIFY(!session.pttInputHeld());
        QVERIFY(session.transmissionAllowed());
        QVERIFY(session.setRemotePttHeld(false));
        QVERIFY(!session.transmissionAllowed());
    }

    void anUnboundKeyCannotRemainHeldAfterBindingItAgain() {
        VoiceSession session;
        PushToTalkKey input(session, false);
        QObject window;
        QVERIFY(session.setPttShortcut({59, 0}, {"Ctrl", "A"}));
        (void)key(window, QEvent::KeyPress, Qt::Key_Control, 59);
        QVERIFY(!session.pttKeyHeld());
        QVERIFY(session.setPttShortcut({100}, {"F8"}));
        (void)key(window, QEvent::KeyRelease, Qt::Key_Control, 59);
        QVERIFY(session.setPttShortcut({59, 11}, {"Ctrl", "B"}));
        (void)key(window, QEvent::KeyPress, Qt::Key_B, 11);
        QVERIFY(!session.pttKeyHeld());
        (void)key(window, QEvent::KeyPress, Qt::Key_Control, 59);
        QVERIFY(session.pttKeyHeld());
        (void)key(window, QEvent::KeyRelease, Qt::Key_Control, 59);
        QVERIFY(!session.pttKeyHeld());
        (void)key(window, QEvent::KeyRelease, Qt::Key_B, 11);
    }

    void clearingDuringCaptureCancelsAndCannotCommitOnRelease() {
        VoiceSession session;
        PushToTalkKey input(session, false);
        QObject window;
        QVERIFY(session.setPttShortcut({11}, {"B"}));
        QVERIFY(input.capture());
        (void)key(window, QEvent::KeyPress, Qt::Key_A, 0);
        QVERIFY(input.clear());
        QVERIFY(!input.capturing());
        (void)key(window, QEvent::KeyRelease, Qt::Key_A, 0);
        QVERIFY(session.pttKeyCodes().isEmpty());
        QVERIFY(!session.pttInputHeld());
    }

#ifdef Q_OS_WIN
    void windowsBackgroundKeysUpdateTheSharedChord() {
        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        QVERIFY(session.setPttShortcut({VK_F23, VK_F24}, {"F23", "F24"}));
        QVERIFY(session.setPushToTalk(true));
        PushToTalkKey input(session, true);
        QTRY_VERIFY(input.globalAvailable());
        const auto send = [](WORD code, bool down) {
            INPUT event{};
            event.type = INPUT_KEYBOARD;
            event.ki.wVk = code;
            event.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
            return SendInput(1, &event, sizeof(event)) == 1;
        };
        const auto cleanup = qScopeGuard([&] {
            send(VK_F23, false);
            send(VK_F24, false);
        });
        QVERIFY(send(VK_F23, true));
        QVERIFY(send(VK_F24, true));
        QTRY_VERIFY(session.pttKeyHeld());
        const auto revision = session.pttInputRevision();
        QVERIFY(send(VK_F24, true));
        QTest::qWait(50);
        QCOMPARE(session.pttInputRevision(), revision);
        QVERIFY(send(VK_F23, false));
        QTRY_VERIFY(!session.pttKeyHeld());
        QVERIFY(send(VK_F24, false));
        QTRY_VERIFY(!session.pttKeyHeld());
    }

    void windowsRawInputRegistrationIsOwnedAndReleasedWithTheObserver() {
        QTemporaryDir firstFolder;
        VoiceSession firstSession(firstFolder.filePath("session.json"));
        QVERIFY(firstSession.setPttShortcut(65, "A"));
        QVERIFY(firstSession.setPushToTalk(true));
        auto first = std::make_unique<PushToTalkKey>(firstSession, true);
        QTRY_VERIFY(first->globalAvailable());

        {
            QTemporaryDir secondFolder;
            VoiceSession secondSession(secondFolder.filePath("session.json"));
            QVERIFY(secondSession.setPttShortcut(66, "B"));
            QVERIFY(secondSession.setPushToTalk(true));
            PushToTalkKey second(secondSession, true);
            QTRY_VERIFY(second.status().contains("only in the app window"));
            QVERIFY(!second.globalAvailable());
        }

        auto takeover = CreateWindowExW(0, L"STATIC", L"SquadSpeak raw input takeover", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        QVERIFY(takeover != nullptr);
        const auto releaseTakeover = [&] {
            if (!takeover) return true;
            RAWINPUTDEVICE remove{};
            remove.usUsagePage = 0x01;
            remove.usUsage = 0x06;
            remove.dwFlags = RIDEV_REMOVE;
            const bool removed = RegisterRawInputDevices(&remove, 1, sizeof(remove)) != FALSE;
            const bool destroyed = DestroyWindow(takeover) != FALSE;
            takeover = nullptr;
            return removed && destroyed;
        };
        const auto cleanup = qScopeGuard([&] { releaseTakeover(); });
        RAWINPUTDEVICE replacement{};
        replacement.usUsagePage = 0x01;
        replacement.usUsage = 0x06;
        replacement.dwFlags = RIDEV_INPUTSINK;
        replacement.hwndTarget = takeover;
        QVERIFY(RegisterRawInputDevices(&replacement, 1, sizeof(replacement)) != FALSE);
        QVERIFY(!first->globalAvailable());
        QVERIFY(firstSession.setPttKeyHeld(true));
        first.reset();
        QVERIFY(firstSession.pttKeyHeld());

        UINT count = 0;
        QVERIFY(GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) != UINT(-1));
        QVector<RAWINPUTDEVICE> devices(static_cast<qsizetype>(count));
        QVERIFY(GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) != UINT(-1));
        QVERIFY(std::any_of(devices.cbegin(), devices.cend(), [takeover](const RAWINPUTDEVICE& device) {
            return device.usUsagePage == 0x01 && device.usUsage == 0x06 && device.hwndTarget == takeover;
        }));
        QVERIFY(releaseTakeover());

        QTemporaryDir thirdFolder;
        VoiceSession thirdSession(thirdFolder.filePath("session.json"));
        QVERIFY(thirdSession.setPttShortcut(66, "B"));
        QVERIFY(thirdSession.setPushToTalk(true));
        PushToTalkKey third(thirdSession, true);
        QTRY_VERIFY(third.globalAvailable());
    }
#endif
};

QTEST_GUILESS_MAIN(KeyTests)
#include "ptt_key_tests.moc"
