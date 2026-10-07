#pragma once

#include <QString>

// Native notification presentation owns authorization, Focus and system sound policy.
namespace AppleNotifications {
void prepare();
void show(const QString& identifier, const QString& title, const QString& body, const QString& soundName);
}
