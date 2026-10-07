#pragma once

#include "tls_identity.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QCache>
#include <QSqlDatabase>
#include <QSqlQuery>

// The trusted host owns receipt time and durable acknowledgement. Disk data is
// authenticated and encrypted with a purpose-separated device secret.
class ChatHistory final {
public:
    static constexpr qint64 lifetime = 24 * 60 * 60 * 1000;
    static constexpr qint64 maximumLifetime = 360 * lifetime;
    [[nodiscard]] static bool validLifetime(qint64 duration) { return duration == lifetime || duration == 7 * lifetime || duration == 30 * lifetime
        || duration == 90 * lifetime || duration == 180 * lifetime || duration == maximumLifetime; }
    static constexpr int maximumTextBytes = 16384;
    static constexpr int maximumPageMessages = 40;
    static constexpr int maximumPageBytes = 40 * 1024;
    ChatHistory(QString file, const TlsIdentity& identity, qint64 now);
    ~ChatHistory();
    ChatHistory(const ChatHistory&) = delete;
    ChatHistory& operator=(const ChatHistory&) = delete;
    [[nodiscard]] QJsonObject page(qint64 cursor = 0, const QString& direction = "latest", bool extended = true) const;
    [[nodiscard]] qint64 time() const { return time_; }
    // Repeating the same sender/request returns its original receipt. Conflicting
    // reuse is rejected. No message becomes visible before the atomic save.
    QJsonObject append(const QString& sender, const QString& name,
                       const QString& request, const QString& text, qint64 now, const QByteArray& image = {},
                       const QString& avatar = QStringLiteral("mossling"), qint64 duration = lifetime,
                       const QJsonObject& event = {});
    [[nodiscard]] QByteArray image(const QString& hash, qint64 now);
    bool expire(qint64 now);
    // Authenticated profile changes keep receipt time, content and identity intact.
    bool updateProfile(const QString& sender, const QString& name, const QString& avatar);
    [[nodiscard]] static bool validText(const QString& text);
    [[nodiscard]] static bool validMessage(const QJsonObject& message);
    [[nodiscard]] static QJsonObject forReader(QJsonObject message, bool extended);
private:
    QSqlQuery query(const QString& sql, const QVariantList& values = {}) const;
    QByteArray index(const QString& value) const;
    QJsonObject record(const QSqlQuery& row) const;
    void insert(const QJsonObject& message);
    void saveClock(qint64 now, qint64 sequence);
    void readClock();
    void pruneImage(const QString& hash);
    void pruneImages();
    QString file_;
    QByteArray key_;
    QByteArray context_;
    QByteArray indexKey_;
    QSqlDatabase database_;
    qint64 time_ = 0;
    qint64 sequence_ = 0;
    QCache<QString, QByteArray> images_{32 * 1024 * 1024};
};
