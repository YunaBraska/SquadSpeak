#include "local_language.hpp"
#include "local_language_native.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <QTimer>
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
};
Native* current = nullptr;
void reply(int64_t request, QJsonObject event) {
    if (current) { const auto data = QJsonDocument(event).toJson(QJsonDocument::Compact); current->callback(current->context, request, data.constData()); }
}
}
extern "C" {
void* squad_language_create(SquadLanguageCallback callback, void* context) { current = new Native{callback, context, {}}; return current; }
void squad_language_destroy(void* handle) { delete static_cast<Native*>(handle); current = nullptr; }
const char* squad_language_capabilities(void*) { return R"({"available":true,"languages":[{"id":"en","name":"English"},{"id":"de","name":"Deutsch"}],"speechLanguages":[{"id":"en-US","name":"English"}]})"; }
void squad_language_prepare(void*, int64_t req, const char*, const char*, int) { current->requests[req] = "prepare"; }
void squad_language_translate(void*, int64_t req, const char*, const char*, const char* text) { current->requests[req] = QString::fromUtf8(text); }
void squad_language_listen(void*, int64_t req, const char*) { current->requests[req] = "listen"; }
void squad_language_audio(void*, int64_t, const float*, int) {}
void squad_language_cancel(void*, int64_t req) { current->requests.erase(req); }
}
class LanguageTests final : public QObject {
    Q_OBJECT
private slots:
#if defined(Q_OS_MACOS)
    void channelSwitchDiscardsSpeechAndRejectsOldCallbacks() {
        LocalLanguage language;
        const auto voices = language.voices();
        QVERIFY(!voices.isEmpty());
        QVERIFY(language.configure("alice", "en", "de", voices.first().toMap().value("id").toString(), false));
        language.setMembers({"alice"}, "first");
        const auto first = current->requests.begin()->first;
        reply(first, {{"kind", "ready"}});
        reply(first, {{"kind", "text"}, {"text", "Previous conversation"}, {"final", true}});
        const auto pending = current->requests.rbegin()->first;
        QCOMPARE(language.member("alice").value("transcript"), "Previous conversation");
        language.setMembers({"alice"}, "second");
        QVERIFY(language.member("alice").value("transcript").toString().isEmpty());
        QCOMPARE(current->requests.size(), size_t(1));
        QVERIFY(current->requests.begin()->first != first);
        reply(first, {{"kind", "ready"}});
        reply(pending, {{"kind", "text"}, {"text", "Too late"}});
        QVERIFY(!language.voiceProcessing());
        language.setMembers({}, "second");
        QVERIFY(current->requests.empty());
    }
    void assignedVoiceReplacesOnlyItsOwnAudioAndFailsBackImmediately() {
        LocalLanguage language;
        const auto voices = language.voices();
        QVERIFY(!voices.isEmpty());
        const auto selected = voices.first().toMap();
        QVERIFY(language.configure("alice", "en", "de", selected.value("id").toString(), false));
        QCOMPARE(language.member("alice").value("target"), selected.value("language"));
        language.setMembers({"alice", "bob"});
        QCOMPARE(current->requests.size(), size_t(1));
        const auto listening = current->requests.begin()->first;
        std::array<float, 960> frame; frame.fill(0.2f);
        language.processVoice("alice", frame, 0);
        QCOMPARE(frame.front(), 0.2f);
        reply(listening, {{"kind", "ready"}});
        QVERIFY(language.voiceProcessing());
        language.processVoice("bob", frame, 0);
        QCOMPARE(frame.front(), 0.2f);
        language.processVoice("alice", frame, 0);
        QCOMPARE(frame.front(), 0.0f);
        reply(listening, {{"kind", "text"}, {"text", "Hello"}, {"final", false}});
        QVERIFY(language.member("alice").value("transcript").toString().isEmpty());
        reply(listening, {{"kind", "text"}, {"text", "Hello"}, {"final", true}});
        QCOMPARE(language.member("alice").value("transcript"), "Hello");
        auto translation = current->requests.rbegin()->first;
        QVERIFY(translation != listening);
        reply(translation, {{"kind", "error"}});
        QVERIFY(!language.voiceProcessing());
        frame.fill(0.2f); language.processVoice("alice", frame, 0); QCOMPARE(frame.front(), 0.2f);
        QVERIFY(!language.member("alice").value("status").toString().isEmpty());
        reply(listening, {{"kind", "ready"}});
        QVERIFY(!language.voiceProcessing());
        language.setMembers({});
        QVERIFY(language.member("alice").value("transcript").toString().isEmpty());
        QVERIFY(current->requests.empty());
        language.setMembers({"alice"});
        QCOMPARE(current->requests.size(), size_t(1));
        language.suspend(true); QVERIFY(current->requests.empty());
    }
#endif
    void preferencesAreLocalValidatedAndPersistent() {
        QTemporaryDir dir;
        const auto file = dir.filePath("languages.json");
        {
            LocalLanguage language(file);
            QVERIFY(language.available());
            QVERIFY(language.member("alice").isEmpty());
            QVERIFY(language.configure("alice", "en-US", "de", {}, true));
            QVERIFY(language.setChannelTarget("channel", "fr"));
            QVERIFY(!language.configure("", "en", "de", {}, true));
            QVERIFY(!language.configure("alice", "../../en", "de", {}, true));
            QVERIFY(!language.configure("alice", "en", "de", "missing-voice", true));
            QVERIFY(!language.setChannelTarget("channel", "" + QString(500, 'x')));
        }
        LocalLanguage restored(file);
        QCOMPARE(restored.member("alice").value("source"), "en-US");
        QVERIFY(restored.member("alice").value("incoming").toBool());
        QCOMPARE(restored.channelTarget("channel"), "fr");
        QVERIFY(restored.setChannelTarget("channel", {}));
        QCOMPARE(restored.channelTarget("channel"), QString{});
        QVERIFY(restored.member("alice").value("transcript").toString().isEmpty());
    }
    void corruptAndUnwritableSettingsDoNotEnableAnything() {
        QTemporaryDir dir;
        QFile file(dir.filePath("broken")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("{}"); file.close();
        {
            LocalLanguage broken(file.fileName());
            QVERIFY(!broken.error().isEmpty()); QVERIFY(broken.member("alice").isEmpty());
        }
        LocalLanguage unwritable(dir.filePath("missing/file"));
        QVERIFY(!unwritable.configure("alice", "en", "de", {}, true));
        QVERIFY(!unwritable.error().isEmpty()); QVERIFY(unwritable.member("alice").isEmpty());
    }
    void markdownCodeImagesAndLinkAddressesSurviveTranslation() {
        LocalLanguage language;
        const QString input = "# Hello\n\n**World** and `code()` [label](https://example.org/path?q=1)\n\n```cpp\nkeep();\n```\n\n![image](attachment:abc)\n\n> Quote\n\n| Table | Value |\n| --- | --- |\n| One | Two |\n";
        QVERIFY(language.request("draft", input, "en", "de"));
        int requests = 0;
        while (language.result("draft").value("pending").toBool()) {
            QVERIFY(++requests < 30);
            const auto id = current->requests.begin()->first;
            const auto text = current->requests.begin()->second;
            QVERIFY(!text.contains("code()")); QVERIFY(!text.contains("keep();"));
            QVERIFY(!text.contains("https://")); QVERIFY(!text.contains("attachment:"));
            current->requests.erase(id);
            reply(id, {{"kind", "text"}, {"text", "Translated " + text}, {"final", true}});
        }
        const auto result = language.result("draft");
        QVERIFY(result.value("error").toString().isEmpty());
        const auto text = result.value("text").toString();
        QVERIFY(text.contains("Translated Hello")); QVERIFY(text.contains("code()"));
        QVERIFY(text.contains("keep();")); QVERIFY(text.contains("https://example.org/path?q=1"));
        QVERIFY(text.contains("attachment:abc"));
        QTextDocument document; document.setMarkdown(text);
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
        QVERIFY(language.configure("alice", "en", "de", {}, true));
        QVERIFY(current->requests.empty());
        QVERIFY(language.prepare("alice"));
        QVERIFY(!language.prepare("alice"));
        reply(current->requests.begin()->first, {{"kind", "error"}});
        QVERIFY(!language.error().isEmpty());
        current->requests.clear();
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
    // Explicit invocation only. Installs OS assets after Apple's local consent UI.
    void prepareAndTranslateInstalledModels() {
        LocalLanguage language;
        QTRY_VERIFY_WITH_TIMEOUT(language.available(), 20000);
        auto voices = language.voices();
        const auto german = std::find_if(voices.begin(), voices.end(), [](const auto& entry) {
            return entry.toMap().value("language").toString().startsWith("de");
        });
        QVERIFY(german != voices.end());
        QVERIFY(language.configure("native-acceptance", "en-US", "de-DE", german->toMap().value("id").toString(), true));
        QVERIFY(language.prepare("native-acceptance"));
        QTRY_VERIFY_WITH_TIMEOUT(!language.preparing(), 180000);
        QVERIFY2(language.error().isEmpty(), qPrintable(language.error()));
        QVERIFY(language.request("native-translation", "Hello, how are you today?", "en", "de"));
        QTRY_VERIFY_WITH_TIMEOUT(!language.result("native-translation").value("pending").toBool(), 15000);
        QVERIFY2(language.result("native-translation").value("error").toString().isEmpty(), qPrintable(language.result("native-translation").value("error").toString()));
        const auto text = language.result("native-translation").value("text").toString();
        QVERIFY(!text.isEmpty() && !text.contains("how are you today"));
        qInfo() << "Native translated text:" << text;
        language.setMembers({"native-acceptance"});
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
