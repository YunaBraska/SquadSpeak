#pragma once

#include <QEventLoop>
#include <QTimer>
#include <utility>

// Connect before checking state so an already delivered event is not missed.
template<class Sender, class Signal, class Predicate>
bool waitForEvents(Sender* sender, Signal changed, Predicate ready, int timeout = 5000) {
    // Cleanup must still receive events after QtTest has marked a test failed.
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(sender, changed, &loop, [&] { if (ready()) loop.quit(); });
    if (ready()) return true;
    deadline.start(timeout);
    loop.exec();
    return deadline.isActive() && ready();
}

// Service network wakeups continuously, without qWait's sleeps between passes.
template<class Predicate>
bool waitForEvents(Predicate ready, int timeout = 5000) {
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.start(5);
    return waitForEvents(&poll, &QTimer::timeout, std::move(ready), timeout);
}
