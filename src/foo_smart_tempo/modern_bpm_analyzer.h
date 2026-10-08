#pragma once

#include "ibpm_analyzer.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

/// Modern BPM engine using the Hodgkinson/Audacity-MIR primary analysis path.
/// Uses KissFFT for MIR spectral analysis; legacy Aubio tempo detection is removed.
class ModernBpmAnalyzer final : public IBpmAnalyzer {
public:
    struct RuntimeStats {
        uint64_t tempo_allocations = 0;
        uint64_t analyze_calls = 0;
        uint64_t hodgkinson_material_risk_candidates = 0;
        uint64_t hodgkinson_material_risk_review_holds = 0;
        uint64_t mir_primary_auto_candidates = 0;
        uint64_t mir_primary_review_holds = 0;
        uint64_t mir_primary_sparse_acapella_review_holds = 0;
        uint64_t mir_primary_runtime_applied = 0;
        uint64_t mir_policy_writer_overrides = 0;
        uint64_t mir_policy_review_hold_promotions = 0;
        uint64_t mir_policy_keep_current_micro_upgrades = 0;
        uint64_t mir_policy_review_holds = 0;
        uint64_t mir_policy_family_conflict_review_holds = 0;
        double decode_seconds = 0.0;
        double onset_seconds = 0.0;
        double total_seconds = 0.0;
    };

    ModernBpmAnalyzer(std::atomic<uint64_t>*   p_progressMillis = nullptr,
                      std::atomic<bool>*        p_pauseFlag      = nullptr,
                      std::condition_variable*  p_pauseCv        = nullptr,
                      std::mutex*               p_pauseMutex     = nullptr)
        : m_progressMillis(p_progressMillis)
        , m_pauseFlag(p_pauseFlag)
        , m_pauseCv(p_pauseCv)
        , m_pauseMutex(p_pauseMutex)
    {}

    double analyze(metadb_handle_ptr        track,
                   threaded_process_status& status,
                   abort_callback&          p_abort,
                   double                   priorMinBpm = 0.0,
                   double                   priorMaxBpm = 0.0,
                   const char*              trackIdentifier = nullptr,
                   const RoutingLogContext* routingContext = nullptr,
                   bool captureMeasuredCandidates = false) override;

    static void reset_runtime_stats() noexcept;
    static RuntimeStats query_runtime_stats() noexcept;

    [[nodiscard]] const Diagnostics& get_diagnostics() const noexcept override {
        return m_diag;
    }

private:
    void WaitWhilePaused(abort_callback& p_abort);

    std::atomic<uint64_t>*  m_progressMillis;
    std::atomic<bool>*       m_pauseFlag;
    std::condition_variable* m_pauseCv;
    std::mutex*              m_pauseMutex;
    Diagnostics              m_diag;
};
