#pragma once

#include <QSslConfiguration>
#include <QString>
#include <functional>
#include <optional>

// A self-signed device identity. TLS proves possession of the private key;
// explicit host admission and pinned public-key IDs establish local trust.
class TlsIdentity final {
public:
    [[nodiscard]] static TlsIdentity create();
    [[nodiscard]] QByteArray deriveKey(const QByteArray& context, const QByteArray& purpose) const;
    [[nodiscard]] static QByteArray newKey();
    [[nodiscard]] static QByteArray seal(const QByteArray& plain, const QByteArray& key, const QByteArray& context);
    [[nodiscard]] static QByteArray open(const QByteArray& sealed, const QByteArray& key, const QByteArray& context);
    // CPU-bound; callers run this away from the audio and UI event loop.
    [[nodiscard]] static QByteArray passwordHash(const QByteArray& password, const QByteArray& salt);
    [[nodiscard]] static TlsIdentity fromPem(const QByteArray& pem);
    // Explicit POSIX server storage. Requires an owned, non-writable-by-others
    // directory and a regular 0600/0400 file. Missing files are atomically
    // created only when requested; existing identities are never overwritten.
    [[nodiscard]] static TlsIdentity loadFile(const QString& path, bool createIfMissing);
    [[nodiscard]] QByteArray pem() const;
    [[nodiscard]] QString id() const;
    [[nodiscard]] QSslConfiguration configuration() const { return configuration_; }
    [[nodiscard]] static QString peerId(const QSslCertificate& certificate);
    // Secrets never fall back to plaintext storage. The callback runs on the
    // context's thread and is cancelled when its QObject context is destroyed.
    static bool load(const QString& slot, QObject* context,
                     std::function<void(std::optional<TlsIdentity>, QString)> completion);
private:
    explicit TlsIdentity(QSslConfiguration configuration);
    QSslConfiguration configuration_;
};
