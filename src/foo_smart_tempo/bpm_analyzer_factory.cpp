#include "stdafx.h"
#include "bpm_analyzer_factory.h"
#include "modern_bpm_analyzer.h"

std::unique_ptr<IBpmAnalyzer> BpmAnalyzerFactory::create(
    std::atomic<uint64_t>*   p_progressMillis,
    std::atomic<bool>*        p_pauseFlag,
    std::condition_variable*  p_pauseCv,
    std::mutex*               p_pauseMutex)
{
    return std::make_unique<ModernBpmAnalyzer>(
        p_progressMillis, p_pauseFlag, p_pauseCv, p_pauseMutex);
}
