#pragma once

#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <memory>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

class QCoreApplication;
class QIODevice;
class QSocketNotifier;
class RadioPlayer;
class LocalChannel;
class License;

class HeadlessController final : public QObject {
public:
    HeadlessController(QCoreApplication& app, LocalChannel& channel,
                       RadioPlayer& radio, License& license, QIODevice& input, QIODevice& output,
                       bool monitorInput = true, QObject* parent = nullptr);
    ~HeadlessController() override;
    void process(const QJsonObject& request);

private:
    void write(const QJsonObject& object);
    void flushOutput();
    void failOutput(const QString& error);
    void enableInput(bool enabled);
    void readInput();
    void handleRadio(const QJsonObject& request, RadioPlayer& radio);
    void finishRadioSave(const QVariantMap& station, bool ok, const QString& error);
    QJsonObject licenseStatus() const;
    QCoreApplication& app_;
    LocalChannel& channel_;
    RadioPlayer& radio_;
    License& license_;
    QIODevice& output_;
    QByteArray pendingInput_;
    QByteArray pendingOutput_;
    QTimer outputRetry_;
    int inputDescriptor_ = -1;
    int outputDescriptor_ = -1, outputMode_ = -1;
    bool regularInput_ = false;
    bool discardingInputLine_ = false;
    bool inputEof_ = false;
    bool quitting_ = false;
    bool radioSavePending_ = false;
    bool licensePending_ = false;
    QMetaObject::Connection radioSaved_, radioDestroyed_;
#ifdef Q_OS_WIN
    QString consoleLine_;
    OVERLAPPED outputOperation_{};
    QByteArray writingOutput_;
#endif
    std::unique_ptr<QSocketNotifier> notifier_;
    QObject* inputPoller_ = nullptr;
};

namespace Headless {
int run(int argc, char** argv);
}
