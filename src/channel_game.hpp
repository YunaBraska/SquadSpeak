#pragma once

#include <QJsonObject>
#include <QRandomGenerator>
#include <QString>
#include <QVector>
#include <array>
#include <optional>

// One host-owned, in-memory activity. Transport supplies authenticated identities.
class ChannelGame final {
public:
    struct Result final { bool accepted = false; bool changed = false; };
    explicit ChannelGame(std::optional<quint32> seed = std::nullopt) : seed_(seed) {}
    bool start(const QString& owner, const QString& name, qint64 now, qint64 created);
    Result act(const QString& id, int round, const QString& sender, const QString& name,
               const QString& action, const QJsonObject& selection, qint64 now);
    bool advance(qint64 now);
    bool end(const QString& reason);
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool contains(const QString& player) const;
    [[nodiscard]] QJsonObject view(const QString& viewer, qint64 now) const;
    [[nodiscard]] std::optional<qint64> deadline() const;
    [[nodiscard]] static bool validView(const QJsonObject& state);

private:
    struct Player final {
        QString id, name, choice;
        int core = -1, trap = -1;
        bool ready = false;
        qint64 readyAt = 0;
    };
    struct Piece final {
        int owner = -1;
        QString kind;
        bool revealed = false;
    };
    std::optional<quint32> seed_;
    QRandomGenerator random_;
    QString id_, phase_, reason_, winner_;
    qint64 created_ = 0;
    int round_ = 1, turn_ = -1, duelFrom_ = -1, duelTo_ = -1;
    QVector<Player> players_;
    std::array<Piece, 42> board_{};
    QJsonObject lastBattle_;
};
