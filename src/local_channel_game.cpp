#include "local_channel.hpp"
#include <QJsonDocument>
#include <QSslSocket>
#include <QUuid>
#include <algorithm>


QVariantList LocalChannel::games() const {
    const auto* c = chatClient();
    return c ? c->games.toVariantList() : QVariantList{};
}

QString LocalChannel::gameId() const {
    const auto* c = chatClient();
    return c ? c->gameId : QString{};
}

QVariantMap LocalChannel::gameView(const QString& host, const QString& id, const QString& identity) const {
    const auto viewer = identity.isEmpty() ? remoteMode() ? remoteView_.value("ownId").toString() : ownId() : identity;
    if (viewer != ownId()) {
        if (viewer.isEmpty() || viewer != remoteView_.value("ownId").toString()) return {};
        const auto game = remoteView_.value("currentGame").toMap();
        if (game.value("hostId") == host && game.value("id") == id) return game;
        if (remoteView_.value("chatHostId") != host) return {};
        for (const auto& value : remoteView_.value("games").toList()) {
            auto state = value.toMap();
            if (state.value("id") != id) continue;
            state.insert("hostId", host); state.insert("connected", remoteView_.value("chatReady"));
            state.insert("error", remoteView_.value("chatError")); return state;
        }
        return {};
    }
    const auto client = clients_.value(host);
    if (!client) return {};
    for (const auto& value : client->games) if (value.toObject().value("id") == id) {
        auto state = value.toObject().toVariantMap();
        state.insert("hostId", host); state.insert("connected", client->accepted);
        state.insert("error", client->chatError); return state;
    }
    return {};
}

QVariantMap LocalChannel::currentGame() const {
    QVariantMap latest;
    for (const auto& client : clients_) for (const auto& value : client->games) {
        const auto state = value.toObject();
        const auto players = state.value("players").toArray();
        if (!std::any_of(players.cbegin(), players.cend(), [&](const QJsonValue& p) { return p.toObject().value("id") == ownId(); })) continue;
        const auto phase = state.value("phase").toString();
        auto view = state.toVariantMap();
        view.insert("hostId", client->id); view.insert("connected", client->accepted); view.insert("error", client->chatError);
        if (phase != "finished" && phase != "cancelled") return view;
        if (latest.isEmpty() || state.value("created").toInteger() > latest.value("created").toLongLong()) latest = view;
    }
    return latest;
}

bool LocalChannel::canStartGame() const {
    const auto* c = chatClient();
    if (!c || !chatReady() || c->external() || !c->privatePeer.isEmpty()
        || !ownChannel(c->id) || !c->capabilities.contains("channel-games")) return false;
    const auto active = currentGame();
    if (!pendingGameRequest_.isEmpty() || (!active.isEmpty() && active.value("phase") != "finished" && active.value("phase") != "cancelled")) return false;
    const auto phase = c->gameId.isEmpty() || c->games.isEmpty() ? QString{} : c->games.last().toObject().value("phase").toString();
    return phase != "waiting" && phase != "preparing" && phase != "playing" && phase != "duel";
}

bool LocalChannel::gameAction(const QString& action, const QVariantMap& selection, const QString& host, const QString& identity) {
    const auto validActions = {QStringLiteral("start"), QStringLiteral("join"), QStringLiteral("ready"),
        QStringLiteral("choose"), QStringLiteral("leave"), QStringLiteral("setup"), QStringLiteral("move")};
    if (std::find(validActions.begin(), validActions.end(), action) == validActions.end()
        || QJsonDocument(QJsonObject::fromVariantMap(selection)).toJson(QJsonDocument::Compact).size() > 256) return false;
    const auto actor = identity.isEmpty() ? remoteMode() ? remoteView_.value("ownId").toString() : ownId() : identity;
    if (actor != ownId()) {
        if (actor.isEmpty() || actor != remoteView_.value("ownId").toString()) return false;
        return remoteAction("game", {{"action", action}, {"selection", selection}, {"hostId", host.isEmpty() ? remoteView_.value("chatHostId") : QVariant(host)}});
    }
    auto* c = host.isEmpty() ? chatClient() : clients_.value(host).get();
    if (!c || !c->accepted || c->external() || !c->privatePeer.isEmpty() || !c->capabilities.contains("channel-games")) return false;
    auto command = QJsonObject::fromVariantMap(selection);
    command.insert("kind", "game"); command.insert("action", action);
    if (action == "start" || action == "join") {
        const auto active = currentGame();
        if (!pendingGameRequest_.isEmpty() || (!active.isEmpty() && active.value("phase") != "finished" && active.value("phase") != "cancelled")) return false;
        pendingGameHost_ = c->id; pendingGameRequest_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        command.insert("request", pendingGameRequest_);
    }
    if (sendChatCommand(*c, command)) return true;
    if (pendingGameHost_ == c->id) { pendingGameHost_.clear(); pendingGameRequest_.clear(); }
    return false;
}

bool LocalChannel::hostGameAction(QSslSocket* socket, const QJsonObject& command) {
    auto it = peers_.find(socket);
    if (it == peers_.end() || !it->joined || it->controller || !it->capabilities.contains("channel-games")) return false;
    const auto now = controlClock_.elapsed();
    if (now - it->gameWindow >= 10000) { it->gameWindow = now; it->gameRequests = 0; }
    // The quota belongs to this authenticated connection and disappears with it.
    if (++it->gameRequests > 60) { socket->abort(); return true; }
    const auto id = command.value("id").toString();
    const auto round = command.value("round").toInt(-1);
    const auto action = command.value("action").toString();
    const auto request = command.value("request").toString();
    bool accepted = false, changed = false;
    if (QJsonDocument(command).toJson(QJsonDocument::Compact).size() <= 512
        && command.value("id").isString() && (id.isEmpty() || !QUuid(id).isNull())
        && round >= 0 && command.value("round").toDouble(-1) == round
        && action.size() <= 16 && (request.isEmpty() || !QUuid(request).isNull())) {
        const auto* root = service_ ? service_ : this;
        bool elsewhere = root != this && root->hostGame_.active() && root->hostGame_.contains(it->id);
        for (const auto& channel : root->owned_)
            elsewhere |= channel.get() != this && channel->hostGame_.active() && channel->hostGame_.contains(it->id);
        if ((action == "start" || action == "join") && elsewhere) {
            accepted = false;
        } else if (action == "start") {
            const auto previous = hostGame_.view(it->id, now);
            accepted = it->id == ownId() && id == previous.value("id").toString()
                && !hostGame_.active() && hostGame_.start(it->id, it->name, now, clock_());
            changed = accepted;
        } else {
            auto selection = command;
            for (const auto key : {"id", "round", "kind", "action", "request"}) selection.remove(key);
            const auto result = hostGame_.act(id, round, it->id, it->name, action, selection, now);
            accepted = result.accepted; changed = result.changed;
        }
    }
    if (changed) publishGame(nullptr, socket, request);
    else writeChat(socket, {{"kind", "game"}, {"now", std::max(history_->time(), clock_())},
        {"state", hostGame_.view(it->id, now)}, {"request", request},
        {"error", accepted ? QString{} : tr("That game action is no longer available.")}});
    return true;
}

void LocalChannel::publishGame(QSslSocket* recipient, QSslSocket* actor, const QString& receipt) {
    if (!history_) return;
    const auto now = controlClock_.elapsed();
    for (auto* socket : peers_.keys()) {
        const auto& peer = peers_[socket];
        if ((recipient && socket != recipient) || !peer.joined || peer.controller || !peer.capabilities.contains("channel-games")) continue;
        writeChat(socket, {{"kind", "game"}, {"now", std::max(history_->time(), clock_())},
            {"state", hostGame_.view(peer.id, now)}, {"request", socket == actor ? receipt : QString{}}});
    }
    gameTimer_.stop();
    if (const auto deadline = hostGame_.deadline())
        gameTimer_.start(int(std::clamp<qint64>(*deadline - now, 1, 10000)));
    emit hostsChanged();
    if (service_) emit service_->hostsChanged();
}

bool LocalChannel::applyGame(Client& c, const QJsonObject& state) {
    if (state.isEmpty()) {
        c.gameId.clear();
        // A host restart has no live game. Keep this session's old card terminal.
        if (!c.games.isEmpty()) {
            auto previous = c.games.last().toObject();
            const auto phase = previous.value("phase").toString();
            if (phase == "waiting" || phase == "preparing" || (phase == "playing" || phase == "duel")) {
                previous.insert("phase", "cancelled"); previous.insert("reason", "disconnected");
                previous.insert("choice", ""); previous.insert("readyMs", 0); previous.insert("turn", ""); previous.insert("duel", QJsonObject{});
                auto players = previous.value("players").toArray();
                for (qsizetype n = 0; n < players.size(); ++n) { auto player = players[n].toObject(); player.insert("chosen", false); players[n] = player; }
                previous.insert("players", players); c.games[c.games.size() - 1] = previous;
            }
        }
        return true;
    }
    if (!validGameState(state)) return false;
    c.gameId = state.value("id").toString();
    for (qsizetype i = 0; i < c.games.size(); ++i) if (c.games[i].toObject().value("id") == state.value("id")) {
        c.games[i] = state; return true;
    }
    c.games.append(state);
    while (c.games.size() > maximumWindowMessages) c.games.removeFirst();
    return true;
}

bool LocalChannel::validGameState(const QJsonObject& state) {
    return ChannelGame::validView(state);
}
