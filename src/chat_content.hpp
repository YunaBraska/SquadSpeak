#pragma once

#include <QObject>
#include <QColor>
#include <QFont>
#include <QUrl>
#include <QVariantMap>
#include <QSize>
#include <functional>

// Rendering never fetches URLs. Senders stage bounded opaque source bytes;
// only the host invokes the isolated decoder before distribution.
class ChatContent final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QByteArray preparedImage READ preparedImage NOTIFY changed)
    Q_PROPERTY(bool hasImage READ hasImage NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    using QObject::QObject;
    Q_INVOKABLE QString format(const QString& markdown, const QVariantMap& attachment = {},
                               const QColor& linkColor = Qt::blue, const QColor& codeBackground = Qt::lightGray,
                               const QFont& font = {}) const;
    Q_INVOKABLE bool prepareMessage(const QString& markdown);
    [[nodiscard]] static QString embedImage(const QString& markdown, const QByteArray& image);
    Q_INVOKABLE bool openLink(const QString& link) const;
    [[nodiscard]] static bool allowedLink(const QUrl& link);
    static constexpr qint64 maximumImageBytes = 8 * 1024 * 1024;
    static constexpr qint64 maximumSourceBytes = 25 * 1024 * 1024;
    static constexpr qint64 maximumPixels = 24 * 1024 * 1024;
    [[nodiscard]] static QByteArray sanitizeImage(const QByteArray& source);
    [[nodiscard]] static QSize sanitizedImageSize(const QByteArray& png);
    static bool prepare(const QByteArray& source, QObject* context,
                        std::function<void(QByteArray, QString)> completion);
    Q_INVOKABLE bool prepareFile(const QUrl& path);
    Q_INVOKABLE bool pasteImage();
    Q_INVOKABLE void clearImage();
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] QByteArray preparedImage() const { return prepared_; }
    [[nodiscard]] bool hasImage() const { return !prepared_.isEmpty(); }
    [[nodiscard]] QString error() const { return error_; }
signals:
    void changed();
    void messagePrepared(const QString& markdown, const QByteArray& image);
private:
    bool prepareDraft(const QByteArray& source);
    QByteArray prepared_;
    QString error_;
    QString preparedUrl_;
    bool busy_ = false;
};
