#pragma once

#include "ibpm_analyzer.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

/// Thread-safe factory that creates the active IBpmAnalyzer implementation.
struct BpmAnalyzerFactory {
    /// Creates a fresh analyzer instance for use on a background worker thread.
    /// Safe to call from any thread; runtime preferences are read by the analyzer.
    static std::unique_ptr<IBpmAnalyzer> create(
        std::atomic<uint64_t>*   p_progressMillis = nullptr,
        std::atomic<bool>*        p_pauseFlag      = nullptr,
        std::condition_variable*  p_pauseCv        = nullptr,
        std::mutex*               p_pauseMutex     = nullptr);
};
