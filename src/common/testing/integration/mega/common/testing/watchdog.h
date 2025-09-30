#pragma once

#include <mega/common/logger_forward.h>
#include <mega/common/task_executor.h>

#include <chrono>

namespace mega
{
namespace common
{
namespace testing
{

class Watchdog
{
    Logger& mLogger;
    TaskExecutor mExecutor;
    Task mTask;

public:
    Watchdog(Logger& logger);

    // Arm the watchdog to expire at some point in time.
    void arm(std::chrono::steady_clock::time_point when);

    // Arm the watchdog to expire at some point in the future.
    template<typename Rep, typename Period>
    void arm(std::chrono::duration<Rep, Period> when)
    {
        arm(std::chrono::steady_clock::now() + when);
    }

    // Disarm the watchdog.
    void disarm();
}; // Watchdog

class ScopedWatch
{
    Watchdog* mWatchdog;

public:
    ScopedWatch(Watchdog& watchdog, std::chrono::steady_clock::time_point when):
        mWatchdog(&watchdog)
    {
        mWatchdog->arm(when);
    }

    template<typename Rep, typename Period>
    ScopedWatch(Watchdog& watchdog, std::chrono::duration<Rep, Period> when):
        ScopedWatch(watchdog, std::chrono::steady_clock::now() + when)
    {}

    ScopedWatch(const ScopedWatch& other) = delete;

    ~ScopedWatch()
    {
        if (mWatchdog)
            mWatchdog->disarm();
    }

    ScopedWatch& operator=(const ScopedWatch& rhs) = delete;

    void release()
    {
        mWatchdog = nullptr;
    }
}; // ScopedWatch

} // testing
} // common
} // mega
