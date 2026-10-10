#include "headless.hpp"

#include "local_channel.hpp"
#include "radio_player.hpp"
#include "voice_session.hpp"
#include "license.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTranslator>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <climits>
#include <stdexcept>
#include <utility>
#ifndef Q_OS_WIN
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <io.h>
#endif

namespace {
constexpr qsizetype maximumLine = 64 * 1024;
constexpr qsizetype maximumOutput = 2 * 1024 * 1024;

QJsonObject response(const QString& command, bool ok, const QJsonValue& data = {}, const QString& error = {}) {
    QJsonObject result{{"command", command}, {"ok", ok}};
    if (!data.isUndefined()) result.insert("data", data);
    if (!ok) result.insert("error", error.isEmpty() ? QCoreApplication::translate("Headless", "Command failed.") : error);
    return result;
}

bool setTextOption(QJsonObject& values, const QString& key, const QString& text, QString& error) {
    if (key == "channelName" || key == "botName" || key == "password" || key == "passwordFile" || key == "settingsFile" || key == "identityFile" || key == "language") {
        values.insert(key, text); return true;
    }
    if (key == "approvedClients" || key == "blockedClients") {
        QJsonArray ids;
        if (!text.trimmed().isEmpty()) for (const auto& id : text.split(',')) ids.append(id.trimmed());
        values.insert(key, ids); return true;
    }
    if (key == "port" || key == "messageLifetimeDays") {
        bool valid = false;
        const auto number = text.toInt(&valid);
        if (!valid) { error = QCoreApplication::translate("Headless", "%1 must be an integer.").arg(key); return false; }
        values.insert(key, number); return true;
    }
    if (key == "messageTtl") {
        const int days = text == "24h" ? 1 : text == "7d" ? 7 : text == "30d" ? 30 : text == "90d" ? 90 : text == "180d" ? 180 : text == "360d" ? 360 : 0;
        if (!days) { error = QCoreApplication::translate("LocalChannel", "Invalid message lifetime.") + QStringLiteral(" (24h, 7d, 30d, 90d, 180d, 360d)"); return false; }
        values.insert("messageLifetimeDays", days); return true;
    }
    if (key == "requestsAllowed") {
        if (text != "on" && text != "off" && text != "true" && text != "false") {
            error = QCoreApplication::translate("Headless", "Requests must be on/off or true/false."); return false;
        }
        values.insert(key, text == "on" || text == "true"); return true;
    }
    error = QCoreApplication::translate("Headless", "Unknown host property: %1").arg(key); return false;
}

bool readConfiguration(const QString& path, QJsonObject& values, QString& error);
QString executableConfigurationDirectory();

void addHeadlessOptions(QCommandLineParser& parser) {
    parser.addHelpOption();
    parser.addVersionOption();
    for (const auto* name : {"settings", "recording-test", "smoke-test", "screenshot"}) {
        QCommandLineOption option(name);
        if (QString::fromLatin1(name) == "screenshot") option.setValueName("path");
        option.setFlags(QCommandLineOption::HiddenFromHelp);
        parser.addOption(option);
    }
    parser.addOption({"language", QCoreApplication::translate("VoiceSession", "Choose a language from the list."), "code"});
    parser.addOption({"headless", QCoreApplication::translate("Headless", "Run without GUI, microphone capture, or local audio output.")});
    parser.addOption({"settings-file", QCoreApplication::translate("Headless", "Server storage prefix. Personal/audio/GUI profiles are not loaded."), "path"});
    parser.addOption({"identity-file", QCoreApplication::translate("Headless", "Optional private identity file instead of the OS credential store (Linux/macOS). Parent directory must exist and belong to this account."), "path"});
    parser.addOption({QStringList{"f", "config"}, QCoreApplication::translate("Headless", "UTF-8 key=value properties or JSON host preset. Default: application.properties, then config/application.properties beside the app."), "path"});
    parser.addOption({"channel-name", QCoreApplication::translate("Headless", "Channel display name."), "name"});
    parser.addOption({"bot-name", QCoreApplication::translate("Headless", "System and radio bot display name."), "name"});
    parser.addOption({"port", QCoreApplication::translate("Headless", "TCP service port (1-65535)."), "number"});
    parser.addOption({"message-ttl", QCoreApplication::translate("Channels", "New message lifetime") + QStringLiteral(" (24h, 7d, 30d; Supporter: 90d, 180d, 360d)"), "duration"});
    parser.addOption({"requests", QCoreApplication::translate("Headless", "Headless admission is always on; only 'on' or 'true' is accepted."), "mode"});
    parser.addOption({"approve", QCoreApplication::translate("Headless", "Preapprove a device ID; repeat for several devices. Existing bans remain."), "id"});
    parser.addOption({"ban", QCoreApplication::translate("Headless", "Block a device ID; repeat for several devices."), "id"});
    parser.addOption({"password-file", QCoreApplication::translate("Headless", "Read a UTF-8 channel password from a file before listening. An empty file removes it."), "path"});
    parser.addOption({"password", QCoreApplication::translate("Headless", "Channel password. Empty removes it; environment or config avoids process-list exposure."), "text"});
    parser.setApplicationDescription(QCoreApplication::translate("Headless", "SquadSpeak headless local channel host") + '\n'
        + QCoreApplication::translate("Headless", "Settings precedence: arguments > environment > properties > saved server values.") + '\n'
        + QCoreApplication::translate("Headless", "Environment variables (SQUADSPEAK_ prefix):")
        + " CONFIG, CHANNEL_NAME, BOT_NAME, PORT, MESSAGE_TTL, REQUESTS, APPROVED_CLIENTS, BLOCKED_CLIENTS, PASSWORD, PASSWORD_FILE, SETTINGS_FILE, IDENTITY_FILE, LANGUAGE.");
}

bool loadDefaultConfiguration(QJsonObject& configuration, QString& error) {
    const QDir directory(executableConfigurationDirectory());
    for (const auto* relative : {"application.properties", "config/application.properties"}) {
        const auto path = directory.filePath(relative);
        if (QFileInfo::exists(path) && !readConfiguration(path, configuration, error)) return false;
    }
    return true;
}

bool supportedLanguage(const QString& code) {
    const auto languages = VoiceSession::languages();
    return std::any_of(languages.cbegin(), languages.cend(), [&](const auto& entry) {
        return entry.toMap().value("code").toString() == code;
    });
}

bool readConfiguration(const QString& path, QJsonObject& values, QString& error) {
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        error = QCoreApplication::translate("Headless", "Host configuration could not be read or exceeds 1 MiB."); return false;
    }
    const auto bytes = file.read(1024 * 1024 + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > 1024 * 1024) {
        error = QCoreApplication::translate("Headless", "Host configuration could not be read or exceeds 1 MiB."); return false;
    }
    const auto source = QString::fromUtf8(bytes);
    if (source.toUtf8() != bytes) { error = QCoreApplication::translate("Headless", "Host configuration must be UTF-8."); return false; }
    QJsonObject parsed;
    if (bytes.trimmed().startsWith('{')) {
        QJsonParseError failure;
        const auto document = QJsonDocument::fromJson(bytes, &failure);
        if (failure.error != QJsonParseError::NoError || !document.isObject()) {
            error = QCoreApplication::translate("Headless", "Host preset must be a JSON object."); return false;
        }
        parsed = document.object();
    } else {
        int lineNumber = 0;
        for (auto line : source.split('\n')) {
            ++lineNumber;
            if (line.endsWith('\r')) line.chop(1);
            const auto trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith('#') || trimmed.startsWith('!')) continue;
            const auto separator = line.indexOf('=');
            if (separator < 1) { error = QCoreApplication::translate("Headless", "Property line %1 must use key=value.").arg(lineNumber); return false; }
            const auto key = line.left(separator).trimmed();
            const auto value = line.mid(separator + 1);
            if (!setTextOption(parsed, key, key == "password" ? value : value.trimmed(), error)) {
                error = QCoreApplication::translate("Headless", "Property line %1: %2").arg(lineNumber).arg(error); return false;
            }
        }
    }
    for (const auto* key : {"settingsFile", "passwordFile", "identityFile"}) {
        const auto value = parsed.value(key);
        if (value.isString() && !value.toString().isEmpty() && QDir::isRelativePath(value.toString()))
            parsed.insert(key, QFileInfo(path).absoluteDir().absoluteFilePath(value.toString()));
    }
    // A password source in a higher layer replaces the lower layer's source.
    if (parsed.contains("password") || parsed.contains("passwordFile")) {
        values.remove("password"); values.remove("passwordFile");
    }
    for (auto it = parsed.begin(); it != parsed.end(); ++it) values.insert(it.key(), it.value());
    return true;
}

QString executableConfigurationDirectory() {
    QDir directory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    if (directory.dirName() == "MacOS") {
        QDir bundle(directory);
        if (bundle.cdUp() && bundle.dirName() == "Contents" && bundle.cdUp() && bundle.dirName().endsWith(".app")) {
            bundle.cdUp(); return bundle.absolutePath();
        }
    }
#endif
    return directory.absolutePath();
}

}

HeadlessController::HeadlessController(QCoreApplication& app, LocalChannel& channel,
                                       RadioPlayer& radio, License& license, QIODevice& input, QIODevice& output,
                                       bool monitorInput, QObject* parent)
    : QObject(parent ? parent : &app), app_(app), channel_(channel), radio_(radio), license_(license),
      output_(output) {
    outputRetry_.setInterval(25);
    connect(&outputRetry_, &QTimer::timeout, this, &HeadlessController::flushOutput);
    connect(&license_, &License::authorizationReady, this, [this] {
        if (!licensePending_) return;
        write(response("license", true, QJsonObject{{"pending", true}, {"verificationUrl", license_.verificationUrl().toString()},
            {"userCode", license_.userCode()}}));
        licensePending_ = false;
    });
    connect(&license_, &License::changed, this, [this] {
        if (licensePending_ && !license_.busy() && !license_.pending()) {
            licensePending_ = false;
            write(response("license", license_.status().isEmpty(), licenseStatus(), license_.status()));
        }
    });
    if (auto* file = qobject_cast<QFile*>(&input)) inputDescriptor_ = file->handle();
    if (monitorInput && inputDescriptor_ < 0) throw std::runtime_error(QT_TRANSLATE_NOOP("Headless", "Standard input has no file descriptor."));
#ifndef Q_OS_WIN
    if (monitorInput) {
        struct stat metadata {};
        if (::fstat(inputDescriptor_, &metadata) != 0) throw std::runtime_error(QT_TRANSLATE_NOOP("Headless", "Standard input could not be inspected."));
        regularInput_ = S_ISREG(metadata.st_mode);
        if (regularInput_) QTimer::singleShot(0, this, [this] { readInput(); });
        else {
            notifier_ = std::make_unique<QSocketNotifier>(inputDescriptor_, QSocketNotifier::Read, this);
            connect(notifier_.get(), &QSocketNotifier::activated, this, [this] { readInput(); });
        }
    }
#else
    if (monitorInput) {
        auto* timer = new QTimer(this);
        timer->setInterval(25);
        connect(timer, &QTimer::timeout, this, [this] { readInput(); });
        timer->start();
        inputPoller_ = timer;
    }
#endif
    if (auto* file = qobject_cast<QFile*>(&output)) {
        const int descriptor = file->handle();
#ifndef Q_OS_WIN
        struct stat metadata {};
        if (::fstat(descriptor, &metadata) != 0)
            throw std::runtime_error(QT_TRANSLATE_NOOP("Headless", "Standard output could not be opened."));
        if (S_ISFIFO(metadata.st_mode) || S_ISSOCK(metadata.st_mode) || ::isatty(descriptor)) {
            outputMode_ = ::fcntl(descriptor, F_GETFL);
            if (outputMode_ < 0 || ::fcntl(descriptor, F_SETFL, outputMode_ | O_NONBLOCK) != 0)
                throw std::runtime_error(QT_TRANSLATE_NOOP("Headless", "Standard output could not be opened."));
            outputDescriptor_ = descriptor;
        }
#else
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
        if (GetFileType(handle) == FILE_TYPE_PIPE) {
            const auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!event)
                throw std::runtime_error(QT_TRANSLATE_NOOP("Headless", "Standard output could not be opened."));
            // stdout can be synchronous (shell) or overlapped (QProcess).
            // A write-only inherited handle need not allow querying its mode.
            DWORD mode = PIPE_NOWAIT;
            if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
                CloseHandle(event);
                throw std::runtime_error(QT_TRANSLATE_NOOP("Headless", "Standard output could not be opened."));
            }
            outputOperation_.hEvent = event;
            outputDescriptor_ = descriptor;
        }
#endif
    }
}

HeadlessController::~HeadlessController() {
    if (outputDescriptor_ < 0) return;
#ifndef Q_OS_WIN
    ::fcntl(outputDescriptor_, F_SETFL, outputMode_);
#else
    if (!writingOutput_.isEmpty()) {
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(outputDescriptor_));
        CancelIoEx(handle, &outputOperation_);
        DWORD count = 0;
        GetOverlappedResult(handle, &outputOperation_, &count, TRUE);
    }
    CloseHandle(outputOperation_.hEvent);
#endif
}

void HeadlessController::enableInput(bool enabled) {
    if (notifier_) notifier_->setEnabled(enabled && !inputEof_);
    if (auto* timer = qobject_cast<QTimer*>(inputPoller_)) {
        if (enabled && !inputEof_) timer->start();
        else timer->stop();
    }
}

void HeadlessController::failOutput(const QString& error) {
    quitting_ = true; enableInput(false); outputRetry_.stop(); pendingOutput_.clear();
    qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Could not write the command response: %1").arg(error)));
    app_.exit(1);
}

void HeadlessController::write(const QJsonObject& object) {
    if (quitting_) return;
    const auto line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (line.size() > maximumOutput - pendingOutput_.size()) {
        failOutput(QString::fromLocal8Bit(strerror(ENOMEM))); return;
    }
    pendingOutput_.append(line); flushOutput();
}

void HeadlessController::flushOutput() {
    qsizetype budget = maximumLine;
    while (!pendingOutput_.isEmpty() && budget > 0) {
        const auto size = std::min(budget, pendingOutput_.size());
        qint64 written = -1;
        if (outputDescriptor_ < 0) {
            written = output_.write(pendingOutput_.constData(), size);
            auto* file = qobject_cast<QFile*>(&output_);
            if (written <= 0 || (file && !file->flush())) { failOutput(output_.errorString()); return; }
        } else {
#ifndef Q_OS_WIN
            written = ::write(outputDescriptor_, pendingOutput_.constData(), size);
            if (written < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                failOutput(QString::fromLocal8Bit(strerror(errno))); return;
            }
#else
            const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(outputDescriptor_));
            if (writingOutput_.isEmpty()) {
                writingOutput_ = pendingOutput_.first(size);
                const auto event = outputOperation_.hEvent;
                outputOperation_ = {}; outputOperation_.hEvent = event;
                if (!WriteFile(handle, writingOutput_.constData(), DWORD(writingOutput_.size()), nullptr, &outputOperation_)) {
                    const auto error = GetLastError();
                    if (error != ERROR_IO_PENDING) {
                        writingOutput_.clear(); failOutput(QString::number(error)); return;
                    }
                }
            }
            DWORD count = 0;
            if (!GetOverlappedResult(handle, &outputOperation_, &count, FALSE)) {
                const auto error = GetLastError();
                if (error == ERROR_IO_INCOMPLETE) break;
                writingOutput_.clear();
                failOutput(QString::number(error)); return;
            }
            writingOutput_.clear(); written = count;
#endif
            if (written == 0) break;
        }
        pendingOutput_.remove(0, written); budget -= written;
    }
    if (!pendingOutput_.isEmpty()) {
        enableInput(false);
        if (!outputRetry_.isActive()) outputRetry_.start();
    } else if (quitting_) {
        outputRetry_.stop(); app_.quit();
    } else if (outputRetry_.isActive()) {
        outputRetry_.stop(); enableInput(true);
        if (!pendingInput_.isEmpty() || regularInput_) QTimer::singleShot(0, this, [this] { readInput(); });
    }
}

void HeadlessController::readInput() {
    if (quitting_ || !pendingOutput_.isEmpty() || (inputEof_ && pendingInput_.isEmpty())) return;
    qsizetype readBudget = std::max<qsizetype>(0, maximumLine + 1 - pendingInput_.size());
#ifndef Q_OS_WIN
        char buffer[16 * 1024];
        while (!inputEof_ && readBudget > 0 && inputDescriptor_ >= 0) {
            pollfd readiness{inputDescriptor_, POLLIN, 0};
            if (::poll(&readiness, 1, 0) <= 0) break;
            const auto count = ::read(inputDescriptor_, buffer, std::min<qsizetype>(sizeof(buffer), readBudget));
            if (count > 0) pendingInput_.append(buffer, int(count));
            else if (count == 0) { inputEof_ = true; break; }
            else if (errno == EINTR) continue;
            else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                qWarning("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Standard input could not be read: %1").arg(QString::fromLocal8Bit(strerror(errno))))); inputEof_ = true; break;
            }
            else break;
            if (count > 0) readBudget -= count;
        }
#else
        const auto descriptor = inputDescriptor_;
        if (descriptor < 0) return;
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
        const auto type = GetFileType(handle);
        DWORD available = 0;
        if (type == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
                inputEof_ = true;
            }
        } else if (type == FILE_TYPE_CHAR) {
            DWORD mode = 0;
            if (GetConsoleMode(handle, &mode)) {
                INPUT_RECORD records[64];
                DWORD count = 0;
                if (PeekConsoleInputW(handle, records, DWORD(std::size(records)), &count)) {
                    for (DWORD i = 0; i < count; ++i) {
                        if (records[i].EventType == KEY_EVENT && records[i].Event.KeyEvent.bKeyDown &&
                            records[i].Event.KeyEvent.uChar.UnicodeChar != 0) {
                            const QChar ch(records[i].Event.KeyEvent.uChar.UnicodeChar);
                            QString echo;
                            if (ch == '\r') {
                                pendingInput_.append(consoleLine_.toUtf8() + '\n');
                                consoleLine_.clear(); echo = "\r\n";
                            } else if (ch == '\b') {
                                if (!consoleLine_.isEmpty()) {
                                    const bool pair = consoleLine_.back().isLowSurrogate() && consoleLine_.size() > 1
                                        && consoleLine_.at(consoleLine_.size() - 2).isHighSurrogate();
                                    consoleLine_.chop(pair ? 2 : 1); echo = "\b \b";
                                }
                            } else if (!discardingInputLine_) {
                                consoleLine_.append(ch); echo.append(ch);
                                if (consoleLine_.size() > maximumLine) {
                                    consoleLine_.clear(); discardingInputLine_ = true;
                                }
                            }
                            DWORD written = 0;
                            if (!echo.isEmpty()) WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), echo.utf16(), DWORD(echo.size()), &written, nullptr);
                        }
                    }
                    if (count > 0) ReadConsoleInputW(handle, records, count, &count);
                }
                available = 0;
            }
        } else if (type == FILE_TYPE_DISK) available = 16 * 1024;
        if (!inputEof_ && available > 0 && readBudget > 0) {
            char buffer[16 * 1024];
            const auto count = _read(descriptor, buffer, unsigned(std::min<qsizetype>({sizeof(buffer), readBudget, available})));
            if (count > 0) pendingInput_.append(buffer, count);
            else if (count == 0) inputEof_ = true;
        }
#endif
        while (!quitting_ && pendingOutput_.isEmpty()) {
            if (discardingInputLine_) {
                const auto end = pendingInput_.indexOf('\n');
                if (end < 0) { pendingInput_.clear(); break; }
                pendingInput_.remove(0, end + 1);
                discardingInputLine_ = false;
                write(response("", false, {}, QCoreApplication::translate("Headless", "Input line exceeds 65536 bytes.")));
                continue;
            }
            const auto end = pendingInput_.indexOf('\n');
            if (end < 0) {
                if (pendingInput_.size() > maximumLine) {
                    pendingInput_.clear();
                    discardingInputLine_ = true;
                }
                break;
            }
            const auto line = pendingInput_.left(end + 1);
            pendingInput_.remove(0, end + 1);
            if (line.size() > maximumLine) {
                write(response("", false, {}, QCoreApplication::translate("Headless", "Input line exceeds 65536 bytes.")));
                continue;
            }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                write(response("", false, {}, QCoreApplication::translate("Headless", "Each input line must be a JSON object.")));
                continue;
            }
            process(document.object());
        }
        if (quitting_ || !pendingOutput_.isEmpty()) return;
        if (inputEof_) {
            if (!pendingInput_.trimmed().isEmpty() || discardingInputLine_)
                write(response("", false, {}, QCoreApplication::translate("Headless", "Input ended before a complete JSON line.")));
            pendingInput_.clear(); discardingInputLine_ = false;
#ifndef Q_OS_WIN
            if (notifier_) notifier_->setEnabled(false);
#endif
            if (inputPoller_) static_cast<QTimer*>(inputPoller_)->stop();
        } else if (regularInput_) QTimer::singleShot(0, this, [this] { readInput(); });
}
void HeadlessController::process(const QJsonObject& request) {
    if (quitting_) return;
    const auto command = request.value("command").toString();
    if (command == "help") {
        write(response(command, true, QJsonObject{
            {"commands", QStringLiteral("status, channels, configure, password, requests, approve, reject, kick, ban, unban, history, chat, radio, license, quit")},
            {"channelId", QCoreApplication::translate("Headless", "Optional own channel ID for status, configure, password, admission, history, chat and radio commands; defaults to the primary channel.")},
            {"examples", QJsonArray{
                QJsonObject{{"command", "channels"}, {"action", "list"}},
                QJsonObject{{"command", "channels"}, {"action", "add"}, {"name", "Second room"}},
                QJsonObject{{"command", "channels"}, {"action", "remove"}, {"id", "channel-id"}, {"confirmed", true}},
                QJsonObject{{"command", "status"}}, QJsonObject{{"command", "approve"}, {"id", "device-id"}},
                QJsonObject{{"command", "configure"}, {"values", QJsonObject{{"channelName", "Lounge"}, {"messageLifetimeDays", 7}}}},
                QJsonObject{{"command", "password"}, {"value", "new-channel-password"}},
                QJsonObject{{"command", "history"}, {"direction", "older"}, {"cursor", 41}},
                QJsonObject{{"command", "chat"}, {"text", "Hello"}},
                QJsonObject{{"command", "radio"}, {"action", "list"}},
                QJsonObject{{"command", "license"}, {"action", "status"}},
                QJsonObject{{"command", "license"}, {"action", "sign-in"}},
                QJsonObject{{"command", "radio"}, {"action", "search"}, {"query", "jazz"}, {"limit", 20}}}}
        }));
        return;
    }
    if (command == "quit") {
        write(response(command, true));
        if (!quitting_) {
            quitting_ = true; enableInput(false);
            if (pendingOutput_.isEmpty()) app_.quit();
        }
        return;
    }
    if (command == "license") {
        const auto action = request.value("action").toString("status");
        if ((request.contains("action") && !request.value("action").isString())
            || (action != "status" && action != "sign-in" && action != "refresh" && action != "sign-out" && action != "cancel")) {
            write(response(command, false, {}, QCoreApplication::translate("Headless", "Supporter action must be status, sign-in, refresh, sign-out or cancel."))); return;
        }
        if (action == "status") { write(response(command, true, licenseStatus())); return; }
        if (!license_.configured() || (license_.busy() && !(action == "cancel" && license_.pending()))) {
            write(response(command, false, licenseStatus(), license_.busy() ? QCoreApplication::translate("Headless", "A Supporter operation is in progress.") : QCoreApplication::translate("Headless", "Supporter access is unavailable in this build."))); return;
        }
        licensePending_ = true;
        const bool accepted = action == "sign-in" ? license_.signIn()
            : action == "refresh" ? license_.refresh() : action == "sign-out" ? license_.signOut() : license_.cancelSignIn();
        if (!accepted && licensePending_) {
            licensePending_ = false;
            write(response(command, false, licenseStatus(), license_.status()));
        }
        return;
    }
    if (command == "channels") {
        if (request.contains("action") && !request.value("action").isString()) {
            write(response(command, false, {}, QCoreApplication::translate("Headless", "Channel action must be list, add or remove."))); return;
        }
        const auto action = request.value("action").toString("list");
        if (action == "list") { write(response(command, true, QJsonValue::fromVariant(channel_.ownedChannels()))); return; }
        if (action == "add") {
            const auto id = channel_.addOwnedChannel(request.value("name").toString());
            write(response(command, !id.isEmpty(), QJsonObject{{"id", id}}, channel_.status())); return;
        }
        if (action == "remove") {
            if (!request.value("confirmed").isBool() || !request.value("confirmed").toBool()) {
                write(response(command, false, {}, QCoreApplication::translate("Headless", "Removing an own channel deletes its history, images, permissions and radio settings. Set confirmed=true to proceed."))); return;
            }
            const auto id = request.value("id").toString();
            if (id == channel_.channelId() || !channel_.ownChannel(id)) {
                write(response(command, false, {}, QCoreApplication::translate("Headless", "id must identify an additional own channel."))); return;
            }
            const auto ok = channel_.removeOwnedChannel(id);
            write(response(command, ok, {}, ok ? QString{} : QCoreApplication::translate("Headless", "The additional own channel could not be removed: %1").arg(channel_.status()))); return;
        }
        write(response(command, false, {}, QCoreApplication::translate("Headless", "Channel action must be list, add or remove."))); return;
    }
    const auto channelId = request.value("channelId");
    auto* selected = channelId.isUndefined() ? &channel_ : channel_.ownChannel(channelId.toString());
    if ((!channelId.isUndefined() && (!channelId.isString() || channelId.toString().isEmpty())) || !selected) {
        write(response(command, false, {}, QCoreApplication::translate("Headless", "channelId must identify an own channel."))); return;
    }
    auto& channel = *selected;
    auto* player = selected == &channel_ ? &radio_ : radio_.forChannel(channel.channelId());
    if (command == "status") {
        write(response(command, true, QJsonObject{{"ready", channel.ready()}, {"hosting", channel.hosting()},
            {"port", int(channel.port())}, {"ownId", channel.ownId()}, {"channelId", channel.channelId()}, {"channel", channel.channelName()},
            {"channels", QJsonValue::fromVariant(channel_.ownedChannels())},
            {"configuration", channel.hostConfiguration()},
            {"passwordProtected", channel.passwordProtected()},
            {"admissionPolicy", "automatic-except-blocked"},
            {"requestsAllowed", channel.requestsAllowed()}, {"hostClients", QJsonValue::fromVariant(channel.hostClients())},
            {"hostParticipants", QJsonValue::fromVariant(channel.hostParticipants())},
            {"blockedClients", QJsonValue::fromVariant(channel.blockedClients())}, {"radio", player ? player->state() : QString("unavailable")},
            {"status", channel.status()}, {"discoveryError", channel.discoveryError()}}));
        return;
    }
    if (command == "configure") {
        if (!request.value("values").isObject()) { write(response(command, false, {}, QCoreApplication::translate("Headless", "values must be a host settings object."))); return; }
        const auto values = request.value("values").toObject();
        if (values.contains("requestsAllowed") && values.value("requestsAllowed").isBool()
            && !values.value("requestsAllowed").toBool()) {
            write(response(command, false, {}, QCoreApplication::translate("Headless", "Headless admission is always enabled for non-banned devices; requests off is not supported.")));
            return;
        }
        const auto ok = channel.configureHost(values);
        write(response(command, ok, ok ? QJsonValue(channel.hostConfiguration()) : QJsonValue{}, channel.status()));
        return;
    }
    if (command == "history") {
        try { write(response(command, true, channel.hostHistory(request))); }
        catch (const std::exception& error) { write(response(command, false, {}, QString::fromUtf8(error.what()))); }
        return;
    }
    if (command == "password") {
        if (!request.value("value").isString() || !LocalChannel::validHostPassword(request.value("value").toString())) {
            write(response(command, false, {}, QCoreApplication::translate("Headless", "value must be a password of at most 1024 UTF-8 bytes without control characters; an empty string removes it."))); return;
        }
        if (!channel.ready() || channel.passwordBusy()) {
            write(response(command, false, {}, QCoreApplication::translate("Headless", "The channel is not ready or a password change is in progress."))); return;
        }
        auto* operation = new QObject(this);
        const auto id = channel.channelId();
        connect(&channel, &LocalChannel::hostPasswordSaved, operation, [this, operation, &channel, id](bool ok) {
            QObject::disconnect(&channel, nullptr, operation, nullptr);
            operation->deleteLater();
            write(response("password", ok, QJsonObject{{"channelId", id}, {"passwordProtected", channel.passwordProtected()}}, channel.status()));
        });
        connect(&channel, &QObject::destroyed, operation, [this, operation, id] {
            operation->deleteLater();
            write(response("password", false, QJsonObject{{"channelId", id}}, QCoreApplication::translate("Headless", "The channel was removed while saving the password.")));
        });
        if (!channel.setHostPassword(request.value("value").toString())) {
            delete operation;
            write(response(command, false, {}, channel.status()));
        }
        return;
    }
    if (command == "requests") { write(response(command, true, QJsonValue::fromVariant(channel.requests()))); return; }
    if (command == "approve" || command == "reject") {
        const auto id = request.value("id").toString();
        if (id.isEmpty()) { write(response(command, false, {}, QCoreApplication::translate("Headless", "id is required."))); return; }
        const auto ok = channel.decide(id, command == "approve");
        write(response(command, ok, {}, ok ? QString{} : channel.status()));
        return;
    }
    if (command == "kick" || command == "ban" || command == "unban") {
        const auto id = request.value("id").toString();
        if (id.isEmpty()) { write(response(command, false, {}, QCoreApplication::translate("Headless", "id is required."))); return; }
        bool ok = false;
        if (command == "kick") ok = channel.kick(id);
        else ok = channel.setBlocked(id, command == "ban");
        write(response(command, ok, {}, ok ? QString{} : channel.status()));
        return;
    }
    if (command == "chat") {
        const auto text = request.value("text").toString();
        if (!ChatHistory::validText(text)) { write(response(command, false, {}, QCoreApplication::translate("Headless", "text must be a valid non-empty chat message."))); return; }
        if (!channel.ready() || !channel.hosting()) { write(response(command, false, {}, QCoreApplication::translate("Headless", "The local channel is not ready."))); return; }
        const auto ok = channel.sendSystemMessage(text);
        write(response(command, ok, ok ? QJsonValue(QJsonObject{{"delivered", true}, {"text", text}}) : QJsonValue{}, channel.status()));
        return;
    }
    if (command == "radio") {
        if (!player) { write(response(command, false, {}, QCoreApplication::translate("Headless", "The channel's radio is unavailable."))); return; }
        handleRadio(request, *player); return;
    }
    write(response(command, false, {}, QCoreApplication::translate("Headless", "Unknown command. Use help.")));
}
QJsonObject HeadlessController::licenseStatus() const {
    return {{"configured", license_.configured()}, {"active", license_.active()}, {"busy", license_.busy()},
        {"pending", license_.pending()}, {"signedIn", license_.signedIn()}, {"account", license_.account()},
        {"userCode", license_.userCode()}, {"verificationUrl", license_.verificationUrl().toString()},
        {"expiresAt", license_.expiresAt().toString(Qt::ISODate)}, {"status", license_.status()}};
}
void HeadlessController::handleRadio(const QJsonObject& request, RadioPlayer& radio) {
    const auto action = request.value("action").toString();
    if (action == "list") { write(response("radio", true, QJsonValue::fromVariant(radio.stations()))); return; }
    if (action == "search") {
        const auto offset = request.value("offset").toDouble(0);
        const auto limit = request.value("limit").toDouble(50);
        if (!request.value("query").isString() || request.value("query").toString().size() > 256
            || (request.contains("offset") && !request.value("offset").isDouble())
            || (request.contains("limit") && !request.value("limit").isDouble())
            || !std::isfinite(offset) || std::floor(offset) != offset || offset < 0 || offset > INT_MAX
            || !std::isfinite(limit) || std::floor(limit) != limit || limit < 1 || limit > 128) {
            write(response("radio", false, {}, QCoreApplication::translate("Headless", "search requires a query, non-negative offset and limit from 1 to 128."))); return;
        }
        auto page = radio.searchStations(request.value("query").toString(), int(offset), int(limit));
        page.insert("catalog", radio.catalogInfo());
        write(response("radio", true, QJsonValue::fromVariant(page))); return;
    }
    bool ok = false;
    if (action == "add" || action == "update") {
        const auto id = request.value("id").toString();
        if ((action == "add") != id.isEmpty()) {
            write(response("radio", false, {}, QCoreApplication::translate("Headless", "add creates an id; update requires an existing id."))); return;
        }
        if (radioSavePending_) { write(response("radio", false, {}, QCoreApplication::translate("Headless", "A station is already being checked."))); return; }
        ok = radio.saveStation(id, request.value("name").toString(), request.value("url").toString());
        if (ok) {
            radioSavePending_ = true;
            radioSaved_ = connect(&radio, &RadioPlayer::stationSaved, this, [this, &radio](const QVariantMap& station, bool valid) {
                finishRadioSave(station, valid, valid ? QString{} : radio.error());
            });
            radioDestroyed_ = connect(&radio, &QObject::destroyed, this, [this] {
                finishRadioSave({}, false, QCoreApplication::translate("Headless", "The channel was removed while checking the station."));
            });
            return;
        }
    } else if (action == "remove") ok = radio.removeStation(request.value("id").toString());
    else if (action == "play") ok = radio.play(request.value("id").toString());
    else if (action == "stop") ok = radio.stop();
    else { write(response("radio", false, {}, QCoreApplication::translate("Headless", "Unknown radio action."))); return; }
    write(response("radio", ok, {}, ok ? QString{} : radio.error()));
}

void HeadlessController::finishRadioSave(const QVariantMap& station, bool ok, const QString& error) {
    if (!std::exchange(radioSavePending_, false)) return;
    disconnect(radioSaved_); disconnect(radioDestroyed_);
    write(response("radio", ok, ok ? QJsonValue(QJsonObject::fromVariantMap(station)) : QJsonValue{}, error));
}

int Headless::run(int argc, char** argv) {
#ifdef Q_OS_WIN
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
#endif
#ifdef Q_OS_DARWIN
    // Keychain completion callbacks use the main dispatch queue, which the
    // default UNIX dispatcher of QCoreApplication does not service.
    qputenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION", "1");
#endif
    QCoreApplication app(argc, argv);
#ifdef Q_OS_DARWIN
    // Qt consults this switch again for every worker thread. Only the main
    // dispatcher needs Core Foundation; FFmpeg's media workers use UNIX events.
    qputenv("QT_EVENT_DISPATCHER_CORE_FOUNDATION", "0");
#endif
    app.setApplicationName("SquadSpeak");
    app.setOrganizationName("SquadSpeak");
    app.setApplicationVersion(QStringLiteral(SQUADSPEAK_VERSION));
    VoiceSession session;
    QCommandLineParser bootstrapParser;
    addHeadlessOptions(bootstrapParser);
    (void)bootstrapParser.parse(app.arguments());
    const auto configPath = bootstrapParser.isSet("config") ? bootstrapParser.value("config") : qEnvironmentVariable("SQUADSPEAK_CONFIG");
    QJsonObject configuration;
    QString error;
    const auto readConfig = [&] {
        return configPath.isEmpty() ? loadDefaultConfiguration(configuration, error)
            : readConfiguration(configPath, configuration, error);
    };
    const bool explicitLanguage = bootstrapParser.isSet("language") || qEnvironmentVariableIsSet("SQUADSPEAK_LANGUAGE");
    if (!explicitLanguage) (void)readConfig();
    const auto language = bootstrapParser.isSet("language") ? bootstrapParser.value("language")
        : qEnvironmentVariableIsSet("SQUADSPEAK_LANGUAGE") ? qEnvironmentVariable("SQUADSPEAK_LANGUAGE")
        : configuration.contains("language") ? configuration.value("language").toString() : QStringLiteral("en");
    if (!supportedLanguage(language)) {
        qCritical("%s", qUtf8Printable(QCoreApplication::translate("VoiceSession", "Choose a language from the list.")));
        return 2;
    }
    QTranslator languageCatalog, baseCatalog;
    if (language != "en") {
        for (const auto& entry : {std::pair{&languageCatalog, "squadspeak"}, std::pair{&baseCatalog, "qtbase"}}) {
            const auto path = ":/i18n/" + QString::fromLatin1(entry.second) + "_" + language + ".qm";
            if (entry.first == &baseCatalog && !QFile::exists(path)) continue;
            if (!entry.first->load(path)) {
                qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Could not load language: %1").arg(path)));
                return 1;
            }
            app.installTranslator(entry.first);
        }
    }
    QCommandLineParser parser;
    addHeadlessOptions(parser);
    for (const auto* forbidden : {"settings", "recording-test", "smoke-test", "screenshot"}) {
        if (bootstrapParser.isSet(forbidden)) {
            qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "The %1 option is incompatible with --headless.").arg("--" + QString::fromLatin1(forbidden))));
            return 2;
        }
    }
    parser.process(app);
    if (!parser.positionalArguments().isEmpty()) { qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Unexpected positional arguments. Use --help."))); return 2; }
    if (parser.isSet("config") && configPath.isEmpty()) { qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "--config / -f requires a non-empty path."))); return 2; }
    // Load once in the selected language. Only a failed config-language probe is
    // repeated so its diagnostic can use an earlier default file's language.
    if (explicitLanguage || !error.isEmpty()) {
        configuration = {}; error.clear();
        if (!readConfig()) { qCritical("%s", qUtf8Printable(error)); return 2; }
    }
    configuration.remove("language");
    for (const auto& setting : {std::pair{"CHANNEL_NAME", "channelName"}, {"BOT_NAME", "botName"}, {"PORT", "port"},
             {"MESSAGE_TTL", "messageTtl"}, {"REQUESTS", "requestsAllowed"}, {"APPROVED_CLIENTS", "approvedClients"},
             {"BLOCKED_CLIENTS", "blockedClients"}, {"SETTINGS_FILE", "settingsFile"}, {"IDENTITY_FILE", "identityFile"}}) {
        const auto variable = QByteArray("SQUADSPEAK_") + setting.first;
        if (qEnvironmentVariableIsSet(variable.constData())
            && !setTextOption(configuration, setting.second, qEnvironmentVariable(variable.constData()), error)) {
            qCritical("%s: %s", variable.constData(), qUtf8Printable(error)); return 2;
        }
    }
    for (const auto& setting : {std::pair{"channel-name", "channelName"}, {"bot-name", "botName"}, {"port", "port"},
             {"message-ttl", "messageTtl"}, {"requests", "requestsAllowed"}, {"settings-file", "settingsFile"}, {"identity-file", "identityFile"}})
        if (parser.isSet(setting.first) && !setTextOption(configuration, setting.second, parser.value(setting.first), error)) {
            qCritical("%s", qUtf8Printable(error)); return 2;
        }
    if (configuration.value("requestsAllowed").isBool() && !configuration.value("requestsAllowed").toBool()) {
        qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Headless admission is always enabled for non-banned devices; requests off is not supported."))); return 2;
    }
    for (const auto& entry : {std::pair{"approve", "approvedClients"}, std::pair{"ban", "blockedClients"}})
        if (parser.isSet(entry.first)) configuration.insert(entry.second, QJsonArray::fromStringList(parser.values(entry.first)));
    if (parser.isSet("password") && parser.isSet("password-file")) {
        qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Choose --password or --password-file, not both."))); return 2;
    }
    if (parser.isSet("password") || parser.isSet("password-file")) {
        configuration.remove("password"); configuration.remove("passwordFile");
        const bool direct = parser.isSet("password");
        configuration.insert(direct ? "password" : "passwordFile", parser.value(direct ? "password" : "password-file"));
    } else if (qEnvironmentVariableIsSet("SQUADSPEAK_PASSWORD") || qEnvironmentVariableIsSet("SQUADSPEAK_PASSWORD_FILE")) {
        if (qEnvironmentVariableIsSet("SQUADSPEAK_PASSWORD") && qEnvironmentVariableIsSet("SQUADSPEAK_PASSWORD_FILE")) {
            qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Choose SQUADSPEAK_PASSWORD or SQUADSPEAK_PASSWORD_FILE, not both."))); return 2;
        }
        configuration.remove("password"); configuration.remove("passwordFile");
        const bool direct = qEnvironmentVariableIsSet("SQUADSPEAK_PASSWORD");
        configuration.insert(direct ? "password" : "passwordFile", qEnvironmentVariable(direct ? "SQUADSPEAK_PASSWORD" : "SQUADSPEAK_PASSWORD_FILE"));
    }
    const bool passwordSet = configuration.contains("password") || configuration.contains("passwordFile");
    if (configuration.contains("password") && configuration.contains("passwordFile")) {
        qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Choose password or passwordFile in one configuration layer, not both."))); return 2;
    }
    QString password;
    if (configuration.contains("passwordFile")) {
        const auto path = configuration.take("passwordFile");
        if (!path.isString() || path.toString().isEmpty()) { qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "passwordFile must be a non-empty path."))); return 2; }
        QFile file(path.toString());
        if (!QFileInfo(path.toString()).isFile() || !file.open(QIODevice::ReadOnly) || file.size() > 1026) {
            qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Password file could not be read or is too large."))); return 2;
        }
        auto bytes = file.read(1027);
        if (file.error() != QFileDevice::NoError || bytes.size() > 1026) {
            bytes.fill(0);
            qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Password file could not be read or is too large."))); return 2;
        }
        if (bytes.endsWith('\n')) { bytes.chop(1); if (bytes.endsWith('\r')) bytes.chop(1); }
        password = QString::fromUtf8(bytes);
        const bool valid = password.toUtf8() == bytes;
        bytes.fill(0);
        if (!valid || !LocalChannel::validHostPassword(password)) {
            qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Password file must contain at most 1024 UTF-8 bytes without control characters."))); return 2;
        }
    } else if (configuration.contains("password")) {
        const auto value = configuration.take("password");
        if (!value.isString() || !LocalChannel::validHostPassword(value.toString())) {
            qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Password must be a string of at most 1024 UTF-8 bytes without control characters."))); return 2;
        }
        password = value.toString();
    }
    const auto storage = configuration.take("settingsFile");
    if (!storage.isUndefined() && (!storage.isString() || storage.toString().isEmpty())) {
        qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "settingsFile must be a non-empty path."))); return 2;
    }
    auto settingsFile = storage.toString();
    const auto identityPath = configuration.take("identityFile");
    if (!identityPath.isUndefined() && (!identityPath.isString() || identityPath.toString().isEmpty())) {
        qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "identityFile must be a non-empty path."))); return 2;
    }
    if (settingsFile.isEmpty()) settingsFile = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/server";
    if (!QDir().mkpath(QFileInfo(settingsFile).absolutePath())) { qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "The settings directory could not be created."))); return 1; }
    QLockFile lock(settingsFile + ".instance.lock");
    if (!lock.tryLock(0)) { qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "This SquadSpeak profile is already running or cannot be locked."))); return 1; }
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) { qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Standard input could not be opened."))); return 1; }
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly)) { qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Standard output could not be opened."))); return 1; }
    try {
        std::optional<TlsIdentity> identity;
        std::unique_ptr<QLockFile> identityLock;
        if (!identityPath.isUndefined()) {
            const bool fresh = !QFileInfo::exists(settingsFile + ".channel.json")
                && !QFileInfo::exists(settingsFile + ".channel.json.chat")
                && !QFileInfo::exists(settingsFile + ".channel.json.chat.sqlite");
            identity = TlsIdentity::loadFile(identityPath.toString(), fresh);
            identityLock = std::make_unique<QLockFile>(QFileInfo(identityPath.toString()).canonicalFilePath() + ".lock");
            if (!identityLock->tryLock(0)) throw std::runtime_error(QT_TRANSLATE_NOOP("Headless", "This identity file is already in use or cannot be locked."));
        }
        License license(License::storageDirectory(),
            License::distributionProduct(), QUrl("https://api.github.com/graphql"), {}, {}, identityPath.toString());
        QObject::connect(&license, &License::changed, &session, [&] { session.setSupporterEnabled(license.active()); });
        LocalChannel channel(session, settingsFile + ".channel.json", std::move(identity));
        RadioPlayer radio(settingsFile + ".radio.json");
        if (!radio.bind(channel)) throw std::runtime_error(QT_TRANSLATE_NOOP("Headless", "Radio channels could not be initialized."));
        HeadlessController controller(app, channel, radio, license, input, output);
        bool startupReady = false, startupAttempted = false;
        const auto startHost = [&] {
            if (!startupReady || startupAttempted) return;
            if (configuration.value("messageLifetimeDays").toInt() > 30 && license.busy() && !license.active()) return;
            startupAttempted = true;
            if (!configuration.isEmpty() && !channel.configureHost(configuration)) {
                qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Invalid host configuration: %1").arg(channel.status())));
                app.exit(2); return;
            }
            if (!channel.initialize(true)) {
                qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Headless host could not start: %1").arg(channel.status())));
                app.exit(1);
            }
        };
        QObject::connect(&license, &License::changed, &app, startHost);
        QTimer::singleShot(0, &app, [&] {
            license.refresh();
            if (!passwordSet) { startupReady = true; startHost(); return; }
            QObject::connect(&channel, &LocalChannel::hostPasswordSaved, &app, [&](bool success) {
                if (success) { startupReady = true; startHost(); }
                else { qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Channel password could not be saved."))); app.exit(1); }
            }, Qt::SingleShotConnection);
            const auto accepted = channel.setHostPassword(password);
            password.fill(QChar(0)); password.clear();
            if (!accepted) { qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Invalid channel password: %1").arg(channel.status()))); app.exit(2); }
        });
        QTimer::singleShot(15000, &app, [&] {
            if (!channel.hosting()) {
                qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "Headless host could not initialize: %1").arg(channel.status())));
                app.exit(1);
            }
        });
        return app.exec();
    } catch (const std::exception& error) {
        qCritical("%s", qUtf8Printable(QCoreApplication::translate("Headless", "SquadSpeak headless could not start: %1").arg(QCoreApplication::translate("Headless", error.what()))));
        return 1;
    }
}
