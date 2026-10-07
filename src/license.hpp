#pragma once

#include <QObject>
#include <QDateTime>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QUrl>
#include <functional>

// Owns one OS account's provider activation and encrypted receipt. Channel
// identities, audio profiles and remote controllers never receive its secrets.
class License final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool configured READ configured CONSTANT)
    Q_PROPERTY(bool directDistribution READ directDistribution CONSTANT)
    Q_PROPERTY(QUrl purchaseUrl READ purchaseUrl CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(bool recoveryNeeded READ recoveryNeeded NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString supportReference READ supportReference NOTIFY changed)
    Q_PROPERTY(QDateTime expiresAt READ expiresAt NOTIFY changed)
public:
    // Product contains the expected public store_id, product_id and variant_id.
    // An empty product disables licensing. The directory is shared by all local
    // profiles and modes. An explicit identity file permits headless storage
    // without a keychain; its location stays in the receipt envelope
    // so the desktop can reuse it. A supplied key isolates integration tests.
    License(QString directory, QJsonObject product, QUrl endpoint,
            QByteArray storageKey = {}, std::function<qint64()> clock = {},
            QString identityFile = {}, QObject* parent = nullptr);
    ~License() override;
    [[nodiscard]] static QJsonObject distributionProduct();
    [[nodiscard]] static bool directDistribution();
    [[nodiscard]] static QUrl purchaseUrl();
    [[nodiscard]] bool configured() const { return directDistribution() && !product_.isEmpty(); }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool pending() const { return record_.contains("pending"); }
    [[nodiscard]] bool recoveryNeeded() const;
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString supportReference() const;
    [[nodiscard]] QDateTime expiresAt() const;
    // Reads the existing receipt and checks it online. Repeating activation of
    // a known instance validates it instead of consuming another device slot.
    Q_INVOKABLE bool refresh();
    // Also called when the app resumes; elapsed offline time does not delay a
    // due check, and focus changes never create extra provider requests.
    bool refreshIfDue();
    Q_INVOKABLE bool activate(const QString& key);
    Q_INVOKABLE bool deactivate();
    // Use only after manual support released an uncertain or invalid device slot.
    // A still-valid activation and all app preferences remain untouched.
    Q_INVOKABLE bool resetActivation();
signals:
    void changed();
private:
    enum class Action { Check, Activate, Deactivate, Reset };
    bool begin(Action action, QString key = {});
    void read(Action action, const QString& key, const QByteArray& sealed);
    void request(const QString& action, const QString& key, const QString& instance,
                 std::function<void(QJsonObject, bool)> completion);
    void validate(const QString& key, const QString& instance, bool activation);
    [[nodiscard]] QJsonObject receipt(const QJsonObject& response, const QString& key,
                                      const QString& instance, bool activated) const;
    bool save(const QJsonObject& record);
    void finish(QString status, bool transient = false);
    void schedule();
    [[nodiscard]] qint64 now() const;
    QString directory_;
    QJsonObject product_;
    QUrl endpoint_;
    QByteArray storageKey_;
    QString identityFile_, receiptIdentityFile_;
    QByteArray suppliedStorageKey_;
    std::function<qint64()> clock_;
    QLockFile lock_;
    QNetworkAccessManager network_;
    QTimer timer_;
    QJsonObject record_;
    QJsonObject persisted_;
    QString status_;
    bool busy_ = false;
    bool storageHealthy_ = true;
    bool unsaved_ = false;
    qint64 nextCheck_ = 0;
    int failures_ = 0;
};
