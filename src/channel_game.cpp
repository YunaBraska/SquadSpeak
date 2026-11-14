#include "channel_game.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <limits>

namespace {
bool playerId(const QString& id) {
    return id.size() == 64 && std::all_of(id.begin(), id.end(), [](QChar c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool playerName(const QJsonValue& value) {
    if (!value.isString() || value.toString() != value.toString().trimmed()) return false;
    const auto points = value.toString().toUcs4();
    return !points.isEmpty() && points.size() <= 64 && std::none_of(points.begin(), points.end(), [](char32_t point) {
        const auto category = QChar::category(point);
        return category == QChar::Other_Control || category == QChar::Other_Format
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph;
    });
}
bool integer(const QJsonValue& value, qint64 low, qint64 high) {
    const auto number = value.toInteger(std::numeric_limits<qint64>::min());
    return value.isDouble() && number >= low && number <= high && value.toDouble() == double(number);
}
bool element(const QString& kind) { return kind == "fire" || kind == "water" || kind == "ice"; }
bool movable(const QString& kind) { return kind == "neutral" || element(kind); }
bool chooses(const QString& own, const QString& other) { return own == "neutral" || (element(own) && own == other); }
bool beats(const QString& attack, const QString& defence) {
    return (attack == "water" && defence == "fire") || (attack == "fire" && defence == "ice")
        || (attack == "ice" && defence == "water");
}
bool adjacent(int from, int to) { return std::abs(from / 7 - to / 7) + std::abs(from % 7 - to % 7) == 1; }
bool hasFields(const QJsonObject& object, const QStringList& expected) {
    return std::all_of(expected.begin(), expected.end(), [&](const QString& key) { return object.contains(key); });
}
bool fields(const QJsonObject& object, QStringList expected) {
    expected.sort();
    return object.keys() == expected;
}
}

bool ChannelGame::active() const {
    return phase_ == "waiting" || phase_ == "preparing" || phase_ == "playing" || phase_ == "duel";
}

bool ChannelGame::contains(const QString& id) const {
    return std::any_of(players_.cbegin(), players_.cend(), [&](const Player& player) { return player.id == id; });
}

bool ChannelGame::start(const QString& owner, const QString& name, qint64 now, qint64 created) {
    if (active() || !playerId(owner) || !playerName(name) || now < 0 || created < 0 || created > 9007199254740991LL) return false;
    random_ = seed_ ? QRandomGenerator(*seed_) : QRandomGenerator::securelySeeded();
    id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    phase_ = "waiting"; reason_.clear(); winner_.clear(); round_ = 1; turn_ = -1;
    duelFrom_ = -1; duelTo_ = -1; lastBattle_ = {}; board_ = {};
    created_ = created;
    players_ = {{owner, name}};
    for (int i = 28; i < 42; ++i) board_[i] = {0, "neutral", false};
    return true;
}

ChannelGame::Result ChannelGame::act(const QString& id, int round, const QString& sender,
        const QString& name, const QString& action, const QJsonObject& selection, qint64 now) {
    if (!active() || id != id_ || now < 0) return {};
    auto player = std::find_if(players_.begin(), players_.end(), [&](const Player& p) { return p.id == sender; });
    if (action == "join") {
        if (!selection.isEmpty()) return {};
        if (player != players_.end()) return {true, false};
        if (players_.size() == 2 || phase_ != "waiting" || !playerId(sender) || !playerName(name)) return {};
        players_.push_back({sender, name});
        for (int i = 0; i < 14; ++i) board_[i] = {1, "neutral", false};
        phase_ = "preparing";
        return {true, true};
    }
    if (player == players_.end()) return {};
    const auto who = int(player - players_.begin());
    if (action == "leave") return selection.isEmpty() ? Result{true, end("left", sender)} : Result{};
    if (action == "setup") {
        if ((phase_ != "waiting" && phase_ != "preparing")
            || !fields(selection, {"core", "trap"}) || !integer(selection.value("core"), 0, 41)
            || !integer(selection.value("trap"), 0, 41)) return {};
        const auto core = selection.value("core").toInt(), trap = selection.value("trap").toInt();
        const auto home = who == 0 ? 28 : 0;
        if (core == trap || core < home || core >= home + 14 || trap < home || trap >= home + 14) return {};
        if (player->core == core && player->trap == trap) return {true, false};
        if (player->core >= 0) return {};
        for (int i = home; i < home + 14; ++i)
            board_[i] = {who, i == core ? QString("core") : i == trap ? QString("trap") : QString("neutral"), false};
        player->core = core; player->trap = trap;
        if (players_.size() == 2 && players_[0].core >= 0 && players_[1].core >= 0) {
            phase_ = "playing"; turn_ = int(random_.bounded(2));
        }
        return {true, true};
    }
    if (round != round_) return {};
    int from = -1, to = -1;
    if (action == "move") {
        if (phase_ != "playing" || turn_ != who || !fields(selection, {"from", "to"})
            || !integer(selection.value("from"), 0, 41) || !integer(selection.value("to"), 0, 41)) return {};
        from = selection.value("from").toInt(); to = selection.value("to").toInt();
        if (!adjacent(from, to) || board_[from].owner != who || !movable(board_[from].kind) || board_[to].owner == who) return {};
        if (board_[to].owner < 0) {
            board_[to] = board_[from]; board_[from] = {};
            turn_ = 1 - turn_;
            if (round_ == std::numeric_limits<int>::max()) end("cancelled");
            else ++round_;
            return {true, true};
        }
        board_[from].revealed = true; board_[to].revealed = true;
    } else if (action == "choose") {
        if (phase_ != "duel" || !fields(selection, {"choice"}) || !selection.value("choice").isString()
            || !element(selection.value("choice").toString())) return {};
        from = duelFrom_; to = duelTo_;
        const bool attackChooses = chooses(board_[from].kind, board_[to].kind);
        const bool defenceChooses = chooses(board_[to].kind, board_[from].kind);
        if (!(board_[from].owner == who ? attackChooses : defenceChooses)) return {};
        const auto choice = selection.value("choice").toString();
        if (!player->choice.isEmpty()) return {player->choice == choice, false};
        player->choice = choice;
        if ((attackChooses && players_[board_[from].owner].choice.isEmpty())
            || (defenceChooses && players_[board_[to].owner].choice.isEmpty())) return {true, true};
        if (attackChooses) board_[from].kind = players_[board_[from].owner].choice;
        if (defenceChooses) board_[to].kind = players_[board_[to].owner].choice;
        for (auto& p : players_) p.choice.clear();
    } else return {};

    const auto attacker = board_[from].owner, defender = board_[to].owner;
    const auto attack = board_[from].kind, defence = board_[to].kind;
    QString effect, battleWinner;
    if (movable(defence) && (attack == "neutral" || defence == "neutral" || attack == defence)) {
        effect = attack == "neutral" || defence == "neutral" ? "choose" : "tie";
        phase_ = "duel"; duelFrom_ = from; duelTo_ = to;
    } else {
        const bool attackerWins = defence == "core" || (defence != "trap" && beats(attack, defence));
        battleWinner = players_[attackerWins ? attacker : defender].id;
        const auto survivor = attackerWins ? attack : defence;
        effect = defence == "core" ? "capture" : defence == "trap" ? "trap"
            : survivor == "water" ? "extinguish" : survivor == "fire" ? "melt" : "freeze";
        if (attackerWins) board_[to] = board_[from];
        board_[from] = {};
        phase_ = "playing"; duelFrom_ = -1; duelTo_ = -1; turn_ = defender;
        const bool attackerHasMoves = std::any_of(board_.begin(), board_.end(), [&](const Piece& p) { return p.owner == attacker && movable(p.kind); });
        const bool defenderHasMoves = std::any_of(board_.begin(), board_.end(), [&](const Piece& p) { return p.owner == defender && movable(p.kind); });
        if (defence == "core" || !attackerHasMoves || !defenderHasMoves) {
            phase_ = "finished"; turn_ = -1;
            winner_ = defence == "core" || !defenderHasMoves ? players_[attacker].id : players_[defender].id;
        }
    }
    lastBattle_ = {{"number", round_}, {"from", from}, {"to", to}, {"attacker", players_[attacker].id},
        {"defender", players_[defender].id}, {"attack", attack}, {"defence", defence}, {"winner", battleWinner}, {"effect", effect}};
    if (action == "choose" || (effect != "tie" && effect != "choose")) {
        if (round_ == std::numeric_limits<int>::max()) end("cancelled");
        else ++round_;
    }
    return {true, true};
}

bool ChannelGame::end(const QString& reason, const QString& departingIdentity) {
    if (!active()) return false;
    if (!departingIdentity.isEmpty() && (!contains(departingIdentity) || (reason != "left" && reason != "disconnected"))) return false;
    const bool forfeit = players_.size() == 2 && !departingIdentity.isEmpty();
    phase_ = forfeit ? "finished" : "cancelled";
    winner_ = forfeit ? players_[players_[0].id == departingIdentity ? 1 : 0].id : QString{};
    reason_ = reason.left(64); turn_ = -1; duelFrom_ = -1; duelTo_ = -1;
    for (auto& player : players_) player.choice.clear();
    return true;
}

QJsonObject ChannelGame::view(const QString& viewer) const {
    if (id_.isEmpty()) return {};
    QJsonArray players, board;
    QString ownChoice;
    int core = -1, trap = -1;
    bool setup = false;
    for (qsizetype i = 0; i < players_.size(); ++i) {
        const auto& player = players_[i];
        players.append(QJsonObject{{"id", player.id}, {"name", player.name}, {"ready", player.core >= 0}, {"chosen", !player.choice.isEmpty()}});
        if (player.id == viewer) {
            ownChoice = player.choice; setup = player.core >= 0;
            if (setup) {
                if (board_[player.core].owner == i && board_[player.core].kind == "core") core = player.core;
                trap = player.trap;
            }
        }
    }
    for (const auto& piece : board_) {
        if (piece.owner < 0) board.append(QJsonObject{});
        else board.append(QJsonObject{{"owner", players_[piece.owner].id},
            {"kind", piece.revealed || players_[piece.owner].id == viewer ? piece.kind : QString("hidden")}, {"revealed", piece.revealed}});
    }
    const QJsonObject duel = phase_ == "duel" ? QJsonObject{{"from", duelFrom_}, {"to", duelTo_}} : QJsonObject{};
    return {{"id", id_}, {"game", "elements"}, {"phase", phase_}, {"created", created_},
        {"owner", players_.front().id}, {"players", players}, {"capacity", 2}, {"round", round_},
        {"choice", ownChoice}, {"winner", winner_}, {"reason", reason_},
        {"board", board}, {"turn", turn_ >= 0 ? players_[turn_].id : QString{}},
        {"core", core}, {"trap", trap}, {"setup", setup}, {"duel", duel}, {"lastBattle", lastBattle_}};
}

bool ChannelGame::validView(const QJsonObject& state) {
    if (QJsonDocument(state).toJson(QJsonDocument::Compact).size() > 16 * 1024
        || !hasFields(state, {"id", "game", "phase", "created", "owner", "players", "capacity", "round", "choice", "winner", "reason", "board", "turn", "core", "trap", "setup", "duel", "lastBattle"})
        || !state.value("id").isString() || QUuid(state.value("id").toString()).isNull() || state.value("game") != "elements"
        || !integer(state.value("created"), 0, 9007199254740991LL) || !integer(state.value("round"), 1, std::numeric_limits<int>::max())
        || !integer(state.value("capacity"), 2, 2)
        || !state.value("setup").isBool() || !integer(state.value("core"), -1, 41) || !integer(state.value("trap"), -1, 41)
        || !state.value("players").isArray() || !state.value("board").isArray()
        || !state.value("duel").isObject() || !state.value("lastBattle").isObject()) return false;
    for (const auto* key : {"phase", "owner", "choice", "winner", "reason", "turn"}) if (!state.value(key).isString()) return false;
    const auto phase = state.value("phase").toString();
    const auto reason = state.value("reason").toString();
    const bool forfeit = phase == "finished" && (reason == "left" || reason == "disconnected");
    const auto players = state.value("players").toArray();
    const bool preparation = phase == "waiting" || phase == "preparing";
    const bool terminal = phase == "finished" || phase == "cancelled";
    if ((!preparation && !terminal && phase != "playing" && phase != "duel") || players.isEmpty() || players.size() > 2
        || (phase == "waiting" && players.size() != 1) || (phase != "waiting" && phase != "cancelled" && players.size() != 2)) return false;
    QStringList ids;
    int chosen = 0, ready = 0;
    for (const auto& value : players) {
        const auto player = value.toObject();
        const auto id = player.value("id").toString();
        if (!value.isObject() || !hasFields(player, {"id", "name", "ready", "chosen"}) || !playerId(id) || ids.contains(id)
            || !playerName(player.value("name")) || !player.value("ready").isBool() || !player.value("chosen").isBool()
            || ((phase == "playing" || phase == "duel" || (phase == "finished" && !forfeit)) && !player.value("ready").toBool())) return false;
        ids.append(id);
        if (player.value("chosen").toBool()) ++chosen;
        if (player.value("ready").toBool()) ++ready;
    }
    if (state.value("owner") != ids.front() || chosen > 1 || (chosen && phase != "duel")
        || (phase == "preparing" && ready == 2)
        || (forfeit && ready < 2 && (state.value("round") != 1 || !state.value("lastBattle").toObject().isEmpty()))) return false;
    const auto winner = state.value("winner").toString(), turn = state.value("turn").toString(), choice = state.value("choice").toString();
    if ((phase == "finished" ? !ids.contains(winner) : !winner.isEmpty())
        || ((phase == "playing" || phase == "duel") ? !ids.contains(turn) : !turn.isEmpty())
        || (phase != "cancelled" && !forfeit && !reason.isEmpty()) || reason.size() > 64
        || (!choice.isEmpty() && (phase != "duel" || !element(choice) || !chosen))) return false;
    const auto board = state.value("board").toArray();
    if (board.size() != 42) return false;
    const bool initial = preparation || (forfeit && ready < 2);
    std::array<int, 2> counts{}, cores{}, traps{}, hidden{}, movers{};
    for (int i = 0; i < 42; ++i) {
        if (!board[i].isObject()) return false;
        const auto piece = board[i].toObject();
        if (piece.isEmpty()) continue;
        const auto who = ids.indexOf(piece.value("owner").toString());
        const auto kind = piece.value("kind").toString();
        if (!hasFields(piece, {"owner", "kind", "revealed"}) || who < 0 || !piece.value("revealed").isBool()
            || (!movable(kind) && kind != "hidden" && kind != "core" && kind != "trap")
            || (kind == "hidden" && piece.value("revealed").toBool()) || ++counts[who] > 14) return false;
        if (kind == "core" && ++cores[who] > 1) return false;
        if (kind == "trap" && ++traps[who] > 1) return false;
        if (kind == "hidden") ++hidden[who];
        if (movable(kind)) ++movers[who];
        const bool home = who == 0 ? i >= 28 : i < 14;
        if (((kind == "core" || kind == "trap") && !home)
            || (initial && (!home || piece.value("revealed").toBool() || element(kind)))) return false;
    }
    for (int i = 0; i < ids.size(); ++i) {
        if (counts[i] < 1 || (initial && counts[i] != 14)) return false;
        const bool prepared = players[i].toObject().value("ready").toBool();
        if (initial && ((!prepared && (cores[i] || traps[i]))
            || (prepared && hidden[i] == 0 && (cores[i] != 1 || traps[i] != 1)))) return false;
    }
    const auto core = state.value("core").toInt(), trap = state.value("trap").toInt();
    if (!state.value("setup").toBool()) {
        if (core != -1 || trap != -1 || !choice.isEmpty()) return false;
    } else {
        if (trap < 0 || core == trap || (core < 0 && (phase != "finished" || forfeit))) return false;
        const auto own = board[trap].toObject().value("owner").toString();
        if (board[trap].toObject().value("kind") != "trap" || !ids.contains(own)
            || (core >= 0 && (board[core].toObject().value("kind") != "core" || board[core].toObject().value("owner") != own))) return false;
        const auto player = players[ids.indexOf(own)].toObject();
        if (!player.value("ready").toBool() || (!choice.isEmpty() && !player.value("chosen").toBool())) return false;
    }
    const auto duel = state.value("duel").toObject();
    if (phase == "duel") {
        if (!hasFields(duel, {"from", "to"}) || !integer(duel.value("from"), 0, 41) || !integer(duel.value("to"), 0, 41)) return false;
        const auto from = duel.value("from").toInt(), to = duel.value("to").toInt();
        const auto a = board[from].toObject(), b = board[to].toObject();
        if (!adjacent(from, to) || a.value("owner") != turn || a.value("owner") == b.value("owner")
            || !movable(a.value("kind").toString()) || !movable(b.value("kind").toString())
            || (!chooses(a.value("kind").toString(), b.value("kind").toString())
                && !chooses(b.value("kind").toString(), a.value("kind").toString()))
            || !a.value("revealed").toBool() || !b.value("revealed").toBool()) return false;
        int eligible = 0;
        for (const auto& value : players) {
            const auto p = value.toObject();
            const bool attacking = p.value("id") == a.value("owner");
            const bool required = chooses((attacking ? a : b).value("kind").toString(), (attacking ? b : a).value("kind").toString());
            if (required) ++eligible;
            else if (p.value("chosen").toBool()) return false;
        }
        if (chosen >= eligible) return false;
    } else if (!duel.isEmpty()) return false;
    const auto battle = state.value("lastBattle").toObject();
    if (battle.isEmpty()) return phase != "duel" && (phase != "finished" || forfeit);
    if (!hasFields(battle, {"number", "from", "to", "attacker", "defender", "attack", "defence", "winner", "effect"})
        || !integer(battle.value("number"), 1, state.value("round").toInt()) || !integer(battle.value("from"), 0, 41)
        || !integer(battle.value("to"), 0, 41) || !adjacent(battle.value("from").toInt(), battle.value("to").toInt())) return false;
    for (const auto* key : {"attacker", "defender", "attack", "defence", "winner", "effect"}) if (!battle.value(key).isString()) return false;
    const auto attacker = battle.value("attacker").toString(), defender = battle.value("defender").toString();
    const auto attack = battle.value("attack").toString(), defence = battle.value("defence").toString();
    if (!ids.contains(attacker) || !ids.contains(defender) || attacker == defender || !movable(attack)
        || (!movable(defence) && defence != "core" && defence != "trap") || (forfeit && defence == "core")) return false;
    const bool attackWins = defence == "core" || (defence != "trap" && beats(attack, defence));
    const bool pending = movable(defence) && (attack == "neutral" || defence == "neutral" || attack == defence);
    const auto expectedWinner = pending ? QString{} : attackWins ? attacker : defender;
    const auto surviving = attackWins ? attack : defence;
    const auto effect = pending ? (attack == "neutral" || defence == "neutral" ? QString("choose") : QString("tie"))
        : defence == "core" ? QString("capture") : defence == "trap" ? QString("trap")
        : surviving == "water" ? QString("extinguish") : surviving == "fire" ? QString("melt") : QString("freeze");
    if (phase == "finished" && !forfeit && defence != "core") {
        const auto loser = winner == ids[0] ? 1 : 0;
        if (movers[loser] > 0 || hidden[loser] > 2 - cores[loser] - traps[loser]) return false;
    }
    return battle.value("winner") == expectedWinner && battle.value("effect") == effect
        && (!pending || ((phase == "duel" || phase == "cancelled" || forfeit)
            && attack == board[battle.value("from").toInt()].toObject().value("kind")
            && defence == board[battle.value("to").toInt()].toObject().value("kind")
            && attacker == board[battle.value("from").toInt()].toObject().value("owner")
            && defender == board[battle.value("to").toInt()].toObject().value("owner")
            && board[battle.value("from").toInt()].toObject().value("revealed").toBool()
            && board[battle.value("to").toInt()].toObject().value("revealed").toBool()))
        && (phase != "finished" || forfeit || winner == expectedWinner)
        && (phase != "duel" || (pending && battle.value("from") == duel.value("from") && battle.value("to") == duel.value("to")));
}
