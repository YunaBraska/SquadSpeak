#include "ptt_key.hpp"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QGuiApplication>
#include <QTimer>
#include <QVector>
#include <algorithm>
#include <climits>

namespace {
QString plainKeyName(int qtKey, quint32 nativeKey) {
    switch (qtKey) {
    case Qt::Key_Control: return QStringLiteral("Ctrl");
    case Qt::Key_Shift: return QStringLiteral("Shift");
    case Qt::Key_Alt: return QStringLiteral("Alt");
    case Qt::Key_Meta: return QStringLiteral("Meta");
    default: {
        const auto name = QKeySequence(qtKey).toString(QKeySequence::NativeText);
        return name.isEmpty() ? QStringLiteral("Key %1").arg(nativeKey) : name;
    }
    }
}
}
#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <QSocketNotifier>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut
#undef None
#endif

#ifdef Q_OS_WIN
namespace {
int canonicalWindowsKey(USHORT key) {
    switch (key) {
    case VK_LSHIFT: case VK_RSHIFT: return VK_SHIFT;
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU: case VK_RMENU: return VK_MENU;
    default: return int(key);
    }
}
}
#endif

PushToTalkKey::PushToTalkKey(VoiceSession& session, bool observeGlobal, QObject* parent)
    : QObject(parent), session_(session), observeGlobal_(observeGlobal) {
    QCoreApplication::instance()->installEventFilter(this);
    connect(&session_, &VoiceSession::preferencesChanged, this, &PushToTalkKey::refreshBindings);
    // Wait until the application and its window have finished initialization.
    QTimer::singleShot(0, this, &PushToTalkKey::refreshBindings);
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (observeGlobal_ && state == Qt::ApplicationActive && !globalAvailable()
                && (observedPtt_ || !observedKeys_.isEmpty() || !observedRemoteKeys_.isEmpty()))
                observe(false);
        });
}

PushToTalkKey::~PushToTalkKey() { stopObserving(); }

void PushToTalkKey::refreshBindings() {
    const auto local = session_.pttKeyCodes(false);
    const auto remote = session_.pttKeyCodes(true);
    const bool ptt = session_.pushToTalk();
    const bool request = (ptt && !observedPtt_)
        || (!local.isEmpty() && local != observedKeys_)
        || (!remote.isEmpty() && remote != observedRemoteKeys_);
    heldKeys_.removeIf([&](int code) { return !local.contains(code) && !remote.contains(code); });
    observedKeys_ = local; observedRemoteKeys_ = remote; observedPtt_ = ptt;
    if (request) observe(true);
    else if (!ptt && local.isEmpty() && remote.isEmpty() && tap_) {
        stopObserving();
        setStatus(tr("No push-to-talk key selected yet."));
    }
}

void PushToTalkKey::stopObserving() {
#ifdef Q_OS_MACOS
    if (source_) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), static_cast<CFRunLoopSourceRef>(source_), kCFRunLoopCommonModes);
        CFRelease(source_);
    }
    if (tap_) { CFMachPortInvalidate(static_cast<CFMachPortRef>(tap_)); CFRelease(tap_); }
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    delete static_cast<QSocketNotifier*>(source_);
    if (tap_) XCloseDisplay(static_cast<Display*>(tap_));
    nativeHeldKeys_.clear();
#elif defined(Q_OS_WIN)
    nativeHeldKeys_.clear();
    if (tap_ && rawKeyboardRegistrationAvailable()) {
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01;
        device.usUsage = 0x06;
        device.dwFlags = RIDEV_REMOVE;
        RegisterRawInputDevices(&device, 1, sizeof(device));
    }
    if (tap_) {
        DestroyWindow(static_cast<HWND>(tap_));
        tap_ = nullptr;
    }
    if (!nativeClassName_.isEmpty()) {
        UnregisterClassW(reinterpret_cast<LPCWSTR>(nativeClassName_.utf16()), GetModuleHandleW(nullptr));
        nativeClassName_.clear();
    }
#endif
    source_ = nullptr; tap_ = nullptr;
}

bool PushToTalkKey::setStatus(QString text, bool result) {
    status_ = std::move(text); emit changed(); return result;
}

bool PushToTalkKey::capture(bool remote) {
    if (capturing_ && captureRemote_ == remote) {
        capturing_ = false; captureStarted_ = false; captureKeys_.clear(); captureSequence_.clear(); captureNames_.clear();
        return setStatus(tr("Key selection canceled."));
    }
    discardObservedKeys(remote);
    if (!session_.releasePttKey(remote)) return setStatus(session_.error(), false);
    captureKeys_.clear(); captureSequence_.clear(); captureNames_.clear(); captureStarted_ = false;
    capturing_ = true;
    captureRemote_ = remote;
    return setStatus(tr("Press and release a key or combination. Escape cancels."));
}

bool PushToTalkKey::globalAvailable() const {
#ifdef Q_OS_MACOS
    return tap_ && CGEventTapIsEnabled(static_cast<CFMachPortRef>(tap_));
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    return tap_ && source_;
#elif defined(Q_OS_WIN)
    return tap_ && rawKeyboardRegistrationAvailable();
#else
    return false;
#endif
}

bool PushToTalkKey::observe(bool requestPermission) {
    if (!observeGlobal_) return setStatus(tr("Global input is disabled in this test run."), false);
#ifdef Q_OS_MACOS
    if (!CGPreflightListenEventAccess() && (!requestPermission || !CGRequestListenEventAccess()))
        return setStatus(tr("Allow Input Monitoring in macOS for global push-to-talk."), false);
    if (tap_) {
        CGEventTapEnable(static_cast<CFMachPortRef>(tap_), true);
        return setStatus(tr("Push-to-talk also works outside the app."));
    }
    const auto mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);
    tap_ = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, mask,
        [](CGEventTapProxy, CGEventType type, CGEventRef event, void* context) -> CGEventRef {
            auto* self = static_cast<PushToTalkKey*>(context);
            if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
                if (self->tap_ && CGPreflightListenEventAccess()) CGEventTapEnable(static_cast<CFMachPortRef>(self->tap_), true);
                QMetaObject::invokeMethod(self, [self] {
                    if (!self->globalAvailable()) self->setStatus(QCoreApplication::translate("PushToTalkKey",
                        "Global push-to-talk was interrupted. The last state is retained."), false);
                    else emit self->changed();
                }, Qt::QueuedConnection);
                return event;
            }
            if (!event || (type != kCGEventKeyDown && type != kCGEventKeyUp && type != kCGEventFlagsChanged)) return event;
            const auto code = int(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
            if (code < 0 || code > 65535 || CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat)) return event;
            const bool held = type == kCGEventFlagsChanged
                ? CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, CGKeyCode(code)) : type == kCGEventKeyDown;
            if (self->capturing_ && held) return event;
            QMetaObject::invokeMethod(self, [self, code, held] {
                if (!self->capturing_ || !held) self->processObservedKey(code, held);
            }, Qt::QueuedConnection);
            return event;
        }, this);
    if (!tap_) return setStatus(tr("Global push-to-talk could not start. Check Input Monitoring."), false);
    source_ = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, static_cast<CFMachPortRef>(tap_), 0);
    if (!source_) {
        CFMachPortInvalidate(static_cast<CFMachPortRef>(tap_)); CFRelease(tap_); tap_ = nullptr;
        return setStatus(tr("Global push-to-talk could not be added to the event loop."), false);
    }
    CFRunLoopAddSource(CFRunLoopGetMain(), static_cast<CFRunLoopSourceRef>(source_), kCFRunLoopCommonModes);
    CGEventTapEnable(static_cast<CFMachPortRef>(tap_), true);
    return setStatus(tr("Push-to-talk also works outside the app."));
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    Q_UNUSED(requestPermission)
    // Xwayland sees only its own applications, not the entire Wayland desktop.
    const bool x11 = QGuiApplication::platformName() == "xcb"
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
        && qgetenv("XDG_SESSION_TYPE") != "wayland";
    if (!x11)
        return setStatus(tr("Push-to-talk currently works only in the app window. Global keys are not connected on this system yet."), false);
    if (tap_) return setStatus(tr("Push-to-talk also works outside the app."));
    auto* display = XOpenDisplay(nullptr);
    int opcode = 0, event = 0, error = 0, major = 2, minor = 1;
    if (!display || !XQueryExtension(display, "XInputExtension", &opcode, &event, &error)
        || XIQueryVersion(display, &major, &minor) != Success || major < 2 || (major == 2 && minor < 1)) {
        if (display) XCloseDisplay(display);
        return setStatus(tr("Push-to-talk currently works only in the app window. Global keys are not connected on this system yet."), false);
    }
    unsigned char mask[XIMaskLen(XI_LASTEVENT)]{};
    XISetMask(mask, XI_RawKeyPress);
    XISetMask(mask, XI_RawKeyRelease);
    XIEventMask selection{XIAllMasterDevices, int(sizeof(mask)), mask};
    if (XISelectEvents(display, DefaultRootWindow(display), &selection, 1) != Success) {
        XCloseDisplay(display);
        return setStatus(tr("Push-to-talk currently works only in the app window. Global keys are not connected on this system yet."), false);
    }
    // Register before reporting readiness or a release during reconnect can be lost.
    XSync(display, false);
    tap_ = display;
    auto* notifier = new QSocketNotifier(ConnectionNumber(display), QSocketNotifier::Read, this);
    source_ = notifier;
    connect(notifier, &QSocketNotifier::activated, this, [this, display, opcode] {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == MappingNotify) { XRefreshKeyboardMapping(&event.xmapping); continue; }
            if (event.type != GenericEvent || event.xcookie.extension != opcode
                || !XGetEventData(display, &event.xcookie)) continue;
            if (event.xcookie.evtype == XI_RawKeyPress || event.xcookie.evtype == XI_RawKeyRelease) {
                const auto* raw = static_cast<const XIRawEvent*>(event.xcookie.data);
                const bool down = event.xcookie.evtype == XI_RawKeyPress;
                if (!(raw->flags & XIKeyRepeat) && (!capturing_ || !down)) {
                    auto codes = down ? QList<int>{} : nativeHeldKeys_.take(raw->detail);
                    if (codes.isEmpty()) {
                        auto bindings = observedKeys_ + observedRemoteKeys_;
                        for (const auto code : bindings)
                            if (!codes.contains(code) && XKeysymToKeycode(display, KeySym(code)) == raw->detail) codes.append(code);
                    }
                    if (down && !codes.isEmpty()) nativeHeldKeys_.insert(raw->detail, codes);
                    for (const auto code : codes)
                        QMetaObject::invokeMethod(this, [this, code, down] {
                            if (!capturing_ || !down) processObservedKey(code, down);
                        }, Qt::QueuedConnection);
                }
            }
            XFreeEventData(display, &event.xcookie);
        }
    });
    return setStatus(tr("Push-to-talk also works outside the app."));
#elif defined(Q_OS_WIN)
    Q_UNUSED(requestPermission)
    if (tap_ && rawKeyboardRegistrationAvailable())
        return setStatus(tr("Push-to-talk also works outside the app."));
    if (tap_) stopObserving();
    if (!rawKeyboardRegistrationAvailable())
        return setStatus(tr("Push-to-talk currently works only in the app window. Global keys are not connected on this system yet."), false);
    if (!registerNativeInput())
        return setStatus(tr("Push-to-talk currently works only in the app window. Global keys are not connected on this system yet."), false);
    return setStatus(tr("Push-to-talk also works outside the app."));
#else
    Q_UNUSED(requestPermission)
    return setStatus(tr("Push-to-talk currently works only in the app window. Global keys are not connected on this system yet."), false);
#endif
}

#ifdef Q_OS_WIN
bool PushToTalkKey::rawKeyboardRegistrationAvailable() const {
    UINT count = 0;
    if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == UINT(-1)) return false;
    if (!count) return !tap_;
    QVector<RAWINPUTDEVICE> devices(static_cast<qsizetype>(count));
    UINT actual = count;
    if (GetRegisteredRawInputDevices(devices.data(), &actual, sizeof(RAWINPUTDEVICE)) == UINT(-1)) return false;
    const auto keyboard = std::find_if(devices.cbegin(), devices.cbegin() + actual, [](const RAWINPUTDEVICE& device) {
        return device.usUsagePage == 0x01 && device.usUsage == 0x06;
    });
    if (keyboard == devices.cbegin() + actual) return !tap_;
    return tap_ && keyboard->hwndTarget == static_cast<HWND>(tap_);
}

bool PushToTalkKey::registerNativeInput() {
    nativeClassName_ = QStringLiteral("SquadSpeakPtt_%1").arg(quintptr(this), 0, 16);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &PushToTalkKey::nativeWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = reinterpret_cast<LPCWSTR>(nativeClassName_.utf16());
    if (!RegisterClassExW(&windowClass)) {
        nativeClassName_.clear();
        return false;
    }
    const auto className = reinterpret_cast<LPCWSTR>(nativeClassName_.utf16());
    const auto window = CreateWindowExW(0, className, L"SquadSpeak PTT", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, windowClass.hInstance, this);
    if (!window) {
        UnregisterClassW(className, windowClass.hInstance);
        nativeClassName_.clear();
        return false;
    }
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    device.hwndTarget = window;
    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
        DestroyWindow(window);
        UnregisterClassW(className, windowClass.hInstance);
        nativeClassName_.clear();
        return false;
    }
    tap_ = window;
    return true;
}

LRESULT CALLBACK PushToTalkKey::nativeWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PushToTalkKey*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<PushToTalkKey*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self && message == WM_INPUT) self->processNativeInput(lParam);
    if (self && message == WM_INPUT_DEVICE_CHANGE && wParam == GIDC_REMOVAL) {
        // Discard physical tracking without inventing a release of saved PTT.
        for (auto it = self->nativeHeldKeys_.begin(); it != self->nativeHeldKeys_.end();)
            if (it.key().first == quintptr(lParam)) it = self->nativeHeldKeys_.erase(it);
            else ++it;
    }
    if (message == WM_NCDESTROY) SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return DefWindowProcW(window, message, wParam, lParam);
}

void PushToTalkKey::processNativeInput(LPARAM lParam) {
    UINT size = sizeof(RAWINPUT);
    RAWINPUT input{};
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size,
                        sizeof(RAWINPUTHEADER)) == UINT(-1)
        || input.header.dwType != RIM_TYPEKEYBOARD)
        return;
    const auto& keyboard = input.data.keyboard;
    if (keyboard.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE || keyboard.VKey >= UCHAR_MAX) return;
    int code = canonicalWindowsKey(keyboard.VKey);
    const bool down = !(keyboard.Flags & RI_KEY_BREAK);
    const quint32 scan = keyboard.MakeCode
        ? quint32(keyboard.MakeCode) | (quint32(keyboard.Flags & (RI_KEY_E0 | RI_KEY_E1)) << 16)
        : 0x80000000u | quint32(keyboard.VKey);
    const auto physical = qMakePair(quintptr(input.header.hDevice), scan);
    const auto held = nativeHeldKeys_.find(physical);
    if (down) {
        if (held != nativeHeldKeys_.end()) return;
        nativeHeldKeys_.insert(physical, code);
    } else if (held != nativeHeldKeys_.end()) {
        code = held.value();
        nativeHeldKeys_.erase(held);
    }
    if (!down && std::any_of(nativeHeldKeys_.cbegin(), nativeHeldKeys_.cend(),
                            [code](int heldCode) { return heldCode == code; })) return;
    if (!capturing_ || !down) {
        QMetaObject::invokeMethod(this, [this, code, down] {
            if (!capturing_ || !down) processObservedKey(code, down);
        }, Qt::QueuedConnection);
    }
}
#endif

bool PushToTalkKey::eventFilter(QObject* object, QEvent* event) {
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) return QObject::eventFilter(object, event);
    const auto* key = static_cast<QKeyEvent*>(event);
    if (key->isAutoRepeat()) return capturing_;
    if (capturing_) {
        if (event->type() == QEvent::KeyRelease) processObservedKey(int(key->nativeVirtualKey()), false);
        if (key->key() == Qt::Key_Escape && event->type() == QEvent::KeyPress) {
            cancelCapture();
            return setStatus(tr("Key selection canceled."));
        }
        if (key->key() == Qt::Key_unknown || key->key() >= Qt::Key_MediaPlay) {
            setStatus(tr("This special key cannot be used for push-to-talk."), false);
            return true;
        }
        const auto name = plainKeyName(key->key(), key->nativeVirtualKey());
        processCaptureKey(int(key->nativeVirtualKey()), event->type() == QEvent::KeyPress, name);
        return true;
    }
    if (!globalAvailable()) processObservedKey(int(key->nativeVirtualKey()), event->type() == QEvent::KeyPress);
    return QObject::eventFilter(object, event);
}

bool PushToTalkKey::clear(bool remote) {
    if (capturing_ && captureRemote_ == remote) cancelCapture();
    discardObservedKeys(remote);
    if (!session_.releasePttKey(remote)) return setStatus(session_.error(), false);
    const auto ok = session_.setPttShortcut(QList<int>{}, QStringList{}, remote);
    if (!ok) setStatus(session_.error(), false);
    return ok;
}

bool PushToTalkKey::chordHeld(bool remote) const {
    const auto keys = session_.pttKeyCodes(remote);
    return !keys.isEmpty() && std::all_of(keys.cbegin(), keys.cend(), [this](int code) { return heldKeys_.contains(code); });
}

void PushToTalkKey::processObservedKey(int code, bool down, const QString&) {
    if (code < 0 || code > 65535) return;
    const auto localKeys = session_.pttKeyCodes(false);
    const auto remoteKeys = session_.pttKeyCodes(true);
    const auto localAffected = localKeys.contains(code);
    const auto remoteAffected = remoteKeys.contains(code);
    if (!localAffected && !remoteAffected) return;
    if (down) heldKeys_.insert(code); else heldKeys_.remove(code);
    if (localAffected) session_.setPttKeyHeld(chordHeld(false));
    if (remoteAffected) session_.setPttKeyHeld(chordHeld(true), true);
}

void PushToTalkKey::processCaptureKey(int code, bool down, const QString& name) {
    if (code < 0 || code > 65535) return;
    if (down) {
        captureStarted_ = true;
        captureKeys_.insert(code);
        if (!captureSequence_.contains(code)) captureSequence_.append(code);
        captureNames_.insert(code, name.isEmpty() ? tr("Key %1").arg(code) : name);
    } else if (captureStarted_) {
        captureKeys_.remove(code);
        if (captureKeys_.isEmpty()) (void)finishCapture();
    }
}

bool PushToTalkKey::finishCapture() {
    capturing_ = false;
    captureStarted_ = false;
    QList<int> keys = captureSequence_;
    QStringList names;
    for (const auto code : keys) names.append(captureNames_.value(code, tr("Key %1").arg(code)));
    const bool unchanged = keys == session_.pttKeyCodes(captureRemote_);
    const auto ok = session_.setPttShortcut(keys, names, captureRemote_);
    captureKeys_.clear(); captureSequence_.clear(); captureNames_.clear();
    if (!ok) return setStatus(session_.error(), false);
    if (unchanged) observe(true);
    // Keep a missing-permission diagnostic visible after saving the shortcut.
    if (globalAvailable()) return setStatus(tr("Push-to-talk also works outside the app."));
    emit changed();
    return true;
}

void PushToTalkKey::cancelCapture() {
    capturing_ = false;
    captureStarted_ = false;
    captureKeys_.clear();
    captureSequence_.clear();
    captureNames_.clear();
}

void PushToTalkKey::discardObservedKeys(bool remote) {
    const auto keys = session_.pttKeyCodes(remote);
    const auto otherKeys = session_.pttKeyCodes(!remote);
    for (const auto code : keys)
        if (!otherKeys.contains(code)) heldKeys_.remove(code);
}
