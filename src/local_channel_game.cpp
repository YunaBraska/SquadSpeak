#include "local_channel.hpp"
#include <QJsonDocument>
#include <QSslSocket>
#include <QUuid>
#include <algorithm>


QVariantList LocalChannel::games() const {
    const auto* c = chatClient();
    auto result = c ? c->games.toVariantList() : QVariantList{};
    if (c && detachedGame_.value("hostId") == c->id) result.append(detachedGame_);
    return result;
}

QString LocalChannel::gameId() const {
    const auto* c = chatClient();
    if (!c) return {};
    if (!c->gameId.isEmpty()) return c->gameId;
    return detachedGame_.value("hostId") == c->id ? detachedGame_.value("id").toString() : QString{};
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
            state.insert("hostId", host); state.insert("connected", remoteView_.value("joined").toBool() && remoteView_.value("joinedHostId") == host);
            state.insert("error", remoteView_.value("chatError")); return state;
        }
        return {};
    }
    if (detachedGame_.value("hostId") == host && detachedGame_.value("id") == id) return detachedGame_;
    const auto client = clients_.value(host);
    if (!client) return {};
    for (const auto& value : client->games) if (value.toObject().value("id") == id) {
        auto state = value.toObject().toVariantMap();
        state.insert("hostId", host); state.insert("connected", client->accepted && client->voice);
        state.insert("error", client->chatError); return state;
    }
    return {};
}

QVariantMap LocalChannel::currentGame() const {
    QVariantMap latest = detachedGame_;
    for (const auto& client : clients_) for (const auto& value : client->games) {
        const auto state = value.toObject();
        const auto players = state.value("players").toArray();
        if (!std::any_of(players.cbegin(), players.cend(), [&](const QJsonValue& p) { return p.toObject().value("id") == ownId(); })) continue;
        const auto phase = state.value("phase").toString();
        auto view = state.toVariantMap();
        view.insert("hostId", client->id); view.insert("connected", client->accepted && client->voice); view.insert("error", client->chatError);
        if (phase != "finished" && phase != "cancelled") return view;
        if (latest.isEmpty() || state.value("created").toInteger() > latest.value("created").toLongLong()) latest = view;
    }
    return latest;
}

bool LocalChannel::canStartGame() const {
    const auto* c = chatClient();
    if (!c || !chatReady() || !c->accepted || !c->voice || c->id != joinedHostId_ || c->external() || !c->privatePeer.isEmpty()
        || !c->capabilities.contains("channel-games")) return false;
    const auto active = currentGame();
    return pendingGameRequest_.isEmpty() && (active.isEmpty() || active.value("phase") == "finished" || active.value("phase") == "cancelled");
}

bool LocalChannel::gameAction(const QString& action, const QVariantMap& selection, const QString& host, const QString& identity) {
    const auto validActions = {QStringLiteral("start"), QStringLiteral("join"), QStringLiteral("dismiss"),
        QStringLiteral("choose"), QStringLiteral("leave"), QStringLiteral("setup"), QStringLiteral("move")};
    if (std::find(validActions.begin(), validActions.end(), action) == validActions.end()
        || QJsonDocument(QJsonObject::fromVariantMap(selection)).toJson(QJsonDocument::Compact).size() > 256) return false;
    const auto actor = identity.isEmpty() ? remoteMode() ? remoteView_.value("ownId").toString() : ownId() : identity;
    if (actor != ownId()) {
        if (actor.isEmpty() || actor != remoteView_.value("ownId").toString()) return false;
        return remoteAction("game", {{"action", action}, {"selection", selection}, {"hostId", host.isEmpty() ? remoteView_.value("chatHostId") : QVariant(host)}});
    }
    if (action == "dismiss" && !detachedGame_.isEmpty() && detachedGame_.value("id") == selection.value("id")
        && detachedGame_.value("hostId") == (host.isEmpty() ? chatHostId_ : host)) {
        detachedGame_.clear(); emit chatChanged(); emit hostsChanged(); return true;
    }
    auto* c = host.isEmpty() ? chatClient() : clients_.value(host).get();
    if (!c || c->external() || !c->privatePeer.isEmpty()) return false;
    if (action == "dismiss") {
        const auto id = selection.value("id").toString();
        for (qsizetype i = 0; i < c->games.size(); ++i) {
            const auto state = c->games[i].toObject();
            if (state.value("id") != id || c->gameId != id) continue;
            const auto phase = state.value("phase").toString();
            if (phase != "finished" && phase != "cancelled") return false;
            c->games.removeAt(i); c->gameId.clear();
            emit chatChanged(); emit hostsChanged(); return true;
        }
        return false;
    }
    if (!c->accepted || !c->voice || c->id != joinedHostId_ || !c->capabilities.contains("channel-games")) return false;
    auto command = QJsonObject::fromVariantMap(selection);
    command.insert("kind", "game"); command.insert("action", action);
    if (action == "start" || action == "join") {
        const auto active = currentGame();
        if (!pendingGameRequest_.isEmpty() || (!active.isEmpty() && active.value("phase") != "finished" && active.value("phase") != "cancelled")) return false;
        pendingGameHost_ = c->id; pendingGameRequest_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        command.insert("request", pendingGameRequest_);
    }
    c->chatError.clear();
    if (sendChatCommand(*c, command)) return true;
    if (pendingGameHost_ == c->id) { pendingGameHost_.clear(); pendingGameRequest_.clear(); }
    return false;
}

bool LocalChannel::hostGameAction(QSslSocket* socket, const QJsonObject& command) {
    auto peer = peers_.find(socket);
    if (peer == peers_.end() || !peer->joined || peer->controller || peer->observer || !peer->capabilities.contains("channel-games")) return false;
    const auto now = controlClock_.elapsed();
    if (now - peer->gameWindow >= 10000) { peer->gameWindow = now; peer->gameRequests = 0; }
    if (++peer->gameRequests > 60) { socket->abort(); return true; }
    QString id = command.value("id").toString();
    const auto round = command.value("round").toInt(-1);
    const auto action = command.value("action").toString();
    const auto request = command.value("request").toString();
    bool accepted = false, changed = false;
    if (QJsonDocument(command).toJson(QJsonDocument::Compact).size() <= 512
        && command.value("id").isString() && (id.isEmpty() || !QUuid(id).isNull())
        && round >= 0 && command.value("round").toDouble(-1) == round
        && command.value("action").isString() && action.size() <= 16
        && (!command.contains("request") || command.value("request").isString())
        && (request.isEmpty() || !QUuid(request).isNull())) {
        const auto* root = service_ ? service_ : this;
        const auto occupied = [&](const LocalChannel& channel) {
            return std::any_of(channel.hostGames_.cbegin(), channel.hostGames_.cend(), [&](const auto& game) { return game.contains(peer->id); });
        };
        bool elsewhere = occupied(*root);
        for (const auto& channel : root->owned_) elsewhere |= occupied(*channel);
        auto selection = command;
        for (const auto* key : {"id", "round", "kind", "action", "request"}) selection.remove(key);
        if ((action == "start" || action == "join") && elsewhere) {
            accepted = false;
        } else if (action == "start") {
            ChannelGame game;
            accepted = id.isEmpty() && round == 0 && selection.isEmpty()
                && game.start(peer->id, peer->name, now, clock_());
            if (accepted) { id = game.view(peer->id).value("id").toString(); hostGames_.insert(id, game); changed = true; }
        } else if (auto game = hostGames_.find(id); game != hostGames_.end()) {
            const auto result = game->act(id, round, peer->id, peer->name, action, selection, now);
            accepted = result.accepted; changed = result.changed;
        }
    }
    if (changed) publishGame(id, socket, request);
    else {
        QJsonObject state;
        const auto game = hostGames_.constFind(id);
        if (game != hostGames_.cend() && (game->contains(peer->id) || game->view(peer->id).value("phase") == "waiting")) state = game->view(peer->id);
        writeChat(socket, {{"kind", "game"}, {"now", std::max(history_->time(), clock_())},
            {"state", state}, {"request", request}, {"error", accepted ? QString{} : tr("That game action is no longer available.")}});
    }
    return true;
}

void LocalChannel::sendGames(QSslSocket* socket) {
    const auto peer = peers_.constFind(socket);
    if (!history_ || peer == peers_.cend() || !peer->joined || peer->controller || peer->observer || !peer->capabilities.contains("channel-games")) return;
    const auto games = hostGames_;
    const auto identity = peer->id;
    for (auto game = games.cbegin(); game != games.cend(); ++game) {
        if (!peers_.contains(socket)) return;
        const auto state = game->view(identity);
        if (!game->contains(identity) && state.value("phase") != "waiting") continue;
        if (!game->contains(identity)) peers_[socket].gameInvitations.insert(game.key());
        writeChat(socket, {{"kind", "game"}, {"now", std::max(history_->time(), clock_())}, {"state", state}});
    }
}

void LocalChannel::publishGame(const QString& id, QSslSocket* actor, const QString& receipt) {
    const auto game = hostGames_.constFind(id);
    if (!history_ || game == hostGames_.cend()) return;
    const auto snapshot = *game;
    for (auto* socket : peers_.keys()) {
        auto peerPosition = peers_.find(socket);
        if (peerPosition == peers_.end()) continue;
        auto& peer = *peerPosition;
        if (!peer.joined || peer.controller || !peer.capabilities.contains("channel-games")) continue;
        const bool participant = snapshot.contains(peer.id);
        const auto state = snapshot.view(peer.id);
        const bool invitation = !peer.observer && state.value("phase") == "waiting";
        const bool removed = peer.gameInvitations.remove(id);
        if (!participant && !invitation && !removed) continue;
        if (invitation && !participant) peer.gameInvitations.insert(id);
        writeChat(socket, {{"kind", "game"}, {"now", std::max(history_->time(), clock_())},
            {"state", participant || invitation ? state : QJsonObject{}}, {"removed", participant || invitation ? QString{} : id},
            {"request", socket == actor ? receipt : QString{}}});
    }
    if (!snapshot.active()) hostGames_.remove(id);
}

void LocalChannel::endGames(const QString& identity, const QString& reason) {
    for (const auto& id : hostGames_.keys()) {
        auto game = hostGames_.find(id);
        if (game != hostGames_.end() && game->contains(identity) && game->end(reason, identity)) publishGame(id);
    }
}

bool LocalChannel::applyGame(Client& c, const QJsonObject& state, const QString& removed) {
    if (!removed.isEmpty()) {
        if (!state.isEmpty() || QUuid(removed).isNull() || removed == c.gameId) return false;
        for (qsizetype i = c.games.size(); i > 0; --i) if (c.games[i - 1].toObject().value("id") == removed) c.games.removeAt(i - 1);
        return true;
    }
    if (state.isEmpty()) return true;
    if (!validGameState(state)) return false;
    const auto id = state.value("id").toString(), phase = state.value("phase").toString();
    const auto players = state.value("players").toArray();
    const bool personal = std::any_of(players.cbegin(), players.cend(), [&](const auto& p) { return p.toObject().value("id") == ownId(); });
    const bool terminal = phase == "finished" || phase == "cancelled";
    if (!c.voice && !(personal && terminal && c.gameId == id)) return true;
    if (!personal && phase != "waiting") return false;
    if (personal) {
        const auto previous = currentGame();
        if (!previous.isEmpty() && previous.value("id") != id && previous.value("phase") != "finished" && previous.value("phase") != "cancelled") {
            if (terminal) return true;
            return false;
        }
        detachedGame_.clear();
        for (const auto& client : clients_) {
            if (client->gameId.isEmpty() || client->gameId == id) continue;
            for (qsizetype i = client->games.size(); i > 0; --i)
                if (client->games[i - 1].toObject().value("id") == client->gameId) client->games.removeAt(i - 1);
            client->gameId.clear();
        }
        c.gameId = id;
    }
    for (qsizetype i = 0; i < c.games.size(); ++i) if (c.games[i].toObject().value("id") == id) {
        const auto oldPhase = c.games[i].toObject().value("phase").toString();
        if ((oldPhase == "finished" || oldPhase == "cancelled") && !terminal) return false;
        c.games[i] = state; return true;
    }
    c.games.append(state);
    return true;
}

bool LocalChannel::validGameState(const QJsonObject& state) {
    return ChannelGame::validView(state);
}
