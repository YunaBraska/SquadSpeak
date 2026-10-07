#include "radio_player.hpp"
#include "radio_station.hpp"
#include "local_channel.hpp"
#include "event_wait.hpp"
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTranslator>
#include <QTest>
#include <QtEndian>
#include <cmath>
#include <numbers>
#include <algorithm>
#include <memory>


class RadioTests final : public QObject {
    Q_OBJECT
private slots:
    void damagedSettingsUseSelectedLanguage_data() {
        QTest::addColumn<QString>("language");
        QTest::addColumn<QByteArray>("settings");
        QTest::addColumn<QByteArray>("source");
        const QJsonObject station{{"id", "78bc50c7-99e8-439f-8939-839ab4be0293"},
                                  {"name", "Station"}, {"url", "https://example.test/live"}};
        for (const auto* language : {"en", "de", "ar"}) {
            const auto row = [language](const char* name, const QByteArray& settings, const char* source) {
                QTest::newRow(qPrintable(QString::fromLatin1(language) + '-' + name))
                    << QString::fromLatin1(language) << settings << QByteArray(source);
            };
            row("oversized", QByteArray(384 * 1024 + 1, ' '), "Radio stations could not be read.");
            row("document", "{", "Saved radio stations are invalid.");
            const QJsonObject empty{{"version", 1}, {"stations", QJsonArray{}}, {"playing", false}, {"selected", ""}};
            auto document = empty;
            auto invalid = station;
            invalid.insert("unexpected", true);
            document.insert("stations", QJsonArray{invalid});
            row("station", QJsonDocument(document).toJson(), "Saved radio station is invalid.");
            document.insert("stations", QJsonArray{station, station});
            row("duplicate", QJsonDocument(document).toJson(), "Saved radio station is invalid.");
            document = empty;
            document.insert("catalogSelection", invalid);
            row("catalog-selection", QJsonDocument(document).toJson(), "Saved catalog selection is invalid.");
            document = empty;
            document.insert("selected", station.value("id"));
            row("selection", QJsonDocument(document).toJson(), "Saved radio selection is invalid.");
            invalid = station;
            invalid.insert("id", "invalid");
            document = empty;
            document.insert("stations", QJsonArray{invalid});
            row("identity", QJsonDocument(document).toJson(), "Invalid radio station identity.");
            invalid = station;
            invalid.insert("url", "https://user:secret@example.test/live");
            document.insert("stations", QJsonArray{invalid});
            row("url", QJsonDocument(document).toJson(), "Enter a direct HTTP or HTTPS audio stream URL without credentials.");
        }
    }
    void damagedSettingsUseSelectedLanguage() {
        QFETCH(QString, language);
        QFETCH(QByteArray, settings);
        QFETCH(QByteArray, source);
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session.json"));
        QTranslator catalog;
        if (language != "en") {
            QVERIFY(catalog.load(":/i18n/squadspeak_" + language + ".qm"));
            QVERIFY(QCoreApplication::installTranslator(&catalog));
        }
        const auto restore = qScopeGuard([&] { QCoreApplication::removeTranslator(&catalog); });
        const auto expected = QCoreApplication::translate("RadioPlayer", source.constData());
        if (language != "en") QVERIFY(expected != QString::fromUtf8(source));
        const auto path = dir.filePath("radio.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(settings), settings.size());
        file.close();
        try {
            RadioPlayer radio(path);
            QFAIL("Damaged radio settings were accepted.");
        } catch (const std::exception& error) {
            QCOMPARE(QString::fromUtf8(error.what()), expected);
        }
    }
    void invalidStreamUsesSelectedLanguageWithoutStartingAProbe() {
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session.json"));
        QVERIFY(session.setLanguage("de"));
        QTranslator catalog;
        QVERIFY(catalog.load(":/i18n/squadspeak_" + session.language() + ".qm"));
        QVERIFY(QCoreApplication::installTranslator(&catalog));
        const auto restore = qScopeGuard([&] { QCoreApplication::removeTranslator(&catalog); });
        RadioPlayer radio(dir.filePath("radio.json"));
        for (const auto* url : {"file:///private/test", "https://user:secret@example.test/live", "https://example.test/live#fragment"}) {
            QVERIFY(!radio.saveStation("", "Station", url));
            QCOMPARE(radio.error(), QString::fromUtf8("Gib eine direkte HTTP- oder HTTPS-Audiostream-URL ohne Zugangsdaten ein."));
            QVERIFY(!radio.checking());
            QVERIFY(radio.stations().isEmpty());
        }
        QVERIFY(!radio.saveStation("invalid", "Station", "https://example.test/live"));
        QCOMPARE(radio.error(), QString::fromUtf8("Ungültige Radiosender-ID."));
        QVERIFY(!radio.checking());
    }
    void catalogIsOfflinePagedSearchableAndResumesWithoutBecomingAManualStation() {
        QTemporaryDir dir;
        const auto path = dir.filePath("radio.json");
        RadioPlayer radio(path);
        const auto first = radio.searchStations("", 0, 12);
        const auto entries = first.value("items").toList();
        QCOMPARE(entries.size(), 12);
        QSet<QString> countries;
        for (const auto& entry : entries) countries.insert(entry.toMap().value("countryCode").toString());
        QVERIFY(countries.size() >= 8);
        QVERIFY(first.value("total").toInt() > 2000);
        QVERIFY(radio.stations().isEmpty());
        QVERIFY(radio.catalogInfo().value("source").toString().startsWith("https://"));
        QVERIFY(radio.catalogInfo().value("retrievedAt").toString().startsWith("2026-10-03"));
        const auto next = radio.searchStations("", 12, 12).value("items").toList();
        QCOMPARE(next.size(), 12);
        QSet<QString> ids;
        for (const auto& value : entries + next) {
            const auto item = value.toMap();
            QVERIFY(!item.value("manual").toBool());
            QVERIFY(!item.value("checkedAt").toString().isEmpty());
            QVERIFY(!ids.contains(item.value("id").toString()));
            ids.insert(item.value("id").toString());
        }
        QVERIFY(radio.searchStations("", -1, 12).value("items").toList().isEmpty());
        QVERIFY(radio.searchStations("", 0, 129).value("items").toList().isEmpty());
        QCOMPARE(radio.searchStations(QString(257, 'a'), 0, 12).value("total").toInt(), 0);
        for (const auto& query : {"jazz", "classical", "rock", "brazil", "japan", "arabic"})
            QVERIFY2(radio.searchStations(query).value("total").toInt() > 0, query);
        const auto chosen = entries.first().toMap();
        QVERIFY(radio.play(chosen.value("id").toString()));
        QCOMPARE(radio.state(), QString("unavailable"));
        QVERIFY(radio.stations().isEmpty());
        RadioPlayer restarted(path);
        QVERIFY(restarted.active());
        QCOMPARE(restarted.selectedId(), chosen.value("id").toString());
        QCOMPARE(restarted.stationName(), chosen.value("name").toString());
        QVERIFY(!restarted.removeStation(chosen.value("id").toString()));
        QVERIFY(restarted.stop());
    }

    void searchRanksManualStationsAndToleratesAccentsInitialsAndTypos() {
        QTemporaryDir dir; RadioStation station;
        RadioPlayer radio(dir.filePath("radio.json"));
        QSignalSpy saved(&radio, &RadioPlayer::stationSaved);
        QVERIFY(radio.saveStation("", "Café Night Jazz", station.url()));
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 17000);
        QVERIFY(saved.first().at(1).toBool());
        const auto id = radio.stations().first().toMap().value("id").toString();
        for (const auto& query : {"", "cafe", "cnj", "night jaz", "nigth jazz"}) {
            const auto results = radio.searchStations(query).value("items").toList();
            QVERIFY2(!results.isEmpty(), query);
            QCOMPARE(results.first().toMap().value("id").toString(), id);
            QVERIFY(results.first().toMap().value("manual").toBool());
        }
        QVERIFY(radio.searchStations("unfindablexyzq").value("items").toList().isEmpty());
        QVERIFY(radio.removeStation(id));
        const auto after = radio.searchStations("cnj").value("items").toList();
        QVERIFY(std::none_of(after.cbegin(), after.cend(), [&](const auto& v) { return v.toMap().value("id").toString() == id; }));
    }

    void oneNamedSystemBotWritesAndStreamsWithoutJoiningAsAMember() {
        QTemporaryDir dir;
        qint64 now = 1700000000000;
        const auto identity = TlsIdentity::create();
        VoiceSession owner(dir.filePath("owner.session")), member(dir.filePath("member.session"));
        LocalChannel host(owner, dir.filePath("host.channel"), identity, [&now] { return now; });
        LocalChannel client(member, dir.filePath("member.channel"), TlsIdentity::create());
        QVERIFY(host.configureHost({{"botName", "Clockwork"}}));
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.decide(client.ownId(), true));
        QVERIFY(client.join(host.ownId(), "127.0.0.1", host.port())); QTRY_VERIFY(client.joined());
        QVERIFY(host.sendSystemMessage("The system is ready."));
        QTRY_COMPARE(client.messages().size(), 2);
        const auto announcement = client.messages().last().toMap();
        QCOMPARE(announcement.value("name").toString(), QString("Clockwork"));
        QCOMPARE(announcement.value("sender").toString(), host.musicId());
        QVERIFY(host.setMusicState("Station title", "playing", true));
        QSignalSpy packets(&client, &LocalChannel::audioReceived);
        squad::VoiceMixer encoder;
        std::array<float, 960> input{}; input.fill(0.1f);
        const auto encoded = encoder.encode(input, 48000).first();
        QTRY_VERIFY(host.sendMusic(encoded));
        QTRY_VERIFY_WITH_TIMEOUT(([&] { host.sendMusic(encoded); return !packets.isEmpty(); })(), 3000);
        QCOMPARE(packets.last().first().toString(), announcement.value("sender").toString());
        QCOMPARE(host.hostClients().size(), 1); QCOMPARE(host.hostParticipants().size(), 1);
        QCOMPARE(client.participants().size(), 1); QCOMPARE(client.chatMembers().size(), 1);
        QCOMPARE(client.chatBot().value("id").toString(), host.musicId());
        QCOMPARE(client.chatBot().value("name").toString(), QString("Clockwork"));
        const auto clientMembers = client.chatMembers();
        QVERIFY(std::none_of(clientMembers.cbegin(), clientMembers.cend(), [](const auto& value) {
            return value.toMap().value("music").toBool();
        }));
        const auto hostMembers = host.hostParticipants();
        QVERIFY(std::none_of(hostMembers.cbegin(), hostMembers.cend(), [&](const auto& value) {
            return value.toMap().value("id").toString() == host.musicId();
        }));
        QVERIFY(!client.chatBot().value("sleeping").toBool());
        now += 3600001;
        QTRY_VERIFY_WITH_TIMEOUT(client.chatBot().value("sleeping").toBool(), 3000);
        QVERIFY(host.sendMusic(encoded));
        QTRY_VERIFY_WITH_TIMEOUT(!client.chatBot().value("sleeping").toBool(), 3000);
        QVERIFY(host.configureHost({{"botName", "Renamed owl"}}));
        QTRY_COMPARE(client.messages().last().toMap().value("name").toString(), QString("Renamed owl"));
        QVERIFY(!host.configureHost({{"botName", ""}}));
        LocalChannel reloaded(owner, dir.filePath("host.channel"), identity);
        QCOMPARE(reloaded.hostConfiguration().value("botName").toString(), QString("Renamed owl"));
        QCOMPARE(reloaded.musicId(), host.musicId());
    }

    void playlistValidationPersistenceAndRestartIntent() {
        QTemporaryDir dir; RadioStation station;
        const auto path = dir.filePath("radio.json");
        QString id;
        {
            RadioPlayer radio(path);
            QVERIFY(!radio.active()); QVERIFY(radio.stations().isEmpty());
            for (const auto* url : {"file:///etc/passwd", "ftp://example.test/a", "https://user:password@example.test/a", "https://example.test/a#fragment", "missing"})
                QVERIFY(!radio.saveStation("", "Station", url));
            QVERIFY(!radio.saveStation("", "", "https://example.test/live"));
            QVERIFY(radio.saveStation("", "Station", station.url()));
            QTRY_COMPARE(radio.stations().size(), 1);
            id = radio.stations().first().toMap().value("id").toString();
            QVERIFY(radio.play(id)); QVERIFY(radio.active()); QCOMPARE(radio.state(), "unavailable");
            QVERIFY(!radio.play("unknown")); QCOMPARE(radio.selectedId(), id);
            QVERIFY(radio.saveStation(id, "Renamed", station.url() + "?updated"));
            QTRY_COMPARE(radio.stationName(), "Renamed");
        }
        RadioPlayer restored(path);
        QVERIFY(restored.active()); QCOMPARE(restored.selectedId(), id); QCOMPARE(restored.stationName(), "Renamed");
        QVERIFY(restored.stop()); QVERIFY(!restored.active());
        QVERIFY(!restored.removeStation("missing"));
        QVERIFY(restored.removeStation(id)); QVERIFY(restored.stations().isEmpty()); QVERIFY(restored.selectedId().isEmpty());
        RadioPlayer reloaded(path); QVERIFY(reloaded.stations().isEmpty()); QVERIFY(!reloaded.active());
        RadioPlayer failed(dir.filePath("missing/radio.json"));
        QSignalSpy failedSave(&failed, &RadioPlayer::stationSaved);
        QVERIFY(failed.saveStation("", "Station", station.url()));
        QTRY_COMPARE(failedSave.size(), 1); QVERIFY(!failedSave.first().at(1).toBool());
        QVERIFY(failed.stations().isEmpty());
    }

    void stationIsPersistedOnlyAfterDecodedAudio() {
        QTemporaryDir dir; RadioStation station;
        const auto path = dir.filePath("radio.json");
        RadioPlayer radio(path);
        QVERIFY(radio.setAvailable(true));
        QVERIFY(radio.saveStation("", "Verified radio", station.url()));
        QVERIFY(radio.stations().isEmpty());
        QVERIFY(!QFile::exists(path));
        QTRY_COMPARE_WITH_TIMEOUT(radio.stations().size(), 1, 15000);
        RadioPlayer reloaded(path);
        QCOMPARE(reloaded.stations().size(), 1);
        QCOMPARE(reloaded.stations().first().toMap().value("name").toString(), QString("Verified radio"));
        QVERIFY(!reloaded.active());
    }

    void invalidAudioResponseIsNeverSaved() {
        QTemporaryDir dir; RadioStation station; station.audio = false;
        const auto path = dir.filePath("radio.json");
        RadioPlayer radio(path);
        QVERIFY(radio.setAvailable(true));
        QVERIFY(radio.saveStation("", "HTML radio", station.url()));
        QVERIFY(radio.stations().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!radio.checking(), 16000);
        QVERIFY(!radio.error().isEmpty());
        QVERIFY(radio.stations().isEmpty());
        QVERIFY(!QFile::exists(path));
    }

    void checksCanBeCancelledAndRetriedWithoutInterruptingPlayback() {
        QTemporaryDir dir; RadioStation station, pending; pending.stalled = true;
        RadioPlayer radio(dir.filePath("radio.json"));
        QSignalSpy saved(&radio, &RadioPlayer::stationSaved), packets(&radio, &RadioPlayer::audioPacket);
        QVERIFY(radio.setAvailable(true));
        QVERIFY(radio.saveStation("", "First", station.url()));
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 17000);
        QVERIFY2(saved.last().at(1).toBool(), qPrintable(radio.error()));
        const auto id = radio.stations().first().toMap().value("id").toString();
        QVERIFY(radio.play(id)); QTRY_VERIFY(packets.size() >= 3);
        QVERIFY(radio.saveStation("", "Pending", pending.url()));
        QTRY_COMPARE(pending.requests, 1);
        QVERIFY(!radio.saveStation("", "Concurrent", station.url()));
        QCOMPARE(radio.stations().size(), 1);
        const auto count = packets.size(); QTRY_VERIFY(packets.size() > count + 3);
        QCOMPARE(radio.state(), "playing");
        QVERIFY(radio.cancelStationCheck()); QVERIFY(!radio.checking());
        QCOMPARE(saved.size(), 2); QVERIFY(!saved.last().at(1).toBool());
        pending.stalled = false;
        QVERIFY(radio.saveStation("", "Second", pending.url()));
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 3, 17000);
        QVERIFY2(saved.last().at(1).toBool(), qPrintable(radio.error()));
        QCOMPARE(radio.stations().size(), 2);
        QCOMPARE(radio.stations().first().toMap().value("name").toString(), QString("Second"));
        QCOMPARE(radio.selectedId(), id); QCOMPARE(radio.state(), "playing");
        const auto secondId = radio.stations().first().toMap().value("id").toString();
        QVERIFY(radio.saveStation("", "Repeated", pending.url()));
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 4, 17000);
        QVERIFY2(saved.last().at(1).toBool(), qPrintable(radio.error()));
        QCOMPARE(radio.stations().size(), 2);
        QCOMPARE(radio.stations().first().toMap().value("id").toString(), secondId);
        QVERIFY(!radio.saveStation(id, "Duplicate", pending.url()));
        pending.audio = false;
        QVERIFY(radio.saveStation(id, "Rejected replacement", pending.url() + "?invalid"));
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 5, 17000); QVERIFY(!saved.last().at(1).toBool());
        QCOMPARE(radio.stationName(), QString("First"));
        const auto afterFailure = packets.size(); QTRY_VERIFY(packets.size() > afterFailure + 3);
        QVERIFY(radio.stop());
    }

    void stalledCheckTimesOutWithoutSavingOrRetrying() {
        QTemporaryDir dir; RadioStation station; station.stalled = true;
        RadioPlayer radio(dir.filePath("radio.json"));
        QSignalSpy saved(&radio, &RadioPlayer::stationSaved);
        QVERIFY(radio.saveStation("", "Stalled", station.url()));
        QTRY_COMPARE(station.requests, 1);
        QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, 17000);
        QVERIFY(!saved.first().at(1).toBool()); QVERIFY(!radio.checking());
        QVERIFY(radio.error().contains("15 seconds")); QVERIFY(radio.stations().isEmpty());
        QVERIFY(!QFile::exists(dir.filePath("radio.json")));
        QTest::qWait(100); QCOMPARE(station.requests, 1);
    }

    void streamReachesTwoClientsWithoutHostOwnerAndRecoversAfterDisconnect() {
        QTemporaryDir dir; RadioStation station;
        VoiceSession owner(dir.filePath("owner.session")), a(dir.filePath("a.session")), b(dir.filePath("b.session"));
        LocalChannel host(owner, dir.filePath("host.channel"), TlsIdentity::create());
        LocalChannel first(a, dir.filePath("a.channel"), TlsIdentity::create());
        LocalChannel second(b, dir.filePath("b.channel"), TlsIdentity::create());
        QVERIFY(host.listen(QHostAddress::LocalHost)); QVERIFY(host.startHost());
        QVERIFY(host.decide(first.ownId(), true)); QVERIFY(host.decide(second.ownId(), true));
        QVERIFY(first.join(host.ownId(), "127.0.0.1", host.port()));
        QVERIFY(second.join(host.ownId(), "127.0.0.1", host.port()));
        QTRY_VERIFY(first.joined() && second.joined()); QVERIFY(!host.joined());
        RadioPlayer radio(dir.filePath("radio.json"));
        connect(&radio, &RadioPlayer::changed, &host, [&] { host.setMusicState(radio.stationName(), radio.state(), radio.active()); });
        connect(&radio, &RadioPlayer::audioPacket, &host, &LocalChannel::sendMusic);
        QSignalSpy audioA(&first, &LocalChannel::audioReceived), audioB(&second, &LocalChannel::audioReceived);
        QVERIFY(radio.setAvailable(true)); QVERIFY(radio.saveStation("", "Local radio", station.url()));
        QTRY_COMPARE(radio.stations().size(), 1);
        const auto id = radio.stations().first().toMap().value("id").toString();
        QVERIFY(radio.play(id));
        QTRY_COMPARE_WITH_TIMEOUT(radio.state(), QString("playing"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(audioA.size() >= 15 && audioB.size() >= 15, 10000);
        QCOMPARE(first.participants().size(), 2); QCOMPARE(second.participants().size(), 2);
        QCOMPARE(audioA.first().at(0).toString(), host.musicId());
        squad::VoiceMixer decoder; decoder.setAutomatic(false);
        double rms = 0;
        for (qsizetype i = 0; i < audioA.size(); ++i) {
            QVERIFY(decoder.receive(host.musicId(), audioA.at(i).at(1).toByteArray(), i * 20));
            const auto pcm = decoder.render(48000, i * 20);
            double energy = 0; for (const auto sample : pcm) energy += sample * sample;
            rms = std::max(rms, std::sqrt(energy / pcm.size()));
        }
        QVERIFY2(rms > 0.05, qPrintable(QString::number(rms)));
        const auto requests = station.requests;
        station.drop();
        QTRY_VERIFY_WITH_TIMEOUT(station.requests > requests, 20000);
        QTRY_COMPARE_WITH_TIMEOUT(radio.state(), QString("playing"), 15000);
        const auto received = audioA.size(); QTRY_VERIFY(audioA.size() > received + 5);
        QVERIFY(radio.stop()); QTRY_COMPARE(first.participants().size(), 2);
        QTest::qWait(150); const auto stopped = audioA.size();
        QTest::qWait(250); QCOMPARE(audioA.size(), stopped);
        const auto stoppedRequests = station.requests;
        QTest::qWait(1200); QCOMPARE(station.requests, stoppedRequests);
    }

    void pendingReconnectIsCancelledByStopAndRemoval() {
        QTemporaryDir dir; RadioStation station;
        RadioPlayer radio(dir.filePath("radio.json")); radio.setAvailable(true);
        QVERIFY(radio.saveStation("", "Unavailable", station.url()));
        QTRY_COMPARE(radio.stations().size(), 1);
        station.running = false;
        const auto id = radio.stations().first().toMap().value("id").toString();
        QVERIFY(radio.play(id));
        QTRY_COMPARE_WITH_TIMEOUT(radio.state(), QString("reconnecting"), 15000);
        QVERIFY(radio.removeStation(id)); QCOMPARE(radio.state(), "stopped");
        const auto requests = station.requests;
        QTest::qWait(1300); QCOMPARE(station.requests, requests);
        QVERIFY(!radio.active());
    }

    void boundPlayersEagerlyRestoreOwnedChannelIntent() {
        QTemporaryDir dir;
        RadioStation station;
        VoiceSession session(dir.filePath("session.json"));
        LocalChannel host(session, dir.filePath("channel.json"), TlsIdentity::create());
#if SQUADSPEAK_STORE_BUILD
        QVERIFY(!session.setSupporterEnabled(true));
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.startHost());
        QVERIFY(host.addOwnedChannel("Restored radio").isEmpty());
        return;
#else
        QVERIFY(session.setSupporterEnabled(true));
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.startHost());
        const auto id = host.addOwnedChannel("Restored radio");
        QVERIFY(!id.isEmpty());
        const auto storage = host.ownChannel(id)->radioStorageFile();
        QString stationId;
        {
            RadioPlayer seed(storage);
            QVERIFY(seed.saveStation("", "Restored", station.url()));
            QTRY_COMPARE_WITH_TIMEOUT(seed.stations().size(), 1, 15000);
            stationId = seed.stations().first().toMap().value("id").toString();
            QVERIFY(seed.play(stationId));
            QCOMPARE(seed.state(), QString("unavailable"));
        }
        const auto requestsAfterSeed = station.requests;
        RadioPlayer root(dir.filePath("radio.json"));
        QVERIFY(root.bind(host));
        QTRY_VERIFY_WITH_TIMEOUT(station.requests > requestsAfterSeed, 15000);
        auto* restored = root.forChannel(id);
        QVERIFY(restored);
        QCOMPARE(restored->selectedId(), stationId);
        QVERIFY(restored->active());
        QVERIFY(restored->state() == "playing" || restored->state() == "connecting" || restored->state() == "reconnecting");
#endif
    }

    void boundPlayersUseIndependentChannelStorageAndTwoStreamLimit() {
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session.json"));
        LocalChannel host(session, dir.filePath("channel.json"), TlsIdentity::create());
#if SQUADSPEAK_STORE_BUILD
        QVERIFY(!session.setSupporterEnabled(true));
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.startHost());
        QVERIFY(host.addOwnedChannel("Second radio").isEmpty());
        return;
#else
        QVERIFY(session.setSupporterEnabled(true));
        QVERIFY(host.listen(QHostAddress::LocalHost));
        QVERIFY(host.startHost());
        RadioPlayer root(dir.filePath("radio.json"));
        QVERIFY(root.bind(host));
        const auto firstId = host.addOwnedChannel("Second radio");
        const auto secondId = host.addOwnedChannel("Third radio");
        QVERIFY(!firstId.isEmpty()); QVERIFY(!secondId.isEmpty());
        QTRY_VERIFY(host.ownChannel(firstId)->hosting() && host.ownChannel(secondId)->hosting());
        auto* first = root.forChannel(firstId);
        auto* second = root.forChannel(secondId);
        QVERIFY(first && second && first != &root && second != &root && first != second);
        QCOMPARE(root.forChannel(host.channelId()), &root);
        QVERIFY(!root.forChannel("missing-channel"));

        RadioStation station;
        for (auto* radio : {&root, first, second}) {
            QVERIFY(radio->setAvailable(true));
            QVERIFY(radio->saveStation("", "Shared stream", station.url()));
            QTRY_COMPARE_WITH_TIMEOUT(radio->stations().size(), 1, 15000);
        }
        const auto rootId = root.stations().first().toMap().value("id").toString();
        const auto firstStationId = first->stations().first().toMap().value("id").toString();
        const auto secondStationId = second->stations().first().toMap().value("id").toString();
        QVERIFY(root.play(rootId)); QVERIFY(first->play(firstStationId));
        QTRY_VERIFY_WITH_TIMEOUT(root.state() == "playing" && first->state() == "playing", 15000);
        auto* firstChannel = host.ownChannel(firstId);
        QVERIFY(firstChannel);
        QVERIFY(firstChannel->setScreenSharing(true));
        QTRY_COMPARE_WITH_TIMEOUT(first->state(), QString("unavailable"), 5000);
        QCOMPARE(root.state(), QString("playing"));
        QVERIFY(firstChannel->setScreenSharing(false));
        QTRY_COMPARE_WITH_TIMEOUT(first->state(), QString("playing"), 15000);
        QVERIFY(second->setAvailable(false));
        QVERIFY(second->play(secondStationId));
        QVERIFY(second->setAvailable(true));
        QCOMPARE(second->state(), QString("unavailable"));
        const auto pendingSelection = second->selectedId();
        QVERIFY(!second->play(secondStationId));
        QVERIFY(second->stations().size() == 1);
        QCOMPARE(second->selectedId(), pendingSelection);
        QVERIFY(root.removeStation(rootId));
        QCOMPARE(root.state(), QString("stopped"));
        QTRY_VERIFY_WITH_TIMEOUT(second->state() == "playing", 15000);
        QVERIFY(second->active());
        QPointer<RadioPlayer> removed(second);
        QVERIFY(host.removeOwnedChannel(secondId));
        QTRY_VERIFY(removed.isNull());
        QVERIFY(!root.forChannel(secondId));
#endif
    }

    void boundRadioStopsOnSupporterExpiryAndResumesAfterRenewal() {
#if SQUADSPEAK_STORE_BUILD
        QTemporaryDir dir;
        VoiceSession session(dir.filePath("session.json"));
        QVERIFY(!session.setSupporterEnabled(true));
        return;
#else
        QTemporaryDir dir; RadioStation station;
        VoiceSession session(dir.filePath("session.json"));
        LocalChannel host(session, dir.filePath("channel.json"), TlsIdentity::create());
        QVERIFY(session.setSupporterEnabled(true));
        QVERIFY(host.listen(QHostAddress::LocalHost)); QVERIFY(host.startHost());
        const auto id = host.addOwnedChannel("Expiry radio"); QVERIFY(!id.isEmpty());
        RadioPlayer root(dir.filePath("radio.json")); QVERIFY(root.bind(host));
        auto* child = root.forChannel(id); QVERIFY(child);
        QVERIFY(child->setAvailable(true));
        QVERIFY(child->saveStation("", "Expiry stream", station.url()));
        QTRY_COMPARE_WITH_TIMEOUT(child->stations().size(), 1, 15000);
        QVERIFY(child->play(child->stations().first().toMap().value("id").toString()));
        QTRY_COMPARE_WITH_TIMEOUT(child->state(), QString("playing"), 15000);
        QVERIFY(session.setSupporterEnabled(false));
        QTRY_VERIFY_WITH_TIMEOUT(child->state() == "unavailable" || child->state() == "stopped", 5000);
        QVERIFY(child->active());
        QVERIFY(session.setSupporterEnabled(true));
        QTRY_VERIFY_WITH_TIMEOUT(host.ownChannel(id)->hosting(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(child->state(), QString("playing"), 15000);
#endif
    }

    void boundRadioOutlivesDestroyedChannel() {
        QTemporaryDir dir; RadioStation station;
        auto radio = std::make_unique<RadioPlayer>(dir.filePath("radio.json"));
        QPointer<RadioPlayer> ownedRadio;
        {
            VoiceSession session(dir.filePath("session.json"));
            LocalChannel host(session, dir.filePath("channel.json"), TlsIdentity::create());
            QVERIFY(host.listen(QHostAddress::LocalHost)); QVERIFY(host.startHost());
            QVERIFY(radio->bind(host)); QVERIFY(radio->setAvailable(true));
            QVERIFY(radio->saveStation("", "Surviving stream", station.url()));
            QTRY_COMPARE_WITH_TIMEOUT(radio->stations().size(), 1, 15000);
            QVERIFY(radio->play(radio->stations().first().toMap().value("id").toString()));
            QTRY_COMPARE_WITH_TIMEOUT(radio->state(), QString("playing"), 15000);
#if !SQUADSPEAK_STORE_BUILD
            QVERIFY(session.setSupporterEnabled(true));
            const auto id = host.addOwnedChannel("Removed with host"); QVERIFY(!id.isEmpty());
            ownedRadio = radio->forChannel(id); QVERIFY(ownedRadio);
            QVERIFY(ownedRadio->saveStation("", "Second stream", station.url()));
            QTRY_COMPARE_WITH_TIMEOUT(ownedRadio->stations().size(), 1, 15000);
            QVERIFY(ownedRadio->play(ownedRadio->stations().first().toMap().value("id").toString()));
            QTRY_COMPARE_WITH_TIMEOUT(ownedRadio->state(), QString("playing"), 15000);
#endif
        }
        QVERIFY(ownedRadio.isNull());
        QVERIFY(radio);
        QTRY_VERIFY_WITH_TIMEOUT(radio->state() == "unavailable" || radio->state() == "stopped", 5000);
        QVERIFY(radio->active());
    }

    void restartResumesPlaybackAndRepeatedShortFailuresBackOff() {
        QTemporaryDir dir; RadioStation station;
        const auto path = dir.filePath("radio.json");
        {
            RadioPlayer original(path);
            QVERIFY(original.saveStation("", "Resume radio", station.url()));
            QTRY_COMPARE(original.stations().size(), 1);
            QVERIFY(original.play(original.stations().first().toMap().value("id").toString()));
        }
        RadioPlayer restored(path);
        QSignalSpy audio(&restored, &RadioPlayer::audioPacket);
        QVERIFY(restored.setAvailable(true));
        QTRY_VERIFY_WITH_TIMEOUT(audio.size() >= 5, 15000);
        station.drop();
        QTRY_COMPARE_WITH_TIMEOUT(restored.state(), QString("reconnecting"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(restored.state(), QString("playing"), 15000);
        const auto count = audio.size();
        QTRY_VERIFY(audio.size() >= count + 5);
        station.drop();
        QTRY_COMPARE_WITH_TIMEOUT(restored.state(), QString("reconnecting"), 15000);
        const auto requests = station.requests;
        QTest::qWait(1400);
        QCOMPARE(station.requests, requests);
        QTRY_VERIFY_WITH_TIMEOUT(station.requests > requests, 5000);
        QVERIFY(restored.stop());
    }

    void receiverLocalNormalizationCoversVoiceAndMusic() {
        QTemporaryDir dir;
        VoiceSession owner(dir.filePath("owner.session")), quietSession(dir.filePath("quiet.session")),
            loudSession(dir.filePath("loud.session")), firstSession(dir.filePath("first.session")),
            secondSession(dir.filePath("second.session"));
        LocalChannel host(owner, dir.filePath("host.channel"), TlsIdentity::create());
        LocalChannel quietSource(quietSession, dir.filePath("quiet.channel"), TlsIdentity::create());
        LocalChannel loudSource(loudSession, dir.filePath("loud.channel"), TlsIdentity::create());
        LocalChannel first(firstSession, dir.filePath("first.channel"), TlsIdentity::create());
        LocalChannel second(secondSession, dir.filePath("second.channel"), TlsIdentity::create());
        QVERIFY(quietSession.setMuted(false)); QVERIFY(loudSession.setMuted(false));
        QVERIFY(host.listen(QHostAddress::LocalHost)); QVERIFY(host.startHost());
        for (const auto* id : {&quietSource, &loudSource, &first, &second}) QVERIFY(host.decide(id->ownId(), true));
        QVERIFY(quietSource.join(host.ownId(), "127.0.0.1", host.port()));
        QVERIFY(loudSource.join(host.ownId(), "127.0.0.1", host.port()));
        QVERIFY(first.join(host.ownId(), "127.0.0.1", host.port()));
        QVERIFY(second.join(host.ownId(), "127.0.0.1", host.port()));
        QTRY_VERIFY(quietSource.joined() && loudSource.joined() && first.joined() && second.joined());
        QVERIFY(!host.joined());

        QSignalSpy firstAudio(&first, &LocalChannel::audioReceived);
        QSignalSpy secondAudio(&second, &LocalChannel::audioReceived);
        QVERIFY(host.setMusicState("Local radio", "playing", true));
        const auto musicId = host.musicId();
        squad::VoiceMixer firstMixer, secondMixer;
        firstMixer.setGain(musicId, 1.0);
        secondMixer.setGain(musicId, 1.0);

        auto packetsFor = [](float amplitude) {
            squad::VoiceMixer encoder;
            QList<QByteArray> packets;
            for (int frame = 0; frame < 100; ++frame) {
                std::array<float, 960> samples{};
                for (int i = 0; i < 960; ++i)
                    samples[i] = amplitude * float(std::sin(2 * std::numbers::pi * 440 * (frame * 960 + i) / 48000));
                packets.append(encoder.encode(samples, 48000).first());
            }
            return packets;
        };
        const auto quiet = packetsFor(0.03f), loud = packetsFor(0.30f), music = packetsFor(0.30f), zero = packetsFor(0.0f);
        // Match capture cadence while servicing network callbacks continuously.
        qsizetype expected = 0;
        auto deliver = [&](const QList<QByteArray>& packets, auto send) {
            expected += packets.size();
            qsizetype index = 0;
            bool sent = true;
            QTimer capture; capture.setTimerType(Qt::PreciseTimer); capture.setInterval(20);
            connect(&capture, &QTimer::timeout, &capture, [&] {
                sent = send(packets[index++]);
                if (!sent || index == packets.size()) capture.stop();
            });
            capture.start();
            const bool received = waitForEvents([&] {
                return !sent || (index == packets.size() && firstAudio.size() >= expected && secondAudio.size() >= expected);
            }, 12000);
            return sent && received;
        };
        QVERIFY(deliver(quiet, [&](const QByteArray& packet) { return quietSource.sendAudio(packet); }));
        QVERIFY(deliver(loud, [&](const QByteArray& packet) { return loudSource.sendAudio(packet); }));
        QVERIFY(deliver(music, [&](const QByteArray& packet) { return host.sendMusic(packet); }));
        QVERIFY(deliver(zero, [&](const QByteArray& packet) { return host.sendMusic(packet); }));

        auto packetsFrom = [](const QSignalSpy& spy, const QString& peer) {
            QList<QByteArray> result;
            for (const auto& row : spy) if (row.at(0).toString() == peer) result.append(row.at(1).toByteArray());
            return result;
        };
        auto render = [](const QList<QByteArray>& packets, squad::VoiceMixer& mixer, qint64& clock, const QString& peer) {
            double energy = 0;
            for (qsizetype index = 0; index < packets.size(); ++index) {
                const auto& packet = packets.at(index);
                if (!mixer.receive(peer, packet, clock)) return -1.0;
                const auto pcm = mixer.render(48000, clock); clock += 20;
                if (index >= packets.size() - 40) for (const auto sample : pcm) energy += sample * sample;
            }
            return energy;
        };
        const auto quietId = quietSource.ownId();
        const auto loudId = loudSource.ownId();
        const auto firstQuiet = packetsFrom(firstAudio, quietId);
        const auto secondQuiet = packetsFrom(secondAudio, quietId);
        const auto firstLoud = packetsFrom(firstAudio, loudId);
        const auto secondLoud = packetsFrom(secondAudio, loudId);
        const auto firstMusic = packetsFrom(firstAudio, musicId);
        const auto secondMusic = packetsFrom(secondAudio, musicId);
        QCOMPARE(firstQuiet.size(), quiet.size()); QCOMPARE(secondQuiet.size(), quiet.size());
        QCOMPARE(firstLoud.size(), loud.size()); QCOMPARE(secondLoud.size(), loud.size());
        QCOMPARE(firstMusic.size(), music.size() + zero.size());
        QCOMPARE(secondMusic.size(), music.size() + zero.size());
        QCOMPARE(firstQuiet, quiet);
        QCOMPARE(firstLoud, loud);
        QCOMPARE(firstMusic.mid(0, music.size()), music);
        qint64 firstClock = 0, secondClock = 0;
        const auto quietFirst = render(firstQuiet, firstMixer, firstClock, quietId);
        const auto quietSecond = render(secondQuiet, secondMixer, secondClock, quietId);
        const auto loudFirst = render(firstLoud, firstMixer, firstClock, loudId);
        const auto loudSecond = render(secondLoud, secondMixer, secondClock, loudId);
        const auto musicFirst = render(firstMusic.mid(0, music.size()), firstMixer, firstClock, musicId);
        const auto musicSecond = render(secondMusic.mid(0, music.size()), secondMixer, secondClock, musicId);
        QVERIFY(quietFirst > 0); QVERIFY(quietSecond > 0); QVERIFY(loudFirst > 0); QVERIFY(loudSecond > 0);
        QVERIFY(musicFirst > 0); QVERIFY(musicSecond > 0);
        QVERIFY(std::abs(std::log((loudFirst + 1e-9) / (quietFirst + 1e-9))) < 0.15);
        QVERIFY(std::abs(std::log((musicFirst + 1e-9) / (loudFirst + 1e-9))) < 0.15);
        QVERIFY(std::abs(std::log((musicSecond + 1e-9) / (loudSecond + 1e-9))) < 0.15);
        for (qsizetype i = 0; i < firstQuiet.size(); ++i) QCOMPARE(firstQuiet.at(i), secondQuiet.at(i));
        for (qsizetype i = 0; i < firstLoud.size(); ++i) QCOMPARE(firstLoud.at(i), secondLoud.at(i));
        QCOMPARE(firstMusic, secondMusic);

        squad::VoiceMixer adjustedFirst, unchangedSecond;
        adjustedFirst.setGain(musicId, 0.25); unchangedSecond.setGain(musicId, 1.0);
        qint64 adjustedClock = 0, unchangedClock = 0;
        const auto adjustedQuiet = render(firstQuiet, adjustedFirst, adjustedClock, quietId);
        const auto unchangedQuiet = render(secondQuiet, unchangedSecond, unchangedClock, quietId);
        const auto adjustedLoud = render(firstLoud, adjustedFirst, adjustedClock, loudId);
        const auto unchangedLoud = render(secondLoud, unchangedSecond, unchangedClock, loudId);
        const auto adjustedMusic = render(firstMusic.mid(0, music.size()), adjustedFirst, adjustedClock, musicId);
        const auto unchangedMusic = render(secondMusic.mid(0, music.size()), unchangedSecond, unchangedClock, musicId);
        const auto adjustedZero = render(firstMusic.mid(music.size()), adjustedFirst, adjustedClock, musicId);
        const auto unchangedZero = render(secondMusic.mid(music.size()), unchangedSecond, unchangedClock, musicId);
        QVERIFY(std::abs(std::log((adjustedQuiet + 1e-9) / (quietFirst + 1e-9))) < 0.1);
        QVERIFY(std::abs(std::log((adjustedLoud + 1e-9) / (loudFirst + 1e-9))) < 0.1);
        QVERIFY(std::abs(std::log((unchangedQuiet + 1e-9) / (quietSecond + 1e-9))) < 0.1);
        QVERIFY(std::abs(std::log((unchangedLoud + 1e-9) / (loudSecond + 1e-9))) < 0.1);
        QVERIFY(std::abs(adjustedMusic / musicFirst - 0.0625) < 0.0001);
        QVERIFY(std::abs(std::log((unchangedMusic + 1e-9) / (musicSecond + 1e-9))) < 0.1);
        // Codec silence may retain negligible numerical residue. Local mute
        // below must still produce exact zero for a nonzero source.
        QVERIFY(adjustedZero >= 0 && adjustedZero < adjustedMusic * 1e-6);
        QVERIFY(unchangedZero >= 0 && unchangedZero < unchangedMusic * 1e-6);
        adjustedFirst.setGain(musicId, 0);
        QCOMPARE(render(firstMusic.mid(0, music.size()), adjustedFirst, adjustedClock, musicId), 0.0);
        QVERIFY(render(secondMusic.mid(0, music.size()), unchangedSecond, unchangedClock, musicId) > 0);

    }
};
QTEST_GUILESS_MAIN(RadioTests)
#include "radio_tests.moc"
