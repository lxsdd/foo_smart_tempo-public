#ifndef __BPM_AUTO_ANALYSIS_THREAD_H__
#define __BPM_AUTO_ANALYSIS_THREAD_H__

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "foobar2000/SDK/foobar2000.h"
#include "foobar2000/helpers/foobar2000+atl.h"
#include "foobar2000/SDK/coreDarkMode.h"
#include <atlctrls.h>

#include "resource.h"
#include "ibpm_analyzer.h"

class bpm_auto_analysis_thread : public CDialogImpl<bpm_auto_analysis_thread>, private message_filter_impl_base
{
public:
    enum { IDD = IDD_BPM_PROGRESS_DIALOG };

    bpm_auto_analysis_thread(
        metadb_handle_list_cref p_tracks,
        bool p_measuredCandidateInspectionMode = false);
    bool start();

    BEGIN_MSG_MAP_EX(bpm_auto_analysis_thread)
        MSG_WM_INITDIALOG(OnInitDialog)
        MSG_WM_TIMER(OnTimer)
        COMMAND_HANDLER_EX(ID_BPM_PROGRESS_PAUSE, BN_CLICKED, OnPauseClicked)
        COMMAND_HANDLER_EX(ID_BPM_PROGRESS_ABORT, BN_CLICKED, OnAbortClicked)
        COMMAND_HANDLER_EX(IDCANCEL, BN_CLICKED, OnAbortClicked)
        MSG_WM_CLOSE(OnClose)
        MESSAGE_HANDLER_EX(WM_DPICHANGED, OnDpiChanged)
        MESSAGE_HANDLER_EX(WM_APP + 120, OnWorkerDone)
    END_MSG_MAP()

private:
    BOOL OnInitDialog(CWindow wndFocus, LPARAM lInitParam);
    void OnTimer(UINT_PTR id);
    LRESULT OnPauseClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
    LRESULT OnAbortClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
    void OnClose();
    LRESULT OnDpiChanged(UINT, WPARAM, LPARAM);
    LRESULT OnWorkerDone(UINT, WPARAM, LPARAM);
    void PostNcDestroy();
    bool pretranslate_message(MSG* p_msg);

    void WorkerMain();
    void NotifyWorkerDoneMainThread(bool wasAborted);
    void RequestAbort();
    void UpdateStatusLine();
    void SetProgressInfo(const char* text, const char* tooltipText);
    void InitLayoutMetrics();
    void ApplyDynamicLayout(size_t activeLineCount, bool force = false);
    void InitTooltips();
    void AddTooltip(int controlID, const TCHAR* text);
    void UpdateTooltip(int controlID, const TCHAR* text);
    int MeasureDialogTextLineHeight() const;
    static pfc::string8 FormatClock(double seconds);
    static uint64_t SteadyMillis();
    void RecordTrackWallTime(uint64_t elapsedMillis);
    double EstimateTrackP95Seconds() const;
    void ShiftActiveWorkerStarts(uint64_t pauseMillis);

    struct layout_metrics {
        int marginLeft = 0;
        int marginRight = 0;
        int marginTop = 0;
        int marginBottom = 0;
        int fileToBarGap = 0;
        int barToBottomRowGap = 0;
        int barHeight = 0;
        int infoHeight = 0;
        int buttonWidth = 0;
        int buttonHeight = 0;
        int buttonGap = 0;
        int infoToButtonsGap = 0;
        int minClientHeight = 0;
    } m_layout;

    pfc::list_t<metadb_handle_ptr> m_tracks;
    pfc::list_t<metadb_handle_ptr> m_unmatched_tracks;
    pfc::list_t<file_info_impl> m_infos;
    std::vector<double> m_bpm_results;
    std::vector<double> m_confidence_results;
    std::vector<pfc::string8> m_routed_genre_results;
    std::vector<pfc::string8> m_method_results;
    std::vector<pfc::string8> m_error_results;
    std::vector<uint8_t> m_suppress_write_results;
    std::vector<IBpmAnalyzer::RoutingContextSnapshot>
        m_routing_context_results;
    std::vector<std::vector<IBpmAnalyzer::MeasuredBpmCandidate>>
        m_measured_bpm_candidate_results;

    std::atomic<t_size> m_nextIndex{ 0 };
    std::atomic<t_size> m_doneCount{ 0 };
    std::atomic<uint64_t> m_doneAudioMillis{ 0 };
    std::atomic<uint64_t> m_completedAudioMillis{ 0 };
    std::atomic<uint64_t> m_totalAudioMillis{ 1000 };
    std::atomic<bool> m_pauseRequested{ false };
    std::atomic<bool> m_abortRequested{ false };

    struct alignas(64) worker_live_slot {
        static constexpr size_t kPadBytes =
            (sizeof(std::atomic<t_size>) + sizeof(std::atomic<uint64_t>) < 64)
                ? (64 - sizeof(std::atomic<t_size>) - sizeof(std::atomic<uint64_t>))
                : 1;
        std::atomic<t_size> index{ (t_size)-1 };
        std::atomic<uint64_t> started_at_millis{ 0 };
        std::array<std::byte, kPadBytes> padding{};
    };
    std::unique_ptr<worker_live_slot[]> m_workerSlots;
    size_t m_workerSlotCount = 0;
    std::atomic<bool> m_workerSlotsReady{ false };
    std::atomic<size_t> m_workerCount{ 0 };
    static constexpr size_t kTrackDurationBinCount = 12;
    std::array<std::atomic<uint64_t>, kTrackDurationBinCount>
        m_trackDurationHistogram{};

    std::mutex m_unmatchedTracksMutex;
    std::condition_variable m_pauseCv;
    std::mutex m_pauseMutex;

    foobar2000_io::abort_callback_impl m_abort;
    std::thread m_workerThread;

    std::chrono::steady_clock::time_point m_startedAt{};
    std::chrono::steady_clock::time_point m_pauseBegan{};
    double m_pausedSeconds = 0.0;
    bool m_pauseLatched = false;
    bool m_wasAborted = false;
    t_size m_processedTrackCount = 0;
    double m_analysisElapsedSeconds = 0.0;
    bool m_measuredCandidateInspectionMode = false;
    double m_etaDisplayedRemaining = 0.0;
    double m_smoothedTotalTimeSeconds = 0.0;
    bool m_etaUiInitialized = false;
    t_size m_etaLastProjectionDone = 0;
    double m_etaLastProjectionElapsed = 0.0;
    std::chrono::steady_clock::time_point m_etaLastUiUpdate{};
    size_t m_lastLayoutLines = static_cast<size_t>(-1);
    int m_lastLayoutClientWidth = -1;
    int m_lastLayoutClientHeight = -1;
    size_t m_lastActiveCount = static_cast<size_t>(-1);
    pfc::string8 m_lastFileLine;
    pfc::string8 m_lastInfoLine;
    std::wstring m_progressInfoTooltip;

    fb2k::CCoreDarkModeHooks m_dark;
    CToolTipCtrl m_tooltip;
};

#endif // __BPM_AUTO_ANALYSIS_THREAD_H__
