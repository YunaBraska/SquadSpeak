#pragma once

class QQmlPropertyMap;

// Observe Apple accessibility settings for the lifetime of the supplied map.
void observeSystemAppearance(QQmlPropertyMap& appearance);
