#include "license.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <array>
#include <memory>

namespace {
bool complete(License& license, bool started) {
    if (!started) return false;
    if (!license.busy()) return true;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&license, &License::changed, &loop, [&] { if (!license.busy()) loop.quit(); });
    timeout.start(45000);
    loop.exec();
    return !license.busy();
}
}

// Explicit merchant smoke, excluded from ordinary builds and CTest. Input is a
// private JSON response from the public /licenses/validate endpoint in test mode.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        qCritical("Usage: license_provider_probe test-validation.json"); return 2;
    }
    QFile file(app.arguments().at(1));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        qCritical("Cannot read the bounded test validation response."); return 2;
    }
    const auto validation = QJsonDocument::fromJson(file.readAll()).object();
    const auto keyData = validation.value("license_key").toObject();
    const auto key = keyData.value("key").toString();
    if (!validation.value("valid").toBool() || !keyData.value("test_mode").toBool()
        || keyData.value("activation_usage").toInt(-1) != 0 || keyData.value("activation_limit").toInt() != 3
        || key.isEmpty() || !License::directDistribution()) {
        qCritical("Use a valid, unused test-mode key with exactly three slots in a direct build."); return 2;
    }
    const auto metadata = validation.value("meta").toObject();
    const QJsonObject product{{"store_id", metadata.value("store_id")},
        {"product_id", metadata.value("product_id")}, {"variant_id", metadata.value("variant_id")}};
    const QUrl endpoint("https://api.lemonsqueezy.com/v1/licenses/");
    QTemporaryDir directory;
    if (!directory.isValid()) { qCritical("Cannot isolate test receipts."); return 2; }
    // Only this disposable test key protects these test receipts. No OS account
    // keychain or real Supporter activation is read or changed.
    const auto storageKey = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256);
    std::array<std::unique_ptr<License>, 4> devices;
    bool passed = true;
    const auto check = [&](bool result, const char* step) {
        qInfo("%s: %s", result ? "PASS" : "FAIL", step); passed &= result; return result;
    };
    try {
        for (int i = 0; i < 4; ++i) devices[i] = std::make_unique<License>(
            directory.filePath(QString::number(i)), product, endpoint, storageKey);
        for (int i = 0; i < 3 && passed; ++i)
            check(complete(*devices[i], devices[i]->activate(key)) && devices[i]->active(), "activate a device");
        if (passed) {
            License restored(directory.filePath("0"), product, endpoint, storageKey);
            check(complete(restored, restored.activate(key)) && restored.active()
                && restored.expiresAt() == devices[0]->expiresAt(), "reuse persisted activation without another slot");
        }
        if (passed) check(complete(*devices[3], devices[3]->activate(key)) && !devices[3]->active()
            && !devices[3]->recoveryNeeded() && !devices[3]->status().isEmpty(), "reject a fourth device");
        if (passed) check(complete(*devices[0], devices[0]->deactivate()) && !devices[0]->recoveryNeeded(), "release a slot");
        if (passed) check(complete(*devices[3], devices[3]->activate(key)) && devices[3]->active(), "reuse the released slot");
    } catch (const std::exception& error) {
        qCritical("Merchant test failed: %s", error.what()); passed = false;
    }
    bool cleaned = true;
    for (auto& device : devices) if (device) {
        if (device->active()) cleaned &= complete(*device, device->deactivate()) && !device->recoveryNeeded();
        else if (device->recoveryNeeded()) cleaned = false;
    }
    check(cleaned, "release all test activations");
    if (!cleaned) {
        directory.setAutoRemove(false);
        qCritical("Inspect the test license in the merchant dashboard before retrying. Receipts: %s", qPrintable(directory.path()));
    }
    return passed ? 0 : 1;
}
