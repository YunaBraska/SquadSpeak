#pragma once

#include <QTestEventLoop>
#include <QTimer>

// Service network wakeups continuously, without qWait's sleeps between passes.
template<class Predicate>
bool waitForEvents(Predicate ready, int timeout = 5000) {
    if (ready()) return true;
    QTestEventLoop loop;
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (ready()) loop.exitLoop(); });
    poll.start(5);
    loop.enterLoopMSecs(timeout);
    return !loop.timeout() && ready();
}
