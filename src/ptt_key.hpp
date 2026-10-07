#pragma once

#include "voice_session.hpp"
#include <QObject>
#include <QHash>
#include <QSet>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

// Observes physical keys. State and persistence belong to VoiceSession;
// losing the OS hook never invents a key-up event.
class PushToTalkKey final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool capturing READ capturing NOTIFY changed)
    Q_PROPERTY(bool capturingRemote READ capturingRemote NOTIFY changed)
    Q_PROPERTY(bool globalAvailable READ globalAvailable NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit PushToTalkKey(VoiceSession& session, bool observeGlobal, QObject* parent = nullptr);
    ~PushToTalkKey() override;
    [[nodiscard]] bool capturing() const { return capturing_; }
    [[nodiscard]] bool capturingRemote() const { return capturing_ && captureRemote_; }
    [[nodiscard]] bool globalAvailable() const;
    [[nodiscard]] QString status() const { return status_; }
    Q_INVOKABLE bool capture(bool remote = false);
    Q_INVOKABLE bool clear(bool remote = false);
signals:
    void changed();
protected:
    bool eventFilter(QObject* object, QEvent* event) override;
private:
    bool observe(bool requestPermission);
    void refreshBindings();
    void stopObserving();
    bool setStatus(QString status, bool result = true);
    void processObservedKey(int code, bool down, const QString& name = {});
    void processCaptureKey(int code, bool down, const QString& name);
    bool finishCapture();
    void cancelCapture();
    void discardObservedKeys(bool remote);
    [[nodiscard]] bool chordHeld(bool remote) const;
#ifdef Q_OS_WIN
    static LRESULT CALLBACK nativeWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void processNativeInput(LPARAM lParam);
    [[nodiscard]] bool registerNativeInput();
    [[nodiscard]] bool rawKeyboardRegistrationAvailable() const;
#endif
    VoiceSession& session_;
    bool observeGlobal_;
    bool capturing_ = false;
    bool captureRemote_ = false;
    QList<int> observedKeys_;
    QList<int> observedRemoteKeys_;
    bool observedPtt_ = false;
    QSet<int> heldKeys_;
    QSet<int> captureKeys_;
    QList<int> captureSequence_;
    QHash<int, QString> captureNames_;
    bool captureStarted_ = false;
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    QHash<int, QList<int>> nativeHeldKeys_;
#endif
#ifdef Q_OS_WIN
    QHash<QPair<quintptr, quint32>, int> nativeHeldKeys_;
    QString nativeClassName_;
#endif
    void* tap_ = nullptr;
    void* source_ = nullptr;
    QString status_ = QStringLiteral("No push-to-talk key selected yet.");
};
