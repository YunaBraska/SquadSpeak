#pragma once

#include <QString>

// Native banners respect notification authorization and Focus. Audible cues use
// the app's selected output device and local notification volume.
namespace AppleNotifications {
void prepare();
void show(const QString& identifier, const QString& title, const QString& body);
}
