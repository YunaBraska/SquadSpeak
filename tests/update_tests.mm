#include "app_updates.hpp"
#include <QTest>
#import <Foundation/Foundation.h>

class UpdateTests final : public QObject {
    Q_OBJECT
private slots:
    void disabledPreviewNeverStartsAnUpdater() {
        AppUpdates updates(false);
        QVERIFY(!updates.available());
        QVERIFY(!updates.check());
    }
    void configuredUpdaterStartsWithoutAutomaticInstallation() {
        AppUpdates updates(true);
        QVERIFY(updates.available());
        auto* defaults = [NSUserDefaults standardUserDefaults];
        QVERIFY([defaults boolForKey:@"SUEnableAutomaticChecks"]);
        QVERIFY(![defaults boolForKey:@"SUAutomaticallyUpdate"]);
        QVERIFY(![[[NSBundle mainBundle] objectForInfoDictionaryKey:@"SUAllowsAutomaticUpdates"] boolValue]);
    }
    void cleanupTestCase() {
        [[NSUserDefaults standardUserDefaults] removePersistentDomainForName:[[NSBundle mainBundle] bundleIdentifier]];
    }
};
QTEST_MAIN(UpdateTests)
#include "update_tests.moc"
