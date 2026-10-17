#include "apple_notifications.hpp"

#include <QDebug>
#import <UserNotifications/UserNotifications.h>

@interface ChatNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation ChatNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
      willPresentNotification:(UNNotification*)notification
        withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completion {
    (void)center;
    (void)notification;
    completion(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}
@end

namespace {
UNUserNotificationCenter* center() {
    static ChatNotificationDelegate* delegate = [[ChatNotificationDelegate alloc] init];
    auto* result = [UNUserNotificationCenter currentNotificationCenter];
    result.delegate = delegate;
    return result;
}
}

void AppleNotifications::prepare() {
    auto* notifications = center();
    [notifications getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
        if (settings.authorizationStatus != UNAuthorizationStatusNotDetermined) return;
        [notifications requestAuthorizationWithOptions:UNAuthorizationOptionAlert
            completionHandler:^(BOOL granted, NSError* error) {
                (void)granted;
                if (error) qWarning() << "Notification authorization:" << QString::fromNSString(error.localizedDescription);
            }];
    }];
}

void AppleNotifications::show(const QString& identifier, const QString& title, const QString& body) {
    auto* content = [[UNMutableNotificationContent alloc] init];
    content.title = title.toNSString();
    content.body = body.toNSString();
    auto* request = [UNNotificationRequest requestWithIdentifier:identifier.toNSString() content:content trigger:nil];
    [center() addNotificationRequest:request withCompletionHandler:^(NSError* error) {
        if (error) qWarning() << "Chat notification:" << QString::fromNSString(error.localizedDescription);
    }];
}
