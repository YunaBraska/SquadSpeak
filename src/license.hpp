#pragma once

#include <QObject>
#include <QDateTime>
#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QUrl>
#include <functional>

// Owns the OS account's GitHub sign-in and encrypted supporter evidence.
// Channel identities, audio profiles and remote controllers never receive tokens.
class License final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool configured READ configured CONSTANT)
    Q_PROPERTY(bool directDistribution READ directDistribution CONSTANT)
    Q_PROPERTY(QUrl purchaseUrl READ purchaseUrl CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString account READ account NOTIFY changed)
    Q_PROPERTY(QString userCode READ userCode NOTIFY changed)
    Q_PROPERTY(QUrl verificationUrl READ verificationUrl NOTIFY changed)
    Q_PROPERTY(QDateTime expiresAt READ expiresAt NOTIFY changed)
public:
    // Configuration contains client_id, recipient_id, tier_id, owner and repository.
    // An empty configuration disables sign-in. Endpoints are injectable HTTPS
    // boundaries. An explicit identity file supports servers without a keychain.
    // A supplied storage key and clock isolate deterministic integration tests.
    License(QString directory, QJsonObject product, QUrl endpoint,
            QByteArray storageKey = {}, std::function<qint64()> clock = {},
            QString identityFile = {}, QObject* parent = nullptr,
            QUrl oauthEndpoint = QUrl("https://github.com"));
    ~License() override;
    [[nodiscard]] static QJsonObject distributionProduct();
    [[nodiscard]] static QString storageDirectory();
    [[nodiscard]] static bool directDistribution();
    [[nodiscard]] static QUrl purchaseUrl();
    [[nodiscard]] bool configured() const { return directDistribution() && !product_.isEmpty(); }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool signedIn() const { return !record_.value("token").toString().isEmpty(); }
    [[nodiscard]] bool pending() const { return !deviceCode_.isEmpty(); }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString account() const { return record_.value("login").toString(); }
    [[nodiscard]] QString userCode() const { return userCode_; }
    [[nodiscard]] QUrl verificationUrl() const { return verificationUrl_; }
    [[nodiscard]] QDateTime expiresAt() const;
    Q_INVOKABLE bool signIn();
    Q_INVOKABLE bool cancelSignIn();
    Q_INVOKABLE bool signOut();
    Q_INVOKABLE bool refresh();
    // Also used on resume. Polling honors GitHub's interval, even if called often.
    bool refreshIfDue();
signals:
    void changed();
    void authorizationReady();
private:
    enum class Action { Check, SignIn, SignOut };
    bool begin(Action action);
    void read(Action action, const QByteArray& sealed);
    void request(const QUrl& url, const QJsonObject& payload, bool authenticated,
                 std::function<void(QJsonObject, int)> completion);
    void startAuthorization();
    void pollAuthorization();
    bool acceptToken(const QJsonObject& response);
    void refreshToken();
    void checkPage(const QString& cursor = {}, bool accountOnly = false);
    void failCheck();
    [[nodiscard]] QJsonObject policy() const;
    bool save(const QJsonObject& record);
    void finish(QString status, bool transient = false);
    void schedule();
    [[nodiscard]] qint64 now() const;
    [[nodiscard]] qint64 usableUntil() const;
    QString directory_;
    QJsonObject product_;
    QUrl endpoint_, oauthEndpoint_;
    QByteArray storageKey_;
    QString identityFile_, receiptIdentityFile_;
    QByteArray suppliedStorageKey_;
    std::function<qint64()> clock_;
    QLockFile lock_;
    QNetworkAccessManager network_;
    QTimer timer_;
    QJsonObject record_, persisted_, viewer_;
    QHash<QString, QJsonObject> events_;
    QSet<QString> cursors_;
    QString status_, deviceCode_, userCode_;
    QUrl verificationUrl_;
    bool rotated_ = false;
    bool busy_ = false, storageHealthy_ = true, unsaved_ = false, contributor_ = false;
    qint64 retryAfter_ = 0;
    qint64 nextCheck_ = 0, authorizationExpires_ = 0, nextPoll_ = 0;
    int failures_ = 0, pollInterval_ = 5000;
    quint64 generation_ = 0;
};
