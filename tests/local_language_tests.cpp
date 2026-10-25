#include "local_language.hpp"
#include "local_language_native.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <QTimer>
#include <QSignalSpy>
#include <set>
#include <array>

#ifndef SQUAD_NATIVE_LANGUAGE_ACCEPTANCE
// The OS boundary is asynchronous and controllable. Production policy, rich
// text conversion, persistence and cancellation run unchanged.
namespace {
struct Native {
    SquadLanguageCallback callback;
    void* context;
    std::map<int64_t, QString> requests;
    QString source, detectionText;
};
Native* current = nullptr;
void reply(int64_t request, QJsonObject event) {
    if (current) { const auto data = QJsonDocument(event).toJson(QJsonDocument::Compact); current->callback(current->context, request, data.constData()); }
}
}
extern "C" {
void* squad_language_create(SquadLanguageCallback callback, void* context) { current = new Native{callback, context, {}}; return current; }
void squad_language_refresh(void*) {}
void squad_language_destroy(void* handle) { delete static_cast<Native*>(handle); current = nullptr; }
const char* squad_language_capabilities(void*) { return R"({"available":true,"speechAvailable":true,"languages":[{"id":"en","name":"English"},{"id":"de","name":"Deutsch"}],"speechLanguages":[{"id":"en-US","name":"English"}]})"; }
void squad_language_prepare(void*, int64_t req, const char*, const char*, int) { current->requests[req] = "prepare"; }
void squad_language_translate(void*, int64_t req, const char* source, const char*, const char* text, const char* detectionText) {
    current->requests[req] = QString::fromUtf8(text);
    current->source = QString::fromUtf8(source); current->detectionText = QString::fromUtf8(detectionText);
}
void squad_language_listen(void*, int64_t req, const char*) { current->requests[req] = "listen"; }
void squad_language_audio(void*, int64_t, const float*, int) {}
void squad_language_cancel(void*, int64_t req) { current->requests.erase(req); }
}
class LanguageTests final : public QObject {
    Q_OBJECT
private slots:
    void unusedLanguagePreferencesExpireWithoutDroppingUnknownFields() {
        QTemporaryDir dir;
        const auto path = dir.filePath("language.json");
        qint64 now = 1700000000000;
        const auto clock = [&] { return now; };
        {
            LocalLanguage language(path, nullptr, clock);
            QVERIFY(language.configureChannel("seen", true, "de", false, ""));
            QVERIFY(language.configureChannel("old", false, "fr", true, "en"));
            now += 364LL * 86400000;
            QVERIFY(language.observeContacts({"seen", "not-saved"}));
        }
        {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
            auto value = QJsonDocument::fromJson(file.readAll()).object(); file.close();
            value.insert("futureOption", "preserved");
            auto channels = value.value("channels").toObject(), seen = channels.value("seen").toObject();
            seen.remove("voice"); seen.insert("futureMode", "preserved");
            channels.insert("seen", seen); value.insert("channels", channels);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QJsonDocument(value).toJson());
        }
        now += 86400000;
        LocalLanguage restored(path, nullptr, clock);
        QVERIFY(restored.channelPreferences("seen").value("messages").toBool());
        QVERIFY(restored.channelPreferences("old").value("outgoing").toString().isEmpty());
        QVERIFY(restored.channelPreferences("old").value("listenTarget") == "en");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QCOMPARE(saved.value("channels").toObject().size(), 1);
        QCOMPARE(saved.value("futureOption").toString(), QString("preserved"));
        QVERIFY(restored.configureChannel("seen", false, "en", true, "de"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value("channels").toObject()
            .value("seen").toObject().value("futureMode").toString(), QString("preserved"));
    }
    void channelPreferencesFollowAppLanguageAndStayLocal() {
        QTemporaryDir dir;
        const auto path = dir.filePath("languages.json");
        {
            LocalLanguage language(path);
            QVERIFY(!language.channelPreferences("a").value("messages").toBool());
            QVERIFY(language.configureChannel("a", true, "fr", true, ""));
            QVERIFY(language.configureChannel("b", false, "", true, "es"));
            language.setAppLanguage("de");
            QCOMPARE(language.channelPreferences("a").value("incomingTarget"), "de");
            QCOMPARE(language.channelPreferences("a").value("listenTarget"), "de");
            QCOMPARE(language.channelPreferences("a").value("outgoing"), "fr");
            QCOMPARE(language.channelPreferences("b").value("listenTarget"), "es");
            language.setAppLanguage("en");
            QCOMPARE(language.channelPreferences("a").value("listenTarget"), "en");
            QCOMPARE(language.channelPreferences("b").value("listenTarget"), "es");
            QVERIFY(!language.configureChannel("", true, "en", false, ""));
            QVERIFY(!language.configureChannel("a", true, "../../en", false, ""));
            QVERIFY(!language.configureChannel("a", true, "en", false, QString(500, 'x')));
        }
        LocalLanguage restored(path);
        QVERIFY(restored.channelPreferences("a").value("messages").toBool());
        QCOMPARE(restored.channelPreferences("a").value("outgoing"), "fr");
        QVERIFY(restored.configureChannel("a", false, "fr", false, ""));
        QVERIFY(!restored.channelPreferences("a").value("messages").toBool());
        QCOMPARE(restored.channelPreferences("a").value("outgoing"), "fr");
    }
    void legacyPreferencesKeepTheTargetWithoutEnablingNewProcessing() {
        QTemporaryDir dir;
        QFile file(dir.filePath("language.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({"version":1,"members":{"alice":{"source":"en","target":"de","voice":"old","incoming":true}},"channels":{"a":"fr"}})"); file.close();
        LocalLanguage language(file.fileName());
        QVERIFY(language.error().isEmpty());
        QCOMPARE(language.channelPreferences("a").value("outgoing"), "fr");
        QVERIFY(!language.channelPreferences("a").value("messages").toBool());
        QVERIFY(!language.channelPreferences("a").value("voice").toBool());
        QVERIFY(language.member("alice").value("voice").toString().isEmpty());
    }
    void textTranslationDoesNotRequireSpeechRecognition() {
        LocalLanguage language;
        reply(0, {{"kind", "capabilities"}, {"available", true}, {"speechAvailable", false}});
        QVERIFY(language.configureChannel("a", true, "fr", true, "de"));
        QVERIFY(language.request("french", "Bonjour tout le monde", "", "de"));
        reply(current->requests.begin()->first, {{"kind", "text"}, {"text", "Hallo zusammen"}});
        QVERIFY(language.result("french").value("text").toString().contains("Hallo zusammen"));
        language.setMembers({QVariantMap{{"id", "alice"}, {"language", "fr"}}}, "a");
        QVERIFY(!language.voiceProcessing());
    }
    void missingModelsAskOnceAndWaitForConsent() {
        LocalLanguage language;
        QVERIFY(language.request("french", "Bonjour tout le monde", "", "de"));
        reply(current->requests.begin()->first, {{"kind", "error"}, {"code", "modelsRequired"}, {"source", "fr"}});
        QCOMPARE(language.result("french").value("original"), "Bonjour tout le monde");
        QVERIFY(language.result("french").value("needsPreparation").toBool());
        QCOMPARE(language.preparation().value("source"), "fr");
        QVERIFY(current->requests.empty());
        QVERIFY(!language.preparing());
        language.cancelPreparation();
        QVERIFY(language.preparation().isEmpty());
        QVERIFY(!language.acceptPreparation());
        QVERIFY(language.request("again", "Bonjour encore", "", "de"));
        reply(current->requests.begin()->first, {{"kind", "error"}, {"code", "modelsRequired"}, {"source", "fr"}});
        QVERIFY(language.preparation().isEmpty());
        QVERIFY(language.prepareTranslation("again"));
        QVERIFY(!language.prepareTranslation("again"));
        const auto preparation = current->requests.begin()->first;
        reply(preparation, {{"kind", "ready"}}); current->requests.erase(preparation);
        QVERIFY(!language.preparing());
        QVERIFY(language.result("again").isEmpty());
        QVERIFY(language.configureChannel("a", true, "", false, ""));
        QVERIFY(language.request("new", "Hola a todos", "", "de"));
        reply(current->requests.begin()->first, {{"kind", "error"}, {"code", "modelsRequired"}, {"source", "es"}});
        QVERIFY(language.acceptPreparation());
        QVERIFY(language.preparing());
        const auto cancelled = current->requests.begin()->first;
        language.cancelPreparation();
        reply(cancelled, {{"kind", "ready"}});
        QVERIFY(!language.preparing());
        QVERIFY(language.request("failure", "Hello", "en", "de"));
        reply(current->requests.rbegin()->first, {{"kind", "error"}, {"error", "A specific native failure"}});
        QCOMPARE(language.result("failure").value("error"), "A specific native failure");
    }
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    void channelVoiceUsesSenderHintsAndLeavesUnknownOrSameLanguageAudible() {
        LocalLanguage language;
        QVERIFY(language.configureChannel("a", false, "", true, "de"));
        language.setMembers({QVariantMap{{"id", "alice"}, {"language", "en"}}, QVariantMap{{"id", "bob"}},
                            QVariantMap{{"id", "carol"}, {"language", "de"}}}, "a");
        QCOMPARE(current->requests.size(), size_t(1));
        const auto listening = current->requests.begin()->first;
        std::array<float, 960> frame; frame.fill(0.2f);
        language.processVoice("alice", frame, 0); QCOMPARE(frame.front(), 0.2f);
        reply(listening, {{"kind", "ready"}});
        QVERIFY(language.voiceProcessing());
        for (const auto* id : {"bob", "carol"}) { language.processVoice(id, frame, 0); QCOMPARE(frame.front(), 0.2f); }
        language.processVoice("alice", frame, 0); QCOMPARE(frame.front(), 0.0f);
        reply(listening, {{"kind", "text"}, {"text", "Hello"}, {"final", true}});
        QCOMPARE(language.member("alice").value("transcript"), "Hello");
        const auto pending = current->requests.rbegin()->first;
        reply(pending, {{"kind", "error"}});
        QVERIFY(!language.voiceProcessing());
        frame.fill(0.2f); language.processVoice("alice", frame, 0); QCOMPARE(frame.front(), 0.2f);
        QVERIFY(!language.member("alice").value("status").toString().isEmpty());
        QVERIFY(language.configureChannel("a", false, "", false, "de"));
        reply(listening, {{"kind", "ready"}});
        QVERIFY(!language.voiceProcessing());
        QVERIFY(current->requests.empty());
    }
    void channelAndLanguageChangesCancelOldSpeech() {
        LocalLanguage language;
        QVERIFY(language.configureChannel("a", false, "", true, "de"));
        QVERIFY(language.configureChannel("b", false, "", true, "de"));
        const QVariantList members{QVariantMap{{"id", "alice"}, {"language", "en"}}};
        language.setMembers(members, "a");
        const auto first = current->requests.begin()->first;
        reply(first, {{"kind", "ready"}});
        reply(first, {{"kind", "text"}, {"text", "Previous conversation"}, {"final", true}});
        const auto pending = current->requests.rbegin()->first;
        language.setMembers(members, "b");
        QVERIFY(language.member("alice").value("transcript").toString().isEmpty());
        QCOMPARE(current->requests.size(), size_t(1));
        QVERIFY(current->requests.begin()->first != first);
        reply(first, {{"kind", "ready"}}); reply(pending, {{"kind", "text"}, {"text", "Too late"}});
        QVERIFY(!language.voiceProcessing());
        language.setMembers({QVariantMap{{"id", "alice"}, {"language", "de"}}}, "b");
        QVERIFY(current->requests.empty());
        language.suspend(true); language.setMembers(members, "b");
        QVERIFY(current->requests.empty());
        language.suspend(false); QCOMPARE(current->requests.size(), size_t(1));
        const auto missing = current->requests.begin()->first;
        reply(missing, {{"kind", "error"}, {"code", "modelsRequired"}});
        QVERIFY(language.preparation().value("speech").toBool());
        QVERIFY(!language.voiceProcessing());
        language.setMembers({}, "b");
        QVERIFY(current->requests.empty());
        QVERIFY(language.preparation().isEmpty());
    }
#endif
    void corruptAndUnwritableSettingsDoNotEnableAnything() {
        QTemporaryDir dir;
        for (const auto* json : {"{}", R"({"version":2,"channels":{"a":{"messages":"yes"}}})"}) {
            QFile file(dir.filePath("broken")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(json); file.close();
            LocalLanguage broken(file.fileName());
            QVERIFY(!broken.error().isEmpty()); QVERIFY(!broken.channelPreferences("a").value("messages").toBool());
        }
        LocalLanguage unwritable(dir.filePath("missing/file"));
        QVERIFY(!unwritable.configureChannel("a", true, "en", false, ""));
        QVERIFY(!unwritable.error().isEmpty()); QVERIFY(!unwritable.channelPreferences("a").value("messages").toBool());
    }
    void markdownCodeImagesAndLinkAddressesSurviveTranslation() {
        LocalLanguage language;
        const QString input = "# Hello\n\n**World** and `code()` [label](https://example.org/path?q=1)\n\n```cpp\nkeep();\n```\n\n![image](attachment:abc)\n\n> Quote\n\n| Table | Value |\n| --- | --- |\n| One | Two |\n";
        QVERIFY(language.request("draft", input, "", "de"));
        int requests = 0;
        while (language.result("draft").value("pending").toBool()) {
            QVERIFY(++requests < 30);
            const auto id = current->requests.begin()->first;
            const auto text = current->requests.begin()->second;
            QVERIFY(!text.contains("code()")); QVERIFY(!text.contains("keep();"));
            QVERIFY(!text.contains("https://")); QVERIFY(!text.contains("attachment:"));
            QCOMPARE(current->source, requests == 1 ? "" : "en");
            QVERIFY(current->detectionText.contains("Hello World"));
            QVERIFY(current->detectionText.contains("Quote"));
            QVERIFY(!current->detectionText.contains("code()"));
            QVERIFY(!current->detectionText.contains("https://"));
            current->requests.erase(id);
            reply(id, {{"kind", "text"}, {"text", "Translated " + text.trimmed()}, {"source", "en"}, {"final", true}});
        }
        const auto result = language.result("draft");
        QVERIFY(result.value("error").toString().isEmpty());
        const auto text = result.value("text").toString();
        QVERIFY(text.contains("Translated Hello")); QVERIFY(text.contains("code()"));
        QVERIFY(text.contains("keep();")); QVERIFY(text.contains("https://example.org/path?q=1"));
        QVERIFY(text.contains("attachment:abc"));
        QTextDocument document; document.setMarkdown(text);
        QVERIFY(document.toPlainText().contains("Translated World Translated and code() Translated label"));
        bool bold = false;
        for (auto block = document.begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
                if (it.fragment().text().contains("World")) bold = it.fragment().charFormat().fontWeight() == QFont::Bold;
        QVERIFY(bold);
    }
    void cancellationAndOutOfOrderResponsesNeverReplaceNewDraft() {
        LocalLanguage language;
        QVERIFY(language.request("draft", "First", "en", "de"));
        const auto old = current->requests.begin()->first;
        QVERIFY(language.request("draft", "Second", "en", "de"));
        const auto latest = current->requests.begin()->first;
        QVERIFY(latest != old);
        reply(old, {{"kind", "text"}, {"text", "Stale"}});
        QVERIFY(language.result("draft").value("pending").toBool());
        reply(latest, {{"kind", "text"}, {"text", "Zweite"}});
        QVERIFY(language.result("draft").value("text").toString().contains("Zweite"));
        language.cancel("draft");
        reply(latest, {{"kind", "text"}, {"text", "Too late"}});
        QVERIFY(language.result("draft").isEmpty());
    }
    void overloadFailureAndEmptyResultsKeepOriginal() {
        LocalLanguage language;
        QVERIFY(!language.property("busy").toBool());
        for (int i = 0; i < 4; ++i) QVERIFY(language.request(QString::number(i), "Hello", "en", "de"));
        QVERIFY(language.property("busy").toBool());
        QVERIFY(!language.request("overflow", "Unchanged", "en", "de"));
        QCOMPARE(language.result("overflow").value("original"), "Unchanged");
        QVERIFY(!language.result("overflow").value("error").toString().isEmpty());
        auto id = current->requests.begin()->first;
        reply(id, {{"kind", "text"}, {"text", ""}});
        QVERIFY(!language.property("busy").toBool());
        QVERIFY(!language.result("0").value("pending").toBool());
        QVERIFY(!language.result("0").value("error").toString().isEmpty());
        language.clearResults(); QVERIFY(current->requests.empty());
        QVERIFY(!language.request("", "Hello", "en", "de"));
        QVERIFY(!language.request("large", QString(16385, 'a'), "en", "de"));
        QVERIFY(language.request("retry", "Hello", "en", "de"));
    }
    void preparesOnlyOnExplicitActionAndDoesNotRetainResultsForever() {
        LocalLanguage language;
        QVERIFY(language.configureChannel("a", true, "", false, ""));
        QVERIFY(current->requests.empty());
        for (int i = 0; i < 200; ++i) {
            QVERIFY(language.request(QString::number(i), "Hello", "en", "de"));
            const auto id = current->requests.begin()->first; current->requests.erase(id);
            reply(id, {{"kind", "text"}, {"text", "Hallo"}});
        }
        int retained = 0;
        for (int i = 0; i < 200; ++i) retained += !language.result(QString::number(i)).isEmpty();
        QCOMPARE(retained, 160);
    }
};
QTEST_MAIN(LanguageTests)
#else
class NativeLanguageTests final : public QObject {
    Q_OBJECT
private slots:
    void nativeCapabilitiesAndSameLanguageTranslation() {
        LocalLanguage language;
        QVERIFY(language.supported());
        QTRY_VERIFY_WITH_TIMEOUT(!language.languages().isEmpty(), 20000);
        QSet<QString> choices;
        for (const auto& entry : language.languages()) {
            const auto code = entry.toMap().value("id").toString().section('-', 0, 0);
            QVERIFY2(!choices.contains(code), qPrintable("Duplicate language choice: " + code));
            choices.insert(code);
        }
        if (!language.available()) { QVERIFY(!language.request("unavailable", "Hello", "en", "de")); return; }
        QVERIFY(language.request("same", "**Hello world** with `code()`.", "en", "en"));
        QTRY_VERIFY_WITH_TIMEOUT(!language.result("same").value("pending").toBool(), 10000);
        const auto result = language.result("same");
        QVERIFY2(result.value("error").toString().isEmpty(), qPrintable(result.value("error").toString()));
        QVERIFY(result.value("text").toString().contains("Hello world"));
        QVERIFY(result.value("text").toString().contains("code()"));
        QVERIFY(language.request("missing", "This is a sentence.", "en", "zz"));
        QTRY_VERIFY_WITH_TIMEOUT(!language.result("missing").value("pending").toBool(), 12000);
        QVERIFY(!language.result("missing").value("error").toString().isEmpty());
        QVERIFY(language.request("cancelled", "This is another sentence.", "en", "de"));
        language.cancel("cancelled");
        QVERIFY(language.result("cancelled").isEmpty());
    }
    // Explicit local acceptance. Missing Apple models can require a download prompt.
    void prepareAndTranslateText_data() {
        QTest::addColumn<QString>("source"); QTest::addColumn<QString>("target");
        QTest::addColumn<QString>("input"); QTest::addColumn<QString>("expected");
        QTest::newRow("regional-german") << "de-AT" << "en-US" << "Guten Morgen, wie geht es dir?" << "morning";
        QTest::newRow("german-markdown") << "de" << "en"
            << "Bedienprüfung: Hallo, wie geht es dir heute?\n\n**Fett**, `Code` und ein Link: https://example.com\n\n> Eine kurze Nachricht."
            << "today";
        QTest::newRow("english") << "en-US" << "de-DE" << "Hello, how are you today?" << "Hallo";
        QTest::newRow("french") << "fr-FR" << "de-DE" << "Bonjour, comment allez-vous?" << "wie";
        QTest::newRow("spanish") << "es-ES" << "en-US" << "Hola, como estas hoy?" << "Hello";
    }
    void prepareAndTranslateText() {
        QFETCH(QString, source); QFETCH(QString, target); QFETCH(QString, input); QFETCH(QString, expected);
        LocalLanguage language;
        QTRY_VERIFY_WITH_TIMEOUT(language.available(), 20000);
        QVERIFY(language.request("preparation", input, source, target));
        QTRY_VERIFY_WITH_TIMEOUT(!language.result("preparation").value("pending").toBool(), 15000);
        if (!language.preparation().isEmpty()) {
            QVERIFY(language.acceptPreparation());
            QTRY_VERIFY_WITH_TIMEOUT(!language.preparing(), 180000);
            QVERIFY2(language.error().isEmpty(), qPrintable(language.error()));
        }
        QVERIFY(language.request("message", input, "", target));
        QTRY_VERIFY_WITH_TIMEOUT(!language.result("message").value("pending").toBool(), 15000);
        const auto result = language.result("message");
        QVERIFY2(result.value("error").toString().isEmpty(), qPrintable(result.value("error").toString()));
        QVERIFY2(result.value("text").toString().contains(expected, Qt::CaseInsensitive), qPrintable(result.value("text").toString()));
        qInfo() << source << "to" << target << result.value("text");
    }
    // Explicit invocation only. Installs OS assets after Apple's local consent UI.
    void prepareAndTranslateInstalledModels() {
        LocalLanguage language;
        QTRY_VERIFY_WITH_TIMEOUT(language.available(), 20000);
        auto voices = language.voices();
        const auto german = std::find_if(voices.begin(), voices.end(), [](const auto& entry) {
            return entry.toMap().value("language").toString().startsWith("de");
        });
        QVERIFY(german != voices.end());
        QVERIFY(language.configureChannel("native", false, "", true, "de-DE"));
        language.setMembers({QVariantMap{{"id", "native-acceptance"}, {"language", "en-US"}}}, "native");
        QTRY_VERIFY_WITH_TIMEOUT(language.voiceProcessing() || !language.preparation().isEmpty(), 15000);
        if (!language.preparation().isEmpty()) {
            QVERIFY(language.acceptPreparation());
            QTRY_VERIFY_WITH_TIMEOUT(!language.preparing(), 180000);
            QVERIFY2(language.error().isEmpty(), qPrintable(language.error()));
        }
        QVERIFY(language.request("native-translation", "Hello, how are you today?", "en", "de"));
        QTRY_VERIFY_WITH_TIMEOUT(!language.result("native-translation").value("pending").toBool(), 15000);
        QVERIFY2(language.result("native-translation").value("error").toString().isEmpty(), qPrintable(language.result("native-translation").value("error").toString()));
        const auto text = language.result("native-translation").value("text").toString();
        QVERIFY(!text.isEmpty() && !text.contains("how are you today"));
        qInfo() << "Native translated text:" << text;
        QTRY_VERIFY_WITH_TIMEOUT(language.voiceProcessing(), 15000);
        SpeechOutput source;
        const auto english = std::find_if(voices.begin(), voices.end(), [](const auto& entry) {
            return entry.toMap().value("language").toString() == "en-US";
        });
        QVERIFY(english != voices.end());
        QVERIFY(source.start("Hello, how are you today?", 48000, english->toMap().value("id").toString()));
        QTimer feed; feed.setInterval(20);
        double replacementEnergy = 0;
        connect(&feed, &QTimer::timeout, this, [&] {
            std::array<float, 960> frame{}; source.mix(frame, 1);
            language.processVoice("native-acceptance", frame, 0);
            if (language.voiceProcessing()) for (auto sample : frame) replacementEnergy += double(sample) * sample;
        });
        feed.start();
        QTRY_VERIFY_WITH_TIMEOUT(!language.member("native-acceptance").value("transcript").toString().isEmpty(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(replacementEnergy > 0.1, 15000);
        qInfo() << "Native transcript:" << language.member("native-acceptance").value("transcript");
        language.setMembers({}); feed.stop();
    }
    void nativePendingCallbacksDoNotOutliveTheirOwner() {
        for (int i = 0; i < 20; ++i) {
            auto language = std::make_unique<LocalLanguage>();
            language.reset();
        }
        bool drained = false;
        QTimer::singleShot(0, this, [&] { drained = true; });
        QTRY_VERIFY(drained);
    }
};
QTEST_MAIN(NativeLanguageTests)
#endif
#include "local_language_tests.moc"
