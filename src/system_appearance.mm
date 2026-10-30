#include "system_appearance.hpp"
#include <QQmlPropertyMap>
#include <QPointer>

#ifdef Q_OS_MACOS
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif

void observeSystemAppearance(QQmlPropertyMap& appearance) {
    const QPointer<QQmlPropertyMap> target(&appearance);
    const auto refresh = [target] {
        if (!target) return;
#ifdef Q_OS_MACOS
        target->insert("reducedMotion", bool(NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion));
        target->insert("textScale", 1.0);
#else
        target->insert("reducedMotion", bool(UIAccessibilityIsReduceMotionEnabled()));
        target->insert("textScale", [UIFont preferredFontForTextStyle:UIFontTextStyleBody].pointSize / 17.0);
#endif
    };
    refresh();
#ifdef Q_OS_MACOS
    NSNotificationCenter* center = NSWorkspace.sharedWorkspace.notificationCenter;
    NSArray<NSNotificationName>* names = @[NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification];
#else
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    NSArray<NSNotificationName>* names = @[UIAccessibilityReduceMotionStatusDidChangeNotification, UIContentSizeCategoryDidChangeNotification];
#endif
    for (NSNotificationName name in names) {
        id observer = [center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification*) { refresh(); }];
        QObject::connect(&appearance, &QObject::destroyed, [center, observer] { [center removeObserver:observer]; });
    }
}
