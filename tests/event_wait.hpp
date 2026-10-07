#pragma once

#include <QTestEventLoop>
#include <QTimer>

// Connect before checking state so an already delivered event is not missed.
template<class Sender, class Signal, class Predicate>
bool waitForEvents(Sender* sender, Signal changed, Predicate ready, int timeout = 5000) {
    QTestEventLoop loop;
    QObject::connect(sender, changed, &loop, [&] { if (ready()) loop.exitLoop(); });
    if (ready()) return true;
    loop.enterLoopMSecs(timeout);
    return !loop.timeout() && ready();
}

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
