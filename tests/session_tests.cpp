#include "voice_session.hpp"
#include "tls_identity.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <QTranslator>
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QScopeGuard>
#include <openssl/pem.h>
#include <QSslKey>
#include <openssl/x509.h>

class SessionTests final : public QObject {
    Q_OBJECT
private slots:
    void firstProfileNameIsGeneratedOnceAndRemainsEditable() {
        QTemporaryDir dir;
        const auto path = dir.filePath("session.json");
        VoiceSession first(path);
        const auto name = first.userName();
        QVERIFY(QRegularExpression("^[A-Za-z]+ [A-Za-z]+$").match(name).hasMatch());
        QVERIFY(!QFile::exists(path));
        QVERIFY(first.setUserName(name));
        QVERIFY(first.error().isEmpty());
        VoiceSession restored(path);
        QCOMPARE(restored.userName(), name);
        QVERIFY(restored.setUserName("Member"));
        VoiceSession existing(path);
        QCOMPARE(existing.userName(), QString("Member"));
    }

    void unavailableAudioPreservesIntentAndDoesNotPersistHardwareState() {
        QTemporaryDir dir;
        const auto path = dir.filePath("session.json");
        VoiceSession session(path);
        QVERIFY(session.setMuted(false));
        QSignalSpy changes(&session, &VoiceSession::presenceChanged);
        QVERIFY(session.setAudioReadiness(false, false));
        QVERIFY(!session.inputReady()); QVERIFY(!session.outputReady());
        QVERIFY(!session.muted()); QVERIFY(!session.deafened());
        QVERIFY(session.transmissionAllowed()); // Capture may still request permission or retry.
        QCOMPARE(changes.count(), 1);
        QVERIFY(session.setAudioReadiness(false, false)); QCOMPARE(changes.count(), 1);
        QVERIFY(session.setAudioReadiness(true, false)); QCOMPARE(changes.count(), 2);
        QVERIFY(session.inputReady()); QVERIFY(!session.outputReady());
        VoiceSession restored(path);
        QVERIFY(restored.inputReady()); QVERIFY(restored.outputReady());
        QVERIFY(!restored.muted()); QVERIFY(!restored.deafened());
        QVERIFY(session.setMuted(true)); QVERIFY(session.setDeafened(true));
        QVERIFY(session.setAudioReadiness(true, true));
        QVERIFY(session.muted()); QVERIFY(session.deafened());
    }
    void nameValidationUsesTheSelectedTranslation() {
        VoiceSession session;
        QTranslator catalog;
        QVERIFY(catalog.load(":/i18n/squadspeak_de.qm"));
        QVERIFY(QCoreApplication::installTranslator(&catalog));
        const auto remove = qScopeGuard([&] { QCoreApplication::removeTranslator(&catalog); });
        const auto original = session.userName();
        for (const auto& name : {QString{}, QString(65, 'x')}) {
            QVERIFY(!session.setUserName(name));
            QCOMPARE(session.error(), QString::fromUtf8("Der Name muss 1 bis 64 Zeichen enthalten."));
            QCOMPARE(session.userName(), original);
        }
        QVERIFY(!session.setUserName("First\nSecond"));
        QCOMPARE(session.error(), QString::fromUtf8("Der Name darf keine Steuerzeichen oder Zeilenumbrüche enthalten."));
        QCOMPARE(session.userName(), original);
        QVERIFY(session.setUserName(QString::fromUtf8("Jürgen")));
        QVERIFY(session.error().isEmpty());
    }
    void languagesPersistAndTheirCatalogsPreservePlaceholders() {
        QTemporaryDir dir;
        const auto path = dir.filePath("session.json");
        VoiceSession session(path);
        QCOMPARE(session.language(), QString("en"));
        QSet<QString> offeredLanguages, sourceLanguages{"en"};
        for (const auto& entry : session.languages()) {
            const auto language = entry.toMap();
            QVERIFY(!language.value("label").toString().isEmpty());
            offeredLanguages.insert(language.value("code").toString());
        }
        for (const auto& file : QDir(QStringLiteral(SQUAD_TRANSLATION_DIRECTORY)).entryList({"squadspeak_*.ts"}, QDir::Files))
            sourceLanguages.insert(file.sliced(11, file.size() - 14));
        QCOMPARE(offeredLanguages, sourceLanguages);
        const auto promised = QStringLiteral("ar az be bg bn bs ca cnr cs da de el en es et fi fr ga ha hi hr hu hy "
            "id is it ja ka kk lb lt lv mk mr mt nb nl pcm pl pt rm ro ru sk sl sq sr sv sw te tr uk ur vi zh").split(' ');
        QCOMPARE(offeredLanguages, QSet<QString>(promised.cbegin(), promised.cend()));
        QVERIFY(session.setMuted(false));
        QVERIFY(session.setLanguage("de"));
        VoiceSession restored(path);
        QCOMPARE(restored.language(), QString("de"));
        QVERIFY(restored.transmissionAllowed());
        QVERIFY(!session.setLanguage("unreviewed"));
        QCOMPARE(session.language(), QString("de"));
        QTranslator catalog;
        QVERIFY(catalog.load(":/i18n/squadspeak_de.qm"));
        QCOMPARE(catalog.translate("Channels", "Mute microphone"), QString::fromUtf8("Mikrofon stummschalten"));
        QCOMPARE(catalog.translate("PushToTalkKey", "Global push-to-talk was interrupted. The last state is retained."),
            QString::fromUtf8("Globales Push-to-Talk wurde unterbrochen. Der letzte Zustand bleibt erhalten."));
        const auto placeholders = [](const QString& text) {
            static const QRegularExpression pattern("%[1-9][0-9]*|%n");
            QStringList values;
            auto matches = pattern.globalMatch(text);
            while (matches.hasNext()) values.append(matches.next().captured());
            values.sort(); return values;
        };
        QSet<QString> codes, canonicalKeys;
        for (const auto& entry : session.languages()) {
            const auto code = entry.toMap().value("code").toString();
            QVERIFY(!codes.contains(code)); QVERIFY(!code.contains('_')); codes.insert(code);
            QVERIFY(session.setLanguage(code));
            VoiceSession translatedSession(path); QCOMPARE(translatedSession.language(), code);
            QVERIFY(translatedSession.transmissionAllowed());
            if (code == "en") continue;
            QVERIFY2(catalog.load(":/i18n/squadspeak_" + code + ".qm"), qPrintable(code));
            QFile source(QStringLiteral(SQUAD_TRANSLATION_DIRECTORY) + "/squadspeak_" + code + ".ts");
            QVERIFY(source.open(QIODevice::ReadOnly));
            QXmlStreamReader xml(&source);
            QString context, original;
            QSet<QString> keys;
            int checked = 0;
            while (!xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()) continue;
                if (xml.name() == u"name") context = xml.readElementText();
                else if (xml.name() == u"source") original = xml.readElementText();
                else if (xml.name() == u"translation") {
                    QVERIFY(xml.attributes().value("type") != u"unfinished");
                    const auto translated = xml.readElementText();
                    QVERIFY(!translated.isEmpty());
                    const auto key = context + '\n' + original;
                    QVERIFY(!keys.contains(key)); keys.insert(key);
                    QCOMPARE(placeholders(translated), placeholders(original));
                    QCOMPARE(catalog.translate(context.toUtf8().constData(), original.toUtf8().constData()), translated);
                    ++checked;
                }
            }
            QVERIFY(!xml.hasError()); QVERIFY(checked > 400);
            if (canonicalKeys.isEmpty()) canonicalKeys = keys;
            else QCOMPARE(keys, canonicalKeys);
        }
        for (const auto* name : {"qtbase_de", "qtdeclarative_de", "qtbase_pt", "qtdeclarative_pt"}) {
            QTranslator standard; QVERIFY(standard.load(":/i18n/" + QString::fromLatin1(name) + ".qm"));
        }
    }
    void appearancePersistsWithoutChangingTransmission() {
        QTemporaryDir dir;
        const auto path = dir.filePath("session.json");
        VoiceSession session(path);
        QCOMPARE(session.theme(), QString("system"));
        QVERIFY(session.setMuted(false));
        for (const auto* mode : {"dark", "light", "system"}) {
            QVERIFY(session.setTheme(mode));
            VoiceSession restored(path);
            QCOMPARE(restored.theme(), QString(mode));
            QVERIFY(restored.transmissionAllowed());
        }
        QVERIFY(!session.setTheme("unknown"));
        QCOMPARE(session.theme(), QString("system"));
        QVERIFY(!session.error().isEmpty());
        QVERIFY(session.setTheme("system"));
        QVERIFY(session.error().isEmpty());
    }
    void dockPreferencePersistsWithoutChangingTransmission() {
        QTemporaryDir dir;
        const auto path = dir.filePath("session.json");
        VoiceSession session(path);
        QVERIFY(session.showInDock());
        QVERIFY(session.setMuted(false));
        VoiceSession withoutSavedDockChoice(path);
        QVERIFY(withoutSavedDockChoice.showInDock());
        QVERIFY(withoutSavedDockChoice.transmissionAllowed());
        for (const bool enabled : {true, true, false}) {
            QVERIFY(session.setShowInDock(enabled));
            VoiceSession restored(path);
            QCOMPARE(restored.showInDock(), enabled);
            QVERIFY(restored.transmissionAllowed());
        }
        VoiceSession failure(dir.filePath("missing/session.json"));
        QVERIFY(!failure.setShowInDock(false));
        QVERIFY(failure.showInDock());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        auto invalid = QJsonDocument::fromJson(file.readAll()).object();
        invalid.insert("showInDock", "yes");
        file.resize(0); file.write(QJsonDocument(invalid).toJson()); file.close();
        QVERIFY_EXCEPTION_THROWN(VoiceSession{path}, std::runtime_error);
    }
    void palettesPersistIndependentlyOfAppearanceAndTransmission() {
        QTemporaryDir dir;
        const auto path = dir.filePath("session.json");
        VoiceSession session(path);
        QCOMPARE(session.palette(), QString("plum"));
        QVERIFY(session.setMuted(false));
        QVERIFY(session.setTheme("dark"));
        for (const auto* palette : {"ocean", "forest", "graphite", "plum"}) {
            QVERIFY(session.setPalette(palette));
            VoiceSession restored(path);
            QCOMPARE(restored.palette(), QString(palette));
            QCOMPARE(restored.theme(), QString("dark"));
            QVERIFY(restored.transmissionAllowed());
        }
        QVERIFY(!session.setPalette("missing"));
        QCOMPARE(session.palette(), QString("plum"));
        VoiceSession failure(dir.filePath("missing/session.json"));
        QVERIFY(!failure.setPalette("ocean"));
        QCOMPARE(failure.palette(), QString("plum"));
    }

    void profileAndOutputPreferencesRestoreWithoutAffectingMicrophoneIntent() {
        QTemporaryDir folder;
        const auto path = folder.filePath("session.json");
        VoiceSession session(path);
        QVERIFY(session.userName() != "Member");
        QCOMPARE(session.avatar(), QString("mossling"));
        QVERIFY(session.eventSounds()); QVERIFY(!session.deafened());
        QVERIFY(session.setAvatar("courier")); QVERIFY(session.setDeafened(true));
        QVERIFY(session.setEventSounds(false)); QVERIFY(session.setMuted(false));
        QVERIFY(session.transmissionAllowed());
        QVERIFY(!session.setAvatar("unknown")); QCOMPARE(session.avatar(), QString("courier"));
        VoiceSession restored(path);
        QCOMPARE(restored.avatar(), QString("courier")); QVERIFY(restored.deafened());
        QVERIFY(!restored.eventSounds()); QVERIFY(restored.transmissionAllowed());
        VoiceSession failed(folder.filePath("absent/session"));
        QVERIFY(!failed.setDeafened(true)); QVERIFY(!failed.deafened());
        QVERIFY(!failed.setAvatar("mechanic")); QCOMPARE(failed.avatar(), QString("mossling"));
    }

    void catalogAvatarsAreUniqueSelectableAndPersisted() {
        QTemporaryDir dir;
        const auto path = dir.filePath("avatars.json");
        VoiceSession session(path);
        const bool supporter = session.setSupporterEnabled(true);
        const auto avatars = session.avatars();
        QCOMPARE(avatars.size(), 20);
        QCOMPARE(QSet<QString>(avatars.begin(), avatars.end()).size(), avatars.size());
        const auto unknownFallback = VoiceSession::avatarFallback("future-dragon-v2");
        QVERIFY(QStringList({"mossling", "courier", "mechanic"}).contains(unknownFallback));
        for (const auto& id : avatars) {
            QVERIFY(VoiceSession::validAvatar(id));
            if (!supporter && avatars.indexOf(id) >= 10) { QVERIFY(!session.setAvatar(id)); continue; }
            QVERIFY(session.setAvatar(id));
            QCOMPARE(session.avatar(), id);
            QVERIFY(QStringList({"mossling", "courier", "mechanic"}).contains(VoiceSession::avatarFallback(id)));
            VoiceSession restored(path);
            restored.setSupporterEnabled(supporter);
            QCOMPARE(restored.avatar(), id);
        }
        QVERIFY(!session.setAvatar("../portrait"));
        QCOMPARE(session.avatar(), avatars.at(supporter ? 19 : 9));
    }

    void paidAvatarSelectionIsRuntimeOnlyAndSurvivesLossOfEntitlement() {
        QTemporaryDir dir; const auto path = dir.filePath("profile.json");
        VoiceSession session(path);
        const auto paid = session.avatars().at(10);
        QVERIFY(!session.setAvatar(paid)); QVERIFY(session.setMuted(false));
        if (SQUADSPEAK_STORE_BUILD) { QVERIFY(!session.setSupporterEnabled(true)); return; }
        QVERIFY(session.setSupporterEnabled(true)); QVERIFY(session.setAvatar(paid));
        QVERIFY(session.setSupporterEnabled(false)); QCOMPARE(session.avatar(), VoiceSession::avatarFallback(paid));
        QVERIFY(session.transmissionAllowed());
        VoiceSession restored(path); QVERIFY(!restored.supporterEnabled());
        QCOMPARE(restored.avatar(), VoiceSession::avatarFallback(paid));
        QVERIFY(restored.setSupporterEnabled(true)); QCOMPARE(restored.avatar(), paid);
        QVERIFY(restored.transmissionAllowed());
    }

    void newerAvatarDoesNotPreventLoadingSavedProfile() {
        QTemporaryDir dir;
        const auto path = dir.filePath("profile.json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"version":1,"userName":"Member","channelName":"Channel","muted":true,"avatar":"future-dragon-v2"})");
        file.close();
        try {
            VoiceSession restored(path);
            QCOMPARE(restored.avatar(), QString("future-dragon-v2"));
            QVERIFY(!restored.setAvatar("../arbitrary-file"));
        } catch (const std::exception& error) { QFAIL(error.what()); }
    }
    void avatarPreferencePersistsWithoutChangingTransmission() {
        QTemporaryDir directory;
        const auto path = directory.filePath("session");
        VoiceSession session(path);
        QVERIFY(session.animatedAvatars()); QVERIFY(session.setMuted(false));
        QVERIFY(session.setAnimatedAvatars(false)); QVERIFY(!session.animatedAvatars());
        QVERIFY(session.transmissionAllowed());
        VoiceSession restored(path); QVERIFY(!restored.animatedAvatars()); QVERIFY(restored.transmissionAllowed());
        QVERIFY(restored.setAnimatedAvatars(true)); QVERIFY(restored.animatedAvatars());
        VoiceSession failed(directory.filePath("absent/session"));
        QVERIFY(!failed.setAnimatedAvatars(false)); QVERIFY(failed.animatedAvatars());
    }

    void pushToTalkSourcesCombineAndRestoreTheLastKnownState() {
        QTemporaryDir folder;
        const auto path = folder.filePath("session.json");
        {
            VoiceSession session(path);
            QVERIFY(session.setPttShortcut(100, "F8"));
            QVERIFY(session.setPushToTalk(true)); QVERIFY(session.setMuted(false));
            QVERIFY(!session.transmissionAllowed());
            QVERIFY(session.setPttButtonHeld(true)); QVERIFY(session.transmissionAllowed());
            QVERIFY(session.setPttKeyHeld(true));
            QVERIFY(session.setPttButtonHeld(false)); QVERIFY(session.transmissionAllowed());
            QVERIFY(session.setRemotePttHeld(true));
            QVERIFY(session.setPttKeyHeld(false)); QVERIFY(session.transmissionAllowed());
            QVERIFY(session.setRemotePttHeld(false)); QVERIFY(!session.transmissionAllowed());
            QVERIFY(session.setPttKeyHeld(true));
            QVERIFY(session.setMuted(true)); QVERIFY(!session.transmissionAllowed());
            QVERIFY(session.setMuted(false));
            QVERIFY(session.setAudioSettingsOpen(true)); QVERIFY(!session.transmissionAllowed());
            QVERIFY(!session.setPttShortcut(101, "F9"));
        }
        VoiceSession restored(path);
        QVERIFY(restored.pushToTalk()); QVERIFY(restored.pttKeyHeld());
        QCOMPARE(restored.pttKeyCode(), 100); QCOMPARE(restored.pttKeyName(), QString("F8"));
        QVERIFY(restored.transmissionAllowed());
        QVERIFY(restored.setPttKeyHeld(false)); QVERIFY(!restored.transmissionAllowed());
        QVERIFY(restored.setPushToTalk(false)); QVERIFY(restored.transmissionAllowed());
        QVERIFY(restored.setPushToTalk(true)); QVERIFY(!restored.transmissionAllowed());
    }
    void failedPttPersistenceCannotChangeTransmissionAndOldSettingsMigrate() {
        QTemporaryDir folder;
        VoiceSession unavailable(folder.filePath("missing/session.json"));
        QVERIFY(!unavailable.setPushToTalk(true)); QVERIFY(!unavailable.pushToTalk());
        QVERIFY(!unavailable.setPttButtonHeld(true)); QVERIFY(!unavailable.pttHeld());
        QVERIFY(!unavailable.setPttShortcut(100, "F8")); QCOMPARE(unavailable.pttKeyCode(), -1);
        const auto path = folder.filePath("old.json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"version\":1,\"userName\":\"Mira\",\"channelName\":\"LAN\",\"muted\":false}"); file.close();
        VoiceSession old(path);
        QVERIFY(!old.pushToTalk()); QVERIFY(old.transmissionAllowed());
        QVERIFY(old.setPushToTalk(true)); QVERIFY(!old.transmissionAllowed());
        VoiceSession restored(path); QVERIFY(restored.pushToTalk()); QVERIFY(!restored.muted());
        QVERIFY(!restored.setPttShortcut(-2, "Invalid"));
        QVERIFY(!restored.setPttShortcut(100, "\n"));
        QVERIFY(!restored.setPttShortcut(-1, "F8"));
    }

    void legacyNormalKeySurvivesAndObsoleteRemoteBindingIsIgnored() {
        QTemporaryDir folder;
        const auto path = folder.filePath("legacy-v2.json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"version":2,"userName":"Member","channelName":"Channel","muted":true,"pushToTalk":true,"pttButtonHeld":false,"pttKeyHeld":false,"pttKeyCode":100,"pttKeyName":"F8","remotePttKeyHeld":true,"remotePttKeyCode":101,"remotePttKeyName":"F9"})");
        file.close();
        VoiceSession loaded(path);
        QCOMPARE(loaded.pttKeyCodes(), QList<int>({100}));
        QCOMPARE(loaded.pttKeyName(), QString("F8"));
        QVERIFY(!loaded.pttHeld());
        QCOMPARE(loaded.pttInputRevision(), 1);
        QVERIFY(loaded.setMuted(false));
        VoiceSession restored(path);
        QCOMPARE(restored.pttKeyCodes(), QList<int>({100}));
        QCOMPARE(restored.pttKeyName(), QString("F8"));
        QCOMPARE(restored.pttInputRevision(), 1);
        QVERIFY(restored.setPttLocal(false));
        QVERIFY(!restored.pttInputHeld());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains("remotePttKey"));
    }
    void overlappingAudioRestrictionsRestoreTheLatestIntent() {
        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        QVERIFY(session.setMuted(false));
        QVERIFY(session.transmissionAllowed());
        QVERIFY(session.setAudioSettingsOpen(true));
        QVERIFY(session.setAudioSettingsOpen(true));
        QVERIFY(!session.available());
        QVERIFY(!session.transmissionAllowed());
        QVERIFY(session.setAudioTestActive(true));
        QVERIFY(session.setAudioSettingsOpen(false));
        QVERIFY(!session.available());
        QVERIFY(session.setMuted(true));
        QVERIFY(session.setAudioTestActive(false));
        QVERIFY(session.available());
        QVERIFY(!session.transmissionAllowed());
        QVERIFY(session.setAudioSettingsOpen(true));
        QVERIFY(session.setMuted(false));
        QVERIFY(!session.transmissionAllowed());
        QVERIFY(session.setAudioSettingsOpen(false));
        QVERIFY(session.transmissionAllowed());
        QVERIFY(session.setAudioSettingsOpen(false));
        QVERIFY(session.transmissionAllowed());
    }

    void transientRestrictionDoesNotReplacePersistedMuteState() {
        QTemporaryDir folder;
        const auto path = folder.filePath("session.json");
        {
            VoiceSession session(path);
            QVERIFY(session.muted());
            QVERIFY(session.setMuted(false));
            QVERIFY(session.setAudioSettingsOpen(true));
            QVERIFY(session.setAudioTestActive(true));
        }
        VoiceSession restored(path);
        QVERIFY(!restored.muted());
        QVERIFY(restored.available());
        QVERIFY(restored.transmissionAllowed());
    }

    void namesAreIndependentAndPersistAcrossRestart() {
        QTemporaryDir folder;
        const auto first = folder.filePath("a.json");
        const auto second = folder.filePath("b.json");
        {
            VoiceSession a(first), b(second);
            QVERIFY(a.setUserName("  Yuna  "));
            QVERIFY(b.setUserName("Yuna"));
            QCOMPARE(a.userName(), QString("Yuna"));
            QVERIFY(a.setUserName("Mira"));
            QCOMPARE(b.userName(), QString("Yuna"));
        }
        VoiceSession restored(first);
        QCOMPARE(restored.userName(), QString("Mira"));
    }

    void invalidEditsAndFailedPersistenceKeepTheLastState() {
        QTemporaryDir folder;
        VoiceSession session(folder.filePath("session.json"));
        QVERIFY(session.setUserName("Mira"));
        QVERIFY(!session.setUserName(" \n "));
        QVERIFY(!session.setUserName(QString(65, 'a')));
        QVERIFY(!session.setUserName(QString("X") + QChar(0x202E)));
        QCOMPARE(session.userName(), QString("Mira"));
        QVERIFY(!session.error().isEmpty());
        VoiceSession unavailable(folder.filePath("missing/session.json"));
        const auto initialName = unavailable.userName();
        QVERIFY(!unavailable.setMuted(false));
        QVERIFY(unavailable.muted());
        QVERIFY(!unavailable.setUserName("Mira"));
        QCOMPARE(unavailable.userName(), initialName);
    }

    void malformedPersistedPreferencesFailAtTheBoundary() {
        QTemporaryDir folder;
        const auto path = folder.filePath("session.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"version\":1,\"userName\":\"Mira\",\"channelName\":\"LAN\",\"muted\":\"false\"}");
        file.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, VoiceSession{path});
    }

    void cmsMessageEnvelopeAuthenticatesRecipientsAndContext() {
        const auto alice = TlsIdentity::create();
        const auto bob = TlsIdentity::create();
        const auto relay = TlsIdentity::create();
        const auto context = QByteArray("SquadSpeak/private/host-a/bob");
        const auto plain = QByteArray("private message with attachment metadata");
        const auto envelope = alice.sealFor(plain, bob.configuration().localCertificate(), context);
        QCOMPARE(bob.openFrom(envelope, alice.configuration().localCertificate(), context), plain);
        QCOMPARE(alice.openFrom(envelope, alice.configuration().localCertificate(), context), plain);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            relay.openFrom(envelope, alice.configuration().localCertificate(), context));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            bob.openFrom(envelope, relay.configuration().localCertificate(), context));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            bob.openFrom(envelope, alice.configuration().localCertificate(), "wrong-context"));

        auto tampered = envelope;
        tampered[tampered.size() / 2] = char(tampered.at(tampered.size() / 2) ^ 0x01);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            bob.openFrom(tampered, alice.configuration().localCertificate(), context));
        auto trailing = envelope;
        trailing.append('\0');
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            bob.openFrom(trailing, alice.configuration().localCertificate(), context));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            alice.sealFor(QByteArray(24 * 1024 + 1, 'x'), bob.configuration().localCertificate(), context));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            bob.openFrom(envelope, alice.configuration().localCertificate(), QByteArray(4097, 'x')));

        const auto restored = TlsIdentity::fromPem(bob.pem());
        QCOMPARE(restored.openFrom(envelope, alice.configuration().localCertificate(), context), plain);
    }

    void cmsRejectsSignerWithMatchingIssuerAndSerialButDifferentKey() {
        const auto alice = TlsIdentity::create();
        const auto bob = TlsIdentity::create();
        const auto attacker = TlsIdentity::create();
        const auto readCertificate = [](const QByteArray& pem) {
            BIO* input = BIO_new_mem_buf(pem.constData(), pem.size());
            if (!input) return static_cast<X509*>(nullptr);
            X509* certificate = PEM_read_bio_X509(input, nullptr, nullptr, nullptr);
            BIO_free(input);
            return certificate;
        };
        const auto readPrivateKey = [](const QByteArray& pem) {
            BIO* input = BIO_new_mem_buf(pem.constData(), pem.size());
            if (!input) return static_cast<EVP_PKEY*>(nullptr);
            EVP_PKEY* key = PEM_read_bio_PrivateKey(input, nullptr, nullptr, nullptr);
            BIO_free(input);
            return key;
        };
        X509* aliceCertificate = readCertificate(alice.configuration().localCertificate().toPem());
        X509* attackerCertificate = readCertificate(attacker.configuration().localCertificate().toPem());
        EVP_PKEY* attackerKey = readPrivateKey(attacker.configuration().privateKey().toPem());
        QVERIFY(aliceCertificate != nullptr);
        QVERIFY(attackerCertificate != nullptr);
        QVERIFY(attackerKey != nullptr);
        const auto cleanup = qScopeGuard([&] {
            X509_free(aliceCertificate);
            X509_free(attackerCertificate);
            EVP_PKEY_free(attackerKey);
        });

        X509* forgedCertificate = X509_dup(attackerCertificate);
        QVERIFY(forgedCertificate != nullptr);
        const auto forgedCleanup = qScopeGuard([&] { X509_free(forgedCertificate); });
        QVERIFY(X509_set_issuer_name(forgedCertificate, X509_get_issuer_name(aliceCertificate)) == 1);
        ASN1_INTEGER* serial = ASN1_INTEGER_dup(X509_get0_serialNumber(aliceCertificate));
        QVERIFY(serial != nullptr);
        QVERIFY(X509_set_serialNumber(forgedCertificate, serial) == 1);
        ASN1_INTEGER_free(serial);
        QVERIFY(X509_sign(forgedCertificate, attackerKey, EVP_sha256()) > 0);

        BIO* output = BIO_new(BIO_s_mem());
        QVERIFY(output != nullptr);
        const auto outputCleanup = qScopeGuard([&] { BIO_free(output); });
        QVERIFY(PEM_write_bio_X509(output, forgedCertificate) == 1);
        QVERIFY(PEM_write_bio_PrivateKey(output, attackerKey, nullptr, nullptr, 0, nullptr, nullptr) == 1);
        char* bytes = nullptr;
        const auto size = BIO_get_mem_data(output, &bytes);
        QVERIFY(size > 0);
        const auto forged = TlsIdentity::fromPem(QByteArray(bytes, int(size)));
        const auto context = QByteArray("SquadSpeak/private/signer-key");
        const auto plain = QByteArray("sender key binding");
        const auto envelope = forged.sealFor(plain, bob.configuration().localCertificate(), context);

        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            bob.openFrom(envelope, alice.configuration().localCertificate(), context));
        QCOMPARE(bob.openFrom(envelope, forged.configuration().localCertificate(), context), plain);
    }
};

QTEST_GUILESS_MAIN(SessionTests)
#include "session_tests.moc"
