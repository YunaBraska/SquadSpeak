#include "../src/channel_game.hpp"
#include <QJsonArray>
#include <QTest>

namespace {
const QString owner(64, 'a');
const QString guest(64, 'b');
const QString observer(64, 'c');

struct Match final {
    ChannelGame game;
    QString id;
    qint64 now = 1000;
    explicit Match(quint32 seed) : game(seed) {}
    bool prepare(int core0 = 35, int trap0 = 36, int core1 = 0, int trap1 = 1) {
        if (!game.start(owner, "Owner", now++, 123456)) return false;
        id = game.view(owner, now).value("id").toString();
        return act(1, "join").accepted && act(0, "setup", {{"core", core0}, {"trap", trap0}}).accepted
            && act(1, "setup", {{"core", core1}, {"trap", trap1}}).accepted
            && act(0, "ready").accepted && act(1, "ready").accepted;
    }
    QJsonObject view(const QString& viewer = owner) const { return game.view(viewer, now); }
    ChannelGame::Result act(int player, const QString& action, const QJsonObject& selection = {}, int round = -1) {
        return game.act(id, round < 0 ? view().value("round").toInt() : round,
            player == 0 ? owner : guest, player == 0 ? "Owner" : "Guest", action, selection, now++);
    }
    bool move(int player, int from, int to) { return act(player, "move", {{"from", from}, {"to", to}}).accepted; }
    QJsonArray board() const {
        auto result = view(owner).value("board").toArray();
        const auto other = view(guest).value("board").toArray();
        for (int i = 0; i < 42; ++i) if (other[i].toObject().value("owner") == guest) result[i] = other[i];
        return result;
    }
};
}

class ChannelGameTests final : public QObject {
    Q_OBJECT
private slots:
    void elementalBattles_data() {
        QTest::addColumn<quint32>("seed");
        QTest::addColumn<int>("column");
        QTest::addColumn<QString>("attack");
        QTest::addColumn<QString>("defence");
        QTest::addColumn<QString>("effect");
        QTest::addColumn<bool>("attackerWins");
        QTest::newRow("fire-water") << quint32(1) << 0 << QString("fire") << QString("water") << QString("extinguish") << false;
        QTest::newRow("ice-fire") << quint32(1) << 1 << QString("ice") << QString("fire") << QString("melt") << false;
        QTest::newRow("water-ice") << quint32(1) << 3 << QString("water") << QString("ice") << QString("freeze") << false;
        QTest::newRow("fire-ice") << quint32(1) << 4 << QString("fire") << QString("ice") << QString("melt") << true;
        QTest::newRow("fire-fire") << quint32(1) << 6 << QString("fire") << QString("fire") << QString("tie") << false;
        QTest::newRow("water-fire") << quint32(2) << 2 << QString("water") << QString("fire") << QString("extinguish") << true;
        QTest::newRow("ice-water") << quint32(2) << 3 << QString("ice") << QString("water") << QString("freeze") << true;
        QTest::newRow("water-water") << quint32(3) << 4 << QString("water") << QString("water") << QString("tie") << false;
        QTest::newRow("ice-ice") << quint32(3) << 6 << QString("ice") << QString("ice") << QString("tie") << false;
    }
    void elementalBattles() {
        QFETCH(quint32, seed); QFETCH(int, column); QFETCH(QString, attack); QFETCH(QString, defence);
        QFETCH(QString, effect); QFETCH(bool, attackerWins);
        Match match(seed); QVERIFY(match.prepare());
        const auto first = match.view().value("turn") == owner ? 0 : 1;
        const auto a = first == 0 ? 28 + column : 7 + column;
        const auto b = first == 0 ? 7 + column : 28 + column;
        const auto from = first == 0 ? 21 + column : 14 + column;
        const auto to = first == 0 ? 14 + column : 21 + column;
        QCOMPARE(match.board()[a].toObject().value("kind").toString(), attack);
        QCOMPARE(match.board()[b].toObject().value("kind").toString(), defence);
        QVERIFY(match.move(first, a, from));
        QVERIFY(!match.view(observer).value("board").toArray()[from].toObject().value("revealed").toBool());
        QCOMPARE(match.view(observer).value("board").toArray()[from].toObject().value("kind").toString(), QString("hidden"));
        QVERIFY(!match.move(first, from, to));
        QVERIFY(match.move(1 - first, b, to));
        QVERIFY(match.move(first, from, to));
        const auto result = match.view(observer);
        const auto battle = result.value("lastBattle").toObject();
        QCOMPARE(battle.value("attack").toString(), attack);
        QCOMPARE(battle.value("defence").toString(), defence);
        QCOMPARE(battle.value("effect").toString(), effect);
        const auto board = result.value("board").toArray();
        if (effect == "tie") {
            QCOMPARE(result.value("phase").toString(), QString("duel"));
            QCOMPARE(result.value("round").toInt(), 3);
            QVERIFY(battle.value("winner").toString().isEmpty());
            QVERIFY(!board[from].toObject().isEmpty());
        } else {
            QCOMPARE(result.value("phase").toString(), QString("playing"));
            QCOMPARE(result.value("round").toInt(), 4);
            QVERIFY(board[from].toObject().isEmpty());
            QCOMPARE(battle.value("winner").toString(), attackerWins ? (first == 0 ? owner : guest) : (first == 0 ? guest : owner));
            QCOMPARE(board[to].toObject().value("kind").toString(), attackerWins ? attack : defence);
            QCOMPARE(result.value("turn").toString(), first == 0 ? guest : owner);
        }
        QVERIFY(ChannelGame::validView(result));
        QVERIFY(ChannelGame::validView(match.view(owner)));
        QVERIFY(ChannelGame::validView(match.view(guest)));
    }

    void tieChoicesRemainSecretAndReplaceBothWeapons() {
        Match match(1); QVERIFY(match.prepare());
        QVERIFY(match.move(1, 13, 20)); QVERIFY(match.move(0, 34, 27)); QVERIFY(match.move(1, 20, 27));
        QVERIFY(match.act(0, "choose", {{"choice", "ice"}}).changed);
        const auto own = match.view(owner);
        QCOMPARE(own.value("choice").toString(), QString("ice"));
        QVERIFY(match.view(guest).value("choice").toString().isEmpty());
        QVERIFY(match.view(observer).value("choice").toString().isEmpty());
        QCOMPARE(match.view(observer).value("board").toArray()[27].toObject().value("kind").toString(), QString("fire"));
        const auto repeated = match.act(0, "choose", {{"choice", "ice"}});
        QVERIFY(repeated.accepted); QVERIFY(!repeated.changed);
        QVERIFY(!match.act(0, "choose", {{"choice", "water"}}).accepted);
        QVERIFY(!match.act(1, "choose", {{"choice", "rock"}}).accepted);
        QVERIFY(match.act(1, "choose", {{"choice", "ice"}}).changed);
        QCOMPARE(match.view().value("round").toInt(), 4);
        QCOMPARE(match.view().value("phase").toString(), QString("duel"));
        for (const auto cell : {20, 27}) QCOMPARE(match.view(observer).value("board").toArray()[cell].toObject().value("kind").toString(), QString("ice"));
        QVERIFY(match.view().value("choice").toString().isEmpty());
        QVERIFY(!match.act(0, "choose", {{"choice", "fire"}}, 3).accepted);
        QVERIFY(match.act(0, "choose", {{"choice", "water"}}).accepted);
        QVERIFY(match.act(1, "choose", {{"choice", "fire"}}).accepted);
        QCOMPARE(match.view().value("round").toInt(), 5);
        QCOMPARE(match.view().value("phase").toString(), QString("playing"));
        const auto survivor = match.view(observer).value("board").toArray()[27].toObject();
        QCOMPARE(survivor.value("owner").toString(), owner);
        QCOMPARE(survivor.value("kind").toString(), QString("water"));
        QVERIFY(survivor.value("revealed").toBool());
        QVERIFY(match.move(0, 27, 20));
        QCOMPARE(match.view(observer).value("board").toArray()[20].toObject(), survivor);
        QVERIFY(ChannelGame::validView(match.view(observer)));
    }

    void coreCaptureAndPersistentTrap_data() {
        QTest::addColumn<bool>("trap");
        QTest::newRow("core") << false;
        QTest::newRow("trap") << true;
    }
    void coreCaptureAndPersistentTrap() {
        QFETCH(bool, trap);
        Match match(3); QVERIFY(match.prepare(35, 36, trap ? 0 : 7, trap ? 7 : 1));
        QCOMPARE(match.view().value("turn").toString(), owner);
        QVERIFY(match.move(0, 28, 21)); QVERIFY(match.move(1, 13, 20));
        QVERIFY(match.move(0, 21, 14)); QVERIFY(match.move(1, 20, 13));
        QVERIFY(match.move(0, 14, 7));
        const auto result = match.view(observer);
        QCOMPARE(result.value("lastBattle").toObject().value("effect").toString(), trap ? QString("trap") : QString("capture"));
        QVERIFY(result.value("board").toArray()[14].toObject().isEmpty());
        if (trap) {
            QCOMPARE(result.value("board").toArray()[7].toObject().value("kind").toString(), QString("trap"));
            QCOMPARE(result.value("board").toArray()[7].toObject().value("owner").toString(), guest);
            QCOMPARE(result.value("phase").toString(), QString("playing"));
            QVERIFY(match.game.active());
        } else {
            QCOMPARE(result.value("winner").toString(), owner);
            QCOMPARE(result.value("phase").toString(), QString("finished"));
            QVERIFY(!match.game.active());
            QVERIFY(!match.move(1, 13, 20));
            QVERIFY(!match.game.advance(match.now + 20000));
            QVERIFY(!match.game.deadline());
            QCOMPARE(match.view(guest).value("core").toInt(), -1);
        }
        QVERIFY(ChannelGame::validView(result));
        QVERIFY(ChannelGame::validView(match.view(guest)));
    }

    void eliminatingEveryMovablePieceWinsWithoutCapturingCore() {
        Match match(3); QVERIFY(match.prepare(35, 28, 0, 1));
        int guestLosses = 0;
        int waitingCell = 34;
        for (int action = 0; action < 400 && match.game.active(); ++action) {
            if (match.view().value("turn") == owner) {
                const auto target = waitingCell == 34 ? 27 : 34;
                QVERIFY(match.move(0, waitingCell, target));
                waitingCell = target;
                continue;
            }
            // Drive each real guest piece into the trap through currently empty cells.
            const auto board = match.board();
            bool moved = false;
            for (int source = 0; source < 42 && !moved; ++source) {
                const auto piece = board[source].toObject();
                if (piece.value("owner") != guest || piece.value("kind") == "core" || piece.value("kind") == "trap") continue;
                QVector<int> previous(42, -1), queue{source};
                previous[source] = source;
                for (int head = 0; head < queue.size() && previous[28] < 0; ++head) {
                    const auto from = queue[head];
                    for (int to = 0; to < 42; ++to) {
                        if (previous[to] >= 0 || std::abs(from / 7 - to / 7) + std::abs(from % 7 - to % 7) != 1
                            || (to != 28 && !board[to].toObject().isEmpty())) continue;
                        previous[to] = from; queue.append(to);
                    }
                }
                if (previous[28] < 0) continue;
                int next = 28;
                while (previous[next] != source) next = previous[next];
                QVERIFY(match.move(1, source, next));
                if (next == 28) ++guestLosses;
                moved = true;
            }
            QVERIFY2(moved, "Every remaining guest weapon must have a route to the exposed trap.");
            QVERIFY(ChannelGame::validView(match.view(owner)));
            QVERIFY(ChannelGame::validView(match.view(guest)));
        }
        QCOMPARE(guestLosses, 12);
        QCOMPARE(match.view().value("phase").toString(), QString("finished"));
        QCOMPARE(match.view().value("winner").toString(), owner);
        const auto board = match.board();
        QCOMPARE(board[0].toObject().value("kind").toString(), QString("core"));
        QCOMPARE(board[28].toObject().value("kind").toString(), QString("trap"));
        QCOMPARE(match.view().value("lastBattle").toObject().value("effect").toString(), QString("trap"));
        QVERIFY(!match.game.deadline());
    }

    void setupChangesResetCountdownAndKeepFourOfEachWeapon() {
        ChannelGame game(3); QVERIFY(game.start(owner, "Owner", 1000, 123456));
        const auto id = game.view(owner, 1000).value("id").toString();
        QVERIFY(game.act(id, 1, owner, "Owner", "setup", {{"core", 35}, {"trap", 36}}, 1001).changed);
        const auto repeated = game.act(id, 1, owner, "Owner", "setup", {{"core", 35}, {"trap", 36}}, 2000);
        QVERIFY(repeated.accepted); QVERIFY(!repeated.changed);
        QCOMPARE(game.deadline(), std::optional<qint64>(11001));
        QVERIFY(game.act(id, 1, owner, "Owner", "setup", {{"core", 37}, {"trap", 38}}, 2001).changed);
        QCOMPARE(game.deadline(), std::optional<qint64>(12001));
        QMap<QString, int> kinds;
        for (const auto& value : game.view(owner, 2001).value("board").toArray()) {
            const auto piece = value.toObject();
            if (!piece.isEmpty()) ++kinds[piece.value("kind").toString()];
        }
        QCOMPARE(kinds, (QMap<QString, int>{{"core", 1}, {"trap", 1}, {"fire", 4}, {"water", 4}, {"ice", 4}}));
        QVERIFY(!game.advance(12000));
        QVERIFY(game.act(id, 1, owner, "Owner", "ready", {}, 12000).accepted);
        QVERIFY(!game.act(id, 1, owner, "Owner", "setup", {{"core", 35}, {"trap", 36}}, 12001).accepted);
        QVERIFY(game.act(id, 1, guest, "Guest", "join", {}, 12002).accepted);
        QVERIFY(!game.advance(90000));
        QCOMPARE(game.view(owner, 90000).value("phase").toString(), QString("preparing"));
    }

    void invalidMoves_data() {
        QTest::addColumn<QString>("action"); QTest::addColumn<QJsonObject>("selection");
        QTest::newRow("negative") << QString("move") << QJsonObject{{"from", -1}, {"to", 21}};
        QTest::newRow("outside") << QString("move") << QJsonObject{{"from", 28}, {"to", 42}};
        QTest::newRow("fractional") << QString("move") << QJsonObject{{"from", 28.5}, {"to", 21}};
        QTest::newRow("diagonal") << QString("move") << QJsonObject{{"from", 28}, {"to", 22}};
        QTest::newRow("jump") << QString("move") << QJsonObject{{"from", 28}, {"to", 14}};
        QTest::newRow("row-wrap") << QString("move") << QJsonObject{{"from", 28}, {"to", 27}};
        QTest::newRow("friendly") << QString("move") << QJsonObject{{"from", 28}, {"to", 29}};
        QTest::newRow("enemy") << QString("move") << QJsonObject{{"from", 7}, {"to", 14}};
        QTest::newRow("core") << QString("move") << QJsonObject{{"from", 35}, {"to", 28}};
        QTest::newRow("trap") << QString("move") << QJsonObject{{"from", 36}, {"to", 29}};
        QTest::newRow("extra-field") << QString("move") << QJsonObject{{"from", 28}, {"to", 21}, {"extra", true}};
        QTest::newRow("setup-locked") << QString("setup") << QJsonObject{{"core", 37}, {"trap", 38}};
        QTest::newRow("no-duel") << QString("choose") << QJsonObject{{"choice", "fire"}};
    }
    void invalidMoves() {
        QFETCH(QString, action); QFETCH(QJsonObject, selection);
        Match match(3); QVERIFY(match.prepare());
        const auto initial = match.view();
        QVERIFY(!match.act(0, action, selection).accepted);
        QCOMPARE(match.view(), initial);
    }

    void replayDepartureAndRestartCannotMutateAnotherGame() {
        Match match(3); QVERIFY(match.prepare());
        const auto initial = match.view();
        QVERIFY(!match.game.act(match.id, 1, observer, "Observer", "move", {{"from", 28}, {"to", 21}}, match.now++).accepted);
        QVERIFY(!match.game.act("wrong-id", 1, owner, "Owner", "move", {{"from", 28}, {"to", 21}}, match.now++).accepted);
        QCOMPARE(match.view(), initial);
        QVERIFY(match.move(0, 28, 21));
        const auto moved = match.view();
        QVERIFY(!match.act(0, "move", {{"from", 28}, {"to", 21}}, 1).accepted);
        QVERIFY(!match.act(1, "move", {{"from", 7}, {"to", 14}}, 1).accepted);
        QCOMPARE(match.view(), moved);
        QVERIFY(match.act(1, "leave").changed);
        QVERIFY(!match.game.active());
        QVERIFY(match.view().value("turn").toString().isEmpty());
        QVERIFY(!match.act(1, "leave").accepted);
        QVERIFY(!match.game.end("again"));
        QVERIFY(ChannelGame::validView(match.view()));
        const auto oldId = match.id;
        QVERIFY(match.game.start(owner, "Owner", match.now++, 234567));
        match.id = match.view().value("id").toString();
        QVERIFY(match.id != oldId);
        QCOMPARE(match.view().value("phase").toString(), QString("waiting"));
        QVERIFY(!match.view().value("setup").toBool());
        QVERIFY(match.view().value("lastBattle").toObject().isEmpty());
        QVERIFY(!match.game.act(oldId, 1, guest, "Guest", "join", {}, match.now++).accepted);
        QCOMPARE(match.view().value("players").toArray().size(), 1);
    }

    void snapshotValidationRejectsMalformedKnownFieldsButAllowsExtensions() {
        Match match(3); QVERIFY(match.prepare());
        const auto state = match.view();
        QVERIFY(ChannelGame::validView(state));
        auto extended = state;
        extended.insert("hostId", observer); extended.insert("connected", true); extended.insert("error", "");
        auto players = extended.value("players").toArray(); auto player = players[0].toObject();
        player.insert("future", "optional"); players[0] = player; extended.insert("players", players);
        QVERIFY(ChannelGame::validView(extended));
        for (const auto& key : state.keys()) {
            auto missing = state; missing.remove(key);
            QVERIFY2(!ChannelGame::validView(missing), qPrintable(key));
        }
        for (const auto& replacement : {QJsonValue(-1), QJsonValue(1.5), QJsonValue("1")}) {
            auto invalid = state; invalid.insert("round", replacement);
            QVERIFY(!ChannelGame::validView(invalid));
        }
        auto board = state.value("board").toArray();
        auto invalid = state; auto shortBoard = board; shortBoard.removeLast(); invalid.insert("board", shortBoard);
        QVERIFY(!ChannelGame::validView(invalid));
        auto cell = board[28].toObject(); cell.insert("owner", observer); board[28] = cell; invalid = state; invalid.insert("board", board);
        QVERIFY(!ChannelGame::validView(invalid));
        board = state.value("board").toArray(); cell = board[28].toObject(); cell.insert("kind", "hidden"); cell.insert("revealed", true); board[28] = cell;
        invalid = state; invalid.insert("board", board); QVERIFY(!ChannelGame::validView(invalid));
        board = state.value("board").toArray(); board[21] = board[28]; invalid = state; invalid.insert("board", board);
        QVERIFY(!ChannelGame::validView(invalid));
        players = state.value("players").toArray(); players[1] = players[0]; invalid = state; invalid.insert("players", players);
        QVERIFY(!ChannelGame::validView(invalid));
        invalid = state; invalid.insert("choice", "fire"); QVERIFY(!ChannelGame::validView(invalid));
        invalid = state; invalid.insert("turn", observer); QVERIFY(!ChannelGame::validView(invalid));
        invalid = state; invalid.insert("duel", QJsonObject{{"from", 14}, {"to", 21}}); QVERIFY(!ChannelGame::validView(invalid));
        invalid = state; invalid.insert("future", QString(17000, 'x')); QVERIFY(!ChannelGame::validView(invalid));
        invalid = state; invalid.insert("setup", "true"); QVERIFY(!ChannelGame::validView(invalid));
    }

    void disconnectSnapshotsRemainValidDuringSecretDuel() {
        Match match(1); QVERIFY(match.prepare());
        QVERIFY(match.move(1, 13, 20)); QVERIFY(match.move(0, 34, 27)); QVERIFY(match.move(1, 20, 27));
        QVERIFY(match.act(0, "choose", {{"choice", "water"}}).accepted);
        auto disconnected = match.view();
        disconnected.insert("phase", "cancelled"); disconnected.insert("reason", "disconnected");
        disconnected.insert("choice", ""); disconnected.insert("turn", ""); disconnected.insert("duel", QJsonObject{});
        disconnected.insert("readyMs", 0);
        auto players = disconnected.value("players").toArray();
        for (qsizetype i = 0; i < players.size(); ++i) { auto player = players[i].toObject(); player.insert("chosen", false); players[i] = player; }
        disconnected.insert("players", players);
        QVERIFY(ChannelGame::validView(disconnected));
        QVERIFY(match.game.end("disconnected"));
        QCOMPARE(match.view(), disconnected);
        QVERIFY(ChannelGame::validView(match.view(observer)));
    }

    void setupIsPrivateAndRequiredBeforeReadiness() {
        ChannelGame game(17);
        QVERIFY(game.start(owner, "Owner", 1000, 123456));
        const auto id = game.view(owner, 1000).value("id").toString();
        QVERIFY(ChannelGame::validView(game.view(owner, 1000)));
        QVERIFY(!game.deadline());
        QVERIFY(!game.advance(12000));
        QVERIFY(!game.act(id, 1, owner, "Owner", "ready", {}, 12001).accepted);
        QVERIFY(game.act(id, 1, guest, "Guest", "join", {}, 12002).accepted);
        QVERIFY(!game.act(id, 1, observer, "Observer", "join", {}, 12003).accepted);
        QVERIFY(!game.act(id, 1, owner, "Owner", "setup", {{"core", 35}, {"trap", 35}}, 12004).accepted);
        QVERIFY(!game.act(id, 1, owner, "Owner", "setup", {{"core", 0}, {"trap", 1}}, 12005).accepted);
        QVERIFY(game.act(id, 1, owner, "Owner", "setup", {{"core", 35}, {"trap", 36}}, 12006).changed);
        const auto ownView = game.view(owner, 12006);
        QCOMPARE(ownView.value("core").toInt(), 35);
        QCOMPARE(ownView.value("trap").toInt(), 36);
        QCOMPARE(ownView.value("readyMs").toInteger(), 10000);
        QCOMPARE(game.deadline(), std::optional<qint64>(22006));
        const auto publicView = game.view(observer, 12006);
        QCOMPARE(publicView.value("core").toInt(), -1);
        QVERIFY(!publicView.value("setup").toBool());
        const auto board = publicView.value("board").toArray();
        QCOMPARE(board.size(), 42);
        for (int i = 28; i < 42; ++i) QCOMPARE(board[i].toObject().value("kind").toString(), QString("hidden"));
        QVERIFY(game.advance(22006));
        QCOMPARE(game.view(owner, 22006).value("phase").toString(), QString("preparing"));
        QVERIFY(!game.deadline());
        QVERIFY(!game.act(id, 1, owner, "Owner", "setup", {{"core", 37}, {"trap", 38}}, 22007).accepted);
        QVERIFY(game.act(id, 1, guest, "Guest", "setup", {{"core", 0}, {"trap", 1}}, 22008).changed);
        QVERIFY(game.act(id, 1, guest, "Guest", "ready", {}, 22009).changed);
        QCOMPARE(game.view(owner, 22009).value("phase").toString(), QString("playing"));
        QVERIFY(ChannelGame::validView(game.view(owner, 22009)));
        QVERIFY(ChannelGame::validView(game.view(guest, 22009)));
        QVERIFY(ChannelGame::validView(game.view(observer, 22009)));
    }
};
QTEST_GUILESS_MAIN(ChannelGameTests)
#include "channel_game_tests.moc"
