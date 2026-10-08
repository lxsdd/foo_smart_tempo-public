#ifndef __DPM_RESULT_DIALOG_H__
#define __DPM_RESULT_DIALOG_H__

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "foobar2000/SDK/foobar2000.h"
#include "foobar2000/helpers/foobar2000+atl.h"
#include "foobar2000/helpers/atl-misc.h"
#include "foobar2000/SDK/coreDarkMode.h"

#include <atlframe.h>
#include <atlctrls.h>

#include "resource.h"
#include "ibpm_analyzer.h"

bool show_direct_measured_bpm_candidate_dialog(
    metadb_handle_ptr track,
    std::vector<IBpmAnalyzer::MeasuredBpmCandidate> candidates,
    const pfc::string8& trackLabel,
    double currentResultBpm,
    bool hasCurrentResult);

class bpm_result_dialog : public CDialogImpl<bpm_result_dialog>, public CDialogResize<bpm_result_dialog>, private message_filter_impl_base
{
public:
    enum { IDD = IDD_BPM_RESULT_DIALOG };
    static constexpr UINT kMeasuredCandidateSelectedMessage = WM_APP + 122;
    static constexpr UINT kMeasuredCandidateAnalysisDoneMessage = WM_APP + 123;
    static constexpr UINT kMeasuredCandidateFollowSelectionMessage = WM_APP + 125;
    static constexpr int kModelColumnCount = 7;
    enum ResultColumn : int {
        ColumnArtist = 0,
        ColumnTitle = 1,
        ColumnGenre = 2,
        ColumnBpm = 3,
        ColumnConfidence = 4,
        ColumnMethod = 5,
        ColumnError = 6
    };

    BEGIN_MSG_MAP_EX(bpm_result_dialog)
        MSG_WM_INITDIALOG(OnInitDialog)
        MSG_WM_SIZE(OnSize)
        MSG_WM_TIMER(OnTimer)
        NOTIFY_HANDLER_EX(ID_BPM_RESULT_LIST, LVN_ITEMCHANGED, OnItemChanged)
        NOTIFY_HANDLER_EX(ID_BPM_RESULT_LIST, NM_DBLCLK, OnResultDoubleClick)
        NOTIFY_HANDLER_EX(ID_BPM_RESULT_LIST, LVN_COLUMNCLICK, OnColumnClick)
        NOTIFY_HANDLER_EX(ID_BPM_RESULT_LIST, HDN_ENDTRACKA, OnHeaderChanged)
        NOTIFY_HANDLER_EX(ID_BPM_RESULT_LIST, HDN_ENDTRACKW, OnHeaderChanged)
        NOTIFY_HANDLER_EX(ID_BPM_RESULT_LIST, HDN_ENDDRAG, OnHeaderChanged)
        COMMAND_HANDLER_EX(IDOK, BN_CLICKED, OnOK)
        COMMAND_HANDLER_EX(IDCANCEL, BN_CLICKED, OnCancel)
        COMMAND_HANDLER_EX(ID_DOUBLE_BPM_BUTTON, BN_CLICKED, OnDoubleBPMClicked)
        COMMAND_HANDLER_EX(ID_HALVE_BPM_BUTTON, BN_CLICKED, OnHalveBPMClicked)
        COMMAND_HANDLER_EX(ID_RESULT_MANUAL_BUTTON, BN_CLICKED, OnManualBpmClicked)
        COMMAND_HANDLER_EX(ID_RESULT_MEASURED_CANDIDATES_BUTTON, BN_CLICKED, OnMeasuredCandidatesClicked)
        COMMAND_HANDLER_EX(ID_RESULT_PREFERENCES_BUTTON, BN_CLICKED, OnPreferencesClicked)
        MESSAGE_HANDLER_EX(WM_DPICHANGED, OnDpiChanged)
        MESSAGE_HANDLER_EX(WM_APP + 121, OnLayoutChanged)
        MESSAGE_HANDLER_EX(kMeasuredCandidateSelectedMessage, OnMeasuredCandidateSelected)
        MESSAGE_HANDLER_EX(kMeasuredCandidateAnalysisDoneMessage, OnMeasuredCandidateAnalysisDone)
        MESSAGE_HANDLER_EX(kMeasuredCandidateFollowSelectionMessage, OnMeasuredCandidateFollowSelection)
        CHAIN_MSG_MAP(CDialogResize<bpm_result_dialog>)
        MSG_WM_CLOSE(OnClose)
    END_MSG_MAP()

    BEGIN_DLGRESIZE_MAP(bpm_result_dialog)
        DLGRESIZE_CONTROL(ID_BPM_RESULT_LIST, DLSZ_SIZE_X | DLSZ_SIZE_Y)
        DLGRESIZE_CONTROL(ID_RESULT_BPM_TAG, DLSZ_SIZE_X | DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(ID_RESULT_MANUAL_BUTTON, DLSZ_MOVE_X | DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(ID_RESULT_PREFERENCES_BUTTON, DLSZ_MOVE_X | DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(ID_DOUBLE_BPM_BUTTON, DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(ID_HALVE_BPM_BUTTON, DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(ID_RESULT_MEASURED_CANDIDATES_BUTTON, DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(IDOK, DLSZ_MOVE_X | DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(IDCANCEL, DLSZ_MOVE_X | DLSZ_MOVE_Y)
    END_DLGRESIZE_MAP()

    bpm_result_dialog(
        metadb_handle_list_cref p_tracks,
        const pfc::list_t<file_info_impl>& p_infos,
        const std::vector<double>& p_bpm_results,
        const std::vector<double>& p_confidence_results,
        const std::vector<pfc::string8>& p_genre_results,
        const std::vector<pfc::string8>& p_method_results,
        const std::vector<pfc::string8>& p_error_results,
        const std::vector<uint8_t>& p_suppress_write_results,
        const std::vector<IBpmAnalyzer::RoutingContextSnapshot>&
            p_routing_context_results,
        const std::vector<std::vector<IBpmAnalyzer::MeasuredBpmCandidate>>&
            p_measured_bpm_candidate_results,
        t_size p_processedTrackCount,
        double p_elapsedSeconds);

private:
    LRESULT OnInitDialog(CWindow wndFocus, LPARAM lInitParam);
    LRESULT OnOK(UINT uNotifyCode, int nID, CWindow wndCtl);
    LRESULT OnCancel(UINT uNotifyCode, int nID, CWindow wndCtl);
    LRESULT OnDoubleBPMClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
    LRESULT OnHalveBPMClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
    LRESULT OnManualBpmClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
    LRESULT OnMeasuredCandidatesClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
    LRESULT OnMeasuredCandidateSelected(UINT, WPARAM, LPARAM);
    LRESULT OnMeasuredCandidateAnalysisDone(UINT, WPARAM, LPARAM);
    LRESULT OnMeasuredCandidateFollowSelection(UINT, WPARAM, LPARAM);
    LRESULT OnPreferencesClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
    LRESULT OnItemChanged(LPNMHDR pnmh);
    LRESULT OnResultDoubleClick(LPNMHDR pnmh);
    LRESULT OnColumnClick(LPNMHDR pnmh);
    LRESULT OnHeaderChanged(LPNMHDR pnmh);
    LRESULT OnDpiChanged(UINT, WPARAM, LPARAM);
    void OnSize(UINT nType, CSize size);
    void OnTimer(UINT_PTR timerId);
    LRESULT OnLayoutChanged(UINT, WPARAM, LPARAM);

    void OnClose();
    void PostNcDestroy();
    bool pretranslate_message(MSG* p_msg);

    void InitTooltips();
    void AddTooltip(int controlID, const TCHAR* text);
    void EnableScaleBPMButtons();
    void EnableMeasuredCandidatesButton();
    void FollowMeasuredCandidateSelection(size_t modelIndex);
    void ClearMeasuredCandidateDialogSelection();
    void RefreshMeasuredCandidateDialog(size_t modelIndex);
    void StartMeasuredCandidateAnalysis(size_t modelIndex);
    void CloseMeasuredCandidateDialog();
    void ApplyMeasuredCandidateSelection(size_t modelIndex, size_t candidateIndex);
    void ScaleSelectionBPM(double p_factor);
    void UpdateResultColumns();
    void UpdateBpmTagLabel();
    void SaveListLayout();
    void LoadListLayout();
    void SaveWindowPlacement();
    void LoadWindowPlacement();
    void BuildTextCache();
    void BuildVisibleColumns();
    int ViewToModelColumn(int viewColumn) const;
    int ModelToViewColumn(int modelColumn) const;
    int VisibleColumnCount() const;
    int ComputeColumnContentWidth(CListViewCtrl& listView, int column, const wchar_t* header) const;
    void InvalidateColumnContentWidthCache();
    void RebuildColumnContentWidthCache(CListViewCtrl& listView);

    pfc::string8 get_artist(size_t index) const;
    pfc::string8 get_title(size_t index) const;
    pfc::string8 get_genre(size_t index) const;
    pfc::string8 get_method(size_t index) const;
    pfc::string8 get_error(size_t index) const;
    int compare_rows(size_t left, size_t right, int column) const;
    void ApplySorting();
    static int CALLBACK SortThunk(LPARAM left, LPARAM right, LPARAM sortParam);

    metadb_handle_list m_tracks;
    pfc::list_t<file_info_impl> m_infos;
    std::vector<double> m_bpm_results;
    std::vector<double> m_confidence_results;
    std::vector<pfc::string8> m_genre_results;
    std::vector<pfc::string8> m_method_results;
    std::vector<pfc::string8> m_error_results;
    std::vector<uint8_t> m_suppress_write_results;
    std::vector<IBpmAnalyzer::RoutingContextSnapshot>
        m_routing_context_results;
    std::vector<std::vector<IBpmAnalyzer::MeasuredBpmCandidate>>
        m_measured_bpm_candidate_results;
    std::vector<uint8_t> m_measuredCandidateAnalysisAttempted;
    std::vector<pfc::string8> m_measuredCandidateAnalysisErrors;
    std::vector<std::wstring> m_artistWide;
    std::vector<std::wstring> m_titleWide;
    std::vector<std::wstring> m_genreWide;
    std::vector<std::wstring> m_methodWide;
    std::vector<std::wstring> m_errorWide;
    t_size m_processedTrackCount = 0;
    double m_elapsedSeconds = 0.0;
    std::vector<int> m_columnWidths;
    std::vector<int> m_columnContentWidthCache;
    std::vector<int> m_visibleColumns;
    std::array<int, kModelColumnCount> m_modelToView{};
    bool m_showConfidenceColumn = true;
    fb2k::CCoreDarkModeHooks m_dark;
    CToolTipCtrl m_tooltip;
    bool m_hasCustomLayout = false;
    bool m_columnContentWidthCacheValid = false;
    bool m_layoutUpdatePending = false;
    HWND m_measuredCandidateDialog = nullptr;
    size_t m_measuredCandidateAnalysisInFlight = static_cast<size_t>(-1);
    static constexpr UINT_PTR kMeasuredCandidateSelectionTimer = 2;
    static constexpr UINT kMeasuredCandidateSelectionDelayMs = 250;
    SIZE m_minTrackSize{ 395, 242 };
    int m_sortColumn = -1;
    bool m_sortAscending = true;
};

#endif // __DPM_RESULT_DIALOG_H__
