#include "tls_identity.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QScopeGuard>
#include <QUuid>
#include <QSslKey>
#include <qtkeychain/keychain.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>
#include <openssl/x509.h>
#include <memory>
#include <stdexcept>
#include <utility>
#ifndef Q_OS_WIN
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif
#ifdef Q_OS_MACOS
#include <sys/acl.h>
#endif

namespace {
template<class T, auto Free> using Owned = std::unique_ptr<T, decltype(Free)>;
void require(bool okay) {
    if (!okay) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "The encrypted device identity could not be created.").toStdString());
}
}

TlsIdentity::TlsIdentity(QSslConfiguration configuration) : configuration_(std::move(configuration)) {}

QString TlsIdentity::peerId(const QSslCertificate& certificate) {
    if (certificate.isNull() || certificate.publicKey().isNull()) return {};
    return QString::fromLatin1(QCryptographicHash::hash(certificate.publicKey().toDer(), QCryptographicHash::Sha256).toHex());
}

QString TlsIdentity::id() const { return peerId(configuration_.localCertificate()); }

QByteArray TlsIdentity::pem() const {
    return configuration_.localCertificate().toPem() + configuration_.privateKey().toPem();
}

TlsIdentity TlsIdentity::fromPem(const QByteArray& pem) {
    if (pem.size() > 16384) throw std::invalid_argument(QCoreApplication::translate("TlsIdentity", "Invalid device identity size.").toStdString());
    const QSslCertificate certificate(pem);
    const QSslKey key(pem, QSsl::Rsa);
    Owned<BIO, BIO_free> input(BIO_new_mem_buf(pem.constData(), int(pem.size())), BIO_free);
    if (!input) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "The device identity could not be loaded.").toStdString());
    Owned<X509, X509_free> x509(PEM_read_bio_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
    Owned<EVP_PKEY, EVP_PKEY_free> privateKey(PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (certificate.isNull() || key.isNull() || !x509 || !privateKey
        || X509_check_private_key(x509.get(), privateKey.get()) != 1
        || X509_cmp_current_time(X509_get0_notBefore(x509.get())) >= 0
        || X509_cmp_current_time(X509_get0_notAfter(x509.get())) <= 0)
        throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "The stored device identity is invalid or expired.").toStdString());
    auto config = QSslConfiguration::defaultConfiguration();
    config.setLocalCertificate(certificate);
    config.setPrivateKey(key);
    config.setProtocol(QSsl::TlsV1_2OrLater);
    config.setPeerVerifyMode(QSslSocket::VerifyPeer);
    config.setMissingCertificateIsFatal(true);
    config.setAllowedNextProtocols({"squadspeak/1"});
    return TlsIdentity(config);
}

TlsIdentity TlsIdentity::create() {
    Owned<EVP_PKEY, EVP_PKEY_free> key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", size_t(3072)), EVP_PKEY_free);
    Owned<X509, X509_free> certificate(X509_new(), X509_free);
    require(key != nullptr && certificate != nullptr);
    unsigned char serialBytes[16];
    require(RAND_bytes(serialBytes, sizeof(serialBytes)) == 1);
    serialBytes[0] &= 0x7f;
    Owned<BIGNUM, BN_free> serial(BN_bin2bn(serialBytes, sizeof(serialBytes), nullptr), BN_free);
    require(serial != nullptr);
    require(BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate.get())) != nullptr);
    require(X509_set_version(certificate.get(), 2) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -300) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 10L * 365 * 24 * 3600) != nullptr);
    require(X509_set_pubkey(certificate.get(), key.get()) == 1);
    auto* name = X509_get_subject_name(certificate.get());
    require(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("SquadSpeak device"), -1, -1, 0) == 1);
    require(X509_set_issuer_name(certificate.get(), name) == 1);
    require(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);
    Owned<BIO, BIO_free> output(BIO_new(BIO_s_mem()), BIO_free);
    require(output != nullptr);
    require(PEM_write_bio_X509(output.get(), certificate.get()) == 1);
    require(PEM_write_bio_PrivateKey(output.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
    char* data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    require(size > 0);
    return fromPem(QByteArray(data, size));
}

TlsIdentity TlsIdentity::loadFile(const QString& path, bool createIfMissing) {
#ifdef Q_OS_WIN
    Q_UNUSED(path)
    Q_UNUSED(createIfMissing)
    throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity files require Linux or macOS. Use the Windows credential store.").toStdString());
#else
    if (path.isEmpty() || path.contains(QChar(0))) throw std::invalid_argument(QCoreApplication::translate("TlsIdentity", "Invalid identity file path.").toStdString());
    const auto checkAccess = [](int descriptor, bool directory) {
        struct stat info {};
        if (::fstat(descriptor, &info) != 0 || info.st_uid != ::geteuid()
            || (directory ? !S_ISDIR(info.st_mode) || (info.st_mode & 0022) != 0
                          : !S_ISREG(info.st_mode) || ((info.st_mode & 0777) != 0600 && (info.st_mode & 0777) != 0400)))
            throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity storage must belong to this account: file mode 0600 or 0400, parent not writable by others.").toStdString());
#ifdef Q_OS_MACOS
        const auto acl = ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
        if (!acl) {
            // Darwin reports an absent extended ACL as ENOENT on an open fd.
            if (errno != ENOENT) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity storage access rules could not be inspected.").toStdString());
        } else {
            const auto release = qScopeGuard([&] { ::acl_free(acl); });
            acl_entry_t entry;
            if (::acl_get_entry(acl, ACL_FIRST_ENTRY, &entry) == 0)
                throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity storage must not have extended access rules.").toStdString());
        }
#endif
        return info.st_size;
    };
    const QFileInfo destination(path);
    const auto directoryPath = destination.dir().canonicalPath();
    if (directoryPath.isEmpty() || destination.fileName().isEmpty())
        throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity file needs an existing private parent directory.").toStdString());
    const int directory = ::open(QFile::encodeName(directoryPath).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity directory could not be opened.").toStdString());
    const auto closeDirectory = qScopeGuard([&] { ::close(directory); });
    checkAccess(directory, true);
    const auto name = QFile::encodeName(destination.fileName());
    const auto readExisting = [&]() -> std::optional<TlsIdentity> {
        const int descriptor = ::openat(directory, name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (descriptor < 0) {
            if (errno == ENOENT) return std::nullopt;
            throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity file could not be opened; symbolic links are not allowed.").toStdString());
        }
        const auto closeFile = qScopeGuard([&] { ::close(descriptor); });
        const auto size = checkAccess(descriptor, false);
        if (size <= 0 || size > 16384) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Invalid device identity size.").toStdString());
        QFile file;
        if (!file.open(descriptor, QIODevice::ReadOnly)) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity file could not be read.").toStdString());
        auto bytes = file.read(16385);
        const auto wipe = qScopeGuard([&] { bytes.fill(0); });
        if (bytes.size() != size) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity file changed while reading.").toStdString());
        return fromPem(bytes);
    };
    if (const auto existing = readExisting()) return *existing;
    if (!createIfMissing) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "The original identity file is missing. Restore it or use a new server storage prefix.").toStdString());
    const auto identity = create();
    const auto temporary = QByteArray(".identity-") + QUuid::createUuid().toByteArray(QUuid::WithoutBraces) + ".tmp";
    const int descriptor = ::openat(directory, temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity file could not be created.").toStdString());
    const auto cleanup = qScopeGuard([&] { ::close(descriptor); ::unlinkat(directory, temporary.constData(), 0); });
    checkAccess(descriptor, false);
    QFile file;
    if (!file.open(descriptor, QIODevice::WriteOnly)) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity file could not be written.").toStdString());
    auto bytes = identity.pem();
    const auto wipe = qScopeGuard([&] { bytes.fill(0); });
    if (file.write(bytes) != bytes.size() || !file.flush() || ::fsync(descriptor) != 0)
        throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity file could not be saved.").toStdString());
    // Publish the complete key without replacing a concurrent creator's key.
    if (::linkat(directory, temporary.constData(), directory, name.constData(), 0) != 0 && errno != EEXIST)
        throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity file could not be published.").toStdString());
    if (::fsync(directory) != 0) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity directory could not be synchronized.").toStdString());
    const auto stored = readExisting();
    if (!stored) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", "Identity file disappeared before startup.").toStdString());
    return *stored;
#endif
}

bool TlsIdentity::load(const QString& slot, QObject* context,
                       std::function<void(std::optional<TlsIdentity>, QString)> completion) {
    auto* job = new QKeychain::ReadPasswordJob("SquadSpeak", context);
    job->setKey(slot);
    job->setInsecureFallback(false);
    QObject::connect(job, &QKeychain::Job::finished, context,
        [slot, context, completion = std::move(completion), job](QKeychain::Job*) {
        if (job->error() == QKeychain::NoError) {
            std::optional<TlsIdentity> identity;
            QString diagnostic;
            try { identity = fromPem(job->binaryData()); }
            catch (const std::exception& error) { diagnostic = QString::fromUtf8(error.what()); }
            completion(std::move(identity), diagnostic);
            return;
        }
        if (job->error() != QKeychain::EntryNotFound) {
            completion(std::nullopt, QCoreApplication::translate("TlsIdentity", "Device secret unavailable: %1").arg(job->errorString()));
            return;
        }
        try {
            const auto identity = create();
            auto* write = new QKeychain::WritePasswordJob("SquadSpeak", context);
            write->setKey(slot);
            write->setInsecureFallback(false);
            write->setBinaryData(identity.pem());
            QObject::connect(write, &QKeychain::Job::finished, context, [completion, identity, write](QKeychain::Job*) {
                if (write->error() == QKeychain::NoError) completion(identity, {});
                else completion(std::nullopt, QCoreApplication::translate("TlsIdentity", "Device secret could not be stored: %1").arg(write->errorString()));
            });
            write->start();
        } catch (const std::exception& error) { completion(std::nullopt, QString::fromUtf8(error.what())); }
    });
    job->start();
    return true;
}

namespace {
void cryptRequire(bool okay, const char* message = QT_TRANSLATE_NOOP("TlsIdentity", "Encryption failed.")) {
    if (!okay) throw std::runtime_error(QCoreApplication::translate("TlsIdentity", message).toStdString());
}
}

QByteArray TlsIdentity::newKey() {
    QByteArray key(32, Qt::Uninitialized);
    cryptRequire(RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), key.size()) == 1);
    return key;
}

QByteArray TlsIdentity::seal(const QByteArray& plain, const QByteArray& key, const QByteArray& context) {
    cryptRequire(key.size() == 32 && plain.size() <= 40 * 1024 * 1024);
    QByteArray result(12 + plain.size() + 16, Qt::Uninitialized);
    auto* bytes = reinterpret_cast<unsigned char*>(result.data());
    cryptRequire(RAND_bytes(bytes, 12) == 1);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    cryptRequire(bool(cipher));
    int written = 0, tail = 0;
    cryptRequire(EVP_EncryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char*>(key.constData()), bytes) == 1);
    cryptRequire(EVP_EncryptUpdate(cipher.get(), nullptr, &written,
        reinterpret_cast<const unsigned char*>(context.constData()), int(context.size())) == 1);
    cryptRequire(EVP_EncryptUpdate(cipher.get(), bytes + 12, &written,
        reinterpret_cast<const unsigned char*>(plain.constData()), int(plain.size())) == 1);
    cryptRequire(EVP_EncryptFinal_ex(cipher.get(), bytes + 12 + written, &tail) == 1);
    cryptRequire(written + tail == plain.size());
    cryptRequire(EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, 16, bytes + 12 + plain.size()) == 1);
    return result;
}

QByteArray TlsIdentity::open(const QByteArray& sealed, const QByteArray& key, const QByteArray& context) {
    cryptRequire(key.size() == 32 && sealed.size() >= 28 && sealed.size() <= 40 * 1024 * 1024 + 28,
        QT_TRANSLATE_NOOP("TlsIdentity", "Invalid encrypted data."));
    const auto* bytes = reinterpret_cast<const unsigned char*>(sealed.constData());
    QByteArray plain(sealed.size() - 28, Qt::Uninitialized);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    cryptRequire(bool(cipher));
    int written = 0, tail = 0;
    cryptRequire(EVP_DecryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char*>(key.constData()), bytes) == 1);
    cryptRequire(EVP_DecryptUpdate(cipher.get(), nullptr, &written,
        reinterpret_cast<const unsigned char*>(context.constData()), int(context.size())) == 1);
    cryptRequire(EVP_DecryptUpdate(cipher.get(), reinterpret_cast<unsigned char*>(plain.data()), &written,
        bytes + 12, int(plain.size())) == 1);
    cryptRequire(EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, 16,
        const_cast<unsigned char*>(bytes + sealed.size() - 16)) == 1);
    cryptRequire(EVP_DecryptFinal_ex(cipher.get(), reinterpret_cast<unsigned char*>(plain.data()) + written, &tail) == 1,
        QT_TRANSLATE_NOOP("TlsIdentity", "Data was modified or belongs to another device identity."));
    cryptRequire(written + tail == plain.size());
    return plain;
}

QByteArray TlsIdentity::deriveKey(const QByteArray& context, const QByteArray& purpose) const {
    auto secret = configuration_.privateKey().toDer();
    Owned<EVP_PKEY_CTX, EVP_PKEY_CTX_free> derive(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
    cryptRequire(bool(derive));
    QByteArray key(32, Qt::Uninitialized); size_t size = 32;
    cryptRequire(EVP_PKEY_derive_init(derive.get()) == 1 && EVP_PKEY_CTX_set_hkdf_md(derive.get(), EVP_sha256()) == 1);
    cryptRequire(EVP_PKEY_CTX_set1_hkdf_salt(derive.get(), reinterpret_cast<const unsigned char*>(context.constData()), context.size()) == 1);
    cryptRequire(EVP_PKEY_CTX_set1_hkdf_key(derive.get(), reinterpret_cast<const unsigned char*>(secret.constData()), secret.size()) == 1);
    cryptRequire(EVP_PKEY_CTX_add1_hkdf_info(derive.get(), reinterpret_cast<const unsigned char*>(purpose.constData()), purpose.size()) == 1);
    cryptRequire(EVP_PKEY_derive(derive.get(), reinterpret_cast<unsigned char*>(key.data()), &size) == 1 && size == 32);
    secret.fill(0);
    return key;
}

QByteArray TlsIdentity::passwordHash(const QByteArray& password, const QByteArray& salt) {
    cryptRequire(!password.isEmpty() && password.size() <= 1024 && salt.size() == 32, QT_TRANSLATE_NOOP("TlsIdentity", "Invalid password size."));
    QByteArray result(32, Qt::Uninitialized);
    cryptRequire(PKCS5_PBKDF2_HMAC(password.constData(), password.size(),
        reinterpret_cast<const unsigned char*>(salt.constData()), salt.size(), 600000, EVP_sha256(),
        result.size(), reinterpret_cast<unsigned char*>(result.data())) == 1, QT_TRANSLATE_NOOP("TlsIdentity", "The password could not be verified."));
    return result;
}
