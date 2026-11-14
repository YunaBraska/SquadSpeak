#pragma once
#include <functional>

class LocalChannel;
class QMenu;
class RadioPlayer;
class VoiceSession;

// Adds live voice and radio controls to a platform tray menu.
void addTrayQuickActions(QMenu& menu, VoiceSession& session, LocalChannel& channel, RadioPlayer& radio,
                         const std::function<void()>& leaveRequested = {});
