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
        id = game.view(owner).value("id").toString();
        return act(1, "join").accepted && act(0, "setup", {{"core", core0}, {"trap", trap0}}).accepted
            && act(1, "setup", {{"core", core1}, {"trap", trap1}}).accepted;
    }
    QJsonObject view(const QString& viewer = owner) const { return game.view(viewer); }
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
    void computerSeatIsCreatorOnlyAndAtomicWithHumanJoin_data() {
        QTest::addColumn<bool>("humanFirst");
        QTest::newRow("computer-first") << false;
        QTest::newRow("human-first") << true;
    }
    void computerSeatIsCreatorOnlyAndAtomicWithHumanJoin() {
        QFETCH(bool, humanFirst);
        Match match(3);
        QVERIFY(match.game.start(owner, "Owner", match.now++, 123456));
        match.id = match.view().value("id").toString();
        const auto waiting = match.view();
        QVERIFY(!match.game.act(match.id, 1, observer, "Observer", "computer", {}, match.now++).accepted);
        QVERIFY(!match.act(0, "computer", {}, 0).accepted);
        QVERIFY(!match.act(0, "computer", {{"extra", true}}).accepted);
        QCOMPARE(match.view(), waiting);
        if (humanFirst) {
            QVERIFY(match.act(1, "join").changed);
            QVERIFY(!match.act(0, "computer").accepted);
            QVERIFY(!match.game.computerPending());
        } else {
            QVERIFY(match.act(0, "computer").changed);
            QCOMPARE(match.view().value("phase").toString(), QString("preparing"));
            const auto players = match.view().value("players").toArray();
            QCOMPARE(players.size(), 2);
            QCOMPARE(players[1].toObject().value("id").toString(), QString("computer"));
            QVERIFY(players[1].toObject().value("computer").toBool());
            const auto full = match.view();
            QVERIFY(!match.act(0, "computer").accepted);
            QVERIFY(!match.act(1, "join").accepted);
            QCOMPARE(match.view(), full);
            QVERIFY(match.game.computerPending());
            QVERIFY(match.game.advanceComputer(match.now++).changed);
            QVERIFY(!match.game.computerPending());
            QVERIFY(match.view("computer").value("setup").toBool());
            for (const auto& viewer : {owner, observer, QString("computer")}) QVERIFY(ChannelGame::validView(match.view(viewer)));
            auto malformed = match.view(); auto people = malformed.value("players").toArray();
            auto computer = people[1].toObject(); computer.insert("computer", false); people[1] = computer;
            malformed.insert("players", people); QVERIFY(!ChannelGame::validView(malformed));
            for (const auto& marker : {QJsonValue("true"), QJsonValue(1), QJsonValue(QJsonValue::Null)}) {
                malformed = match.view(); people = malformed.value("players").toArray();
                computer = people[1].toObject(); computer.insert("computer", marker); people[1] = computer;
                malformed.insert("players", people); QVERIFY(!ChannelGame::validView(malformed));
            }
            malformed = match.view(); people = malformed.value("players").toArray();
            auto human = people[0].toObject(); human.insert("computer", true); people[0] = human;
            malformed.insert("players", people); QVERIFY(!ChannelGame::validView(malformed));
        }
    }

    void computerDecisionsIgnoreOpponentHiddenRolesAndPendingChoice() {
        Match first(7), second(7);
        for (auto* match : {&first, &second}) {
            QVERIFY(match->game.start(owner, "Owner", match->now++, 123456));
            match->id = match->view().value("id").toString();
            QVERIFY(match->act(0, "computer").accepted);
        }
        QVERIFY(first.act(0, "setup", {{"core", 35}, {"trap", 36}}).accepted);
        QVERIFY(second.act(0, "setup", {{"core", 40}, {"trap", 41}}).accepted);
        for (auto* match : {&first, &second}) QVERIFY(match->game.advanceComputer(match->now++).changed);
        QCOMPARE(first.view("computer").value("board"), second.view("computer").value("board"));
        QCOMPARE(first.view().value("turn"), second.view().value("turn"));
        const auto board = first.view("computer").value("board").toArray();
        int column = 0;
        while (board[7 + column].toObject().value("kind") != "neutral") ++column;
        const auto computerMove = [&](Match& match, int from, int to) {
            return match.game.act(match.id, match.view().value("round").toInt(), "computer", "Computer", "move",
                {{"from", from}, {"to", to}}, match.now++).accepted;
        };
        const bool humanStarts = first.view().value("turn") == owner;
        for (auto* match : {&first, &second}) {
            if (humanStarts) {
                QVERIFY(match->move(0, 28 + column, 21 + column));
                QVERIFY(computerMove(*match, 7 + column, 14 + column));
                QVERIFY(match->move(0, 21 + column, 14 + column));
            } else {
                QVERIFY(computerMove(*match, 7 + column, 14 + column));
                QVERIFY(match->move(0, 28 + column, 21 + column));
                QVERIFY(computerMove(*match, 14 + column, 21 + column));
            }
            QCOMPARE(match->view().value("phase").toString(), QString("duel"));
        }
        QVERIFY(first.act(0, "choose", {{"choice", "fire"}}).accepted);
        QVERIFY(second.act(0, "choose", {{"choice", "water"}}).accepted);
        QVERIFY(first.view("computer").value("choice").toString().isEmpty());
        QVERIFY(second.view("computer").value("choice").toString().isEmpty());
        QVERIFY(first.game.advanceComputer(first.now++).changed);
        QVERIFY(second.game.advanceComputer(second.now++).changed);
        const auto computerRole = humanStarts ? "defence" : "attack";
        QCOMPARE(first.view().value("lastBattle").toObject().value(computerRole), second.view().value("lastBattle").toObject().value(computerRole));
        for (auto* match : {&first, &second}) QVERIFY(match->game.end("left", owner));
        for (auto* match : {&first, &second}) {
            const auto terminal = match->view();
            QVERIFY(!match->game.computerPending());
            QVERIFY(!match->game.advanceComputer(match->now++).accepted);
            QCOMPARE(match->view(), terminal);
        }
    }

    void seededComputerMatchCompletesThroughLegalActions() {
        Match match(13);
        QRandomGenerator human(41);
        QVERIFY(match.game.start(owner, "Owner", match.now++, 123456));
        match.id = match.view().value("id").toString();
        QVERIFY(match.act(0, "computer").accepted);
        QVERIFY(match.act(0, "setup", {{"core", 35}, {"trap", 36}}).accepted);
        int actions = 0;
        while (match.game.active() && actions++ < 2000) {
            if (match.game.computerPending()) {
                QVERIFY(match.game.advanceComputer(match.now++).changed);
            } else if (match.view().value("phase") == "duel") {
                const QStringList elements{"fire", "water", "ice"};
                QVERIFY(match.act(0, "choose", {{"choice", elements[int(human.bounded(3))]}}).changed);
            } else {
                QCOMPARE(match.view().value("turn").toString(), owner);
                const auto board = match.view().value("board").toArray();
                QVector<QJsonObject> moves;
                for (int from = 0; from < 42; ++from) {
                    const auto piece = board[from].toObject();
                    if (piece.value("owner") != owner || piece.value("kind") == "core" || piece.value("kind") == "trap") continue;
                    for (int to = 0; to < 42; ++to)
                        if (std::abs(from / 7 - to / 7) + std::abs(from % 7 - to % 7) == 1
                            && board[to].toObject().value("owner") != owner) moves.append(QJsonObject{{"from", from}, {"to", to}});
                }
                if (moves.isEmpty()) QVERIFY(match.act(0, "leave").changed);
                else QVERIFY(match.act(0, "move", moves[int(human.bounded(quint32(moves.size())))]).changed);
            }
            QVERIFY(ChannelGame::validView(match.view(owner)));
            QVERIFY(ChannelGame::validView(match.view("computer")));
        }
        QVERIFY2(actions < 2000, "The seeded public match must complete within its bounded action budget.");
        QCOMPARE(match.view().value("phase").toString(), QString("finished"));
        QVERIFY(match.view().value("winner") == owner || match.view().value("winner") == "computer");
        QVERIFY(!match.game.computerPending());
        QVERIFY(ChannelGame::validView(match.view(observer)));
    }

    void elementalBattles_data() {
        QTest::addColumn<QString>("attack");
        QTest::addColumn<QString>("defence");
        QTest::addColumn<QString>("effect");
        QTest::addColumn<bool>("attackerWins");
        QTest::newRow("fire-water") << QString("fire") << QString("water") << QString("extinguish") << false;
        QTest::newRow("ice-fire") << QString("ice") << QString("fire") << QString("melt") << false;
        QTest::newRow("water-ice") << QString("water") << QString("ice") << QString("freeze") << false;
        QTest::newRow("fire-ice") << QString("fire") << QString("ice") << QString("melt") << true;
        QTest::newRow("fire-fire") << QString("fire") << QString("fire") << QString("tie") << false;
        QTest::newRow("water-fire") << QString("water") << QString("fire") << QString("extinguish") << true;
        QTest::newRow("ice-water") << QString("ice") << QString("water") << QString("freeze") << true;
        QTest::newRow("water-water") << QString("water") << QString("water") << QString("tie") << false;
        QTest::newRow("ice-ice") << QString("ice") << QString("ice") << QString("tie") << false;
    }
    void elementalBattles() {
        QFETCH(QString, attack); QFETCH(QString, defence);
        QFETCH(QString, effect); QFETCH(bool, attackerWins);
        Match match(3); QVERIFY(match.prepare());
        const int column = 0;
        const auto first = match.view().value("turn") == owner ? 0 : 1;
        const auto a = first == 0 ? 28 + column : 7 + column;
        const auto b = first == 0 ? 7 + column : 28 + column;
        const auto from = first == 0 ? 21 + column : 14 + column;
        const auto to = first == 0 ? 14 + column : 21 + column;
        QCOMPARE(match.board()[a].toObject().value("kind").toString(), QString("neutral"));
        QCOMPARE(match.board()[b].toObject().value("kind").toString(), QString("neutral"));
        QVERIFY(match.move(first, a, from));
        QVERIFY(!match.view(observer).value("board").toArray()[from].toObject().value("revealed").toBool());
        QCOMPARE(match.view(observer).value("board").toArray()[from].toObject().value("kind").toString(), QString("hidden"));
        QVERIFY(!match.move(first, from, to));
        QVERIFY(match.move(1 - first, b, to));
        QVERIFY(match.move(first, from, to));
        QCOMPARE(match.view().value("phase").toString(), QString("duel"));
        QCOMPARE(match.view().value("lastBattle").toObject().value("effect").toString(), QString("choose"));
        QVERIFY(ChannelGame::validView(match.view(observer)));
        QVERIFY(match.act(first, "choose", {{"choice", attack}}).accepted);
        QCOMPARE(match.view(observer).value("board").toArray()[from].toObject().value("kind").toString(), QString("neutral"));
        QVERIFY(match.act(1 - first, "choose", {{"choice", defence}}).accepted);
        const auto result = match.view(observer);
        const auto battle = result.value("lastBattle").toObject();
        QCOMPARE(battle.value("attack").toString(), attack);
        QCOMPARE(battle.value("defence").toString(), defence);
        QCOMPARE(battle.value("effect").toString(), effect);
        const auto board = result.value("board").toArray();
        if (effect == "tie") {
            QCOMPARE(result.value("phase").toString(), QString("duel"));
            QCOMPARE(result.value("round").toInt(), 4);
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
        Match match(3); QVERIFY(match.prepare());
        const int first = match.view().value("turn") == owner ? 0 : 1;
        const int from = first == 0 ? 21 : 14, to = first == 0 ? 14 : 21;
        QVERIFY(match.move(first, first == 0 ? 28 : 7, from));
        QVERIFY(match.move(1 - first, first == 0 ? 7 : 28, to));
        QVERIFY(match.move(first, from, to));
        QVERIFY(match.act(0, "choose", {{"choice", "ice"}}).changed);
        QCOMPARE(match.view(owner).value("choice").toString(), QString("ice"));
        QVERIFY(match.view(guest).value("choice").toString().isEmpty());
        QVERIFY(match.view(observer).value("choice").toString().isEmpty());
        QCOMPARE(match.view(observer).value("board").toArray()[to].toObject().value("kind").toString(), QString("neutral"));
        const auto repeated = match.act(0, "choose", {{"choice", "ice"}});
        QVERIFY(repeated.accepted); QVERIFY(!repeated.changed);
        QVERIFY(!match.act(0, "choose", {{"choice", "water"}}).accepted);
        QVERIFY(!match.act(1, "choose", {{"choice", "rock"}}).accepted);
        QVERIFY(ChannelGame::validView(match.view(owner)));
        QVERIFY(ChannelGame::validView(match.view(observer)));
        QVERIFY(match.act(1, "choose", {{"choice", "ice"}}).changed);
        QCOMPARE(match.view().value("round").toInt(), 4);
        QCOMPARE(match.view().value("phase").toString(), QString("duel"));
        for (const auto cell : {from, to}) QCOMPARE(match.view(observer).value("board").toArray()[cell].toObject().value("kind").toString(), QString("ice"));
        QVERIFY(match.view().value("choice").toString().isEmpty());
        QVERIFY(!match.act(0, "choose", {{"choice", "fire"}}, 3).accepted);
        QVERIFY(match.act(first, "choose", {{"choice", "fire"}}).accepted);
        QCOMPARE(match.view(observer).value("board").toArray()[from].toObject().value("kind").toString(), QString("ice"));
        QVERIFY(match.act(1 - first, "choose", {{"choice", "water"}}).accepted);
        QCOMPARE(match.view().value("round").toInt(), 5);
        QCOMPARE(match.view().value("phase").toString(), QString("playing"));
        const auto survivor = match.view(observer).value("board").toArray()[to].toObject();
        QCOMPARE(survivor.value("owner").toString(), first == 0 ? guest : owner);
        QCOMPARE(survivor.value("kind").toString(), QString("water"));
        QVERIFY(survivor.value("revealed").toBool());
        QVERIFY(match.move(1 - first, to, from));
        QCOMPARE(match.view(observer).value("board").toArray()[from].toObject(), survivor);
        QVERIFY(ChannelGame::validView(match.view(observer)));
    }

    void onlyNeutralOpponentChoosesAgainstAnEstablishedElement_data() {
        QTest::addColumn<bool>("establishedAttacks");
        QTest::newRow("neutral-attacks") << false;
        QTest::newRow("established-attacks") << true;
    }
    void onlyNeutralOpponentChoosesAgainstAnEstablishedElement() {
        QFETCH(bool, establishedAttacks);
        Match match(3); QVERIFY(match.prepare());
        const int first = match.view().value("turn") == owner ? 0 : 1;
        const int a = first == 0 ? 28 : 7, b = first == 0 ? 7 : 28;
        const int from = first == 0 ? 21 : 14, to = first == 0 ? 14 : 21;
        QVERIFY(match.move(first, a, from)); QVERIFY(match.move(1 - first, b, to));
        QVERIFY(match.move(first, from, to));
        QVERIFY(match.act(first, "choose", {{"choice", "fire"}}).accepted);
        QVERIFY(match.act(1 - first, "choose", {{"choice", "ice"}}).accepted);
        QVERIFY(match.move(1 - first, b + 1, to + 1));
        const int target = establishedAttacks ? to + 1 : to;
        if (establishedAttacks) QVERIFY(match.move(first, to, target));
        else {
            QVERIFY(match.move(first, a + 1, from + 1));
            QVERIFY(match.move(1 - first, to + 1, target));
        }
        QCOMPARE(match.view().value("phase").toString(), QString("duel"));
        QVERIFY(!match.act(first, "choose", {{"choice", "ice"}}).accepted);
        auto completedChoice = match.view(first == 0 ? guest : owner);
        auto players = completedChoice.value("players").toArray();
        auto neutral = players[1 - first].toObject(); neutral.insert("chosen", true); players[1 - first] = neutral;
        completedChoice.insert("players", players); completedChoice.insert("choice", "water");
        QVERIFY(!ChannelGame::validView(completedChoice));
        auto invalidPhase = match.view(observer); invalidPhase.insert("phase", "playing"); invalidPhase.insert("duel", QJsonObject{});
        QVERIFY(!ChannelGame::validView(invalidPhase));
        auto wrongBattle = match.view(observer); auto battle = wrongBattle.value("lastBattle").toObject();
        battle.insert("defence", "ice"); wrongBattle.insert("lastBattle", battle);
        QVERIFY(!ChannelGame::validView(wrongBattle));
        QVERIFY(ChannelGame::validView(match.view(owner)));
        QVERIFY(ChannelGame::validView(match.view(guest)));
        QVERIFY(match.act(1 - first, "choose", {{"choice", "water"}}).accepted);
        QCOMPARE(match.view().value("phase").toString(), QString("playing"));
        const auto piece = match.view(observer).value("board").toArray()[target].toObject();
        QCOMPARE(piece.value("kind").toString(), QString("water"));
        QCOMPARE(piece.value("owner").toString(), first == 0 ? guest : owner);
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
            QCOMPARE(match.view(guest).value("core").toInt(), -1);
            auto forgedForfeit = result; forgedForfeit.insert("reason", "left");
            QVERIFY(!ChannelGame::validView(forgedForfeit));
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
    }

    void completingSetupLocksHiddenRolesAndReadiesImmediately() {
        Match match(3);
        QVERIFY(match.game.start(owner, "Owner", match.now++, 123456));
        match.id = match.view().value("id").toString();
        QVERIFY(match.act(0, "setup", {{"core", 35}, {"trap", 36}}).changed);
        QCOMPARE(match.view().value("phase").toString(), QString("waiting"));
        QVERIFY(match.view().value("players").toArray()[0].toObject().value("ready").toBool());
        QVERIFY(!match.view().contains("readyMs"));
        const auto before = match.view();
        const auto repeated = match.act(0, "setup", {{"core", 35}, {"trap", 36}});
        QVERIFY(repeated.accepted); QVERIFY(!repeated.changed);
        QCOMPARE(match.view(), before);
        QVERIFY(!match.act(0, "setup", {{"core", 37}, {"trap", 38}}).accepted);
        QVERIFY(!match.act(0, "ready").accepted);
        QMap<QString, int> kinds;
        for (const auto& value : match.view().value("board").toArray()) {
            const auto piece = value.toObject();
            if (!piece.isEmpty()) ++kinds[piece.value("kind").toString()];
        }
        QCOMPARE(kinds, (QMap<QString, int>{{"core", 1}, {"trap", 1}, {"neutral", 12}}));
        QVERIFY(match.act(1, "join").changed);
        QCOMPARE(match.view().value("phase").toString(), QString("preparing"));
        QVERIFY(match.act(1, "setup", {{"core", 0}, {"trap", 1}}).changed);
        QCOMPARE(match.view().value("phase").toString(), QString("playing"));
        QVERIFY(ChannelGame::validView(match.view(owner)));
        QVERIFY(ChannelGame::validView(match.view(guest)));
        QVERIFY(ChannelGame::validView(match.view(observer)));
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
        QCOMPARE(match.view().value("winner").toString(), owner);
        QCOMPARE(match.view().value("phase").toString(), QString("finished"));
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

    void leavingOrDisconnectingAwardsTheOpponent_data() {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<int>("departing");
        QTest::addColumn<QString>("reason");
        for (const auto* stage : {"unprepared", "one-prepared", "playing", "duel", "after-battle"})
            for (int departing = 0; departing < 2; ++departing)
                for (const auto* reason : {"left", "disconnected"})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(stage).arg(departing).arg(reason)))
                        << QString(stage) << departing << QString(reason);
    }
    void leavingOrDisconnectingAwardsTheOpponent() {
        QFETCH(QString, stage); QFETCH(int, departing); QFETCH(QString, reason);
        Match match(3);
        if (stage == "unprepared" || stage == "one-prepared") {
            QVERIFY(match.game.start(owner, "Owner", match.now++, 123456));
            match.id = match.view().value("id").toString();
            QVERIFY(match.act(1, "join").accepted);
            if (stage == "one-prepared") QVERIFY(match.act(0, "setup", {{"core", 35}, {"trap", 36}}).accepted);
        } else {
            QVERIFY(match.prepare());
            if (stage == "duel" || stage == "after-battle") {
                const int first = match.view().value("turn") == owner ? 0 : 1;
                const int from = first == 0 ? 21 : 14, to = first == 0 ? 14 : 21;
                QVERIFY(match.move(first, first == 0 ? 28 : 7, from));
                QVERIFY(match.move(1 - first, first == 0 ? 7 : 28, to));
                QVERIFY(match.move(first, from, to));
                if (stage == "duel") QVERIFY(match.act(0, "choose", {{"choice", "water"}}).accepted);
                else {
                    QVERIFY(match.act(first, "choose", {{"choice", "fire"}}).accepted);
                    QVERIFY(match.act(1 - first, "choose", {{"choice", "ice"}}).accepted);
                }
            }
        }
        const auto before = match.view();
        QVERIFY(!match.game.end(reason, observer));
        QVERIFY(!match.game.end("cancelled", departing == 0 ? owner : guest));
        QCOMPARE(match.view(), before);
        if (reason == "left") QVERIFY(match.act(departing, "leave").changed);
        else QVERIFY(match.game.end(reason, departing == 0 ? owner : guest));
        QCOMPARE(match.view().value("phase").toString(), QString("finished"));
        QCOMPARE(match.view().value("winner").toString(), departing == 0 ? guest : owner);
        QCOMPARE(match.view().value("reason").toString(), reason);
        QVERIFY(!match.game.active());
        QVERIFY(match.view().value("turn").toString().isEmpty());
        QVERIFY(match.view().value("duel").toObject().isEmpty());
        QVERIFY(match.view(owner).value("choice").toString().isEmpty());
        QVERIFY(match.view(guest).value("choice").toString().isEmpty());
        for (const auto& player : match.view().value("players").toArray())
            QVERIFY(!player.toObject().value("chosen").toBool());
        for (const auto& viewer : {owner, guest, observer}) QVERIFY(ChannelGame::validView(match.view(viewer)));
        const auto ended = match.view();
        QVERIFY(!match.game.end(reason, departing == 0 ? owner : guest));
        QVERIFY(!match.act(departing, "leave").accepted);
        QCOMPARE(match.view(), ended);
        for (const auto& replacement : {QString{}, QString("arbitrary")}) {
            auto invalid = ended; invalid.insert("reason", replacement);
            QVERIFY(!ChannelGame::validView(invalid));
        }
        auto invalid = ended; invalid.insert("winner", observer);
        QVERIFY(!ChannelGame::validView(invalid));
        invalid = ended; invalid.insert("phase", "cancelled");
        QVERIFY(!ChannelGame::validView(invalid));
        if (stage == "unprepared" || stage == "one-prepared") {
            invalid = ended; invalid.insert("round", 2);
            QVERIFY(!ChannelGame::validView(invalid));
            auto board = ended.value("board").toArray();
            auto piece = board[28].toObject(); piece.insert("revealed", true); board[28] = piece;
            invalid = ended; invalid.insert("board", board);
            QVERIFY(!ChannelGame::validView(invalid));
        }
        if (stage == "duel") {
            auto battle = ended.value("lastBattle").toObject();
            battle.insert("attack", "ice"); battle.insert("defence", "ice"); battle.insert("effect", "tie");
            invalid = ended; invalid.insert("lastBattle", battle);
            QVERIFY(!ChannelGame::validView(invalid));
        }
    }

    void loneDepartureAndUnknownShutdownDoNotInventAWinner_data() {
        QTest::addColumn<bool>("twoPlayers");
        QTest::addColumn<QString>("departing");
        QTest::newRow("lone-departure") << false << owner;
        QTest::newRow("lone-shutdown") << false << QString{};
        QTest::newRow("two-player-unknown-shutdown") << true << QString{};
    }
    void loneDepartureAndUnknownShutdownDoNotInventAWinner() {
        QFETCH(bool, twoPlayers); QFETCH(QString, departing);
        Match match(3);
        QVERIFY(match.game.start(owner, "Owner", match.now++, 123456));
        match.id = match.view().value("id").toString();
        if (twoPlayers) QVERIFY(match.act(1, "join").accepted);
        QVERIFY(match.game.end("disconnected", departing));
        QCOMPARE(match.view().value("phase").toString(), QString("cancelled"));
        QVERIFY(match.view().value("winner").toString().isEmpty());
        QVERIFY(ChannelGame::validView(match.view(owner)));
        QVERIFY(ChannelGame::validView(match.view(observer)));
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
        Match match(3); QVERIFY(match.prepare());
        const int first = match.view().value("turn") == owner ? 0 : 1;
        const int from = first == 0 ? 21 : 14, to = first == 0 ? 14 : 21;
        QVERIFY(match.move(first, first == 0 ? 28 : 7, from));
        QVERIFY(match.move(1 - first, first == 0 ? 7 : 28, to));
        QVERIFY(match.move(first, from, to));
        QVERIFY(match.act(0, "choose", {{"choice", "water"}}).accepted);
        auto disconnected = match.view();
        disconnected.insert("phase", "cancelled"); disconnected.insert("reason", "disconnected");
        disconnected.insert("choice", ""); disconnected.insert("turn", ""); disconnected.insert("duel", QJsonObject{});
        auto players = disconnected.value("players").toArray();
        for (qsizetype i = 0; i < players.size(); ++i) { auto player = players[i].toObject(); player.insert("chosen", false); players[i] = player; }
        disconnected.insert("players", players);
        QVERIFY(ChannelGame::validView(disconnected));
        QVERIFY(match.game.end("disconnected"));
        QCOMPARE(match.view(), disconnected);
        QVERIFY(ChannelGame::validView(match.view(observer)));
    }

    void setupIsPrivateAndRequiredBeforePlay() {
        Match match(17);
        QVERIFY(match.game.start(owner, "Owner", match.now++, 123456));
        match.id = match.view().value("id").toString();
        QVERIFY(ChannelGame::validView(match.view()));
        QVERIFY(!match.act(0, "ready").accepted);
        QVERIFY(match.act(1, "join").accepted);
        QVERIFY(!match.game.act(match.id, 1, observer, "Observer", "join", {}, match.now++).accepted);
        for (const auto& selection : {QJsonObject{{"core", 35}, {"trap", 35}},
                QJsonObject{{"core", 0}, {"trap", 1}}, QJsonObject{{"core", 35}},
                QJsonObject{{"core", 35.5}, {"trap", 36}}}) {
            const auto before = match.view();
            QVERIFY(!match.act(0, "setup", selection).accepted);
            QCOMPARE(match.view(), before);
        }
        QVERIFY(match.act(0, "setup", {{"core", 35}, {"trap", 36}}).changed);
        const auto ownView = match.view(owner);
        QCOMPARE(ownView.value("core").toInt(), 35);
        QCOMPARE(ownView.value("trap").toInt(), 36);
        QVERIFY(ownView.value("players").toArray()[0].toObject().value("ready").toBool());
        const auto publicView = match.view(observer);
        QCOMPARE(publicView.value("core").toInt(), -1);
        QVERIFY(!publicView.value("setup").toBool());
        const auto board = publicView.value("board").toArray();
        QCOMPARE(board.size(), 42);
        for (int i = 28; i < 42; ++i) QCOMPARE(board[i].toObject().value("kind").toString(), QString("hidden"));
        QVERIFY(!match.move(0, 28, 21));
        QVERIFY(match.act(1, "setup", {{"core", 0}, {"trap", 1}}).changed);
        QCOMPARE(match.view().value("phase").toString(), QString("playing"));
        QVERIFY(ChannelGame::validView(match.view(owner)));
        QVERIFY(ChannelGame::validView(match.view(guest)));
        QVERIFY(ChannelGame::validView(match.view(observer)));
    }
};
QTEST_GUILESS_MAIN(ChannelGameTests)
#include "channel_game_tests.moc"
