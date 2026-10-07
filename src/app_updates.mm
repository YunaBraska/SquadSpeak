#include "app_updates.hpp"
#include <QDebug>
#import <Sparkle/Sparkle.h>

static_assert(__has_feature(objc_arc), "AppUpdates requires Objective-C ARC");

AppUpdates::AppUpdates(bool enabled, QObject* parent) : QObject(parent) {
    if (!enabled || ![[NSBundle mainBundle] objectForInfoDictionaryKey:@"SUPublicEDKey"]) return;
    auto* controller = [[SPUStandardUpdaterController alloc]
        initWithStartingUpdater:NO updaterDelegate:nil userDriverDelegate:nil];
    // Enforce the product rule even when an older version saved other defaults.
    controller.updater.automaticallyDownloadsUpdates = NO;
    controller.updater.automaticallyChecksForUpdates = YES;
    NSError* error = nil;
    if (![controller.updater startUpdater:&error]) {
        qWarning() << "Update initialization:" << QString::fromNSString(error.localizedDescription);
        return;
    }
    controller_ = (__bridge_retained void*)controller;
}

AppUpdates::~AppUpdates() {
    if (controller_) CFBridgingRelease(controller_);
}

bool AppUpdates::check() {
    auto* controller = (__bridge SPUStandardUpdaterController*)controller_;
    if (!controller || !controller.updater.canCheckForUpdates) return false;
    [controller checkForUpdates:nil];
    return true;
}
