#include "headless.hpp"
#include "local_channel.hpp"
#include "radio_player.hpp"
#include "radio_station.hpp"
#include "voice_session.hpp"
#include "tls_identity.hpp"
#include "license.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QHash>
#include <QTemporaryDir>
#include <QTest>
#include <QSslSocket>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTcpServer>
#include <qtkeychain/keychain.h>
#include <future>
#include <barrier>
#ifndef Q_OS_WIN
#include <sys/stat.h>
#include <unistd.h>
#endif

class HeadlessTests final : public QObject {
    Q_OBJECT
    const QString executable_ = qEnvironmentVariable("SQUAD_TEST_APP", QStringLiteral(SQUAD_HEADLESS_APP));
    QHash<QString, bool> temporaryIdentitySlots_;
    QString registerProfile(const QString& profile) {
        const auto absolute = QFileInfo(profile + ".channel.json").absoluteFilePath();
        const auto slot = QStringLiteral("device/") + QString::fromLatin1(
            QCryptographicHash::hash(absolute.toUtf8(), QCryptographicHash::Sha256).toHex());
        temporaryIdentitySlots_.insert(slot, false);
        return slot;
    }
private slots:
    void initTestCase() {
        qputenv("QT_SSL_USE_TEMPORARY_KEYCHAIN", "1");
        QVERIFY(QSslSocket::supportsSsl());
    }
    void cleanupTestCase() {
        QStringList failures;
        for (const auto& [slot, created] : temporaryIdentitySlots_.asKeyValueRange()) {
#ifdef Q_OS_MACOS
            // Delete only this test's item. Unlike reading the secret, the
            // system keychain utility supports cleanup across executables.
            QProcess remove;
            remove.start("/usr/bin/security", {"delete-generic-password", "-s", "SquadSpeak", "-a", slot});
            if (!remove.waitForFinished(5000) || remove.exitStatus() != QProcess::NormalExit
                || (remove.exitCode() != 0 && (created || remove.exitCode() != 44))) {
                failures.append(slot + ": test identity could not be removed");
                continue;
            }
            QProcess find;
            find.start("/usr/bin/security", {"find-generic-password", "-s", "SquadSpeak", "-a", slot});
            if (!find.waitForFinished(5000) || find.exitStatus() != QProcess::NormalExit || find.exitCode() != 44)
                failures.append(slot + ": test identity remains in the keychain");
#else
            QKeychain::DeletePasswordJob job("SquadSpeak");
            job.setAutoDelete(false); job.setKey(slot); job.setInsecureFallback(false);
            QSignalSpy finished(&job, &QKeychain::Job::finished); job.start();
            if (!(finished.count() > 0 || finished.wait(5000))
                || (job.error() != QKeychain::NoError && (created || job.error() != QKeychain::EntryNotFound))) {
                failures.append(slot + ": " + job.errorString());
                continue;
            }
            QKeychain::ReadPasswordJob read("SquadSpeak");
            read.setAutoDelete(false); read.setKey(slot); read.setInsecureFallback(false);
            QSignalSpy readFinished(&read, &QKeychain::Job::finished); read.start();
            if (!(readFinished.count() > 0 || readFinished.wait(5000)) || read.error() != QKeychain::EntryNotFound)
                failures.append(slot + ": test identity remains readable");
#endif
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join('\n')));
    }
    void commandLineHelpListsServerSettingsWithoutStartingServices() {
        QTemporaryDir directory;
        const auto profile = directory.filePath("server");
        registerProfile(profile);
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "__headless_platform_must_not_load__");
        process.setProcessEnvironment(environment);
        process.start(executable_, {"--headless", "--settings-file", profile, "--help"});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        const auto help = process.readAllStandardOutput();
        for (const auto* option : {"--config", "--channel-name", "--bot-name", "--port", "--message-ttl", "--requests", "--approve", "--ban", "--password-file", "--identity-file"})
            QVERIFY2(help.contains(option), option);
        QVERIFY(!QFile::exists(profile + ".channel.json"));
        QVERIFY(!QFile::exists(profile + ".instance.lock"));
    }
    void headlessLanguageOptionLoadsCatalogBeforeHelp_data() {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("expected");
        QTest::newRow("german") << QString("de") << QString::fromUtf8("Wähle eine Sprache aus der Liste.");
        QTest::newRow("japanese") << QString("ja") << QString::fromUtf8("リストから言語を選択してください.");
        QTest::newRow("arabic") << QString("ar") << QString::fromUtf8("اختر لغة من القائمة.");
    }
    void headlessLanguageOptionLoadsCatalogBeforeHelp() {
        QFETCH(QString, language); QFETCH(QString, expected);
        QTemporaryDir directory;
        const auto profile = directory.filePath("server");
        registerProfile(profile);
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "__headless_platform_must_not_load__");
        process.setProcessEnvironment(environment);
        process.start(executable_, {"--headless", "--language", language, "--settings-file", profile, "--help"});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        const auto help = QString::fromUtf8(process.readAllStandardOutput());
        QVERIFY2(help.contains(expected), qPrintable(help));
        QVERIFY(!QFile::exists(profile + ".channel.json"));
        QVERIFY(!QFile::exists(profile + ".instance.lock"));
    }
    void headlessLanguagePrecedenceAndValidation_data() {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<QString>("environmentLanguage");
        QTest::addColumn<QByteArray>("configuration");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<QString>("expected");
        const auto german = QString::fromUtf8("Wähle eine Sprache aus der Liste.");
        const auto french = QString::fromUtf8("Choisissez une langue dans la liste.");
        const auto english = QString("Choose a language from the list.");
        QTest::newRow("argument") << QStringList{"--language", "de", "--headless", "--help"} << QString("fr") << QByteArray("language=en\n") << 0 << german;
        QTest::newRow("environment") << QStringList{"--headless", "--help"} << QString("fr") << QByteArray("language=de\n") << 0 << french;
        QTest::newRow("properties") << QStringList{"--headless", "--help"} << QString() << QByteArray("language=de\n") << 0 << german;
        QTest::newRow("json") << QStringList{"--headless", "--help"} << QString() << QByteArray("{\"language\":\"de\"}") << 0 << german;
        QTest::newRow("default-english") << QStringList{"--headless", "--help"} << QString() << QByteArray() << 0 << english;
        QTest::newRow("repeated-option") << QStringList{"--headless", "--language", "fr", "--language=de", "--help"} << QString() << QByteArray() << 0 << german;
        QTest::newRow("end-of-options") << QStringList{"--headless", "--help", "--", "--language=de", "--settings"} << QString() << QByteArray() << 0 << english;
        QTest::newRow("option-like-value") << QStringList{"--headless", "--channel-name", "--settings", "--help"} << QString() << QByteArray() << 0 << english;
        QTest::newRow("unknown-language") << QStringList{"--headless", "--language=xx", "--help"} << QString() << QByteArray() << 2 << english;
        QTest::newRow("empty-argument") << QStringList{"--headless", "--language=", "--help"} << QString("de") << QByteArray() << 2 << english;
        QTest::newRow("empty-environment") << QStringList{"--headless", "--help"} << QString("") << QByteArray("language=de\n") << 2 << english;
        QTest::newRow("invalid-json-type") << QStringList{"--headless", "--help"} << QString() << QByteArray("{\"language\":false}") << 2 << english;
        QTest::newRow("empty-json") << QStringList{"--headless", "--help"} << QString() << QByteArray("{\"language\":\"\"}") << 2 << english;
        QTest::newRow("malformed-config") << QStringList{"--headless", "--language=de"} << QString() << QByteArray("{\n") << 2 << QString::fromUtf8("Die Host-Voreinstellung muss ein JSON-Objekt sein.");
        QTest::newRow("unicode-diagnostic") << QStringList{"--headless", "--language=ja"} << QString() << QByteArray("{\n") << 2 << QString::fromUtf8("ホストプリセット");
    }
    void headlessLanguagePrecedenceAndValidation() {
        QFETCH(QStringList, arguments); QFETCH(QString, environmentLanguage);
        QFETCH(QByteArray, configuration); QFETCH(int, exitCode); QFETCH(QString, expected);
        QTemporaryDir directory;
        const auto profile = directory.filePath("server");
        const auto path = directory.filePath("application.properties");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(configuration), configuration.size()); file.close();
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "__headless_platform_must_not_load__");
        environment.insert("LANG", "de_DE.UTF-8");
        environment.insert("SQUADSPEAK_CONFIG", path);
        environment.remove("SQUADSPEAK_LANGUAGE");
        if (!environmentLanguage.isNull()) environment.insert("SQUADSPEAK_LANGUAGE", environmentLanguage);
        arguments.prepend(profile); arguments.prepend("--settings-file");
        QProcess process; process.setProcessEnvironment(environment);
        process.start(executable_, arguments);
        QVERIFY(process.waitForFinished(5000));
        const auto text = QString::fromUtf8(process.readAllStandardOutput()) + QString::fromUtf8(process.readAllStandardError());
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), exitCode);
        QVERIFY2(text.contains(expected), qPrintable(text));
        QVERIFY(!QFile::exists(profile));
        for (const auto* suffix : {".channel.json", ".session.json", ".instance.lock"})
            QVERIFY(!QFile::exists(profile + suffix));
    }

    void translatedHeadlessCommandsKeepTheirWireFormat() {
        QTemporaryDir directory;
        const auto profile = directory.filePath("server");
        registerProfile(profile);
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "__headless_platform_must_not_load__");
        process.setProcessEnvironment(environment);
        process.start(executable_, {"--headless", "--settings-file", profile, "--language=de"});
        QVERIFY(process.waitForStarted(5000));
        process.write("{\"command\":\"unknown\"}\n{\"command\":\"help\"}\n{\"command\":\"quit\"}\n");
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        const auto lines = process.readAllStandardOutput().trimmed().split('\n');
        QCOMPARE(lines.size(), 3);
        const auto failure = QJsonDocument::fromJson(lines[0]).object();
        QCOMPARE(failure.value("command").toString(), QString("unknown"));
        QCOMPARE(failure.value("ok").toBool(true), false);
        QVERIFY2(failure.value("error").toString().contains("Unbekannt"), lines[0].constData());
        const auto help = QJsonDocument::fromJson(lines[1]).object();
        QCOMPARE(help.value("command").toString(), QString("help"));
        QCOMPARE(help.value("ok").toBool(), true);
        QVERIFY(help.value("data").toObject().value("commands").toString().contains("status, channels, configure"));
        QCOMPARE(QJsonDocument::fromJson(lines[2]).object().value("command").toString(), QString("quit"));
        QVERIFY(!QFile::exists(profile));
        QVERIFY(!QFile::exists(profile + ".session.json"));
        QVERIFY(!QFile::exists(profile + ".instance.lock"));
    }

    void desktopStartupUsesSelectedLanguage_data() {
        QTest::addColumn<QString>("suffix"); QTest::addColumn<QString>("expected"); QTest::addColumn<QByteArray>("contents");
        QTest::newRow("audio") << QString() << QString::fromUtf8("Audioeinstellungen konnten nicht gelesen werden") << QByteArray("[invalid");
        QTest::newRow("radio") << QString(".radio.json") << QString::fromUtf8("Gespeicherte Radiosender sind ungültig.") << QByteArray("{");
        QTest::newRow("channel") << QString(".channel.json") << QString::fromUtf8("Ungültige gespeicherte Kanalberechtigungen.") << QByteArray("{");
    }
    void desktopStartupUsesSelectedLanguage() {
        QFETCH(QString, suffix); QFETCH(QString, expected); QFETCH(QByteArray, contents);
        QTemporaryDir directory;
        const auto profile = directory.filePath("audio.ini");
        VoiceSession session(profile + ".session.json");
        QVERIFY(session.setLanguage("de"));
        QFile invalid(profile + suffix);
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        QCOMPARE(invalid.write(contents), contents.size());
        invalid.close();
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        process.setProcessEnvironment(environment);
        process.start(executable_, {"--smoke-test", "--settings-file", profile});
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 1);
        const auto diagnostic = QString::fromUtf8(process.readAllStandardError());
        QVERIFY2(diagnostic.contains(expected), qPrintable(diagnostic));
        if (suffix == ".radio.json") QVERIFY(!QFile::exists(profile + ".channel.json"));
        QVERIFY(invalid.open(QIODevice::ReadOnly));
        QCOMPARE(invalid.readAll(), contents);
        QVERIFY(!QFile::exists(profile + ".instance.lock"));
    }
    void desktopSessionFailureUsesSavedLanguage_data() {
        QTest::addColumn<QString>("key"); QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<QString>("expectedKey");
        for (const auto* key : {"version", "userName", "muted", "theme", "palette", "avatar", "animatedAvatars",
                "deafened", "eventSounds", "remotePttKeyHeld", "remotePttKeyCode", "pushToTalk", "pttButtonHeld",
                "pttKeyHeld", "pttKeyCode", "pttKeyCodes", "remotePttKeyCodes", "pttRevision"})
            QTest::newRow(key) << QString(key) << QJsonValue(QJsonValue::Null) << QString(key);
        QTest::newRow("empty-name") << QString("userName") << QJsonValue("") << QString("userName");
        QTest::newRow("unknown-language") << QString("language") << QJsonValue("../../de") << QString("language");
        QTest::newRow("language-type") << QString("language") << QJsonValue(false) << QString("language");
    }
    void desktopSessionFailureUsesSavedLanguage() {
        QFETCH(QString, key); QFETCH(QJsonValue, value); QFETCH(QString, expectedKey);
        QTemporaryDir directory;
        const auto profile = directory.filePath("audio.ini");
        VoiceSession session(profile + ".session.json");
        QVERIFY(session.setLanguage("de"));
        QFile file(profile + ".session.json"); QVERIFY(file.open(QIODevice::ReadOnly));
        auto object = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        object.insert(key, value);
        const auto bytes = QJsonDocument(object).toJson();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(bytes), bytes.size()); file.close();
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        process.setProcessEnvironment(environment);
        process.start(executable_, {"--smoke-test", "--settings-file", profile});
        QVERIFY(process.waitForFinished(15000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit); QCOMPARE(process.exitCode(), 1);
        const auto diagnostic = QString::fromUtf8(process.readAllStandardError());
        const auto expected = key == "language" ? QString("Saved setting is invalid: language")
            : QString::fromUtf8("Gespeicherte Einstellung ist ungültig: ") + expectedKey;
        QVERIFY2(diagnostic.contains(expected), qPrintable(diagnostic));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes);
        for (const auto* suffix : {"", ".instance.lock", ".channel.json", ".radio.json"})
            QVERIFY(!QFile::exists(profile + suffix));
    }
    void desktopHelpAndProfileLockUseSavedLanguageWithoutStartingServices() {
        QTemporaryDir directory;
        const auto profile = directory.filePath("audio.ini");
        VoiceSession session(profile + ".session.json");
        QVERIFY(session.setLanguage("de"));
        QFile file(profile + ".session.json"); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll(); file.close();
        QLockFile lock(profile + ".instance.lock"); QVERIFY(lock.tryLock(0));
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        process.setProcessEnvironment(environment);
        process.start(executable_, {"--settings-file", profile, "--help"});
        QVERIFY(process.waitForFinished(5000)); QCOMPARE(process.exitCode(), 0);
        const auto text = QString::fromUtf8(process.readAllStandardOutput());
        QVERIFY2(text.contains(QString::fromUtf8("Einstellungen beim Start öffnen.")), qPrintable(text));
        for (const auto* option : {"--settings", "--recording-test", "--settings-file", "--smoke-test", "--screenshot"})
            QVERIFY2(text.contains(option), option);
        process.start(executable_, {"--settings-file", profile, "--smoke-test"});
        QVERIFY(process.waitForFinished(5000)); QCOMPARE(process.exitCode(), 1);
        const auto error = QString::fromUtf8(process.readAllStandardError());
        QVERIFY2(error.contains("bereits"), qPrintable(error));
        QVERIFY(lock.isLocked()); lock.unlock();
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes);
        for (const auto* suffix : {"", ".instance.lock", ".channel.json", ".radio.json"})
            QVERIFY(!QFile::exists(profile + suffix));
    }
    void stalledOutputKeepsHostingAndResumesCompleteReplies_data() {
        QTest::addColumn<QString>("closure");
        QTest::addColumn<bool>("synchronousPipe");
        QTest::newRow("open-input") << QString() << false;
        QTest::newRow("closed-input") << QString("input") << false;
        QTest::newRow("closed-reader") << QString("reader") << false;
#ifdef Q_OS_WIN
        QTest::newRow("open-input-synchronous") << QString() << true;
        QTest::newRow("closed-input-synchronous") << QString("input") << true;
        QTest::newRow("closed-reader-synchronous") << QString("reader") << true;
#endif
    }
    void stalledOutputKeepsHostingAndResumesCompleteReplies() {
        QFETCH(QString, closure);
        QFETCH(bool, synchronousPipe);
        QTemporaryDir directory;
        QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost));
        const auto port = reservation.serverPort(); reservation.close();
        const auto profile = directory.filePath("server");
        QProcess process, sink;
        const auto cleanup = qScopeGuard([&] {
            for (auto* child : {&process, &sink}) if (child->state() != QProcess::NotRunning) {
                child->kill(); child->waitForFinished(5000);
            }
        });
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "__headless_platform_must_not_load__");
        process.setProcessEnvironment(environment);
#ifdef Q_OS_WIN
        HANDLE pipeReader = nullptr, pipeWriter = nullptr;
        const auto closePipe = qScopeGuard([&] {
            if (pipeReader && pipeReader != INVALID_HANDLE_VALUE) CloseHandle(pipeReader);
            if (pipeWriter && pipeWriter != INVALID_HANDLE_VALUE) CloseHandle(pipeWriter);
        });
        if (synchronousPipe) {
            QVERIFY(CreatePipe(&pipeReader, &pipeWriter, nullptr, 4096));
        } else {
            const auto name = QStringLiteral("\\\\.\\pipe\\squadspeak-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto* path = reinterpret_cast<const wchar_t*>(name.utf16());
            pipeReader = CreateNamedPipeW(path, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1, 0, 1024 * 1024, 0, nullptr);
            QVERIFY(pipeReader != INVALID_HANDLE_VALUE);
            pipeWriter = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            QVERIFY(pipeWriter != INVALID_HANDLE_VALUE);
            QVERIFY(ConnectNamedPipe(pipeReader, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED);
        }
        // QProcess-to-QProcess pipes make both endpoints inheritable. A writer
        // retaining a read handle cannot observe the real reader's closure.
        // Exercise Qt-style overlapped and shell-style synchronous writes with
        // only the intended endpoint inherited by each child.
        QVERIFY(SetHandleInformation(pipeWriter, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
        process.setCreateProcessArgumentsModifier([&](QProcess::CreateProcessArguments* args) {
            args->startupInfo->hStdOutput = pipeWriter;
        });
        sink.setCreateProcessArgumentsModifier([&](QProcess::CreateProcessArguments* args) {
            args->startupInfo->hStdInput = pipeReader;
        });
#else
        process.setStandardOutputProcess(&sink);
        QVERIFY(!synchronousPipe);
#endif
        sink.setProgram(QStringLiteral(SQUAD_TEST_PYTHON));
        sink.setArguments({"-u", "-c", QStringLiteral(R"PY(
import json, pathlib, sys, time
root = pathlib.Path(sys.argv[1])
with (root / 'replies').open('wb') as replies:
    for line in sys.stdin.buffer:
        replies.write(line); replies.flush()
        message = json.loads(line)
        if message.get('command') == 'status':
            (root / 'ready.tmp').write_bytes(line)
            (root / 'ready.tmp').replace(root / 'ready')
            if message.get('data', {}).get('hosting'):
                while not (root / 'resume').exists(): time.sleep(0.01)
)PY"), directory.path()});
        QStringList arguments{"--headless", "--settings-file", profile, "--port", QString::number(port)};
#ifndef Q_OS_WIN
        arguments << "--identity-file" << directory.filePath("identity.pem");
#else
        const auto slot = registerProfile(profile);
#endif
        process.start(executable_, arguments); QVERIFY(process.waitForStarted(5000));
#ifdef Q_OS_WIN
        CloseHandle(pipeWriter); pipeWriter = nullptr;
        QVERIFY(SetHandleInformation(pipeReader, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
#endif
        sink.start(); QVERIFY(sink.waitForStarted(5000));
#ifdef Q_OS_WIN
        CloseHandle(pipeReader); pipeReader = nullptr;
#endif
        const auto ready = directory.filePath("ready");
        QJsonObject state;
        bool awaitingStatus = false;
        QTRY_VERIFY2_WITH_TIMEOUT(([&] {
            if (state.value("hosting").toBool()) return true;
            QFile reply(ready);
            if (reply.open(QIODevice::ReadOnly)) {
                state = QJsonDocument::fromJson(reply.readAll()).object().value("data").toObject();
                reply.close(); reply.remove(); awaitingStatus = false;
                if (state.value("hosting").toBool()) return true;
            }
            if (!awaitingStatus) { process.write("{\"command\":\"status\"}\n"); awaitingStatus = true; }
            return false;
        })(), (process.readAllStandardError() + sink.readAllStandardError() + QJsonDocument(state).toJson()).constData(), 18000);
#ifdef Q_OS_WIN
        temporaryIdentitySlots_[slot] = true;
#endif
        const auto id = state.value("ownId").toString();
        QVERIFY(!id.isEmpty());
        VoiceSession session(directory.filePath("reader.session"));
        LocalChannel reader(session, directory.filePath("reader.channel"), TlsIdentity::create());
        QVERIFY(reader.openChat(id, "127.0.0.1", port)); QTRY_VERIFY(reader.chatReady());
        // Qt's Windows pipe holds 1 MiB; exceed it as well as smaller POSIX
        // pipes so EOF/reader-close cases actually happen under backpressure.
#ifdef Q_OS_WIN
        const int helpCount = synchronousPipe ? 256 : 2048;
#else
        constexpr int helpCount = 256;
#endif
        auto commands = QByteArray("{\"command\":\"help\"}\n").repeated(helpCount);
        if (closure == "input") commands.append("{\"command\":\"quit\"}\n");
        QCOMPARE(process.write(commands), commands.size()); QVERIFY(process.waitForBytesWritten(5000));
        if (closure == "input") process.closeWriteChannel();
        QTest::qWait(250);
        QVERIFY(reader.sendChat("The host keeps serving while stdout is full."));
        QTRY_COMPARE(reader.messages().size(), 1);
        QCOMPARE(reader.messages().first().toMap().value("text").toString(), QString("The host keeps serving while stdout is full."));
        if (closure == "reader") {
            sink.kill(); QVERIFY(sink.waitForFinished(5000));
            QVERIFY(process.waitForFinished(5000));
            QVERIFY(process.exitCode() != 0 || process.exitStatus() == QProcess::CrashExit);
            return;
        }
        QFile resume(directory.filePath("resume")); QVERIFY(resume.open(QIODevice::WriteOnly)); resume.close();
        if (closure != "input") process.write("{\"command\":\"quit\"}\n");
        QVERIFY2(process.waitForFinished(60000), process.readAllStandardError().constData());
        QCOMPARE(process.exitCode(), 0); QVERIFY(sink.waitForFinished(5000)); QCOMPARE(sink.exitCode(), 0);
        QFile replies(directory.filePath("replies")); QVERIFY(replies.open(QIODevice::ReadOnly));
        int help = 0, quit = 0;
        while (!replies.atEnd()) {
            QJsonParseError error;
            const auto reply = QJsonDocument::fromJson(replies.readLine(), &error).object();
            QCOMPARE(error.error, QJsonParseError::NoError); QVERIFY(reply.value("ok").toBool());
            if (reply.value("command") == "help") ++help;
            if (reply.value("command") == "quit") ++quit;
        }
        QCOMPARE(help, helpCount); QCOMPARE(quit, 1);
    }
    void productionServerBecomesReadyWithoutGuiOrAudio_data() {
        QTest::addColumn<QString>("passwordSource");
        QTest::addColumn<bool>("fileIdentity");
        for (const auto* source : {"file", "properties", "environment", "argument"}) {
            QTest::newRow(source) << QString(source) << false;
#ifndef Q_OS_WIN
            QTest::newRow(qPrintable(QString(source) + "-identity-file")) << QString(source) << true;
#endif
        }
    }
    void productionServerBecomesReadyWithoutGuiOrAudio() {
        QFETCH(QString, passwordSource);
        QFETCH(bool, fileIdentity);
        QTemporaryDir directory;
        const auto identityPath = directory.filePath("identity.pem");
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost));
        const auto port = reservation.serverPort();
        reservation.close();
        const auto profile = directory.filePath("production.ini");
        const auto identitySlot = registerProfile(profile);
        VoiceSession readerSession(directory.filePath("reader.session"));
        LocalChannel reader(readerSession, directory.filePath("reader.channel"), TlsIdentity::create());
        const auto bannedId = TlsIdentity::create().id();
        const auto preset = directory.filePath("server-preset.json");
        QFile configuration(preset);
        QVERIFY(configuration.open(QIODevice::WriteOnly));
        configuration.write(QJsonDocument(QJsonObject{{"channelName", "Preset name"}, {"port", 1},
            {"requestsAllowed", true}, {"messageLifetimeDays", 30}, {"password", "overridden-secret"}}).toJson());
        configuration.close();
        if (passwordSource == "properties") {
            QVERIFY(configuration.open(QIODevice::WriteOnly | QIODevice::Truncate));
            configuration.write("channelName=Preset name\nport=1\nrequestsAllowed=true\nmessageLifetimeDays=30\npassword=test-channel-secret\n");
            if (fileIdentity) configuration.write("identityFile=identity.pem\n");
            configuration.close();
        }
        const auto passwordPath = directory.filePath("channel-password.txt");
        QFile passwordFile(passwordPath); QVERIFY(passwordFile.open(QIODevice::WriteOnly));
        passwordFile.write("test-channel-secret\n"); passwordFile.close();
        for (const auto& path : {profile, profile + ".session.json"}) {
            QFile personal(path); QVERIFY(personal.open(QIODevice::WriteOnly));
            personal.write("Personal data must not be read or changed.");
        }
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "__headless_platform_must_not_load__");
        environment.insert("SQUADSPEAK_BOT_NAME", "Environment owl");
        if (fileIdentity) {
            environment.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-squadspeak-test-bus");
            if (passwordSource == "environment") environment.insert("SQUADSPEAK_IDENTITY_FILE", identityPath);
            if (passwordSource == "argument") environment.insert("SQUADSPEAK_IDENTITY_FILE", directory.filePath("overridden/missing.pem"));
        }
        if (passwordSource == "environment") environment.insert("SQUADSPEAK_PASSWORD", "test-channel-secret");
        process.setProcessEnvironment(environment);
        QStringList arguments{"--headless", "--settings-file", profile,
            "--config", preset, "--channel-name", "Quiet server", "--bot-name", "Quiet owl", "--port", QString::number(port),
            "--message-ttl", "7d", "--requests", "on", "--approve", reader.ownId(), "--ban", bannedId};
        if (passwordSource == "file") arguments << "--password-file" << passwordPath;
        if (passwordSource == "argument") arguments << "--password" << "test-channel-secret";
        if (fileIdentity && (passwordSource == "file" || passwordSource == "argument")) arguments << "--identity-file" << identityPath;
        process.start(executable_, arguments);
        QVERIFY(process.waitForStarted(5000));
        const auto awaitReady = [&] {
            QJsonObject state;
            QElapsedTimer deadline; deadline.start();
            while (deadline.elapsed() < 18000 && process.state() != QProcess::NotRunning) {
                process.write("{\"command\":\"status\"}\n");
                process.waitForBytesWritten(1000);
                process.waitForReadyRead(250);
                while (process.canReadLine()) {
                    const auto reply = QJsonDocument::fromJson(process.readLine()).object();
                    if (reply.value("command") == "status") state = reply.value("data").toObject();
                }
                if (state.value("hosting").toBool()) break;
                QTest::qWait(100);
            }
            return state;
        };
        const auto state = awaitReady();
        QVERIFY(!QJsonDocument(state).toJson().contains("test-channel-secret"));
        const auto diagnostic = QJsonDocument(state).toJson() + process.readAllStandardError();
        QVERIFY2(state.value("ready").toBool() && state.value("hosting").toBool(), diagnostic.constData());
        QCOMPARE(state.value("port").toInt(), int(port));
        QVERIFY(state.value("hostParticipants").toArray().isEmpty());
        QVERIFY(state.value("hostClients").toArray().isEmpty());
        QCOMPARE(state.value("channel").toString(), QString("Quiet server"));
        QVERIFY(state.value("passwordProtected").toBool());
        QCOMPARE(state.value("admissionPolicy").toString(), QString("automatic-except-blocked"));
        const auto applied = state.value("configuration").toObject();
        QCOMPARE(applied.value("botName").toString(), QString("Quiet owl"));
        QCOMPARE(applied.value("messageLifetimeDays").toInt(), 7);
        QCOMPARE(applied.value("requestsAllowed").toBool(), true);
        QCOMPARE(applied.value("approvedClients").toArray(), QJsonArray{reader.ownId()});
        QCOMPARE(applied.value("blockedClients").toArray(), QJsonArray{bannedId});
        process.write(QJsonDocument(QJsonObject{{"command", "approve"}, {"id", reader.ownId()}}).toJson(QJsonDocument::Compact) + '\n');
        const auto hostId = state.value("ownId").toString();
        temporaryIdentitySlots_[identitySlot] = !fileIdentity;
#ifndef Q_OS_WIN
        if (fileIdentity) {
            struct stat metadata {};
            QCOMPARE(::stat(QFile::encodeName(identityPath).constData(), &metadata), 0);
            QCOMPARE(metadata.st_mode & 0777, mode_t(0600));
            QCOMPARE(metadata.st_uid, ::geteuid());
            QProcess duplicate;
            registerProfile(directory.filePath("duplicate"));
            duplicate.setProcessEnvironment(environment);
            duplicate.start(executable_, {"--headless", "--settings-file", directory.filePath("duplicate"), "--identity-file", identityPath});
            QVERIFY(duplicate.waitForFinished(5000)); QCOMPARE(duplicate.exitCode(), 1);
            QVERIFY(duplicate.readAllStandardError().contains("identity file is already in use"));
        }
#endif
        QVERIFY(reader.openChat(hostId, "127.0.0.1", port));
        QTRY_VERIFY(reader.passwordRequired());
        QVERIFY(!reader.chatReady());
        QVERIFY(reader.submitPassword("test-channel-secret", true));
        QTRY_VERIFY(reader.chatReady());
        process.write("{\"command\":\"chat\",\"text\":\"Headless process chat\"}\n");
        QTRY_COMPARE(reader.messages().size(), 1);
        QCOMPARE(reader.messages().first().toMap().value("text").toString(), QString("Headless process chat"));
        const auto announcement = reader.messages().first().toMap();
        QCOMPARE(announcement.value("name").toString(), QString("Quiet owl"));
        QCOMPARE(announcement.value("event").toMap().value("kind").toString(), QString("announcement"));
        QVERIFY(announcement.value("sender").toString() != hostId);
        QVERIFY(!reader.chatOnlineIds().contains(hostId));
        QJsonObject delivery;
        const auto delivered = [&] {
            while (process.canReadLine()) {
                const auto reply = QJsonDocument::fromJson(process.readLine()).object();
                if (reply.value("command") == "chat" && reply.value("data").toObject().value("delivered").toBool()) delivery = reply;
            }
            return delivery.value("ok").toBool();
        };
        QTRY_VERIFY(delivered());
        if (passwordSource == "file" && !fileIdentity) {
            RadioStation station;
            process.write(QJsonDocument(QJsonObject{{"command", "radio"}, {"action", "add"},
                {"name", "Server radio"}, {"url", station.url()}}).toJson(QJsonDocument::Compact) + '\n');
            QString stationId;
            QJsonObject stationReply;
            const auto stationAdded = [&] {
                while (process.canReadLine()) {
                    const auto reply = QJsonDocument::fromJson(process.readLine()).object();
                    if (reply.value("command") == "radio") {
                        stationReply = reply;
                        stationId = reply.value("data").toObject().value("id").toString();
                    }
                }
                return !stationReply.isEmpty();
            };
            QTRY_VERIFY_WITH_TIMEOUT(stationAdded(), 17000);
            QVERIFY2(!stationId.isEmpty(), (QJsonDocument(stationReply).toJson() + process.readAllStandardError()).constData());
            process.write(QJsonDocument(QJsonObject{{"command", "radio"}, {"action", "play"}, {"id", stationId}}).toJson(QJsonDocument::Compact) + '\n');
            const auto visibleMusic = [&] {
                return reader.chatBot().value("musicActive").toBool();
            };
            QTRY_VERIFY(visibleMusic());
            QVERIFY(!reader.chatOnlineIds().contains(hostId));
            process.write("{\"command\":\"radio\",\"action\":\"stop\"}\n");
            QTRY_VERIFY(!visibleMusic());
        }
        QVERIFY(reader.closeChat(hostId));
        process.write("{\"command\":\"quit\"}\n");
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        QFile database(profile + ".channel.json.chat.sqlite");
        QVERIFY(database.open(QIODevice::ReadOnly));
        const auto ciphertext = database.readAll();
        QVERIFY(!ciphertext.isEmpty());
        QVERIFY(!ciphertext.contains("Headless process chat"));
        database.close();
        QStringList restart{"--headless", "--settings-file", profile};
        environment.remove("SQUADSPEAK_BOT_NAME"); process.setProcessEnvironment(environment);
        if (fileIdentity) restart << "--identity-file" << identityPath;
        process.start(executable_, restart);
        QVERIFY(process.waitForStarted(5000));
        const auto restarted = awaitReady();
        QVERIFY2(restarted.value("hosting").toBool(), process.readAllStandardError().constData());
        QCOMPARE(restarted.value("ownId").toString(), hostId);
        QCOMPARE(restarted.value("configuration").toObject(), applied);
        QVERIFY(reader.openChat(hostId, "127.0.0.1", port));
        QTRY_VERIFY(reader.chatReady());
        QTRY_COMPARE(reader.messages().size(), 1);
        QCOMPARE(reader.messages().first().toMap().value("text").toString(), QString("Headless process chat"));
        process.write("{\"command\":\"quit\"}\n");
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        for (const auto& path : {profile, profile + ".session.json"}) {
            QFile personal(path); QVERIFY(personal.open(QIODevice::ReadOnly));
            QCOMPARE(personal.readAll(), QByteArray("Personal data must not be read or changed."));
        }
    }

    void propertiesEnvironmentAndArgumentsUseExecutableDirectoryAndPrecedence_data() {
        QTest::addColumn<bool>("bundle");
        QTest::newRow("executable") << false;
#ifdef Q_OS_MACOS
        QTest::newRow("app-bundle") << true;
#endif
    }

#ifndef Q_OS_WIN
    void identityFileCreationIsAtomicAcrossConcurrentCallers() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QString(240, 'k'));
        std::barrier start(4);
        std::vector<std::future<QString>> pending;
        for (int i = 0; i < 4; ++i) pending.push_back(std::async(std::launch::async, [&] {
            start.arrive_and_wait(); return TlsIdentity::loadFile(path, true).id();
        }));
        const auto id = pending.front().get();
        for (qsizetype i = 1; i < qsizetype(pending.size()); ++i) QCOMPARE(pending.at(i).get(), id);
        QCOMPARE(TlsIdentity::loadFile(path, false).id(), id);
        QCOMPARE(QDir(directory.path()).entryList({"*.tmp"}, QDir::Files | QDir::Hidden).size(), 0);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto before = file.readAll(); file.close();
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner));
        QCOMPARE(TlsIdentity::loadFile(path, false).id(), id);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), before);
        const QDir descriptors("/dev/fd");
        const auto beforeDescriptors = descriptors.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        QVERIFY(beforeDescriptors > 0);
        for (int i = 0; i < 40; ++i) QCOMPARE(TlsIdentity::loadFile(path, false).id(), id);
        QCOMPARE(descriptors.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), beforeDescriptors);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, (void)TlsIdentity::loadFile(path + QChar(0) + "ignored", true));
    }
    void identityFileRejectsUnsafeStorage_data() {
        QTest::addColumn<QString>("variant");
        for (const auto* variant : {"invalid", "empty", "oversize", "world-readable", "symlink", "fifo", "directory", "unsafe-parent", "missing-parent", "lost-key"})
            QTest::newRow(variant) << QString(variant);
        if (::geteuid() == 0) QTest::newRow("foreign-owner") << QString("foreign-owner");
#ifdef Q_OS_MACOS
        QTest::newRow("extended-acl") << QString("extended-acl");
#endif
    }
    void identityFileRejectsUnsafeStorage() {
        QFETCH(QString, variant);
        QTemporaryDir directory;
        const auto folder = directory.filePath("secrets");
        QVERIFY(QDir().mkpath(folder));
        QString path = folder + "/identity.pem";
        const auto profile = directory.filePath("server");
        QString expected;
        if (variant == "missing-parent") {
            path = folder + "/missing/identity.pem"; expected = "existing private parent directory";
        } else if (variant == "lost-key") {
            QFile state(profile + ".channel.json"); QVERIFY(state.open(QIODevice::WriteOnly)); state.write("Existing server state");
            expected = "original identity file is missing";
        } else if (variant == "directory") {
            QVERIFY(QDir().mkpath(path)); expected = "file mode 0600 or 0400";
        } else if (variant == "fifo") {
            QCOMPARE(::mkfifo(QFile::encodeName(path).constData(), 0600), 0); expected = "file mode 0600 or 0400";
        } else if (variant == "symlink") {
            QFile target(folder + "/target"); QVERIFY(target.open(QIODevice::WriteOnly)); target.write("Must remain unchanged"); target.close();
            QVERIFY(QFile::link(target.fileName(), path)); expected = "symbolic links are not allowed";
        } else {
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(variant == "empty" ? QByteArray{} : variant == "oversize" ? QByteArray(16385, 'x') : QByteArray("Invalid PEM")); file.close();
            QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            expected = variant == "empty" || variant == "oversize" ? "Invalid device identity size" : "stored device identity is invalid";
            if (variant == "world-readable") {
                QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther)); expected = "file mode 0600 or 0400";
            } else if (variant == "unsafe-parent") {
                QCOMPARE(::chmod(QFile::encodeName(folder).constData(), 0777), 0); expected = "parent not writable by others";
            } else if (variant == "foreign-owner") {
                QCOMPARE(::chown(QFile::encodeName(path).constData(), 65534, 65534), 0); expected = "belong to this account";
            } else if (variant == "extended-acl") {
                QCOMPARE(QProcess::execute("/bin/chmod", {"+a", "everyone allow read", path}), 0); expected = "extended access rules";
            }
        }
        const bool regular = variant != "fifo" && variant != "directory";
        QFile original(path); QByteArray before;
        if (regular && original.open(QIODevice::ReadOnly)) { before = original.readAll(); original.close(); }
        QProcess process;
        process.start(executable_, {"--headless", "--settings-file", profile, "--identity-file", path});
        QVERIFY(process.waitForFinished(5000)); QCOMPARE(process.exitStatus(), QProcess::NormalExit); QCOMPARE(process.exitCode(), 1);
        const auto diagnostic = process.readAllStandardError();
        QVERIFY2(diagnostic.contains(expected.toUtf8()), diagnostic.constData());
        if (regular && original.open(QIODevice::ReadOnly)) QCOMPARE(original.readAll(), before);
        if (variant == "lost-key") QVERIFY(!QFile::exists(path));
        QVERIFY(!QFile::exists(profile + ".channel.json.chat"));
    }
#endif
    void propertiesEnvironmentAndArgumentsUseExecutableDirectoryAndPrecedence() {
        QFETCH(bool, bundle);
        QTemporaryDir directory;
        QVERIFY(QDir().mkpath(directory.filePath("bin/config")));
        QVERIFY(QDir().mkpath(directory.filePath("working")));
        const auto executable = directory.filePath(bundle
            ? "bin/SquadSpeak.app/Contents/MacOS/squadspeak" : "bin/squadspeak");
        QVERIFY(QDir().mkpath(QFileInfo(executable).absolutePath()));
        QVERIFY(QFile::copy(executable_, executable));
        const auto writeFile = [&](const QString& path, const QByteArray& contents) {
            QFile file(directory.filePath(path));
            return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
        };
        QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost));
        const auto port = QByteArray::number(reservation.serverPort()); reservation.close();
        QVERIFY(writeFile("bin/application.properties", "# executable defaults\nchannelName=Base\nport=" + port + "\nrequestsAllowed=true\nmessageLifetimeDays=30\nsettingsFile=../server0\n"));
        QVERIFY(writeFile("bin/config/application.properties", "channelName=Subfolder\nmessageLifetimeDays=7\n"));
        QVERIFY(writeFile("working/application.properties", "this file must never be read"));
        QVERIFY(writeFile("explicit.properties", "channelName=Explicit\nport=" + port + "\n"));
        for (int level = 0; level < 4; ++level) {
            const auto profile = directory.filePath("server" + QString::number(level));
            registerProfile(profile);
            QProcess process;
            process.setWorkingDirectory(directory.filePath("working"));
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("QT_QPA_PLATFORM", "__headless_platform_must_not_load__");
            if (level == 1 || level == 2) {
                environment.insert("SQUADSPEAK_CHANNEL_NAME", "Environment");
                environment.insert("SQUADSPEAK_MESSAGE_TTL", "24h");
                environment.insert("SQUADSPEAK_REQUESTS", "on");
                environment.insert("SQUADSPEAK_SETTINGS_FILE", profile);
            }
            if (level == 3) environment.insert("SQUADSPEAK_CONFIG", directory.filePath("does-not-exist"));
            process.setProcessEnvironment(environment);
            QStringList arguments{"--headless"};
            if (level >= 2) arguments << "--settings-file" << profile;
            if (level == 2) arguments << "--channel-name" << "Argument" << "--message-ttl" << "30d" << "--requests" << "on";
            if (level == 3) arguments << "-f" << directory.filePath("explicit.properties");
            process.start(executable, arguments);
            QVERIFY(process.waitForStarted(5000));
            QJsonObject state;
            QElapsedTimer deadline; deadline.start();
            while (deadline.elapsed() < 18000 && process.state() != QProcess::NotRunning) {
                process.write("{\"command\":\"status\"}\n");
                process.waitForBytesWritten(1000); process.waitForReadyRead(250);
                while (process.canReadLine()) {
                    const auto reply = QJsonDocument::fromJson(process.readLine()).object();
                    if (reply.value("command") == "status") state = reply.value("data").toObject();
                }
                if (state.value("hosting").toBool()) break;
                QTest::qWait(50);
            }
            QVERIFY2(state.value("hosting").toBool(), process.readAllStandardError().constData());
            const auto config = state.value("configuration").toObject();
            const QStringList expectedNames{"Subfolder", "Environment", "Argument", "Explicit"};
            QCOMPARE(config.value("channelName").toString(), expectedNames.at(level));
            QCOMPARE(config.value("messageLifetimeDays").toInt(), level == 0 ? 7 : level == 2 ? 30 : 1);
            QCOMPARE(config.value("requestsAllowed").toBool(), true);
            QCOMPARE(config.value("port").toInt(), port.toInt());
            QVERIFY(state.value("hostClients").toArray().isEmpty());
            process.write("{\"command\":\"quit\"}\n");
            QVERIFY(process.waitForFinished(5000)); QCOMPARE(process.exitCode(), 0);
            QVERIFY(!QFile::exists(profile + ".session.json"));
        }
    }

    void invalidHostPresetsFailWithoutChangingStoredState_data() {
        QTest::addColumn<QJsonObject>("preset");
        QTest::newRow("personal-setting") << QJsonObject{{"userName", "Nobody"}};
        QTest::newRow("audio-setting") << QJsonObject{{"inputDevice", "Default"}};
        QTest::newRow("gui-setting") << QJsonObject{{"theme", "dark"}};
        QTest::newRow("port-range") << QJsonObject{{"port", 65536}};
        QTest::newRow("port-fraction") << QJsonObject{{"port", 5000.5}};
        QTest::newRow("lifetime") << QJsonObject{{"messageLifetimeDays", 8}};
        QTest::newRow("requests-type") << QJsonObject{{"requestsAllowed", "false"}};
        QTest::newRow("name") << QJsonObject{{"channelName", "\n"}};
        QTest::newRow("approve-type") << QJsonObject{{"approvedClients", "id"}};
        QTest::newRow("ban-id") << QJsonObject{{"blockedClients", QJsonArray{"invalid"}}};
    }
    void invalidHostPresetsFailWithoutChangingStoredState() {
        QFETCH(QJsonObject, preset);
        QTemporaryDir directory;
        const auto profile = directory.filePath("server");
        registerProfile(profile);
        const auto path = directory.filePath("preset.json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(preset).toJson()); file.close();
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "__headless_platform_must_not_load__");
        process.setProcessEnvironment(environment);
        process.start(executable_, {"--headless", "--settings-file", profile, "--config", path});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 2);
        QVERIFY2(process.readAllStandardError().contains("Invalid host configuration"), qPrintable(path));
        QVERIFY(!QFile::exists(profile + ".channel.json"));
        QVERIFY(!QFile::exists(profile + ".session.json"));
        QVERIFY(!QFile::exists(profile));
    }

    void invalidServerArgumentsDoNotStartServices_data() {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<QByteArray>("fileContents");
        QTest::newRow("port-text") << QStringList{"--port", "abc"} << QByteArray{};
        QTest::newRow("port-zero") << QStringList{"--port", "0"} << QByteArray{};
        QTest::newRow("ttl") << QStringList{"--message-ttl", "2d"} << QByteArray{};
        QTest::newRow("requests") << QStringList{"--requests", "maybe"} << QByteArray{};
        QTest::newRow("requests-off") << QStringList{"--requests", "off"} << QByteArray{};
        QTest::newRow("requests-off-properties") << QStringList{"-f", "%file%"} << QByteArray("requestsAllowed=false\n");
        QTest::newRow("requests-off-json") << QStringList{"-f", "%file%"} << QByteArray("{\"requestsAllowed\":false}");
        QTest::newRow("unknown-id") << QStringList{"--approve", "name-is-not-an-id"} << QByteArray{};
        QTest::newRow("broken-json") << QStringList{"--config", "%file%"} << QByteArray("{");
        QTest::newRow("array-json") << QStringList{"--config", "%file%"} << QByteArray("[]");
        QTest::newRow("missing-preset") << QStringList{"--config", "%file%.missing"} << QByteArray{};
        QTest::newRow("missing-password") << QStringList{"--password-file", "%file%.missing"} << QByteArray{};
        QTest::newRow("password-directory") << QStringList{"--password-file", "%dir%"} << QByteArray{};
        QTest::newRow("config-directory") << QStringList{"-f", "%dir%"} << QByteArray{};
#ifndef Q_OS_WIN
        QTest::newRow("password-pipe") << QStringList{"--password-file", "%pipe%"} << QByteArray{};
        QTest::newRow("config-pipe") << QStringList{"-f", "%pipe%"} << QByteArray{};
#endif
        QTest::newRow("password-size") << QStringList{"--password-file", "%file%"} << QByteArray(1027, 'x');
        QTest::newRow("password-controls") << QStringList{"--password-file", "%file%"} << QByteArray("one\ntwo");
        QTest::newRow("password-utf8") << QStringList{"--password-file", "%file%"} << QByteArray(1, char(0xff));
        QTest::newRow("empty-config-path") << QStringList{"-f", ""} << QByteArray{};
        QTest::newRow("property-syntax") << QStringList{"-f", "%file%"} << QByteArray("port 1234\n");
        QTest::newRow("property-key") << QStringList{"-f", "%file%"} << QByteArray("inputDevice=Default\n");
        QTest::newRow("property-number") << QStringList{"-f", "%file%"} << QByteArray("port=abc\n");
        QTest::newRow("property-utf8") << QStringList{"-f", "%file%"} << QByteArray(1, char(0xff));
        QTest::newRow("property-size") << QStringList{"-f", "%file%"} << QByteArray(1024 * 1024 + 1, 'x');
        QTest::newRow("property-secrets-conflict") << QStringList{"-f", "%file%"} << QByteArray("password=secret\npasswordFile=unused\n");
        QTest::newRow("argument-secrets-conflict") << QStringList{"--password", "secret", "--password-file", "%file%"} << QByteArray{};
        QTest::newRow("password-type") << QStringList{"-f", "%file%"} << QByteArray("{\"password\": false}");
        QTest::newRow("password-argument-control") << QStringList{"--password", "two\nlines"} << QByteArray{};
        QTest::newRow("empty-storage") << QStringList{"--settings-file", ""} << QByteArray{};
        QTest::newRow("empty-identity") << QStringList{"--identity-file", ""} << QByteArray{};
        QTest::newRow("identity-type") << QStringList{"-f", "%file%"} << QByteArray("{\"identityFile\":false}");
    }
    void invalidServerArgumentsDoNotStartServices() {
        QFETCH(QStringList, arguments);
        QFETCH(QByteArray, fileContents);
        QTemporaryDir directory;
        const auto profile = directory.filePath("server");
        registerProfile(profile);
        const auto path = directory.filePath("input");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(fileContents); file.close();
        for (auto& argument : arguments) {
            argument.replace("%file%", path).replace("%dir%", directory.path());
#ifndef Q_OS_WIN
            if (argument.contains("%pipe%")) {
                const auto pipe = directory.filePath("pipe");
                QCOMPARE(::mkfifo(QFile::encodeName(pipe).constData(), 0600), 0);
                argument.replace("%pipe%", pipe);
            }
#endif
        }
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto* variable : {"QT_FORCE_STDERR_LOGGING", "QT_ASSUME_STDERR_HAS_CONSOLE", "QT_LOGGING_TO_CONSOLE"})
            environment.remove(variable);
        process.setProcessEnvironment(environment);
        process.start(executable_, QStringList{"--headless", "--settings-file", profile} + arguments);
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 2);
        QVERIFY(!process.readAllStandardError().isEmpty());
        QVERIFY(!QFile::exists(profile + ".channel.json"));
        QVERIFY(!QFile::exists(profile + ".session.json"));
    }
    void environmentCannotDisableHeadlessAdmission() {
        QTemporaryDir directory;
        const auto profile = directory.filePath("server");
        registerProfile(profile);
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("SQUADSPEAK_REQUESTS", "off");
        process.setProcessEnvironment(environment);
        process.start(executable_, {"--headless", "--settings-file", profile});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 2);
        QVERIFY(process.readAllStandardError().contains("requests off is not supported"));
        QVERIFY(!QFile::exists(profile + ".channel.json"));
        QVERIFY(!QFile::exists(profile + ".session.json"));
    }
    void jsonLinesWorkWithoutGuiPlatform() {
        // Repeated early exits cover cancellation while the secret read or write is pending.
        for (int attempt = 0; attempt < 5; ++attempt) {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            registerProfile(directory.filePath("audio.ini"));
            QProcess process;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("QT_QPA_PLATFORM", "__headless_platform_must_not_load__");
            process.setProcessEnvironment(environment);
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.start(executable_, {"--headless", "--settings-file", directory.filePath("audio.ini")});
            QVERIFY2(process.waitForStarted(5000), qPrintable(process.errorString()));
            process.write("{\"command\":\"help\"}\n");
            QVERIFY2(process.waitForBytesWritten(5000), qPrintable(process.errorString()));
            QVERIFY2(process.waitForReadyRead(5000), qPrintable(process.errorString()));
            QJsonObject help;
            QElapsedTimer responseTimer;
            responseTimer.start();
            while (help.isEmpty() && responseTimer.elapsed() < 5000) {
                if (!process.canReadLine()) {
                    process.waitForReadyRead(100);
                    continue;
                }
                const auto candidate = QJsonDocument::fromJson(process.readLine()).object();
                if (candidate.value("command").toString() == "help") help = candidate;
            }
            QVERIFY2(help.value("command").toString() == QString("help"),
                     qPrintable(QStringLiteral("headless output: ") + QString::fromUtf8(process.readAll())));
            QVERIFY(help.value("ok").toBool());
            process.write("not-json\n");
            process.write(QByteArray(65537, 'x') + '\n');
            process.write("{\"command\":\"status\"}\n{\"command\":\"quit\"}\n");
            process.closeWriteChannel();
            QVERIFY2(process.waitForFinished(15000), qPrintable(process.errorString()));
            const auto allOutput = process.readAllStandardOutput();
            QVERIFY2(process.exitStatus() == QProcess::NormalExit, allOutput.constData());
            QVERIFY2(process.exitCode() == 0, allOutput.constData());
            const auto lines = allOutput.split('\n');
            QJsonObject status;
            QJsonObject quit;
            for (const auto& line : lines) {
                const auto candidate = QJsonDocument::fromJson(line).object();
                if (candidate.value("command").toString() == "status") status = candidate;
                if (candidate.value("command").toString() == "quit") quit = candidate;
            }
            QCOMPARE(status.value("command").toString(), QString("status"));
            QVERIFY(status.value("ok").toBool());
            QCOMPARE(quit.value("command").toString(), QString("quit"));
            QVERIFY(quit.value("ok").toBool());
            QVERIFY(allOutput.contains("Each input line must be a JSON object."));
            QVERIFY(allOutput.contains("Input line exceeds 65536 bytes."));
            QVERIFY(!allOutput.contains("could not start"));
        }
    }

    void incompatibleGuiOptionFailsBeforeServices() {
        QTemporaryDir directory;
        registerProfile(directory.filePath("audio.ini"));
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "__headless_platform_must_not_load__");
        process.setProcessEnvironment(environment);
        process.start(executable_, {"--headless", "--smoke-test", "--settings-file", directory.filePath("audio.ini")});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 2);
    }

    void channelCommandsKeepHeadlessOwnersAbsentAndDataIsolated() {
        QTemporaryDir dir;
        VoiceSession session, clientSession;
        LocalChannel host(session, dir.filePath("host.channel"), TlsIdentity::create());
        LocalChannel client(clientSession, dir.filePath("client.channel"), TlsIdentity::create());
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.initialize(true));
        RadioPlayer radio(dir.filePath("radio.json")); QVERIFY(radio.bind(host));
        QBuffer input, output; QVERIFY(input.open(QIODevice::ReadWrite)); QVERIFY(output.open(QIODevice::ReadWrite));
        License pass(dir.path(), {}, {});
        HeadlessController controller(*QCoreApplication::instance(), host, radio, pass, input, output, false);
        const auto run = [&](const QJsonObject& request) {
            output.buffer().clear(); output.seek(0); controller.process(request);
            return QJsonDocument::fromJson(output.data().trimmed()).object();
        };
        const auto license = run({{"command", "license"}, {"action", "status"}});
        QVERIFY2(license.value("ok").toBool(), qPrintable(license.value("error").toString()));
        QVERIFY(!license.value("data").toObject().value("active").toBool());
        QVERIFY(license.value("data").toObject().contains("account"));
        QVERIFY(license.value("data").toObject().contains("verificationUrl"));
        QVERIFY(!run({{"command", "channels"}, {"action", "add"}, {"name", "Second"}}).value("ok").toBool());
#if SQUADSPEAK_STORE_BUILD
        QVERIFY(!session.setSupporterEnabled(true)); return;
#else
        QVERIFY(session.setSupporterEnabled(true));
        const auto added = run({{"command", "channels"}, {"action", "add"}, {"name", "Second"}});
        QVERIFY2(added.value("ok").toBool(), qPrintable(added.value("error").toString()));
        const auto id = added.value("data").toObject().value("id").toString(); QVERIFY(!id.isEmpty());
        QVERIFY(run({{"command", "configure"}, {"channelId", id}, {"values", QJsonObject{{"messageLifetimeDays", 30}, {"botName", "Second owl"}}}}).value("ok").toBool());
        QCOMPARE(host.messageLifetimeDays(), 1);
        QCOMPARE(host.ownChannel(id)->messageLifetimeDays(), 30);
        QVERIFY(client.openAddress(QString("localhost:%1").arg(host.port())));
        QTRY_VERIFY(!client.directBusy());
        QVERIFY(client.openChat(id, "localhost", host.port())); QTRY_VERIFY(client.chatReady());
        QVERIFY(host.ownChannel(id)->hostParticipants().isEmpty());
        QVERIFY(!host.joinSaved(id));
        QVERIFY(run({{"command", "chat"}, {"channelId", id}, {"text", "Second announcement"}}).value("ok").toBool());
        QTRY_COMPARE(client.messages().size(), 1);
        QCOMPARE(client.messages().first().toMap().value("name").toString(), QString("Second owl"));
        QVERIFY(!client.chatOnlineIds().contains(host.ownId()));
        QVERIFY(run({{"command", "history"}}).value("data").toObject().value("records").toArray().isEmpty());
        const auto history = run({{"command", "history"}, {"channelId", id}}).value("data").toObject().value("records").toArray();
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().toObject().value("text").toString(), QString("Second announcement"));
        QVERIFY(run({{"command", "ban"}, {"channelId", id}, {"id", client.ownId()}}).value("ok").toBool());
        QVERIFY(host.blockedClients().isEmpty());
        QCOMPARE(host.ownChannel(id)->blockedClients().size(), 1);
        QTRY_VERIFY(!client.chatReady());
        QVERIFY(!run({{"command", "configure"}, {"channelId", "unknown"}, {"values", QJsonObject{{"botName", "Wrong"}}}}).value("ok").toBool());
        QVERIFY(!run({{"command", "status"}, {"channelId", ""}}).value("ok").toBool());
        QVERIFY(!run({{"command", "channels"}, {"action", 12}}).value("ok").toBool());
        QVERIFY(!run({{"command", "channels"}, {"action", "remove"}, {"id", id}}).value("ok").toBool());
        QCOMPARE(host.ownedChannels().size(), 2);
        RadioStation stalled; stalled.stalled = true;
        output.buffer().clear(); output.seek(0);
        controller.process({{"command", "radio"}, {"channelId", id}, {"action", "add"}, {"name", "Pending"}, {"url", stalled.url()}});
        QTRY_COMPARE(stalled.requests, 1);
        QVERIFY(output.data().isEmpty());
        controller.process({{"command", "password"}, {"channelId", id}, {"value", "pending-channel-secret"}});
        QVERIFY(output.data().isEmpty());
        controller.process({{"command", "channels"}, {"action", "remove"}, {"id", id}, {"confirmed", true}});
        const auto lines = output.data().trimmed().split('\n');
        QCOMPARE(lines.size(), 3);
        QSet<QString> canceled;
        for (const auto& line : lines.sliced(0, 2)) {
            const auto reply = QJsonDocument::fromJson(line).object();
            QVERIFY(!reply.value("ok").toBool()); canceled.insert(reply.value("command").toString());
        }
        QCOMPARE(canceled, (QSet<QString>{"radio", "password"}));
        QVERIFY(QJsonDocument::fromJson(lines.last()).object().value("ok").toBool());
        QVERIFY(!output.data().contains("pending-channel-secret"));
        QCOMPARE(host.ownedChannels().size(), 1);
        QVERIFY(!QFileInfo::exists(dir.filePath("host.channel.hosts/") + id));
        QVERIFY(host.hosting());
#endif
    }

    void passwordCommandReportsStorageFailureAndCanBeRetried() {
        QTemporaryDir dir;
        const auto path = dir.filePath("host.channel");
        VoiceSession session;
        LocalChannel host(session, path, TlsIdentity::create());
        QVERIFY(host.listen(QHostAddress::LocalHost)); QVERIFY(host.initialize(true));
        QVERIFY(host.setHostPassword("original-secret")); QTRY_VERIFY(!host.passwordBusy());
        RadioPlayer radio(dir.filePath("radio.json"));
        QBuffer input, output; QVERIFY(input.open(QIODevice::ReadWrite)); QVERIFY(output.open(QIODevice::ReadWrite));
        License pass(dir.path(), {}, {});
        HeadlessController controller(*QCoreApplication::instance(), host, radio, pass, input, output, false);
        QVERIFY(QFile::rename(path, path + ".backup")); QVERIFY(QDir().mkdir(path));
        for (const auto& password : {QString(), QString("cannot-save-secret")}) {
            output.buffer().clear(); output.seek(0);
            controller.process({{"command", "password"}, {"value", password}});
            QTRY_VERIFY(!host.passwordBusy());
            const auto replies = output.data().trimmed().split('\n'); QCOMPARE(replies.size(), 1);
            const auto reply = QJsonDocument::fromJson(replies.first()).object();
            QVERIFY(!reply.value("ok").toBool()); QVERIFY(!reply.value("error").toString().isEmpty());
            QVERIFY(host.passwordProtected()); QVERIFY(!output.data().contains("secret"));
        }
        QVERIFY(QDir().rmdir(path)); QVERIFY(QFile::rename(path + ".backup", path));
        output.buffer().clear(); output.seek(0);
        controller.process({{"command", "password"}, {"value", ""}});
        QVERIFY(QJsonDocument::fromJson(output.data().trimmed()).object().value("ok").toBool());
        QVERIFY(!host.passwordProtected()); QVERIFY(host.hostParticipants().isEmpty());
    }

    void historyCommandsPageAndExpireWithoutCreatingAUser() {
        QTemporaryDir dir;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        VoiceSession session;
        LocalChannel host(session, dir.filePath("host.channel"), TlsIdentity::create(), [&] { return now; });
        RadioPlayer radio(dir.filePath("radio.json"));
        QBuffer input, output; QVERIFY(input.open(QIODevice::ReadWrite)); QVERIFY(output.open(QIODevice::ReadWrite));
        License pass(dir.path(), {}, {});
        HeadlessController controller(*QCoreApplication::instance(), host, radio, pass, input, output, false);
        const auto run = [&](QJsonObject request) {
            output.buffer().clear(); output.seek(0); controller.process(request);
            return QJsonDocument::fromJson(output.data().trimmed()).object();
        };
        QVERIFY(!run({{"command", "history"}}).value("ok").toBool());
        QVERIFY(host.listen(QHostAddress::LocalHost)); QVERIFY(host.initialize(true));
        auto reply = run({{"command", "history"}});
        QVERIFY2(reply.value("ok").toBool(), qPrintable(reply.value("error").toString()));
        QVERIFY(reply.value("data").toObject().value("records").toArray().isEmpty());
        for (int i = 0; i < 45; ++i)
            QVERIFY(run({{"command", "chat"}, {"text", QString("Announcement %1").arg(i)}}).value("ok").toBool());
        reply = run({{"command", "history"}});
        const auto latest = reply.value("data").toObject();
        QCOMPARE(latest.value("records").toArray().size(), 40);
        QVERIFY(latest.value("older").toBool()); QVERIFY(!latest.value("newer").toBool());
        const auto cursor = latest.value("records").toArray().first().toObject().value("sequence");
        reply = run({{"command", "history"}, {"direction", "older"}, {"cursor", cursor}});
        const auto older = reply.value("data").toObject();
        QCOMPARE(older.value("records").toArray().size(), 5);
        QVERIFY(!older.value("older").toBool()); QVERIFY(older.value("newer").toBool());
        const auto previous = older.value("records").toArray().last().toObject().value("sequence");
        reply = run({{"command", "history"}, {"direction", "newer"}, {"cursor", previous}});
        QCOMPARE(reply.value("data").toObject().value("records"), latest.value("records"));
        for (const auto bad : {QJsonValue(-1), QJsonValue(0.5), QJsonValue("1"), QJsonValue(QJsonValue::Null), QJsonValue(1e16)})
            QVERIFY(!run({{"command", "history"}, {"cursor", bad}}).value("ok").toBool());
        for (const auto bad : {QJsonValue("backwards"), QJsonValue(42), QJsonValue(QJsonValue::Null)})
            QVERIFY(!run({{"command", "history"}, {"direction", bad}}).value("ok").toBool());
        QVERIFY(!run({{"command", "history"}, {"channelId", "unknown"}}).value("ok").toBool());
        for (int i = 0; i < 4; ++i)
            QVERIFY(host.sendSystemMessage(QString(16000, QChar('x'))));
        const auto bounded = run({{"command", "history"}}).value("data").toObject();
        QVERIFY(QJsonDocument(bounded).toJson(QJsonDocument::Compact).size() <= 40 * 1024);
        QCOMPARE(bounded.value("records").toArray().size(), 2);
        now += ChatHistory::lifetime + 1;
        const auto expired = run({{"command", "history"}}).value("data").toObject();
        QVERIFY(expired.value("records").toArray().isEmpty());
        QVERIFY(!expired.value("older").toBool()); QVERIFY(!expired.value("newer").toBool());
        QVERIFY(!run({{"command", "configure"}, {"values", QJsonObject{{"messageLifetimeDays", 90}}}}).value("ok").toBool());
        if (License::directDistribution()) {
            QVERIFY(session.setSupporterEnabled(true));
            for (const auto days : {90, 180, 360}) {
                QVERIFY(run({{"command", "configure"}, {"values", QJsonObject{{"messageLifetimeDays", days}}}}).value("ok").toBool());
                QVERIFY(run({{"command", "chat"}, {"text", QString::number(days)}}).value("ok").toBool());
            }
            QVERIFY(session.setSupporterEnabled(false));
            QVERIFY(run({{"command", "chat"}, {"text", "After expiry"}}).value("ok").toBool());
            const auto records = run({{"command", "history"}}).value("data").toObject().value("records").toArray();
            QCOMPARE(records.size(), 4);
            const QList<int> days{90, 180, 360, 30};
            for (int i = 0; i < days.size(); ++i)
                QCOMPARE(records[i].toObject().value("expires").toInteger() - records[i].toObject().value("created").toInteger(), days[i] * ChatHistory::lifetime);
        }
        QVERIFY(!host.joined()); QVERIFY(host.hostParticipants().isEmpty()); QVERIFY(host.hostClients().isEmpty());
    }
    void passwordCommandsWaitForPersistenceAndKeepExistingMembers() {
        QTemporaryDir dir;
        VoiceSession session, clientSession;
        LocalChannel host(session, dir.filePath("host.channel"), TlsIdentity::create());
        LocalChannel client(clientSession, dir.filePath("client.channel"), TlsIdentity::create());
        QVERIFY(host.listen(QHostAddress::LocalHost)); QVERIFY(host.initialize(true));
        QVERIFY(client.join(host.ownId(), "127.0.0.1", host.port())); QTRY_VERIFY(client.chatReady());
        RadioPlayer radio(dir.filePath("radio.json"));
        QBuffer input, output; QVERIFY(input.open(QIODevice::ReadWrite)); QVERIFY(output.open(QIODevice::ReadWrite));
        License pass(dir.path(), {}, {});
        HeadlessController controller(*QCoreApplication::instance(), host, radio, pass, input, output, false);
        const auto clear = [&] { output.buffer().clear(); output.seek(0); };
        for (const auto bad : {QJsonValue(QJsonValue::Null), QJsonValue(42), QJsonValue(QString(1025, QChar('x'))), QJsonValue("bad\npassword")}) {
            clear(); controller.process({{"command", "password"}, {"value", bad}});
            QVERIFY(!QJsonDocument::fromJson(output.data().trimmed()).object().value("ok").toBool());
            QVERIFY(!host.passwordProtected());
        }
        clear(); controller.process({{"command", "password"}, {"value", "private-channel-secret"}});
        QVERIFY(output.data().isEmpty());
        controller.process({{"command", "password"}, {"value", "competing-secret"}});
        QVERIFY(!QJsonDocument::fromJson(output.data().trimmed()).object().value("ok").toBool());
        QTRY_VERIFY(!host.passwordBusy());
        const auto replies = output.data().trimmed().split('\n'); QCOMPARE(replies.size(), 2);
        const auto saved = QJsonDocument::fromJson(replies.last()).object(); QVERIFY(saved.value("ok").toBool());
        QVERIFY(saved.value("data").toObject().value("passwordProtected").toBool());
        QVERIFY(!output.data().contains("private-channel-secret")); QVERIFY(!output.data().contains("competing-secret"));
        QVERIFY(client.chatReady()); QVERIFY(host.passwordProtected());
        QVERIFY(client.closeChat(host.ownId()));
        QVERIFY(client.join(host.ownId(), "127.0.0.1", host.port())); QTRY_VERIFY(client.passwordRequired());
        QVERIFY(client.submitPassword("private-channel-secret", false)); QTRY_VERIFY(client.chatReady());
        clear(); controller.process({{"command", "password"}, {"value", ""}});
        QVERIFY(QJsonDocument::fromJson(output.data().trimmed()).object().value("ok").toBool());
        QVERIFY(!host.passwordProtected()); QVERIFY(client.chatReady());
        QVERIFY(!host.joined()); QCOMPARE(host.hostParticipants().size(), 1);
        QCOMPARE(host.hostParticipants().first().toMap().value("id").toString(), client.ownId());
    }
    void commandHandlerDrivesRealChannelAndRadioObjects() {
        QTemporaryDir directory;
        VoiceSession hostSession(directory.filePath("host.session"));
        VoiceSession clientSession(directory.filePath("client.session"));
        VoiceSession rejectedSession(directory.filePath("rejected.session"));
        QVERIFY(hostSession.setUserName("Headless Host"));
        QVERIFY(clientSession.setUserName("Headless Client"));
        QVERIFY(rejectedSession.setUserName("Rejected Client"));
        LocalChannel host(hostSession, directory.filePath("host.channel"), TlsIdentity::create());
        LocalChannel client(clientSession, directory.filePath("client.channel"), TlsIdentity::create());
        LocalChannel rejected(rejectedSession, directory.filePath("rejected.channel"), TlsIdentity::create());
        QVERIFY(host.listen(QHostAddress::LocalHost, 0));
        QVERIFY(client.join(host.ownId(), "127.0.0.1", host.port()));
        QTRY_VERIFY2_WITH_TIMEOUT(host.requests().size() == 1,
            qPrintable(host.status() + " / " + client.status() + " port=" + QString::number(host.port())
                       + " clientReady=" + QString::number(client.ready())
                       + " clientBusy=" + QString::number(client.directBusy())), 10000);
        RadioPlayer radio(directory.filePath("radio.json"));
        QBuffer input, output;
        QVERIFY(input.open(QIODevice::ReadWrite));
        QVERIFY(output.open(QIODevice::ReadWrite));
        QCoreApplication* app = QCoreApplication::instance();
        QVERIFY(app);
        License pass(directory.path(), {}, {});
        HeadlessController controller(*app, host, radio, pass, input, output, false);
        const auto requestId = client.ownId();
        output.buffer().clear(); output.seek(0);
        controller.process(QJsonObject{{"command", "unknown"}});
        controller.process(QJsonObject{{"command", "approve"}});
        controller.process(QJsonObject{{"command", "chat"}});
        const auto invalidLines = output.data().split('\n');
        int invalidCount = 0;
        for (const auto& line : invalidLines) {
            const auto object = QJsonDocument::fromJson(line).object();
            if (!object.isEmpty()) { QVERIFY(!object.value("ok").toBool()); ++invalidCount; }
        }
        QCOMPARE(invalidCount, 3);
        output.buffer().clear(); output.seek(0);
        controller.process(QJsonObject{{"command", "configure"}, {"values", QJsonObject{{"requestsAllowed", false}}}});
        const auto configureReply = QJsonDocument::fromJson(output.data().trimmed()).object();
        QVERIFY(!configureReply.value("ok").toBool());
        QVERIFY(configureReply.value("error").toString().contains("requests off"));
        controller.process(QJsonObject{{"command", "requests"}});
        controller.process(QJsonObject{{"command", "approve"}, {"id", requestId}});
        QTRY_VERIFY(client.joined());
        controller.process(QJsonObject{{"command", "chat"}, {"text", "headless durable chat"}});
        controller.process(QJsonObject{{"command", "chat"}, {"text", "second independent announcement"}});
        QTRY_VERIFY(output.data().contains("\"delivered\":true"));
        QTRY_COMPARE(client.messages().size(), 3);
        QCOMPARE(client.messages().at(1).toMap().value("text").toString(), QString("headless durable chat"));
        QCOMPARE(client.messages().at(2).toMap().value("text").toString(), QString("second independent announcement"));
        QVERIFY(!client.chatOnlineIds().contains(host.ownId()));
        controller.process(QJsonObject{{"command", "kick"}, {"id", requestId}});
        QTRY_VERIFY(!client.joined());
        QVERIFY(client.join(host.ownId(), "127.0.0.1", host.port()));
        QTRY_VERIFY(client.joined());
        controller.process(QJsonObject{{"command", "ban"}, {"id", requestId}});
        QTRY_COMPARE(host.blockedClients().size(), 1);
        QCOMPARE(host.blockedClients().first().toMap().value("id").toString(), requestId);
        controller.process(QJsonObject{{"command", "unban"}, {"id", requestId}});
        QTRY_VERIFY(host.blockedClients().isEmpty());
        QVERIFY(rejected.join(host.ownId(), "127.0.0.1", host.port()));
        QTRY_VERIFY(host.requests().size() == 1);
        controller.process(QJsonObject{{"command", "reject"}, {"id", rejected.ownId()}});
        QTRY_VERIFY(!rejected.joined());
        output.buffer().clear(); output.seek(0);
        RadioStation station;
        controller.process(QJsonObject{{"command", "radio"}, {"action", "add"},
            {"name", "Test"}, {"url", station.url()}});
        QTRY_VERIFY(!output.data().isEmpty());
        const auto added = QJsonDocument::fromJson(output.data().trimmed()).object();
        QVERIFY2(added.value("ok").toBool(), output.data().constData());
        const auto stationId = added.value("data").toObject().value("id").toString();
        QVERIFY(!stationId.isEmpty());
        controller.process(QJsonObject{{"command", "radio"}, {"action", "list"}});
        controller.process(QJsonObject{{"command", "radio"}, {"action", "update"},
            {"id", stationId}, {"name", "Updated"}, {"url", station.url() + "?updated"}});
        QTRY_VERIFY(!radio.checking());
        controller.process(QJsonObject{{"command", "radio"}, {"action", "play"},
            {"id", stationId}});
        controller.process(QJsonObject{{"command", "radio"}, {"action", "stop"}});
        controller.process(QJsonObject{{"command", "radio"}, {"action", "remove"},
            {"id", stationId}});
        int radioResponses = 0;
        for (const auto& line : output.data().split('\n')) {
            if (line.isEmpty()) continue;
            const auto reply = QJsonDocument::fromJson(line).object();
            QCOMPARE(reply.value("command").toString(), QString("radio"));
            QVERIFY2(reply.value("ok").toBool(), line.constData());
            ++radioResponses;
        }
        QCOMPARE(radioResponses, 6);
        QVERIFY(radio.stations().isEmpty());
        output.buffer().clear(); output.seek(0);
        controller.process(QJsonObject{{"command", "radio"}, {"action", "search"}, {"query", "jazz"}, {"limit", 7}});
        const auto searched = QJsonDocument::fromJson(output.data().trimmed()).object();
        QVERIFY(searched.value("ok").toBool());
        QCOMPARE(searched.value("data").toObject().value("items").toArray().size(), 7);
        QVERIFY(searched.value("data").toObject().value("total").toInt() > 7);
        QVERIFY(searched.value("data").toObject().value("catalog").toObject().value("source").isString());
        for (const auto bad : {QJsonValue(-1), QJsonValue(129), QJsonValue(0.5), QJsonValue("7")}) {
            output.buffer().clear(); output.seek(0);
            controller.process(QJsonObject{{"command", "radio"}, {"action", "search"}, {"query", ""}, {"limit", bad}});
            QVERIFY(!QJsonDocument::fromJson(output.data().trimmed()).object().value("ok").toBool());
        }
    }
};

QTEST_GUILESS_MAIN(HeadlessTests)
#include "headless_tests.moc"
