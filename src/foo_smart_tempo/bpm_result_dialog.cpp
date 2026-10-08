#include "stdafx.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "bpm_result_dialog.h"
#include "preferences.h"
#include "format_bpm.h"
#include "foo_smart_tempo.h"
#include "file_info_filter_bpm.h"
#include "bpm_manual_dialog.h"
#include "bpm_analyzer_factory.h"
#include "manual_metronome.h"
#include "guid.h"
#include "tag_write_dispatch.h"
#include "ui_scaling.h"



namespace {
    static const GUID guid_bpm_result_columns_order = { 0x4163ac50, 0x7f47, 0x4272, { 0x92, 0x75, 0x70, 0x3b, 0xf5, 0x35, 0xca, 0x6a } };
    static const GUID guid_bpm_result_columns_width = { 0x41f494d1, 0x717c, 0x44d8, { 0x9b, 0xa2, 0x3e, 0x9d, 0xdf, 0xc1, 0x82, 0x4f } };
    static const GUID guid_bpm_result_window_w = { 0x810805e7, 0xe6b3, 0x462f, { 0x87, 0xe9, 0x41, 0xfe, 0xcd, 0x8d, 0xb3, 0xf8 } };
    static const GUID guid_bpm_result_window_h = { 0x7bfa60cc, 0x7ead, 0x435e, { 0x83, 0xfc, 0xd5, 0xcf, 0x68, 0xba, 0xdb, 0x6b } };
    static const GUID guid_bpm_result_window_x = { 0x3adc59bb, 0xf822, 0x4e37, { 0x9c, 0xfd, 0xd4, 0xc0, 0x73, 0x79, 0x3f, 0xbc } };
    static const GUID guid_bpm_result_window_y = { 0x0f8e86cb, 0x0492, 0x45d4, { 0xb5, 0x0d, 0x8e, 0x5e, 0xac, 0xd7, 0xeb, 0xec } };
    static const GUID guid_bpm_result_window_show = { 0x692d265a, 0xd484, 0x4062, { 0x9f, 0x3c, 0x0d, 0x82, 0xf9, 0x6b, 0x16, 0x9c } };

    cfg_string bpm_result_columns_order(guid_bpm_result_columns_order, "0,1,2,3,4,5,6");
    cfg_string bpm_result_columns_width(guid_bpm_result_columns_width, "18,24,16,9,10,11,12");
    cfg_int bpm_result_window_width(guid_bpm_result_window_w, 395);
    cfg_int bpm_result_window_height(guid_bpm_result_window_h, 242);
    cfg_int bpm_result_window_x(guid_bpm_result_window_x, INT_MIN);
    cfg_int bpm_result_window_y(guid_bpm_result_window_y, INT_MIN);
    cfg_int bpm_result_window_show(guid_bpm_result_window_show, SW_SHOWNORMAL);
    HWND g_activeMeasuredCandidateDialog = nullptr;
    HWND g_directMeasuredCandidateDialog = nullptr;

    void focus_measured_candidate_list(HWND dialog) {
        if (!::IsWindow(dialog)) return;
        const HWND list = ::GetDlgItem(dialog, IDC_MEASURED_CANDIDATE_LIST);
        if (::IsWindow(list) && ::IsWindowEnabled(list)) {
            ::SetFocus(list);
        }
    }
    constexpr int kDefaultColumnPercents[bpm_result_dialog::kModelColumnCount] = {18, 24, 16, 9, 10, 11, 12};
    constexpr int kBaseColumnWidths[bpm_result_dialog::kModelColumnCount] = {130, 190, 150, 70, 90, 90, 180};
    constexpr int kMinimumColumnWidths[bpm_result_dialog::kModelColumnCount] = {90, 120, 110, 60, 80, 80, 120};
    constexpr const wchar_t* kColumnHeaders[bpm_result_dialog::kModelColumnCount] = {
        L"Artist", L"Title", L"Genre", L"BPM", L"Confidence", L"Method", L"Error"};
    constexpr int kShrinkPriority[bpm_result_dialog::kModelColumnCount] = {
        bpm_result_dialog::ColumnError,
        bpm_result_dialog::ColumnTitle,
        bpm_result_dialog::ColumnGenre,
        bpm_result_dialog::ColumnArtist,
        bpm_result_dialog::ColumnMethod,
        bpm_result_dialog::ColumnConfidence,
        bpm_result_dialog::ColumnBpm};

    std::vector<int> parseCSVInts(const char* s) {
        std::vector<int> out;
        if (s == nullptr) return out;
        std::stringstream ss(s);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token.empty()) continue;
            out.push_back(std::atoi(token.c_str()));
        }
        return out;
    }

    pfc::string8 toCSV(const std::vector<int>& values) {
        pfc::string_formatter out;
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) out << ",";
            out << values[i];
        }
        return out;
    }

    void remove_resolved_tracks_from_review_playlist(
        metadb_handle_list resolvedTracks, pfc::string8 playlistName) {
        if (resolvedTracks.get_count() == 0) return;
        if (playlistName.is_empty()) {
            playlistName = "Smart Tempo: Needs BPM Review";
        }

        static_api_ptr_t<playlist_manager> playlistMan;
        const t_size playlist =
            playlistMan->find_playlist(playlistName.get_ptr());
        if (playlist == SIZE_MAX) return;

        resolvedTracks.sort_by_pointer_remove_duplicates();
        metadb_handle_list existing;
        playlistMan->playlist_get_all_items(playlist, existing);
        bit_array_bittable removeMask(existing.get_count());
        t_size removeCount = 0;
        for (t_size index = 0; index < existing.get_count(); ++index) {
            if (resolvedTracks.bsearch_by_pointer(existing[index]) != SIZE_MAX) {
                removeMask.set(index, true);
                ++removeCount;
            }
        }
        if (removeCount == 0) return;

        const bool removed =
            playlistMan->playlist_remove_items(playlist, removeMask);
        FB2K_console_formatter()
            << "foo_smart_tempo: [Review Playlist] manual resolutions="
            << resolvedTracks.get_count() << ", removed="
            << (removed ? removeCount : 0)
            << ", removal_blocked=" << (removed ? 0 : 1)
            << ", playlist=\"" << playlistName.get_ptr() << "\"";
    }


    int compare_text_natural_ci_w(const std::wstring& a, const std::wstring& b) {
        const int caseInsensitive = CompareStringOrdinal(
            a.c_str(), -1, b.c_str(), -1, TRUE);
        if (caseInsensitive == CSTR_LESS_THAN) return -1;
        if (caseInsensitive == CSTR_GREATER_THAN) return 1;

        const int exact = CompareStringOrdinal(
            a.c_str(), -1, b.c_str(), -1, FALSE);
        if (exact == CSTR_LESS_THAN) return -1;
        if (exact == CSTR_GREATER_THAN) return 1;
        return 0;
    }

    using smart_tempo::ui::scale_for_window_dpi;

    struct measured_bpm_candidate_selection {
        size_t model_index = static_cast<size_t>(-1);
        size_t candidate_index = static_cast<size_t>(-1);
    };

    struct measured_bpm_candidate_reload {
        const std::vector<IBpmAnalyzer::MeasuredBpmCandidate>* candidates = nullptr;
        const char* track_label = nullptr;
        metadb_handle_ptr track;
        size_t model_index = static_cast<size_t>(-1);
        double current_result_bpm = 0.0;
        bool has_current_result = false;
        bool selection_available = true;
        bool analysis_attempted = false;
        bool analysis_in_progress = false;
        bool analysis_queued = false;
        const char* analysis_error = nullptr;
    };

    struct measured_bpm_candidate_analysis_result {
        std::vector<IBpmAnalyzer::MeasuredBpmCandidate> candidates;
        pfc::string8 error;
        size_t model_index = static_cast<size_t>(-1);
    };

    constexpr UINT_PTR kCandidateListSubclassId = 0x4653544c;

    class measured_bpm_candidate_dialog;
    LRESULT CALLBACK candidate_list_subclass(
        HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    class measured_bpm_candidate_dialog
        : public CDialogImpl<measured_bpm_candidate_dialog>,
          public CDialogResize<measured_bpm_candidate_dialog>,
          private play_callback_impl_base {
    public:
        enum { IDD = IDD_BPM_MEASURED_CANDIDATE_DIALOG };
        static constexpr UINT kReloadTrackMessage = WM_APP + 124;

        measured_bpm_candidate_dialog(
            std::vector<IBpmAnalyzer::MeasuredBpmCandidate> candidates,
            const pfc::string8& trackLabel, HWND resultWindow,
            size_t modelIndex, double currentResultBpm,
            bool hasCurrentResult,
            metadb_handle_ptr boundTrack = {})
            : m_candidates(std::move(candidates)), m_trackLabel(trackLabel),
              m_resultWindow(resultWindow), m_modelIndex(modelIndex),
              m_currentResultBpm(currentResultBpm),
              m_hasCurrentResult(hasCurrentResult),
              m_boundTrack(std::move(boundTrack)),
              m_directTrack(m_resultWindow == nullptr ? m_boundTrack
                                                      : metadb_handle_ptr{}) {
            m_analysisAttempted =
                m_directTrack.is_valid() || !m_candidates.empty();
        }

        BEGIN_MSG_MAP_EX(measured_bpm_candidate_dialog)
            MSG_WM_INITDIALOG(OnInitDialog)
            MSG_WM_TIMER(OnTimer)
            MSG_WM_HSCROLL(OnHScroll)
            MSG_WM_DESTROY(OnDestroy)
            COMMAND_HANDLER_EX(IDOK, BN_CLICKED, OnOK)
            COMMAND_HANDLER_EX(IDCANCEL, BN_CLICKED, OnCancel)
            COMMAND_HANDLER_EX(IDC_METRONOME_STOP, BN_CLICKED, OnMetronomeStop)
            COMMAND_HANDLER_EX(IDC_METRONOME_SYNC, BN_PUSHED, OnMetronomeSync)
            COMMAND_HANDLER_EX(IDC_METRONOME_NUDGE_EARLIER, BN_CLICKED, OnMetronomeNudgeEarlier)
            COMMAND_HANDLER_EX(IDC_METRONOME_NUDGE_LATER, BN_CLICKED, OnMetronomeNudgeLater)
            COMMAND_HANDLER_EX(IDC_METRONOME_METER, CBN_SELCHANGE, OnMetronomeMeterChanged)
            MSG_WM_CLOSE(OnClose)
            NOTIFY_HANDLER_EX(IDC_MEASURED_CANDIDATE_LIST, LVN_ITEMCHANGED, OnItemChanged)
            NOTIFY_HANDLER_EX(IDC_MEASURED_CANDIDATE_LIST, LVN_COLUMNCLICK, OnColumnClick)
            NOTIFY_HANDLER_EX(IDC_MEASURED_CANDIDATE_LIST, NM_CLICK, OnCandidateClickFallback)
            NOTIFY_HANDLER_EX(IDC_MEASURED_CANDIDATE_LIST, NM_DBLCLK, OnDoubleClick)
            NOTIFY_HANDLER_EX(IDC_MEASURED_CANDIDATE_LIST, HDN_ENDTRACKA, OnHeaderChanged)
            NOTIFY_HANDLER_EX(IDC_MEASURED_CANDIDATE_LIST, HDN_ENDTRACKW, OnHeaderChanged)
            MESSAGE_HANDLER_EX(kReloadTrackMessage, OnReloadTrack)
            CHAIN_MSG_MAP(CDialogResize<measured_bpm_candidate_dialog>)
        END_MSG_MAP()

        BEGIN_DLGRESIZE_MAP(measured_bpm_candidate_dialog)
            DLGRESIZE_CONTROL(IDC_MEASURED_CANDIDATE_HINT, DLSZ_SIZE_X)
            DLGRESIZE_CONTROL(IDC_MEASURED_CANDIDATE_LIST, DLSZ_SIZE_X | DLSZ_SIZE_Y)
            DLGRESIZE_CONTROL(IDC_METRONOME_GROUP, DLSZ_MOVE_Y | DLSZ_SIZE_X)
            DLGRESIZE_CONTROL(IDC_METRONOME_STOP, DLSZ_MOVE_Y)
            DLGRESIZE_CONTROL(IDC_METRONOME_SYNC, DLSZ_MOVE_Y)
            DLGRESIZE_CONTROL(IDC_METRONOME_NUDGE_EARLIER, DLSZ_MOVE_Y)
            DLGRESIZE_CONTROL(IDC_METRONOME_NUDGE_LATER, DLSZ_MOVE_Y)
            DLGRESIZE_CONTROL(IDC_METRONOME_METER, DLSZ_MOVE_X | DLSZ_MOVE_Y)
            DLGRESIZE_CONTROL(IDC_METRONOME_VOLUME, DLSZ_MOVE_X | DLSZ_MOVE_Y)
            DLGRESIZE_CONTROL(IDC_METRONOME_STATUS, DLSZ_MOVE_Y | DLSZ_SIZE_X)
            DLGRESIZE_CONTROL(IDC_MEASURED_CANDIDATE_FOOTNOTE, DLSZ_MOVE_Y)
            DLGRESIZE_CONTROL(IDOK, DLSZ_MOVE_X | DLSZ_MOVE_Y)
            DLGRESIZE_CONTROL(IDCANCEL, DLSZ_MOVE_X | DLSZ_MOVE_Y)
        END_DLGRESIZE_MAP()

    private:
        friend LRESULT CALLBACK candidate_list_subclass(
            HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

        enum Column : int {
            ColumnRank = 0,
            ColumnBpm,
            ColumnEngineResult,
            ColumnEvidence,
            ColumnSupport,
            ColumnWinnerSupport,
            ColumnLocalExact,
            ColumnAliases,
            ColumnSources,
            ColumnCount
        };

        enum class EngineRole : uint8_t {
            None,
            Final,
            Alternative1,
            Alternative2,
            Hold1,
            Hold2,
        };

        static constexpr UINT_PTR kMetronomeTimer = 1;
        static constexpr UINT kMetronomeTimerIntervalMs = 2;
        static constexpr unsigned kMetronomeStatusRefreshTicks = 125;

        static int meter_from_combo(int selection) {
            constexpr int meters[] = {1, 2, 3, 4, 6};
            return selection >= 0 && selection < 5 ? meters[selection] : 4;
        }

        static int combo_from_meter(int meter) {
            constexpr int meters[] = {1, 2, 3, 4, 6};
            for (int index = 0; index < 5; ++index) {
                if (meters[index] == meter) return index;
            }
            return 3;
        }

        BOOL OnInitDialog(CWindow, LPARAM) {
            m_dark.AddDialogWithControls(m_hWnd);
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            list.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            constexpr const wchar_t* headers[ColumnCount] = {
                L"Rank", L"BPM", L"Engine", L"Evidence", L"Segments", L"Winners",
                L"Exact score", L"Alias evidence", L"Measured source"};
            constexpr int widths[ColumnCount] = {45, 72, 72, 72, 72, 70, 76, 170, 150};
            for (int column = 0; column < ColumnCount; ++column) {
                list.InsertColumn(column, headers[column], LVCFMT_LEFT,
                                  scale_for_window_dpi(m_hWnd, widths[column]));
            }
            ::SetWindowSubclass(
                list, candidate_list_subclass, kCandidateListSubclassId,
                reinterpret_cast<DWORD_PTR>(this));
            InitHeaderTooltips();
            InitMetronomeControls();
            if (::IsWindow(m_resultWindow)) {
                uSetDlgItemText(m_hWnd, IDOK, "Use in results");
                uSetDlgItemText(
                    m_hWnd, IDC_MEASURED_CANDIDATE_FOOTNOTE,
                    "The selected measured BPM is staged in Analysis Results. "
                    "Confirm Results to write tags.");
            } else {
                uSetDlgItemText(m_hWnd, IDOK, "Write selected BPM");
                uSetDlgItemText(
                    m_hWnd, IDC_MEASURED_CANDIDATE_FOOTNOTE,
                    "The selected measured BPM is written directly to this "
                    "track when you confirm.");
            }

            fb2k::std_api_get<play_callback_manager>()->register_callback(
                this,
                play_callback_impl_base::flag_on_playback_new_track |
                    play_callback_impl_base::flag_on_playback_stop,
                false);
            m_playbackCallbackRegistered = true;
            UpdatePlaybackMismatch();

            PopulateCandidateList();

            GetDlgItem(IDOK).EnableWindow(FALSE);
            DlgResize_Init(true, true, WS_THICKFRAME | WS_CLIPCHILDREN);
            if (!m_candidates.empty()) {
                SelectInitialCandidate();
                list.SetFocus();
                return FALSE;
            }
            return TRUE;
        }

        void CueCandidateFromPointer(HWND source, LPARAM coordinates) {
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            if (source != list.m_hWnd) return;
            // Keep Space bound to the selected candidate immediately after a
            // pointer cue. DefSubclassProc would normally focus the ListView
            // later in the same mouse message, but the transport edge runs
            // synchronously before that default processing.
            list.SetFocus();
            LVHITTESTINFO hit{};
            hit.pt.x = GET_X_LPARAM(coordinates);
            hit.pt.y = GET_Y_LPARAM(coordinates);
            const int row = ListView_SubItemHitTest(list, &hit);
            if (row < 0) return;
            CueCandidateRow(list, row, true);
        }

        void PopulateCandidateList() {
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            list.DeleteAllItems();
            BuildEngineRoles();
            for (size_t index = 0; index < m_candidates.size(); ++index) {
                const auto& candidate = m_candidates[index];
                pfc::string_formatter rank;
                if (candidate.evidence_rank > 0) {
                    rank << (uint64_t)candidate.evidence_rank;
                } else {
                    rank << "extra";
                }
                pfc::stringcvt::string_wide_from_utf8 rankW(rank);
                const int row = list.InsertItem((int)index, rankW);
                list.SetItemData(row, (DWORD_PTR)index);

                format_bpm bpm(candidate.bpm, get_write_precision());
                pfc::stringcvt::string_wide_from_utf8 bpmW((const char*)bpm);
                list.SetItemText(row, ColumnBpm, bpmW);

                if (const wchar_t* role = EngineRoleText(index);
                    role[0] != L'\0') {
                    list.SetItemText(row, ColumnEngineResult, role);
                }

                pfc::string_formatter evidence;
                evidence << format_float_locale(candidate.evidence_score, 3);
                pfc::stringcvt::string_wide_from_utf8 evidenceW(evidence);
                list.SetItemText(row, ColumnEvidence, evidenceW);

                pfc::string_formatter support;
                support << format_float_locale(candidate.support * 100.0, 1) << "%";
                pfc::stringcvt::string_wide_from_utf8 supportW(support);
                list.SetItemText(row, ColumnSupport, supportW);

                pfc::string_formatter winnerSupport;
                winnerSupport << format_float_locale(candidate.winner_support * 100.0, 1) << "%";
                pfc::stringcvt::string_wide_from_utf8 winnerSupportW(winnerSupport);
                list.SetItemText(row, ColumnWinnerSupport, winnerSupportW);

                pfc::string_formatter exact;
                exact << format_float_locale(candidate.local_exact_score, 3);
                pfc::stringcvt::string_wide_from_utf8 exactW(exact);
                list.SetItemText(row, ColumnLocalExact, exactW);

                pfc::stringcvt::string_wide_from_utf8 aliasesW(candidate.alias_classes);
                pfc::stringcvt::string_wide_from_utf8 sourcesW(candidate.sources);
                list.SetItemText(row, ColumnAliases, aliasesW);
                list.SetItemText(row, ColumnSources, sourcesW);
            }

            // Present the evidence board in its native order by default.
            // Independent unranked witnesses follow all numbered ranks.
            ListView_SortItems(list, SortThunk, (LPARAM)this);

            pfc::string_formatter hint;
            if (!m_selectionAvailable) {
                hint << "Select exactly one track in Analysis Results.\r\n"
                     << "The candidate inspector remains open and will follow "
                     << "the next stable single-track selection.";
            } else {
                hint << m_trackLabel << "\r\n";
            }
            if (!m_selectionAvailable) {
                // The selection guidance above is the complete empty state.
            } else if (!m_candidates.empty()) {
                hint << (uint64_t)m_candidates.size()
                     << " legal BPM values were measured. Rank is the raw evidence order; Engine marks Final plus two legal measured alternatives, or the two leading Hold contenders. Only the confirmation button changes the result.";
            } else if (m_analysisInProgress) {
                hint << "Measuring legal BPM candidates for this track...";
            } else if (m_analysisQueued) {
                hint << "Candidate measurement is queued behind the current inspection...";
            } else if (!m_analysisError.is_empty()) {
                hint << "Candidate measurement failed: "
                     << m_analysisError.get_ptr();
            } else if (m_analysisAttempted) {
                hint << "No legal measured BPM candidate was produced by the bounded reanalysis.";
            } else {
                hint << "No candidate board is cached. Use Measured BPM... in Analysis Results to run one no-write analysis of this track.";
            }
            uSetDlgItemText(m_hWnd, IDC_MEASURED_CANDIDATE_HINT, hint);
            GetDlgItem(IDOK).EnableWindow(FALSE);
        }

        void SelectInitialCandidate() {
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            const int rowCount = list.GetItemCount();
            if (rowCount <= 0) return;

            int selectedRow = 0;
            if (m_hasCurrentResult) {
                for (int row = 0; row < rowCount; ++row) {
                    const size_t candidateIndex =
                        static_cast<size_t>(list.GetItemData(row));
                    if (candidateIndex < m_candidates.size() &&
                        IsCurrentResultCandidate(
                            m_candidates[candidateIndex])) {
                        selectedRow = row;
                        break;
                    }
                }
            }
            list.SetItemState(selectedRow, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
            list.EnsureVisible(selectedRow, FALSE);
        }

        LRESULT OnOK(UINT, int, CWindow) {
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            const int row = ListView_GetNextItem(list, -1, LVIS_SELECTED);
            if (row < 0) return 0;
            const size_t index = static_cast<size_t>(list.GetItemData(row));
            if (index >= m_candidates.size()) return 0;
            if (::IsWindow(m_resultWindow)) {
                measured_bpm_candidate_selection selection{m_modelIndex, index};
                ::SendMessage(
                    m_resultWindow,
                    bpm_result_dialog::kMeasuredCandidateSelectedMessage,
                    reinterpret_cast<WPARAM>(m_hWnd),
                    reinterpret_cast<LPARAM>(&selection));
            } else if (m_directTrack.is_valid()) {
                const auto& candidate = m_candidates[index];
                if (!std::isfinite(candidate.bpm) || candidate.bpm < 40.0 ||
                    candidate.bpm > 220.0) {
                    return 0;
                }

                metadb_handle_list tracks;
                tracks.add_item(m_directTrack);
                metadb_handle_list resolvedReviewTracks;
                if (cfg_smart_tempo_create_review_playlist) {
                    resolvedReviewTracks.add_item(m_directTrack);
                }
                const pfc::string8 reviewPlaylistName =
                    cfg_smart_tempo_review_playlist_name.get();
                auto completion =
                    [resolvedReviewTracks, reviewPlaylistName](
                        unsigned code) mutable {
                        if (code != metadb_io::update_info_success ||
                            resolvedReviewTracks.get_count() == 0) {
                            return;
                        }
                        fb2k::inMainThread(
                            [resolvedReviewTracks,
                             reviewPlaylistName]() mutable {
                                remove_resolved_tracks_from_review_playlist(
                                    std::move(resolvedReviewTracks),
                                    reviewPlaylistName);
                            });
                    };
                auto changedItemCounter =
                    std::make_shared<std::atomic<uint64_t>>(0);
                if (!smart_tempo::tag_write::safe_update_info_async(
                        tracks,
                        new service_impl_t<file_info_filter_bpm>(
                            m_directTrack,
                            bpm_config_bpm_tag.get().get_ptr(),
                            candidate.bpm, 0.0, changedItemCounter),
                        "measured-candidate-direct", m_hWnd, true,
                        changedItemCounter, std::move(completion))) {
                    return 0;
                }

                FB2K_console_formatter()
                    << "foo_smart_tempo: [" << m_trackLabel.get_ptr()
                    << "] User selected measured candidate directly: bpm="
                    << candidate.bpm << ", evidence_rank="
                    << (uint64_t)candidate.evidence_rank
                    << ", evidence_score=" << candidate.evidence_score
                    << ", support=" << candidate.support
                    << ", winner_support=" << candidate.winner_support
                    << ", local_exact_score="
                    << candidate.local_exact_score
                    << ", alias_classes="
                    << candidate.alias_classes.get_ptr()
                    << ", sources=" << candidate.sources.get_ptr();
            }
            DestroyWindow();
            return 0;
        }

        LRESULT OnCancel(UINT, int, CWindow) {
            DestroyWindow();
            return 0;
        }

        void OnClose() { DestroyWindow(); }

        void OnDestroy() {
            KillTimer(kMetronomeTimer);
            smart_tempo::manual_metronome::controller::instance().release(m_hWnd);
            if (m_playbackCallbackRegistered) {
                fb2k::std_api_get<play_callback_manager>()->unregister_callback(
                    this);
                m_playbackCallbackRegistered = false;
            }
            if (g_activeMeasuredCandidateDialog == m_hWnd) {
                g_activeMeasuredCandidateDialog = nullptr;
            }
            if (g_directMeasuredCandidateDialog == m_hWnd) {
                g_directMeasuredCandidateDialog = nullptr;
            }
        }

        void OnFinalMessage(HWND) override { delete this; }

        LRESULT OnItemChanged(LPNMHDR header) {
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            GetDlgItem(IDOK).EnableWindow(
                ListView_GetNextItem(list, -1, LVIS_SELECTED) >= 0);
            const auto* change = reinterpret_cast<const NMLISTVIEW*>(header);
            const bool becameSelected = change != nullptr &&
                change->iItem >= 0 &&
                (change->uChanged & LVIF_STATE) != 0 &&
                (change->uOldState & LVIS_SELECTED) == 0 &&
                (change->uNewState & LVIS_SELECTED) != 0;
            if (becameSelected) {
                const size_t candidateIndex = static_cast<size_t>(
                    list.GetItemData(change->iItem));
                if (candidateIndex >= m_candidates.size()) return 0;
                const double selectedBpm = m_candidates[candidateIndex].bpm;
                auto& metronome =
                    smart_tempo::manual_metronome::controller::instance();
                const bool active = metronome.is_active_for(m_hWnd);
                metronome.set_bpm(m_hWnd, selectedBpm);
                if (active && candidateIndex != m_lastAuditionCandidate) {
                    // Candidate navigation is an audition action: restart the
                    // active grid immediately so this value begins on beat one.
                    metronome.sync_now(m_hWnd);
                }
                m_lastAuditionCandidate = candidateIndex;
            }
            UpdateMetronomeState();
            return 0;
        }

        void CueCandidateRow(CListViewCtrl list, int row, bool pressEdge) {
            if (row < 0 || row >= list.GetItemCount()) return;
            const size_t candidateIndex =
                static_cast<size_t>(list.GetItemData(row));
            if (candidateIndex >= m_candidates.size()) return;

            auto& metronome =
                smart_tempo::manual_metronome::controller::instance();
            metronome.set_bpm(m_hWnd, m_candidates[candidateIndex].bpm);
            if (metronome.is_active_for(m_hWnd)) {
                metronome.sync_now(m_hWnd);
            }
            // Native list selection follows this synchronous press-edge cue.
            // Suppress the resulting LVN_ITEMCHANGED duplicate for a new row.
            m_lastAuditionCandidate = candidateIndex;
            if (pressEdge) {
                m_pointerCuePendingRelease = true;
                m_pointerCueCandidate = candidateIndex;
            }
            UpdateMetronomeState();
        }

        LRESULT OnCandidateClickFallback(LPNMHDR header) {
            const auto* activation =
                reinterpret_cast<const NMITEMACTIVATE*>(header);
            if (activation == nullptr || activation->iItem < 0) return 0;
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            const size_t candidateIndex = static_cast<size_t>(
                list.GetItemData(activation->iItem));
            if (m_pointerCuePendingRelease &&
                candidateIndex == m_pointerCueCandidate) {
                m_pointerCuePendingRelease = false;
                return 0;
            }
            m_pointerCuePendingRelease = false;
            if (!m_reportedPointerReleaseFallback) {
                m_reportedPointerReleaseFallback = true;
                FB2K_console_formatter()
                    << "foo_smart_tempo: [Manual Metronome] candidate "
                    << "pointer press was unavailable; using release fallback";
            }
            CueCandidateRow(list, activation->iItem, false);
            return 0;
        }

        LRESULT OnDoubleClick(LPNMHDR header) {
            const auto* activation =
                reinterpret_cast<const NMITEMACTIVATE*>(header);
            if (activation == nullptr || activation->iItem < 0) return 0;

            // Selection already auditions a changed row while running. A
            // double-click only starts a stopped audition; accepting a BPM is
            // deliberately reserved for the explicit confirmation button.
            auto& metronome =
                smart_tempo::manual_metronome::controller::instance();
            if (!metronome.is_active_for(m_hWnd)) {
                OnMetronomeSync(0, IDC_METRONOME_SYNC,
                                GetDlgItem(IDC_METRONOME_SYNC));
            }
            ::SetForegroundWindow(m_hWnd);
            focus_measured_candidate_list(m_hWnd);
            return 0;
        }

        LRESULT OnReloadTrack(UINT, WPARAM, LPARAM data) {
            const auto* reload =
                reinterpret_cast<const measured_bpm_candidate_reload*>(data);
            if (reload == nullptr || reload->candidates == nullptr ||
                reload->track_label == nullptr) {
                return 0;
            }
            const bool trackChanged =
                reload->model_index != m_modelIndex ||
                reload->track != m_boundTrack;
            if (trackChanged) {
                smart_tempo::manual_metronome::controller::instance().disable(
                    m_hWnd);
                KillTimer(kMetronomeTimer);
            }
            m_candidates = *reload->candidates;
            m_trackLabel = reload->track_label;
            m_boundTrack = reload->track;
            if (!::IsWindow(m_resultWindow)) {
                m_directTrack = m_boundTrack;
            }
            m_modelIndex = reload->model_index;
            m_currentResultBpm = reload->current_result_bpm;
            m_hasCurrentResult = reload->has_current_result;
            m_selectionAvailable = reload->selection_available;
            m_analysisAttempted = reload->analysis_attempted;
            m_analysisInProgress = reload->analysis_in_progress;
            m_analysisQueued = reload->analysis_queued;
            m_analysisError =
                reload->analysis_error != nullptr ? reload->analysis_error : "";
            m_lastAuditionCandidate = static_cast<size_t>(-1);
            m_pointerCueCandidate = static_cast<size_t>(-1);
            m_pointerCuePendingRelease = false;
            if (trackChanged) {
                // Each track owns an independent evidence board. Do not carry
                // a previous track's inspection sort into the new board.
                m_sortColumn = ColumnRank;
                m_sortAscending = true;
            }
            UpdatePlaybackMismatch();
            PopulateCandidateList();
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            if (!m_candidates.empty()) {
                SelectInitialCandidate();
            }
            UpdateMetronomeState();
            return 0;
        }

        void UpdatePlaybackMismatch() {
            metadb_handle_ptr playingTrack;
            const bool hasPlayingTrack =
                fb2k::std_api_get<playback_control>()->get_now_playing(
                    playingTrack);
            m_playbackMismatch =
                m_selectionAvailable && m_boundTrack.is_valid() &&
                hasPlayingTrack && playingTrack.is_valid() &&
                playingTrack != m_boundTrack;
            if (m_playbackMismatch) {
                smart_tempo::manual_metronome::controller::instance().disable(
                    m_hWnd);
                KillTimer(kMetronomeTimer);
            }
            UpdateMetronomeState();
        }

        void on_playback_new_track(metadb_handle_ptr) override {
            UpdatePlaybackMismatch();
        }

        void on_playback_stop(play_control::t_stop_reason) override {
            UpdatePlaybackMismatch();
        }

        LRESULT OnColumnClick(LPNMHDR header) {
            const auto* view = reinterpret_cast<const NMLISTVIEW*>(header);
            if (view == nullptr || view->iSubItem < 0 ||
                view->iSubItem >= ColumnCount) {
                return 0;
            }
            if (m_sortColumn == view->iSubItem) {
                m_sortAscending = !m_sortAscending;
            } else {
                m_sortColumn = view->iSubItem;
                m_sortAscending = true;
            }
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            ListView_SortItems(list, SortThunk, (LPARAM)this);
            return 0;
        }

        LRESULT OnHeaderChanged(LPNMHDR) {
            RefreshHeaderTooltips();
            return 0;
        }

        void InitMetronomeControls() {
            CComboBox meter(GetDlgItem(IDC_METRONOME_METER));
            meter.AddString(L"No accent");
            meter.AddString(L"2/4");
            meter.AddString(L"3/4");
            meter.AddString(L"4/4");
            meter.AddString(L"6/8");

            auto& metronome = smart_tempo::manual_metronome::controller::instance();
            metronome.prepare(m_hWnd);
            smart_tempo::manual_metronome::install_cue_button(
                GetDlgItem(IDC_METRONOME_SYNC), m_hWnd);
            meter.SetCurSel(combo_from_meter(metronome.meter()));
            smart_tempo::manual_metronome::initialize_volume_slider(
                GetDlgItem(IDC_METRONOME_VOLUME), m_hWnd,
                metronome.volume());

            if (m_controlTooltip.Create(
                    m_hWnd, NULL, NULL,
                    WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP)) {
                m_controlTooltip.SetMaxTipWidth(430);
                m_controlTooltip.SetDelayTime(TTDT_INITIAL, 300);
                AddControlTooltip(IDC_METRONOME_STOP,
                    L"Stop the metronome.");
                AddControlTooltip(IDC_METRONOME_SYNC,
                    L"Start or restart the metronome now. The immediate click defines beat one. Track playback is controlled separately by foobar2000.");
                AddControlTooltip(IDC_METRONOME_NUDGE_EARLIER,
                    L"Move every click 10 milliseconds earlier without changing BPM. Use this when the metronome sounds late.");
                AddControlTooltip(IDC_METRONOME_NUDGE_LATER,
                    L"Move every click 10 milliseconds later without changing BPM. Use this when the metronome sounds early.");
                AddControlTooltip(IDC_METRONOME_METER,
                    L"Choose an accent pattern only. This does not alter the candidate BPM.");
                AddControlTooltip(IDC_METRONOME_VOLUME,
                    L"Adjust the metronome click volume independently of the track. Click anywhere on the slider or drag the thumb.");
            }
            UpdateMetronomeState();
            RedrawWindow(nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                             RDW_UPDATENOW);
        }

        void AddControlTooltip(int controlId, const wchar_t* text) {
            CWindow control(GetDlgItem(controlId));
            if (!m_controlTooltip.IsWindow() || !control.IsWindow()) return;
            TOOLINFO tool{};
            tool.cbSize = sizeof(tool);
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = m_hWnd;
            tool.uId = reinterpret_cast<UINT_PTR>(control.m_hWnd);
            tool.lpszText = const_cast<LPWSTR>(text);
            m_controlTooltip.AddTool(&tool);
        }

        bool SelectedBpm(double& bpm) const {
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            const int row = ListView_GetNextItem(list, -1, LVIS_SELECTED);
            if (row < 0) return false;
            const size_t index = static_cast<size_t>(list.GetItemData(row));
            if (index >= m_candidates.size() ||
                !std::isfinite(m_candidates[index].bpm)) {
                return false;
            }
            bpm = m_candidates[index].bpm;
            return bpm >= 40.0 && bpm <= 220.0;
        }

        bool IsCurrentResultCandidate(
            const IBpmAnalyzer::MeasuredBpmCandidate& candidate) const {
            return m_hasCurrentResult && std::isfinite(m_currentResultBpm) &&
                   std::abs(candidate.bpm - m_currentResultBpm) <= 0.001;
        }

        void BuildEngineRoles() {
            m_engineRoles.assign(m_candidates.size(), EngineRole::None);
            if (m_candidates.empty()) return;

            std::vector<size_t> order(m_candidates.size());
            for (size_t index = 0; index < order.size(); ++index) {
                order[index] = index;
            }
            std::stable_sort(
                order.begin(), order.end(), [this](size_t left, size_t right) {
                    const auto& a = m_candidates[left];
                    const auto& b = m_candidates[right];
                    const size_t aRank = a.evidence_rank > 0
                                             ? a.evidence_rank
                                             : static_cast<size_t>(-1);
                    const size_t bRank = b.evidence_rank > 0
                                             ? b.evidence_rank
                                             : static_cast<size_t>(-1);
                    return std::make_tuple(
                               aRank, -a.evidence_score, -a.support,
                               -a.local_exact_score, a.bpm) <
                           std::make_tuple(
                               bRank, -b.evidence_score, -b.support,
                               -b.local_exact_score, b.bpm);
                });

            size_t finalIndex = static_cast<size_t>(-1);
            if (m_hasCurrentResult) {
                for (const size_t index : order) {
                    if (IsCurrentResultCandidate(m_candidates[index])) {
                        finalIndex = index;
                        m_engineRoles[index] = EngineRole::Final;
                        break;
                    }
                }
                // A writable result without its measured witness is a broken
                // provenance state. Do not invent alternative labels around it.
                if (finalIndex == static_cast<size_t>(-1)) return;
            }

            size_t contender = 0;
            for (const size_t index : order) {
                if (index == finalIndex) continue;
                if (contender == 0) {
                    m_engineRoles[index] = m_hasCurrentResult
                                               ? EngineRole::Alternative1
                                               : EngineRole::Hold1;
                } else if (contender == 1) {
                    m_engineRoles[index] = m_hasCurrentResult
                                               ? EngineRole::Alternative2
                                               : EngineRole::Hold2;
                } else {
                    break;
                }
                ++contender;
            }
        }

        const wchar_t* EngineRoleText(size_t candidateIndex) const noexcept {
            if (candidateIndex >= m_engineRoles.size()) return L"";
            switch (m_engineRoles[candidateIndex]) {
            case EngineRole::Final: return L"Final";
            case EngineRole::Alternative1: return L"Alt 1";
            case EngineRole::Alternative2: return L"Alt 2";
            case EngineRole::Hold1: return L"Hold 1";
            case EngineRole::Hold2: return L"Hold 2";
            default: return L"";
            }
        }

        int EngineRoleSortKey(size_t candidateIndex) const noexcept {
            if (candidateIndex >= m_engineRoles.size()) return 99;
            switch (m_engineRoles[candidateIndex]) {
            case EngineRole::Final:
            case EngineRole::Hold1: return 0;
            case EngineRole::Alternative1:
            case EngineRole::Hold2: return 1;
            case EngineRole::Alternative2: return 2;
            default: return 99;
            }
        }

        void UpdateMetronomeState() {
            auto& metronome = smart_tempo::manual_metronome::controller::instance();
            const bool active = metronome.is_active_for(m_hWnd);
            double selectedBpm = 0.0;
            const bool validBpm = SelectedBpm(selectedBpm);
            GetDlgItem(IDC_METRONOME_STOP).EnableWindow(active);
            GetDlgItem(IDC_METRONOME_SYNC)
                .EnableWindow(!m_playbackMismatch &&
                              (active || validBpm));
            GetDlgItem(IDC_METRONOME_NUDGE_EARLIER).EnableWindow(active);
            GetDlgItem(IDC_METRONOME_NUDGE_LATER).EnableWindow(active);

            pfc::string_formatter status;
            if (m_playbackMismatch) {
                status << "Playback is another track - metronome stopped.";
            } else if (active) {
                status << "Running: " << format_float_locale(metronome.bpm(), 2)
                       << " BPM, ";
                if (metronome.meter() == 1) {
                    status << "no accent";
                } else {
                    status << metronome.meter() << "-beat accent";
                }
                status << ", offset "
                       << format_float_locale(
                              metronome.phase_nudge_milliseconds(), 0)
                       << " ms";
            } else {
                status << "Off - select a candidate, then use Start / Sync.";
            }
            uSetDlgItemText(m_hWnd, IDC_METRONOME_STATUS, status);
        }

        LRESULT OnMetronomeStop(UINT, int, CWindow) {
            smart_tempo::manual_metronome::controller::instance().disable(m_hWnd);
            KillTimer(kMetronomeTimer);
            UpdateMetronomeState();
            return 0;
        }

        LRESULT OnMetronomeSync(UINT, int, CWindow) {
            if (m_playbackMismatch) return 0;
            auto& metronome = smart_tempo::manual_metronome::controller::instance();
            if (metronome.is_active_for(m_hWnd)) {
                metronome.sync_now(m_hWnd);
            } else {
                double selectedBpm = 0.0;
                if (SelectedBpm(selectedBpm)) {
                    CComboBox meter(GetDlgItem(IDC_METRONOME_METER));
                    CTrackBarCtrl volume(GetDlgItem(IDC_METRONOME_VOLUME));
                    metronome.enable(
                        m_hWnd, selectedBpm,
                        meter_from_combo(meter.GetCurSel()), volume.GetPos());
                }
            }
            if (metronome.is_active_for(m_hWnd)) {
                SetTimer(kMetronomeTimer, kMetronomeTimerIntervalMs);
            }
            UpdateMetronomeState();
            return 0;
        }

        LRESULT OnMetronomeNudgeEarlier(UINT, int, CWindow) {
            smart_tempo::manual_metronome::controller::instance().nudge_seconds(
                m_hWnd, -0.010);
            UpdateMetronomeState();
            return 0;
        }

        LRESULT OnMetronomeNudgeLater(UINT, int, CWindow) {
            smart_tempo::manual_metronome::controller::instance().nudge_seconds(
                m_hWnd, 0.010);
            UpdateMetronomeState();
            return 0;
        }

        LRESULT OnMetronomeMeterChanged(UINT, int, CWindow) {
            CComboBox meter(GetDlgItem(IDC_METRONOME_METER));
            smart_tempo::manual_metronome::controller::instance().set_meter(
                m_hWnd, meter_from_combo(meter.GetCurSel()));
            UpdateMetronomeState();
            return 0;
        }

        void OnHScroll(UINT, UINT, CScrollBar scrollBar) {
            if (scrollBar.m_hWnd != GetDlgItem(IDC_METRONOME_VOLUME).m_hWnd) return;
            CTrackBarCtrl volume(GetDlgItem(IDC_METRONOME_VOLUME));
            smart_tempo::manual_metronome::controller::instance().set_volume(
                m_hWnd, volume.GetPos());
        }

        void OnTimer(UINT_PTR timerId) {
            if (timerId != kMetronomeTimer) return;
            auto& metronome = smart_tempo::manual_metronome::controller::instance();
            if (!metronome.is_active_for(m_hWnd)) {
                KillTimer(kMetronomeTimer);
                UpdateMetronomeState();
                return;
            }
            metronome.tick(m_hWnd);
            if ((m_timerTicks++ % kMetronomeStatusRefreshTicks) == 0) {
                UpdateMetronomeState();
            }
        }

        void InitHeaderTooltips() {
            if (!m_headerTooltip.IsWindow() &&
                !m_headerTooltip.Create(
                    m_hWnd, NULL, NULL,
                    WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP)) {
                return;
            }
            m_headerTooltip.SetMaxTipWidth(520);
            m_headerTooltip.SetDelayTime(TTDT_INITIAL, 300);
            m_headerTooltip.Activate(TRUE);
            RefreshHeaderTooltips();
        }

        void RefreshHeaderTooltips() {
            if (!m_headerTooltip.IsWindow()) return;
            CListViewCtrl list(GetDlgItem(IDC_MEASURED_CANDIDATE_LIST));
            CHeaderCtrl header = list.GetHeader();
            if (!header.IsWindow()) return;
            static constexpr const wchar_t* explanations[ColumnCount] = {
                L"Raw audio-evidence-board rank, not the final engine order. Later measured-only matrix, Partial-Bar, cross-view, soft-center, and safety stages may select another rank; 'extra' is an independent refinement witness.",
                L"Exact BPM measured by this analysis. No averaging, snapping, folding, or metadata estimate is applied.",
                L"Final is the complete engine result. Alt 1/2 are the highest-ranked remaining legal measured values, not automatic recommendations. On Review Hold, Hold 1/2 mark the leading measured contenders; no Final exists.",
                L"Relative audio-evidence score used to order measured candidates. It is not a probability or confidence percentage.",
                L"Share of analyzed segments supporting this candidate's pulse family.",
                L"Share of analyzed segments in which this candidate or family was the local winner.",
                L"Strength of phase continuity at the locally refined exact BPM. Higher means a clearer timing peak; it is not an error value.",
                L"Measured rational pulse relationships that produced this BPM, such as direct, half, double, or three-quarters.",
                L"Hodgkinson/MIR measurement surfaces that witnessed this exact BPM value."
            };
            for (int column = 0; column < ColumnCount; ++column) {
                TOOLINFO oldTool{};
                oldTool.cbSize = sizeof(oldTool);
                oldTool.hwnd = header;
                oldTool.uId = static_cast<UINT_PTR>(column + 1);
                m_headerTooltip.DelTool(&oldTool);

                CRect rect;
                if (!header.GetItemRect(column, &rect)) continue;
                TOOLINFO tool{};
                tool.cbSize = sizeof(tool);
                tool.uFlags = TTF_SUBCLASS;
                tool.hwnd = header;
                tool.uId = static_cast<UINT_PTR>(column + 1);
                tool.rect = rect;
                tool.lpszText = const_cast<LPWSTR>(explanations[column]);
                m_headerTooltip.AddTool(&tool);
            }
        }

        static int CALLBACK SortThunk(LPARAM left, LPARAM right, LPARAM context) {
            const auto* self = reinterpret_cast<const measured_bpm_candidate_dialog*>(context);
            if (self == nullptr || left < 0 || right < 0) return 0;
            const size_t l = static_cast<size_t>(left);
            const size_t r = static_cast<size_t>(right);
            if (l >= self->m_candidates.size() || r >= self->m_candidates.size()) return 0;
            const auto& a = self->m_candidates[l];
            const auto& b = self->m_candidates[r];
            int result = 0;
            switch (self->m_sortColumn) {
            case ColumnRank:
                if ((a.evidence_rank == 0) != (b.evidence_rank == 0)) {
                    result = a.evidence_rank == 0 ? 1 : -1;
                } else {
                    result = a.evidence_rank < b.evidence_rank ? -1 : a.evidence_rank > b.evidence_rank ? 1 : 0;
                }
                break;
            case ColumnBpm:
                result = a.bpm < b.bpm ? -1 : a.bpm > b.bpm ? 1 : 0;
                break;
            case ColumnEngineResult: {
                const int aRole = self->EngineRoleSortKey(l);
                const int bRole = self->EngineRoleSortKey(r);
                result = aRole < bRole ? -1 : aRole > bRole ? 1 : 0;
                break;
            }
            case ColumnEvidence:
                result = a.evidence_score < b.evidence_score ? -1 : a.evidence_score > b.evidence_score ? 1 : 0;
                break;
            case ColumnSupport:
                result = a.support < b.support ? -1 : a.support > b.support ? 1 : 0;
                break;
            case ColumnWinnerSupport:
                result = a.winner_support < b.winner_support ? -1 : a.winner_support > b.winner_support ? 1 : 0;
                break;
            case ColumnLocalExact:
                result = a.local_exact_score < b.local_exact_score ? -1 : a.local_exact_score > b.local_exact_score ? 1 : 0;
                break;
            case ColumnAliases:
                result = std::strcmp(a.alias_classes.get_ptr(), b.alias_classes.get_ptr());
                break;
            case ColumnSources:
                result = std::strcmp(a.sources.get_ptr(), b.sources.get_ptr());
                break;
            default:
                break;
            }
            if (result == 0) result = a.bpm < b.bpm ? -1 : a.bpm > b.bpm ? 1 : 0;
            return self->m_sortAscending ? result : -result;
        }

        std::vector<IBpmAnalyzer::MeasuredBpmCandidate> m_candidates;
        std::vector<EngineRole> m_engineRoles;
        pfc::string8 m_trackLabel;
        HWND m_resultWindow = nullptr;
        size_t m_modelIndex = static_cast<size_t>(-1);
        double m_currentResultBpm = 0.0;
        bool m_hasCurrentResult = false;
        metadb_handle_ptr m_boundTrack;
        metadb_handle_ptr m_directTrack;
        bool m_selectionAvailable = true;
        bool m_analysisAttempted = false;
        bool m_analysisInProgress = false;
        bool m_analysisQueued = false;
        pfc::string8 m_analysisError;
        bool m_playbackMismatch = false;
        bool m_playbackCallbackRegistered = false;
        size_t m_lastAuditionCandidate = static_cast<size_t>(-1);
        size_t m_pointerCueCandidate = static_cast<size_t>(-1);
        bool m_pointerCuePendingRelease = false;
        bool m_reportedPointerReleaseFallback = false;
        int m_sortColumn = ColumnRank;
        bool m_sortAscending = true;
        fb2k::CCoreDarkModeHooks m_dark;
        CToolTipCtrl m_headerTooltip;
        CToolTipCtrl m_controlTooltip;
        unsigned m_timerTicks = 0;
    };

    LRESULT CALLBACK candidate_list_subclass(
        HWND list, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR dialogValue) {
        auto* dialog = reinterpret_cast<measured_bpm_candidate_dialog*>(
            dialogValue);
        if (dialog != nullptr && message == WM_KEYDOWN &&
            wParam == VK_SPACE) {
            // Consume Space before the native ListView toggles selection.
            // Ignore auto-repeat as a transport command but still suppress its
            // default row-state mutation.
            if ((lParam & (static_cast<LPARAM>(1) << 30)) == 0) {
                dialog->OnMetronomeSync(
                    0, IDC_METRONOME_SYNC,
                    dialog->GetDlgItem(IDC_METRONOME_SYNC));
            }
            return 0;
        }
        if (message == WM_CHAR && wParam == VK_SPACE) {
            return 0;
        }
        if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) &&
            dialog != nullptr) {
            // Call the same dialog transport synchronously at the physical
            // press edge; Windows replaces the second DOWN of a rapid pair
            // with DBLCLK, so both messages must be lossless cue commands.
            dialog->CueCandidateFromPointer(list, lParam);
        }
        if (message == WM_NCDESTROY) {
            ::RemoveWindowSubclass(
                list, candidate_list_subclass, subclassId);
        }
        return ::DefSubclassProc(list, message, wParam, lParam);
    }

}

bool show_direct_measured_bpm_candidate_dialog(
    metadb_handle_ptr track,
    std::vector<IBpmAnalyzer::MeasuredBpmCandidate> candidates,
    const pfc::string8& trackLabel,
    double currentResultBpm,
    bool hasCurrentResult)
{
    if (!track.is_valid()) return false;

    if (::IsWindow(g_directMeasuredCandidateDialog)) {
        measured_bpm_candidate_reload reload;
        reload.candidates = &candidates;
        reload.track_label = trackLabel.get_ptr();
        reload.track = track;
        reload.current_result_bpm =
            hasCurrentResult ? currentResultBpm : 0.0;
        reload.has_current_result = hasCurrentResult;
        reload.analysis_attempted = true;
        ::SendMessage(
            g_directMeasuredCandidateDialog,
            measured_bpm_candidate_dialog::kReloadTrackMessage, 0,
            reinterpret_cast<LPARAM>(&reload));
        ::ShowWindow(g_directMeasuredCandidateDialog,
                     ::IsIconic(g_directMeasuredCandidateDialog)
                         ? SW_RESTORE
                         : SW_SHOW);
        ::SetForegroundWindow(g_directMeasuredCandidateDialog);
        ::BringWindowToTop(g_directMeasuredCandidateDialog);
        focus_measured_candidate_list(g_directMeasuredCandidateDialog);
        return true;
    }
    if (::IsWindow(g_activeMeasuredCandidateDialog)) {
        ::DestroyWindow(g_activeMeasuredCandidateDialog);
    }

    auto* dialog = new measured_bpm_candidate_dialog(
        std::move(candidates), trackLabel, nullptr,
        static_cast<size_t>(-1), currentResultBpm, hasCurrentResult,
        track);
    const HWND mainWindow = core_api::get_main_window();
    const HWND dialogWindow = dialog->Create(mainWindow);
    if (dialogWindow == nullptr) {
        delete dialog;
        return false;
    }
    g_activeMeasuredCandidateDialog = dialogWindow;
    g_directMeasuredCandidateDialog = dialogWindow;

    const LONG_PTR extendedStyle =
        ::GetWindowLongPtr(dialogWindow, GWL_EXSTYLE);
    ::SetWindowLongPtr(dialogWindow, GWL_EXSTYLE,
                       extendedStyle | WS_EX_TOOLWINDOW);
    ::SetWindowPos(dialogWindow, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                       SWP_NOACTIVATE | SWP_FRAMECHANGED);
    dialog->CenterWindow(mainWindow);
    dialog->ShowWindow(SW_SHOWNORMAL);
    return true;
}

bpm_result_dialog::bpm_result_dialog(
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
    double p_elapsedSeconds)
    : m_tracks(p_tracks)
    , m_infos(p_infos)
    , m_bpm_results(p_bpm_results)
    , m_confidence_results(p_confidence_results)
    , m_genre_results(p_genre_results)
    , m_method_results(p_method_results)
    , m_error_results(p_error_results)
    , m_suppress_write_results(p_suppress_write_results)
    , m_routing_context_results(p_routing_context_results)
    , m_measured_bpm_candidate_results(p_measured_bpm_candidate_results)
    , m_processedTrackCount(p_processedTrackCount)
    , m_elapsedSeconds((std::max)(0.0, p_elapsedSeconds))
{
    const size_t total = m_tracks.get_count();
    if (m_confidence_results.size() != total) m_confidence_results.assign(total, 0.0);
    if (m_genre_results.size() != total) m_genre_results.assign(total, pfc::string8());
    if (m_method_results.size() != total) m_method_results.assign(total, pfc::string8());
    if (m_error_results.size() != total) m_error_results.assign(total, pfc::string8());
    if (m_suppress_write_results.size() != total) m_suppress_write_results.assign(total, 0);
    if (m_routing_context_results.size() != total) {
        m_routing_context_results.assign(total, {});
    }
    if (m_measured_bpm_candidate_results.size() != total) {
        m_measured_bpm_candidate_results.assign(total, {});
    }
    m_measuredCandidateAnalysisAttempted.assign(total, 0);
    m_measuredCandidateAnalysisErrors.assign(total, pfc::string8());
    for (size_t index = 0; index < total; ++index) {
        if (!m_measured_bpm_candidate_results[index].empty()) {
            m_measuredCandidateAnalysisAttempted[index] = 1;
        }
    }
    m_showConfidenceColumn = true;
    BuildVisibleColumns();
    BuildTextCache();
}

void bpm_result_dialog::BuildTextCache()
{
    const size_t total = m_tracks.get_count();
    m_artistWide.assign(total, std::wstring());
    m_titleWide.assign(total, std::wstring());
    m_genreWide.assign(total, std::wstring());
    m_methodWide.assign(total, std::wstring());
    m_errorWide.assign(total, std::wstring());

    for (size_t index = 0; index < total; ++index) {
        const pfc::string8 artist = get_artist(index);
        const pfc::string8 title = get_title(index);
        const pfc::string8 genre = get_genre(index);
        const pfc::string8 method = get_method(index);
        const pfc::string8 error = get_error(index);

        pfc::stringcvt::string_wide_from_utf8 artistW(artist);
        pfc::stringcvt::string_wide_from_utf8 titleW(title);
        pfc::stringcvt::string_wide_from_utf8 genreW(genre);
        pfc::stringcvt::string_wide_from_utf8 methodW(method);
        pfc::stringcvt::string_wide_from_utf8 errorW(error);

        m_artistWide[index] = artistW.get_ptr();
        m_titleWide[index] = titleW.get_ptr();
        m_genreWide[index] = genreW.get_ptr();
        m_methodWide[index] = methodW.get_ptr();
        m_errorWide[index] = errorW.get_ptr();
    }

    InvalidateColumnContentWidthCache();
}

void bpm_result_dialog::BuildVisibleColumns()
{
    m_visibleColumns.clear();
    m_visibleColumns.push_back(ColumnArtist);
    m_visibleColumns.push_back(ColumnTitle);
    m_visibleColumns.push_back(ColumnGenre);
    m_visibleColumns.push_back(ColumnBpm);
    if (m_showConfidenceColumn) {
        m_visibleColumns.push_back(ColumnConfidence);
    }
    m_visibleColumns.push_back(ColumnMethod);
    m_visibleColumns.push_back(ColumnError);

    m_modelToView.fill(-1);
    for (size_t i = 0; i < m_visibleColumns.size(); ++i) {
        const int modelColumn = m_visibleColumns[i];
        if (modelColumn >= 0 && modelColumn < kModelColumnCount) {
            m_modelToView[(size_t)modelColumn] = (int)i;
        }
    }

    InvalidateColumnContentWidthCache();
}

int bpm_result_dialog::ViewToModelColumn(int viewColumn) const
{
    if (viewColumn < 0 || viewColumn >= (int)m_visibleColumns.size()) return -1;
    return m_visibleColumns[(size_t)viewColumn];
}

int bpm_result_dialog::ModelToViewColumn(int modelColumn) const
{
    if (modelColumn < 0 || modelColumn >= kModelColumnCount) return -1;
    return m_modelToView[(size_t)modelColumn];
}

int bpm_result_dialog::VisibleColumnCount() const
{
    return (int)m_visibleColumns.size();
}

pfc::string8 bpm_result_dialog::get_artist(size_t index) const
{
    if (index < m_infos.get_size() && m_infos[index].meta_exists("ARTIST")) {
        return pfc::string8(m_infos[index].meta_get("ARTIST", 0));
    }
    return pfc::string8();
}

pfc::string8 bpm_result_dialog::get_title(size_t index) const
{
    if (index < m_infos.get_size() && m_infos[index].meta_exists("TITLE")) {
        return pfc::string8(m_infos[index].meta_get("TITLE", 0));
    }
    if (index < m_tracks.get_count()) {
        return pfc::string8(pfc::string_filename(m_tracks[index]->get_path()));
    }
    return pfc::string8();
}

pfc::string8 bpm_result_dialog::get_genre(size_t index) const
{
    if (index < m_genre_results.size() && !m_genre_results[index].is_empty()) {
        return m_genre_results[index];
    }
    return pfc::string8("Unmatched");
}

pfc::string8 bpm_result_dialog::get_method(size_t index) const
{
    if (index < m_method_results.size() && !m_method_results[index].is_empty()) {
        return m_method_results[index];
    }
    return pfc::string8("Hodgkinson/MIR");
}

pfc::string8 bpm_result_dialog::get_error(size_t index) const
{
    if (index < m_error_results.size()) return m_error_results[index];
    return pfc::string8();
}

int bpm_result_dialog::compare_rows(size_t left, size_t right, int column) const
{
    if (left >= m_tracks.get_count() || right >= m_tracks.get_count()) return 0;

    auto cmpForColumn = [this](size_t l, size_t r, int c) -> int {
        switch (c) {
        case 0: {
            return compare_text_natural_ci_w(m_artistWide[l], m_artistWide[r]);
        }
        case 1: {
            return compare_text_natural_ci_w(m_titleWide[l], m_titleWide[r]);
        }
        case 2: {
            return compare_text_natural_ci_w(m_genreWide[l], m_genreWide[r]);
        }
        case 3:
            if (m_bpm_results[l] < m_bpm_results[r]) return -1;
            if (m_bpm_results[l] > m_bpm_results[r]) return 1;
            return 0;
        case 4:
            if (m_confidence_results[l] < m_confidence_results[r]) return -1;
            if (m_confidence_results[l] > m_confidence_results[r]) return 1;
            return 0;
        case 5: {
            return compare_text_natural_ci_w(m_methodWide[l], m_methodWide[r]);
        }
        case 6: {
            return compare_text_natural_ci_w(m_errorWide[l], m_errorWide[r]);
        }
        default:
            return 0;
        }
    };

    int cmp = cmpForColumn(left, right, column);
    if (cmp != 0) return cmp;

    // Deterministic tie-breakers for stable/alphanumeric-friendly sorting.
    for (int c : m_visibleColumns) {
        if (c == column) continue;
        cmp = cmpForColumn(left, right, c);
        if (cmp != 0) return cmp;
    }

    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

int CALLBACK bpm_result_dialog::SortThunk(LPARAM left, LPARAM right, LPARAM sortParam)
{
    const auto* self = reinterpret_cast<const bpm_result_dialog*>(sortParam);
    if (self == nullptr) return 0;

    int cmp = self->compare_rows((size_t)left, (size_t)right, self->m_sortColumn);
    if (!self->m_sortAscending) cmp = -cmp;
    return cmp;
}

void bpm_result_dialog::ApplySorting()
{
    CListViewCtrl list(GetDlgItem(ID_BPM_RESULT_LIST));
    if (!list.IsWindow()) return;
    if (m_sortColumn < 0 || m_sortColumn >= kModelColumnCount) return;
    ListView_SortItems(list, SortThunk, (LPARAM)this);
}

LRESULT bpm_result_dialog::OnInitDialog(CWindow wndFocus, LPARAM lInitParam)
{
  (void)wndFocus;
  (void)lInitParam;
  m_dark.AddDialogWithControls(m_hWnd);
  static_api_ptr_t<message_loop>()->add_message_filter(this);
  {
    RECT minDlu = { 0, 0, 395, 242 };
    MapDialogRect(&minDlu);
    m_minTrackSize.cx = minDlu.right;
    m_minTrackSize.cy = minDlu.bottom;
  }

  {
    const auto format_hhmmss = [](double seconds) -> pfc::string8 {
      double safe = (std::max)(0.0, seconds);
      safe = (std::min)(safe, 359999.0);
      const uint64_t total = static_cast<uint64_t>(safe + 0.5);
      const uint64_t hh = total / 3600;
      const uint64_t mm = (total % 3600) / 60;
      const uint64_t ss = total % 60;
      pfc::string_formatter out;
      out << pfc::format_int((t_uint32)hh, 2) << ":"
          << pfc::format_int((t_uint32)mm, 2) << ":"
          << pfc::format_int((t_uint32)ss, 2);
      return pfc::string8(out.get_ptr());
    };

    pfc::string_formatter title;
    const char* unit = (m_processedTrackCount == 1) ? "track" : "tracks";
    const double elapsedSeconds = (std::max)(0.0, m_elapsedSeconds);
    title << "Smart Tempo Analysis Results - Scanned "
          << (uint64_t)m_processedTrackCount << " " << unit << " in "
          << format_hhmmss(elapsedSeconds).get_ptr();
    pfc::stringcvt::string_wide_from_utf8 titleW(title);
    SetWindowText(titleW);
  }

  const size_t suppressWriteCount = static_cast<size_t>(std::count(
      m_suppress_write_results.begin(), m_suppress_write_results.end(),
      static_cast<uint8_t>(1)));

  if (bpm_config_auto_write_tag)
  {
        auto changedItemCounter = std::make_shared<std::atomic<uint64_t>>(0);
        if (smart_tempo::tag_write::safe_update_info_async(
                m_tracks,
                new service_impl_t<file_info_filter_bpm>(
                    m_tracks, bpm_config_bpm_tag.get().get_ptr(), m_bpm_results,
                    &m_confidence_results, changedItemCounter,
                    &m_suppress_write_results),
                "auto-write", m_hWnd, true, changedItemCounter)) {
            if (suppressWriteCount == 0) {
                DestroyWindow();
                return 0;
            }
        }
    }
    CListViewCtrl result_list(GetDlgItem(ID_BPM_RESULT_LIST));

    int colIdx = 0;
    for (int modelColumn : m_visibleColumns) {
        result_list.InsertColumn(colIdx, kColumnHeaders[(size_t)modelColumn],
                                 LVCFMT_LEFT,
                                 scale_for_window_dpi(m_hWnd, kBaseColumnWidths[(size_t)modelColumn]));
        ++colIdx;
    }

    result_list.SetExtendedListViewStyle(
        LVS_EX_GRIDLINES |
        LVS_EX_FULLROWSELECT |
        LVS_EX_HEADERDRAGDROP);

    for (t_size index = 0; index < m_infos.get_size(); index++)
    {
        const size_t rowIndex = (size_t)index;
        const int row = result_list.InsertItem((int)index, m_artistWide[rowIndex].c_str());
        result_list.SetItemData(row, (DWORD_PTR)index);

        const int titleCol = ModelToViewColumn(ColumnTitle);
        if (titleCol >= 0) {
            result_list.SetItemText(row, titleCol, m_titleWide[rowIndex].c_str());
        }

        const int genreCol = ModelToViewColumn(ColumnGenre);
        if (genreCol >= 0) {
            result_list.SetItemText(row, genreCol, m_genreWide[rowIndex].c_str());
        }

        const int bpmCol = ModelToViewColumn(ColumnBpm);
        if (bpmCol >= 0) {
            if (m_suppress_write_results[rowIndex] != 0) {
                result_list.SetItemText(row, bpmCol, L"Review");
            } else {
                format_bpm bpmValue(m_bpm_results[rowIndex], get_write_precision());
                pfc::stringcvt::string_wide_from_utf8 bpmW((const char*)bpmValue);
                result_list.SetItemText(row, bpmCol, bpmW);
            }
        }

        const int confidenceCol = ModelToViewColumn(ColumnConfidence);
        if (confidenceCol >= 0) {
            pfc::string_formatter confidenceText;
            if (m_confidence_results[rowIndex] > 0.0) {
                confidenceText << format_float_locale(m_confidence_results[rowIndex], 1) << "%";
            } else {
                confidenceText << "-";
            }
            pfc::stringcvt::string_wide_from_utf8 confidenceW(confidenceText);
            result_list.SetItemText(row, confidenceCol, confidenceW);
        }

        const int methodCol = ModelToViewColumn(ColumnMethod);
        if (methodCol >= 0) {
            result_list.SetItemText(row, methodCol, m_methodWide[rowIndex].c_str());
        }
        const int errorCol = ModelToViewColumn(ColumnError);
        if (errorCol >= 0) {
            result_list.SetItemText(row, errorCol, m_errorWide[rowIndex].c_str());
        }
    }

    RebuildColumnContentWidthCache(result_list);

    UpdateBpmTagLabel();

    DlgResize_Init(true, true, WS_THICKFRAME | WS_CLIPCHILDREN);
    LoadWindowPlacement();
    LoadListLayout();
    { CRect rc; GetClientRect(&rc); DlgResize_UpdateLayout(rc.Width(), rc.Height()); }
    UpdateResultColumns();
    EnableScaleBPMButtons();
    EnableMeasuredCandidatesButton();
    InitTooltips();

    HWND updateButton = GetDlgItem(IDOK);
    HWND cancelButton = GetDlgItem(IDCANCEL);
    if (updateButton != nullptr) {
        ::SendMessage(updateButton, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
    }
    if (cancelButton != nullptr) {
        ::SendMessage(cancelButton, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        ::SendMessage(m_hWnd, DM_SETDEFID, IDCANCEL, 0);
    }

    return 0;
}

LRESULT bpm_result_dialog::OnOK(UINT uNotifyCode, int nID, CWindow wndCtl)
{
    (void)uNotifyCode;
    (void)nID;
    (void)wndCtl;
    SaveListLayout();
    SaveWindowPlacement();

    metadb_handle_list resolvedReviewTracks;
    if (cfg_smart_tempo_create_review_playlist) {
        for (t_size index = 0;
             index < m_tracks.get_count() &&
             index < m_suppress_write_results.size();
             ++index) {
            if (m_suppress_write_results[index] == 0) {
                resolvedReviewTracks.add_item(m_tracks[index]);
            }
        }
    }
    pfc::string8 reviewPlaylistName =
        cfg_smart_tempo_review_playlist_name.get();
    auto completion = [resolvedReviewTracks, reviewPlaylistName](
                          unsigned code) mutable {
        if (code != metadb_io::update_info_success ||
            resolvedReviewTracks.get_count() == 0) {
            return;
        }
        fb2k::inMainThread(
            [resolvedReviewTracks, reviewPlaylistName]() mutable {
                remove_resolved_tracks_from_review_playlist(
                    std::move(resolvedReviewTracks), reviewPlaylistName);
            });
    };

    auto changedItemCounter = std::make_shared<std::atomic<uint64_t>>(0);
    if (!smart_tempo::tag_write::safe_update_info_async(
            m_tracks,
            new service_impl_t<file_info_filter_bpm>(
                m_tracks, bpm_config_bpm_tag.get().get_ptr(), m_bpm_results,
                &m_confidence_results, changedItemCounter,
                &m_suppress_write_results),
            "manual-update", m_hWnd, true, changedItemCounter,
            std::move(completion))) {
        return 0;
    }

    CloseMeasuredCandidateDialog();
    DestroyWindow();
    return 0;
}

LRESULT bpm_result_dialog::OnCancel(UINT uNotifyCode, int nID, CWindow wndCtl)
{
    (void)uNotifyCode;
    (void)nID;
    (void)wndCtl;
    SaveListLayout();
    SaveWindowPlacement();
    CloseMeasuredCandidateDialog();
    DestroyWindow();
    return 0;
}

LRESULT bpm_result_dialog::OnDoubleBPMClicked(UINT uNotifyCode, int nID, CWindow wndCtl)
{
    (void)uNotifyCode;
    (void)nID;
    (void)wndCtl;
    ScaleSelectionBPM(2.0);
    return 0;
}

LRESULT bpm_result_dialog::OnHalveBPMClicked(UINT uNotifyCode, int nID, CWindow wndCtl)
{
    (void)uNotifyCode;
    (void)nID;
    (void)wndCtl;
    ScaleSelectionBPM(0.5);
    return 0;
}

LRESULT bpm_result_dialog::OnManualBpmClicked(UINT uNotifyCode, int nID, CWindow wndCtl)
{
    (void)uNotifyCode;
    (void)nID;
    (void)wndCtl;
    show_manual_bpm_dialog();
    return 0;
}

LRESULT bpm_result_dialog::OnMeasuredCandidatesClicked(
    UINT uNotifyCode, int nID, CWindow wndCtl)
{
    (void)uNotifyCode;
    (void)nID;
    (void)wndCtl;
    CListViewCtrl list(GetDlgItem(ID_BPM_RESULT_LIST));
    const int selectedRow = ListView_GetNextItem(list, -1, LVIS_SELECTED);
    if (selectedRow < 0 ||
        ListView_GetNextItem(list, selectedRow, LVIS_SELECTED) >= 0) {
        return 0;
    }
    const size_t modelIndex = static_cast<size_t>(list.GetItemData(selectedRow));
    if (modelIndex >= m_measured_bpm_candidate_results.size()) {
        return 0;
    }

    if (!::IsWindow(m_measuredCandidateDialog)) {
        m_measuredCandidateDialog = nullptr;
    }

    if (m_measured_bpm_candidate_results[modelIndex].empty()) {
        const bool attempted =
            modelIndex < m_measuredCandidateAnalysisAttempted.size() &&
            m_measuredCandidateAnalysisAttempted[modelIndex] != 0;
        if (!attempted &&
            m_measuredCandidateAnalysisInFlight == static_cast<size_t>(-1)) {
            StartMeasuredCandidateAnalysis(modelIndex);
        }
    }

    const HWND candidateDialog = m_measuredCandidateDialog;
    if (candidateDialog != nullptr && ::IsWindow(candidateDialog)) {
        RefreshMeasuredCandidateDialog(modelIndex);
        ::ShowWindow(candidateDialog,
                     ::IsIconic(candidateDialog) ? SW_RESTORE : SW_SHOW);
        ::SetForegroundWindow(candidateDialog);
        ::BringWindowToTop(candidateDialog);
        focus_measured_candidate_list(candidateDialog);
        return 0;
    }

    if (::IsWindow(g_activeMeasuredCandidateDialog)) {
        ::DestroyWindow(g_activeMeasuredCandidateDialog);
    }

    pfc::string_formatter trackLabel;
    trackLabel << get_artist(modelIndex).get_ptr() << " - "
               << get_title(modelIndex).get_ptr();
    const bool hasCurrentResult =
        modelIndex < m_bpm_results.size() &&
        modelIndex < m_suppress_write_results.size() &&
        m_suppress_write_results[modelIndex] == 0 &&
        std::isfinite(m_bpm_results[modelIndex]) &&
        m_bpm_results[modelIndex] > 0.0;
    const double currentResultBpm =
        hasCurrentResult ? m_bpm_results[modelIndex] : 0.0;
    auto* dialog = new measured_bpm_candidate_dialog(
        m_measured_bpm_candidate_results[modelIndex], trackLabel, m_hWnd,
        modelIndex, currentResultBpm, hasCurrentResult,
        m_tracks[modelIndex]);
    // Keep this modeless helper in the Results window's z-order family. It can
    // lose focus without falling behind unrelated application windows.
    const HWND dialogWindow = dialog->Create(m_hWnd);
    if (dialogWindow == nullptr) {
        delete dialog;
        return 0;
    }
    m_measuredCandidateDialog = dialogWindow;
    g_activeMeasuredCandidateDialog = dialogWindow;
    g_directMeasuredCandidateDialog = nullptr;
    const LONG_PTR extendedStyle = ::GetWindowLongPtr(dialogWindow, GWL_EXSTYLE);
    ::SetWindowLongPtr(dialogWindow, GWL_EXSTYLE,
                       extendedStyle | WS_EX_TOOLWINDOW);
    ::SetWindowPos(dialogWindow, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                       SWP_NOACTIVATE | SWP_FRAMECHANGED);
    dialog->CenterWindow(m_hWnd);
    dialog->ShowWindow(SW_SHOWNORMAL);
    RefreshMeasuredCandidateDialog(modelIndex);
    return 0;
}

void bpm_result_dialog::RefreshMeasuredCandidateDialog(size_t modelIndex)
{
    if (!::IsWindow(m_measuredCandidateDialog) ||
        modelIndex >= m_measured_bpm_candidate_results.size()) {
        return;
    }
    pfc::string_formatter trackLabel;
    trackLabel << get_artist(modelIndex).get_ptr() << " - "
               << get_title(modelIndex).get_ptr();
    measured_bpm_candidate_reload reload;
    reload.candidates = &m_measured_bpm_candidate_results[modelIndex];
    reload.track_label = trackLabel.get_ptr();
    reload.track = m_tracks[modelIndex];
    reload.model_index = modelIndex;
    reload.has_current_result =
        modelIndex < m_bpm_results.size() &&
        modelIndex < m_suppress_write_results.size() &&
        m_suppress_write_results[modelIndex] == 0 &&
        std::isfinite(m_bpm_results[modelIndex]) &&
        m_bpm_results[modelIndex] > 0.0;
    reload.current_result_bpm =
        reload.has_current_result ? m_bpm_results[modelIndex] : 0.0;
    reload.analysis_attempted =
        modelIndex < m_measuredCandidateAnalysisAttempted.size() &&
        m_measuredCandidateAnalysisAttempted[modelIndex] != 0;
    reload.analysis_in_progress =
        m_measuredCandidateAnalysisInFlight == modelIndex;
    reload.analysis_queued =
        m_measured_bpm_candidate_results[modelIndex].empty() &&
        !reload.analysis_attempted &&
        m_measuredCandidateAnalysisInFlight != static_cast<size_t>(-1) &&
        !reload.analysis_in_progress;
    reload.analysis_error =
        modelIndex < m_measuredCandidateAnalysisErrors.size()
            ? m_measuredCandidateAnalysisErrors[modelIndex].get_ptr()
            : nullptr;
    ::SendMessage(
        m_measuredCandidateDialog,
        measured_bpm_candidate_dialog::kReloadTrackMessage, 0,
        reinterpret_cast<LPARAM>(&reload));
}

void bpm_result_dialog::ClearMeasuredCandidateDialogSelection()
{
    if (!::IsWindow(m_measuredCandidateDialog)) return;
    static const std::vector<IBpmAnalyzer::MeasuredBpmCandidate>
        emptyCandidates;
    measured_bpm_candidate_reload reload;
    reload.candidates = &emptyCandidates;
    reload.track_label = "";
    reload.selection_available = false;
    ::SendMessage(
        m_measuredCandidateDialog,
        measured_bpm_candidate_dialog::kReloadTrackMessage, 0,
        reinterpret_cast<LPARAM>(&reload));
}

void bpm_result_dialog::FollowMeasuredCandidateSelection(size_t modelIndex)
{
    if (!::IsWindow(m_measuredCandidateDialog) ||
        modelIndex >= m_measured_bpm_candidate_results.size()) {
        return;
    }

    const bool attempted =
        modelIndex < m_measuredCandidateAnalysisAttempted.size() &&
        m_measuredCandidateAnalysisAttempted[modelIndex] != 0;
    if (m_measured_bpm_candidate_results[modelIndex].empty() && !attempted &&
        m_measuredCandidateAnalysisInFlight == static_cast<size_t>(-1)) {
        StartMeasuredCandidateAnalysis(modelIndex);
    }
    RefreshMeasuredCandidateDialog(modelIndex);
}

void bpm_result_dialog::StartMeasuredCandidateAnalysis(size_t modelIndex)
{
    if (modelIndex >= m_tracks.get_count() ||
        modelIndex >= m_measured_bpm_candidate_results.size() ||
        m_measuredCandidateAnalysisInFlight != static_cast<size_t>(-1)) {
        return;
    }

    auto result = std::make_shared<measured_bpm_candidate_analysis_result>();
    result->model_index = modelIndex;
    const metadb_handle_ptr track = m_tracks[modelIndex];
    const auto routingSnapshot = m_routing_context_results[modelIndex];
    const std::string trackIdentifier =
        std::string(get_artist(modelIndex).get_ptr()) + " - " +
        get_title(modelIndex).get_ptr();
    const HWND resultWindow = m_hWnd;
    m_measuredCandidateAnalysisInFlight = modelIndex;
    if (modelIndex < m_measuredCandidateAnalysisErrors.size()) {
        m_measuredCandidateAnalysisErrors[modelIndex].reset();
    }
    EnableMeasuredCandidatesButton();

    auto callback = threaded_process_callback_lambda::create(
        threaded_process_callback_lambda::on_init_t{},
        [track, result, routingSnapshot, trackIdentifier](
            threaded_process_status& status, abort_callback& abort) {
            try {
                status.set_item_path(track->get_path());
                auto analyzer = BpmAnalyzerFactory::create();
                if (!analyzer) {
                    result->error = "Could not initialize the BPM analyzer.";
                    return;
                }
                // This bounded inspection is audio-only and no-write. Reuse
                // the original routing snapshot because some measured probe
                // lanes are conditionally collected by material context.
                const auto routingContext = routingSnapshot.view();
                analyzer->analyze(
                    track, status, abort, routingContext.primary_min_bpm,
                    routingContext.primary_max_bpm,
                    trackIdentifier.c_str(), &routingContext, true);
                result->candidates =
                    analyzer->get_diagnostics().measured_bpm_candidates;
                if (result->candidates.empty()) {
                    result->error =
                        "No legal measured BPM candidates were produced.";
                }
            } catch (const pfc::exception& exception) {
                result->error = exception.what();
            } catch (...) {
                result->error = "Unknown candidate-analysis failure.";
            }
        },
        [resultWindow, result](threaded_process_callback::ctx_t, bool aborted) {
            if (aborted && result->error.is_empty()) {
                result->error = "Candidate analysis was cancelled.";
            }
            if (::IsWindow(resultWindow)) {
                ::SendMessage(
                    resultWindow,
                    bpm_result_dialog::kMeasuredCandidateAnalysisDoneMessage,
                    aborted ? 1 : 0,
                    reinterpret_cast<LPARAM>(result.get()));
            }
        });

    try {
        threaded_process::g_run_modeless(
            callback,
            threaded_process::flag_show_abort |
                threaded_process::flag_show_item |
                threaded_process::flag_show_delayed,
            m_hWnd, "Measuring BPM candidates");
    } catch (const pfc::exception& exception) {
        m_measuredCandidateAnalysisInFlight = static_cast<size_t>(-1);
        EnableMeasuredCandidatesButton();
        FB2K_console_formatter()
            << "foo_smart_tempo: Could not start Measured BPM inspection: "
            << exception.what();
    } catch (...) {
        m_measuredCandidateAnalysisInFlight = static_cast<size_t>(-1);
        EnableMeasuredCandidatesButton();
        FB2K_console_formatter()
            << "foo_smart_tempo: Could not start Measured BPM inspection.";
    }
}

LRESULT bpm_result_dialog::OnMeasuredCandidateAnalysisDone(
    UINT, WPARAM aborted, LPARAM resultData)
{
    const auto* result =
        reinterpret_cast<const measured_bpm_candidate_analysis_result*>(
            resultData);
    if (result == nullptr ||
        result->model_index != m_measuredCandidateAnalysisInFlight) {
        return 0;
    }
    const size_t modelIndex = result->model_index;
    m_measuredCandidateAnalysisInFlight = static_cast<size_t>(-1);
    if (modelIndex < m_measuredCandidateAnalysisAttempted.size()) {
        m_measuredCandidateAnalysisAttempted[modelIndex] = 1;
    }
    if (modelIndex < m_measuredCandidateAnalysisErrors.size()) {
        m_measuredCandidateAnalysisErrors[modelIndex] = result->error;
    }
    if (aborted == 0 && modelIndex < m_measured_bpm_candidate_results.size()) {
        m_measured_bpm_candidate_results[modelIndex] = result->candidates;
    }

    if (!result->error.is_empty()) {
        FB2K_console_formatter()
            << "foo_smart_tempo: [Measured BPM inspection] track="
            << m_tracks[modelIndex]->get_path() << ", result=\""
            << result->error.get_ptr() << "\"";
    }
    EnableMeasuredCandidatesButton();

    CListViewCtrl list(GetDlgItem(ID_BPM_RESULT_LIST));
    const int selectedRow = ListView_GetNextItem(list, -1, LVIS_SELECTED);
    if (selectedRow >= 0 &&
        ListView_GetNextItem(list, selectedRow, LVIS_SELECTED) < 0) {
        const size_t selectedModelIndex =
            static_cast<size_t>(list.GetItemData(selectedRow));
        if (::IsWindow(m_measuredCandidateDialog)) {
            FollowMeasuredCandidateSelection(selectedModelIndex);
        } else if (selectedModelIndex == modelIndex &&
                   !m_measured_bpm_candidate_results[modelIndex].empty()) {
            OnMeasuredCandidatesClicked(
                0, ID_RESULT_MEASURED_CANDIDATES_BUTTON,
                GetDlgItem(ID_RESULT_MEASURED_CANDIDATES_BUTTON));
        }
    }
    return 0;
}

LRESULT bpm_result_dialog::OnMeasuredCandidateSelected(
    UINT, WPARAM sourceWindow, LPARAM selectionData)
{
    if (reinterpret_cast<HWND>(sourceWindow) == m_measuredCandidateDialog) {
        m_measuredCandidateDialog = nullptr;
    }
    const auto* selection =
        reinterpret_cast<const measured_bpm_candidate_selection*>(selectionData);
    if (selection != nullptr) {
        ApplyMeasuredCandidateSelection(selection->model_index,
                                        selection->candidate_index);
    }
    return 0;
}

LRESULT bpm_result_dialog::OnMeasuredCandidateFollowSelection(
    UINT, WPARAM, LPARAM)
{
    if (!::IsWindow(m_measuredCandidateDialog)) return 0;
    CListViewCtrl list(GetDlgItem(ID_BPM_RESULT_LIST));
    const int selectedRow = ListView_GetNextItem(list, -1, LVIS_SELECTED);
    if (selectedRow < 0 ||
        ListView_GetNextItem(list, selectedRow, LVIS_SELECTED) >= 0) {
        ClearMeasuredCandidateDialogSelection();
        return 0;
    }
    FollowMeasuredCandidateSelection(
        static_cast<size_t>(list.GetItemData(selectedRow)));
    return 0;
}

void bpm_result_dialog::ApplyMeasuredCandidateSelection(
    size_t modelIndex, size_t candidateIndex)
{
    if (modelIndex >= m_measured_bpm_candidate_results.size()) return;
    if (candidateIndex >= m_measured_bpm_candidate_results[modelIndex].size()) {
        return;
    }
    const auto& candidate =
        m_measured_bpm_candidate_results[modelIndex][candidateIndex];
    if (!std::isfinite(candidate.bpm) || candidate.bpm < 40.0 ||
        candidate.bpm > 220.0) {
        return;
    }

    CListViewCtrl list(GetDlgItem(ID_BPM_RESULT_LIST));
    int resultRow = -1;
    for (int row = 0; row < list.GetItemCount(); ++row) {
        if (static_cast<size_t>(list.GetItemData(row)) == modelIndex) {
            resultRow = row;
            break;
        }
    }
    if (resultRow < 0) return;

    m_bpm_results[modelIndex] = candidate.bpm;
    m_confidence_results[modelIndex] = 0.0;
    m_suppress_write_results[modelIndex] = 0;
    m_method_results[modelIndex] = "Measured BPM (manual choice)";
    m_error_results[modelIndex].reset();

    format_bpm bpmValue(candidate.bpm, get_write_precision());
    m_infos[modelIndex].meta_set(bpm_config_bpm_tag.get().get_ptr(), bpmValue);
    pfc::stringcvt::string_wide_from_utf8 bpmW((const char*)bpmValue);
    const int bpmColumn = ModelToViewColumn(ColumnBpm);
    if (bpmColumn >= 0) list.SetItemText(resultRow, bpmColumn, bpmW);
    const int confidenceColumn = ModelToViewColumn(ColumnConfidence);
    if (confidenceColumn >= 0) list.SetItemText(resultRow, confidenceColumn, L"-");
    const int methodColumn = ModelToViewColumn(ColumnMethod);
    if (methodColumn >= 0) {
        list.SetItemText(resultRow, methodColumn,
                         L"Measured BPM (manual choice)");
    }
    const int errorColumn = ModelToViewColumn(ColumnError);
    if (errorColumn >= 0) list.SetItemText(resultRow, errorColumn, L"");
    m_methodWide[modelIndex] = L"Measured BPM (manual choice)";
    m_errorWide[modelIndex].clear();

    pfc::string_formatter trackLabel;
    trackLabel << get_artist(modelIndex).get_ptr() << " - "
               << get_title(modelIndex).get_ptr();
    FB2K_console_formatter()
        << "foo_smart_tempo: [" << trackLabel.get_ptr()
        << "] User selected measured candidate: bpm="
        << candidate.bpm << ", evidence_rank="
        << (uint64_t)candidate.evidence_rank << ", evidence_score="
        << candidate.evidence_score << ", support=" << candidate.support
        << ", winner_support=" << candidate.winner_support
        << ", local_exact_score=" << candidate.local_exact_score
        << ", alias_classes=" << candidate.alias_classes.get_ptr()
        << ", sources=" << candidate.sources.get_ptr();

    InvalidateColumnContentWidthCache();
    UpdateBpmTagLabel();
    EnableScaleBPMButtons();
    EnableMeasuredCandidatesButton();
}

void bpm_result_dialog::CloseMeasuredCandidateDialog()
{
    KillTimer(kMeasuredCandidateSelectionTimer);
    if (::IsWindow(m_measuredCandidateDialog)) {
        ::DestroyWindow(m_measuredCandidateDialog);
    }
    m_measuredCandidateDialog = nullptr;
}

LRESULT bpm_result_dialog::OnPreferencesClicked(UINT uNotifyCode, int nID, CWindow wndCtl)
{
    (void)uNotifyCode;
    (void)nID;
    (void)wndCtl;
    fb2k::std_api_get<ui_control>()->show_preferences(guid_bpm_preferences);
    return 0;
}

LRESULT bpm_result_dialog::OnItemChanged(LPNMHDR pnmh)
{
    EnableScaleBPMButtons();
    EnableMeasuredCandidatesButton();
    const auto* change = reinterpret_cast<const NMLISTVIEW*>(pnmh);
    const bool selectionChanged = change != nullptr && change->iItem >= 0 &&
        (change->uChanged & LVIF_STATE) != 0 &&
        ((change->uOldState ^ change->uNewState) & LVIS_SELECTED) != 0;
    if (selectionChanged && ::IsWindow(m_measuredCandidateDialog)) {
        // Wait briefly for a stable single-row selection. This avoids
        // analyzing every transient row while the user navigates quickly.
        KillTimer(kMeasuredCandidateSelectionTimer);
        SetTimer(kMeasuredCandidateSelectionTimer,
                 kMeasuredCandidateSelectionDelayMs);
    }
    return 0;
}

LRESULT bpm_result_dialog::OnResultDoubleClick(LPNMHDR pnmh)
{
    const auto* activation = reinterpret_cast<const NMITEMACTIVATE*>(pnmh);
    if (activation == nullptr || activation->iItem < 0) return 0;
    CListViewCtrl list(GetDlgItem(ID_BPM_RESULT_LIST));
    const size_t modelIndex = static_cast<size_t>(
        list.GetItemData(activation->iItem));
    if (modelIndex >= m_tracks.get_count()) return 0;

    static_api_ptr_t<playlist_manager> playlists;
    const metadb_handle_ptr track = m_tracks[modelIndex];
    t_size playlist = playlists->get_active_playlist();
    t_size item = SIZE_MAX;
    if (playlist == SIZE_MAX ||
        !playlists->playlist_find_item(playlist, track, item)) {
        playlist = SIZE_MAX;
        const t_size count = playlists->get_playlist_count();
        for (t_size index = 0; index < count; ++index) {
            if (playlists->playlist_find_item(index, track, item)) {
                playlist = index;
                break;
            }
        }
    }
    if (playlist == SIZE_MAX || item == SIZE_MAX) {
        FB2K_console_formatter()
            << "foo_smart_tempo: Result double-click could not locate track "
            << "in a playlist: " << track->get_path();
        return 0;
    }

    playlists->set_active_playlist(playlist);
    playlists->playlist_clear_selection(playlist);
    playlists->playlist_set_selection_single(playlist, item, true);
    playlists->playlist_set_focus_item(playlist, item);
    playlists->playlist_ensure_visible(playlist, item);
    playlists->playlist_execute_default_action(playlist, item);
    return 0;
}

LRESULT bpm_result_dialog::OnColumnClick(LPNMHDR pnmh)
{
    const auto* lv = reinterpret_cast<const NMLISTVIEW*>(pnmh);
    if (lv == nullptr) return 0;
    const int modelColumn = ViewToModelColumn(lv->iSubItem);
    if (modelColumn < 0) return 0;

    if (m_sortColumn == modelColumn) {
        m_sortAscending = !m_sortAscending;
    } else {
        m_sortColumn = modelColumn;
        m_sortAscending = true;
    }
    ApplySorting();
    return 0;
}

LRESULT bpm_result_dialog::OnHeaderChanged(LPNMHDR pnmh)
{
    (void)pnmh;
    m_hasCustomLayout = true;
    SaveListLayout();
    UpdateResultColumns();
    return 0;
}

void bpm_result_dialog::OnSize(UINT nType, CSize size)
{
    (void)size;
    if (nType == SIZE_MINIMIZED) return;
    SetMsgHandled(FALSE);
    if (m_layoutUpdatePending) return;
    m_layoutUpdatePending = true;
    if (!::PostMessage(m_hWnd, WM_APP + 121, 0, 0)) {
        m_layoutUpdatePending = false;
    }
}

void bpm_result_dialog::OnTimer(UINT_PTR timerId)
{
    if (timerId != kMeasuredCandidateSelectionTimer) return;
    KillTimer(kMeasuredCandidateSelectionTimer);
    OnMeasuredCandidateFollowSelection(0, 0, 0);
}

LRESULT bpm_result_dialog::OnDpiChanged(UINT, WPARAM, LPARAM lParam)
{
    const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
    if (suggested != nullptr) {
        SetWindowPos(NULL, suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }

    RECT minDlu = { 0, 0, 395, 242 };
    MapDialogRect(&minDlu);
    m_minTrackSize.cx = minDlu.right;
    m_minTrackSize.cy = minDlu.bottom;

    CRect rc;
    GetClientRect(&rc);
    m_layoutUpdatePending = false;
    InvalidateColumnContentWidthCache();
    DlgResize_UpdateLayout(rc.Width(), rc.Height());
    UpdateResultColumns();
    return 0;
}

LRESULT bpm_result_dialog::OnLayoutChanged(UINT, WPARAM, LPARAM)
{
    m_layoutUpdatePending = false;
    UpdateResultColumns();
    return 0;
}

void bpm_result_dialog::OnClose()
{
    SaveListLayout();
    SaveWindowPlacement();
    CloseMeasuredCandidateDialog();
    DestroyWindow();
}

void bpm_result_dialog::PostNcDestroy()
{
    CloseMeasuredCandidateDialog();
    static_api_ptr_t<message_loop>()->remove_message_filter(this);
    delete this;
}

bool bpm_result_dialog::pretranslate_message(MSG* p_msg)
{
    if (m_tooltip.IsWindow()) m_tooltip.RelayEvent(p_msg);
    if (m_hWnd != NULL) {
        if (IsDialogMessage(p_msg)) return true;
    }
    return false;
}

void bpm_result_dialog::AddTooltip(int controlID, const TCHAR* text)
{
    if (!m_tooltip.IsWindow()) return;
    CWindow ctrl(GetDlgItem(controlID));
    if (!ctrl.IsWindow()) return;

    TOOLINFO ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = m_hWnd;
    ti.uId = (UINT_PTR)ctrl.m_hWnd;
    ti.lpszText = const_cast<LPTSTR>(text);
    m_tooltip.AddTool(&ti);
}

void bpm_result_dialog::InitTooltips()
{
    if (m_tooltip.IsWindow()) return;
    if (!m_tooltip.Create(m_hWnd, NULL, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP)) return;

    m_tooltip.SetMaxTipWidth(560);
    m_tooltip.SetDelayTime(TTDT_INITIAL, 350);
    m_tooltip.Activate(TRUE);

    AddTooltip(ID_BPM_RESULT_LIST,
        _T("Result columns: Artist, Title, Genre, BPM, Confidence, Method, Error. Review marks an intentionally unwritten result. Confidence is a relative stability estimate from candidate consensus across analysis windows (higher = more stable)."));
    AddTooltip(ID_RESULT_BPM_TAG, _T("Shows which tag field will be written when you click Update files."));
    AddTooltip(ID_DOUBLE_BPM_BUTTON, _T("Multiply BPM for selected rows by 2."));
    AddTooltip(ID_HALVE_BPM_BUTTON, _T("Divide BPM for selected rows by 2."));
    AddTooltip(ID_RESULT_MANUAL_BUTTON, _T("Open Manual BPM Calculation (tap tempo) for the current track."));
    AddTooltip(ID_RESULT_MEASURED_CANDIDATES_BUTTON,
        _T("Inspect BPM values that Hodgkinson/MIR actually measured for one selected track. Review rows reuse their retained candidates; other rows run one bounded audio-only, no-write inspection. This never creates, averages, folds, or estimates a BPM."));
    AddTooltip(ID_RESULT_PREFERENCES_BUTTON, _T("Open the Smart Tempo preferences page."));
    AddTooltip(IDOK, _T("Write selected BPM results to file tags."));
    AddTooltip(IDCANCEL, _T("Close without writing changes."));
}

void bpm_result_dialog::UpdateBpmTagLabel()
{
    const size_t suppressWriteCount = static_cast<size_t>(std::count(
        m_suppress_write_results.begin(), m_suppress_write_results.end(),
        static_cast<uint8_t>(1)));

    pfc::string_formatter bpm_tag_label;
    bpm_tag_label << "BPM will be written to %" << bpm_config_bpm_tag.get().get_ptr() << "% tag.";
    if (suppressWriteCount > 0) {
        bpm_tag_label << " " << (uint64_t)suppressWriteCount
                      << " MIR review-hold result"
                      << (suppressWriteCount == 1 ? " is" : "s are")
                      << " shown but will not be written automatically.";
    }
    uSetDlgItemText(m_hWnd, ID_RESULT_BPM_TAG, bpm_tag_label);
}
void bpm_result_dialog::EnableScaleBPMButtons()
{
    CListViewCtrl listView(GetDlgItem(ID_BPM_RESULT_LIST));
    bool hasScalableSelection = false;
    int item = -1;
    while ((item = ListView_GetNextItem(listView, item, LVIS_SELECTED)) != -1) {
        const size_t modelIndex = static_cast<size_t>(listView.GetItemData(item));
        if (modelIndex < m_bpm_results.size() &&
            modelIndex < m_suppress_write_results.size() &&
            m_suppress_write_results[modelIndex] == 0 &&
            std::isfinite(m_bpm_results[modelIndex]) &&
            m_bpm_results[modelIndex] > 0.0) {
            hasScalableSelection = true;
            break;
        }
    }
    GetDlgItem(ID_DOUBLE_BPM_BUTTON).EnableWindow(hasScalableSelection);
    GetDlgItem(ID_HALVE_BPM_BUTTON).EnableWindow(hasScalableSelection);
}

void bpm_result_dialog::EnableMeasuredCandidatesButton()
{
    CListViewCtrl list(GetDlgItem(ID_BPM_RESULT_LIST));
    int selectedCount = 0;
    bool canInspect = false;
    int row = -1;
    while ((row = ListView_GetNextItem(list, row, LVIS_SELECTED)) != -1) {
        ++selectedCount;
        const size_t modelIndex = static_cast<size_t>(list.GetItemData(row));
        if (modelIndex < m_measured_bpm_candidate_results.size()) {
            const bool hasCandidates =
                !m_measured_bpm_candidate_results[modelIndex].empty();
            const bool attempted =
                modelIndex < m_measuredCandidateAnalysisAttempted.size() &&
                m_measuredCandidateAnalysisAttempted[modelIndex] != 0;
            canInspect = hasCandidates || !attempted;
            if (modelIndex == m_measuredCandidateAnalysisInFlight) {
                canInspect = false;
            }
        }
        if (selectedCount > 1) break;
    }
    GetDlgItem(ID_RESULT_MEASURED_CANDIDATES_BUTTON)
        .EnableWindow(selectedCount == 1 && canInspect);
}

void bpm_result_dialog::ScaleSelectionBPM(double p_factor)
{
    CListViewCtrl result_list(GetDlgItem(ID_BPM_RESULT_LIST));
    const int bpmColumn = ModelToViewColumn(ColumnBpm);
    if (bpmColumn < 0) return;

    int listview_index = -1;
    while ((listview_index = ListView_GetNextItem(result_list, listview_index, LVIS_SELECTED)) != -1)
    {
        const size_t modelIndex = (size_t)result_list.GetItemData(listview_index);
        if (modelIndex >= m_bpm_results.size()) continue;
		if (modelIndex >= m_suppress_write_results.size() ||
		    m_suppress_write_results[modelIndex] != 0 ||
		    !std::isfinite(m_bpm_results[modelIndex]) ||
		    !(m_bpm_results[modelIndex] > 0.0)) {
			continue;
		}

        m_bpm_results[modelIndex] = m_bpm_results[modelIndex] * p_factor;

        format_bpm bpm_value(m_bpm_results[modelIndex], get_write_precision());
        m_infos[modelIndex].meta_set(bpm_config_bpm_tag.get().get_ptr(), bpm_value);
        pfc::stringcvt::string_wide_from_utf8 bpmW((const char*)bpm_value);
        result_list.SetItemText(listview_index, bpmColumn, bpmW);
    }

    InvalidateColumnContentWidthCache();
    UpdateBpmTagLabel();
}

void bpm_result_dialog::LoadListLayout()
{
    CListViewCtrl result_list(GetDlgItem(ID_BPM_RESULT_LIST));
    if (!result_list.IsWindow()) return;

    const int visibleCount = VisibleColumnCount();
    if (visibleCount <= 0) return;

    std::vector<int> storedModelOrder =
        parseCSVInts(bpm_result_columns_order.get().get_ptr());
    if ((int)storedModelOrder.size() != kModelColumnCount) {
        storedModelOrder.clear();
        for (int i = 0; i < kModelColumnCount; ++i) {
            storedModelOrder.push_back(i);
        }
    }

    std::vector<int> filteredModelOrder;
    filteredModelOrder.reserve((size_t)visibleCount);
    for (int modelColumn : storedModelOrder) {
        if (ModelToViewColumn(modelColumn) >= 0) {
            filteredModelOrder.push_back(modelColumn);
        }
    }
    if ((int)filteredModelOrder.size() != visibleCount) {
        filteredModelOrder = m_visibleColumns;
    }

    std::vector<int> viewOrder((size_t)visibleCount, 0);
    for (int i = 0; i < visibleCount; ++i) {
        viewOrder[(size_t)i] = ModelToViewColumn(filteredModelOrder[(size_t)i]);
    }
    ListView_SetColumnOrderArray(result_list, visibleCount, viewOrder.data());

    std::vector<int> storedModelWidths =
        parseCSVInts(bpm_result_columns_width.get().get_ptr());
    if ((int)storedModelWidths.size() != kModelColumnCount) {
        storedModelWidths.assign(kDefaultColumnPercents,
                                 kDefaultColumnPercents + kModelColumnCount);
        m_hasCustomLayout = false;
    } else {
        m_hasCustomLayout = true;
    }

    m_columnWidths.clear();
    m_columnWidths.reserve((size_t)visibleCount);
    for (int modelColumn : m_visibleColumns) {
        int widthPct = storedModelWidths[(size_t)modelColumn];
        if (widthPct < 2) widthPct = 2;
        m_columnWidths.push_back(widthPct);
    }

    int sumPct = 0;
    for (int pct : m_columnWidths) sumPct += pct;
    if (sumPct <= 0) {
        m_columnWidths.clear();
        for (int modelColumn : m_visibleColumns) {
            m_columnWidths.push_back(kDefaultColumnPercents[(size_t)modelColumn]);
        }
        sumPct = 0;
        for (int pct : m_columnWidths) sumPct += pct;
    }
    int accum = 0;
    for (int i = 0; i < visibleCount - 1; ++i) {
        m_columnWidths[(size_t)i] = (m_columnWidths[(size_t)i] * 100) / sumPct;
        if (m_columnWidths[(size_t)i] < 2) m_columnWidths[(size_t)i] = 2;
        accum += m_columnWidths[(size_t)i];
    }
    m_columnWidths[(size_t)(visibleCount - 1)] = (std::max)(2, 100 - accum);
}

void bpm_result_dialog::SaveListLayout()
{
    CListViewCtrl result_list(GetDlgItem(ID_BPM_RESULT_LIST));
    if (!result_list.IsWindow()) return;
    const int visibleCount = VisibleColumnCount();
    if (visibleCount <= 0) return;
    if (result_list.GetHeader().GetItemCount() < visibleCount) return;

    std::vector<int> viewOrder((size_t)visibleCount, 0);
    if (ListView_GetColumnOrderArray(result_list, visibleCount, viewOrder.data())) {
        std::vector<int> modelOrder;
        modelOrder.reserve((size_t)kModelColumnCount);
        for (int viewColumn : viewOrder) {
            const int modelColumn = ViewToModelColumn(viewColumn);
            if (modelColumn >= 0) {
                modelOrder.push_back(modelColumn);
            }
        }
        for (int modelColumn = 0; modelColumn < kModelColumnCount; ++modelColumn) {
            if (std::find(modelOrder.begin(), modelOrder.end(), modelColumn) ==
                modelOrder.end()) {
                modelOrder.push_back(modelColumn);
            }
        }
        bpm_result_columns_order = toCSV(modelOrder).get_ptr();
    }

    std::vector<int> visibleWidths((size_t)visibleCount, 0);
    int sum = 0;
    for (int i = 0; i < visibleCount; ++i) {
        visibleWidths[(size_t)i] = result_list.GetColumnWidth(i);
        if (visibleWidths[(size_t)i] < 1) visibleWidths[(size_t)i] = 1;
        sum += visibleWidths[(size_t)i];
    }
    if (sum <= 0) return;

    std::vector<int> visiblePct((size_t)visibleCount, 0);
    int accum = 0;
    for (int i = 0; i < visibleCount - 1; ++i) {
        visiblePct[(size_t)i] = (visibleWidths[(size_t)i] * 100) / sum;
        accum += visiblePct[(size_t)i];
    }
    visiblePct[(size_t)(visibleCount - 1)] = 100 - accum;

    std::vector<int> modelPct =
        parseCSVInts(bpm_result_columns_width.get().get_ptr());
    if ((int)modelPct.size() != kModelColumnCount) {
        modelPct.assign(kDefaultColumnPercents,
                        kDefaultColumnPercents + kModelColumnCount);
    }
    for (size_t i = 0; i < m_visibleColumns.size() && i < visiblePct.size(); ++i) {
        const int modelColumn = m_visibleColumns[i];
        modelPct[(size_t)modelColumn] = visiblePct[i];
    }

    bpm_result_columns_width = toCSV(modelPct).get_ptr();
    m_columnWidths = visiblePct;
    m_hasCustomLayout = true;
}

void bpm_result_dialog::LoadWindowPlacement()
{
    int w = (int)bpm_result_window_width;
    int h = (int)bpm_result_window_height;
    if (w < m_minTrackSize.cx) w = m_minTrackSize.cx;
    if (h < m_minTrackSize.cy) h = m_minTrackSize.cy;

    int x = (int)bpm_result_window_x;
    int y = (int)bpm_result_window_y;

    if (x == INT_MIN || y == INT_MIN) {
        SetWindowPos(NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        return;
    }

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vw > 0 && vh > 0) {
        const int minVisible = 80;
        const int minX = vx - (w - minVisible);
        const int maxX = vx + vw - minVisible;
        const int minY = vy - (h - minVisible);
        const int maxY = vy + vh - minVisible;

        if (x < minX) x = minX;
        if (x > maxX) x = maxX;
        if (y < minY) y = minY;
        if (y > maxY) y = maxY;
    }

    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    wp.flags = 0;
    wp.showCmd = ((int)bpm_result_window_show == SW_SHOWMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    wp.rcNormalPosition.left = x;
    wp.rcNormalPosition.top = y;
    wp.rcNormalPosition.right = x + w;
    wp.rcNormalPosition.bottom = y + h;
    SetWindowPlacement(&wp);
}

void bpm_result_dialog::SaveWindowPlacement()
{
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);

    if (!GetWindowPlacement(&wp)) {
        CRect wr;
        GetWindowRect(&wr);
        if (wr.Width() >= m_minTrackSize.cx) bpm_result_window_width = wr.Width();
        if (wr.Height() >= m_minTrackSize.cy) bpm_result_window_height = wr.Height();
        if (!IsIconic()) {
            bpm_result_window_x = wr.left;
            bpm_result_window_y = wr.top;
        }
        bpm_result_window_show = IsZoomed() ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        return;
    }

    const RECT& rn = wp.rcNormalPosition;
    const int w = rn.right - rn.left;
    const int h = rn.bottom - rn.top;

    if (w >= m_minTrackSize.cx) bpm_result_window_width = w;
    if (h >= m_minTrackSize.cy) bpm_result_window_height = h;

    if (!IsIconic()) {
        bpm_result_window_x = rn.left;
        bpm_result_window_y = rn.top;
    }

    bpm_result_window_show = (wp.showCmd == SW_SHOWMAXIMIZED || IsZoomed()) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
}

int bpm_result_dialog::ComputeColumnContentWidth(CListViewCtrl& listView, int column, const wchar_t* header) const
{
    int width = listView.GetStringWidth(header);
    if (width < 0) width = 0;

    wchar_t buffer[1024] = {};
    const int count = listView.GetItemCount();
    for (int i = 0; i < count; ++i) {
        buffer[0] = 0;
        listView.GetItemText(i, column, buffer, _countof(buffer));
        const int w = listView.GetStringWidth(buffer);
        if (w > width) width = w;
    }
    return width + scale_for_window_dpi(m_hWnd, 24);
}

void bpm_result_dialog::InvalidateColumnContentWidthCache()
{
    m_columnContentWidthCache.clear();
    m_columnContentWidthCacheValid = false;
}

void bpm_result_dialog::RebuildColumnContentWidthCache(CListViewCtrl& listView)
{
    if (!listView.IsWindow()) return;
    const int visibleCount = VisibleColumnCount();
    if (visibleCount <= 0) return;

    m_columnContentWidthCache.assign((size_t)visibleCount, 0);
    for (int i = 0; i < visibleCount; ++i) {
        const int modelColumn = m_visibleColumns[(size_t)i];
        m_columnContentWidthCache[(size_t)i] =
            ComputeColumnContentWidth(listView, i, kColumnHeaders[(size_t)modelColumn]);
    }
    m_columnContentWidthCacheValid = true;
}

void bpm_result_dialog::UpdateResultColumns()
{
    CListViewCtrl result_list(GetDlgItem(ID_BPM_RESULT_LIST));
    if (!result_list.IsWindow()) return;
    const int visibleCount = VisibleColumnCount();
    if (visibleCount <= 0) return;
    if (result_list.GetHeader().GetItemCount() < visibleCount) return;

    if ((int)m_columnWidths.size() != visibleCount) {
        m_columnWidths.clear();
        for (int modelColumn : m_visibleColumns) {
            m_columnWidths.push_back(kDefaultColumnPercents[(size_t)modelColumn]);
        }
        m_hasCustomLayout = false;
    }

    CRect rc;
    result_list.GetClientRect(&rc);
    int total = rc.Width();
    if (total <= 0) return;

    if (result_list.GetItemCount() > result_list.GetCountPerPage()) {
        total -= GetSystemMetrics(SM_CXVSCROLL);
    }
    const int minTotalWidth = scale_for_window_dpi(m_hWnd, 380);
    if (total < minTotalWidth) total = minTotalWidth;

    if (!m_columnContentWidthCacheValid ||
        (int)m_columnContentWidthCache.size() != visibleCount) {
        RebuildColumnContentWidthCache(result_list);
    }

    std::vector<int> minW((size_t)visibleCount, 40);
    for (int i = 0; i < visibleCount; ++i) {
        const int modelColumn = m_visibleColumns[(size_t)i];
        const int cachedContentWidth =
            (i < (int)m_columnContentWidthCache.size())
                ? m_columnContentWidthCache[(size_t)i]
                : ComputeColumnContentWidth(result_list, i,
                                            kColumnHeaders[(size_t)modelColumn]);
        minW[(size_t)i] = std::max(
            scale_for_window_dpi(m_hWnd, kMinimumColumnWidths[(size_t)modelColumn]),
            cachedContentWidth);
    }

    std::vector<int> widths((size_t)visibleCount, 0);
    if (m_hasCustomLayout) {
        for (int i = 0; i < visibleCount; ++i) {
            widths[i] = (total * m_columnWidths[(size_t)i]) / 100;
            if (widths[i] < 1) widths[i] = 1;
        }
    } else {
        widths = minW;
    }

    int sum = 0;
    for (int v : widths) sum += v;

    if (sum < total) {
        const int titleCol = ModelToViewColumn(ColumnTitle);
        if (titleCol >= 0 && titleCol < visibleCount) {
            widths[(size_t)titleCol] += (total - sum);
        } else {
            widths[0] += (total - sum);
        }
    } else if (sum > total) {
        int over = sum - total;
        for (int modelColumn : kShrinkPriority) {
            if (over <= 0) break;
            const int idx = ModelToViewColumn(modelColumn);
            if (idx < 0 || idx >= visibleCount) continue;
            int reducible = widths[(size_t)idx] - minW[(size_t)idx];
            if (reducible <= 0) continue;
            int take = (over < reducible) ? over : reducible;
            widths[(size_t)idx] -= take;
            over -= take;
        }
    }

    for (int i = 0; i < visibleCount; ++i) {
        if (widths[(size_t)i] < minW[(size_t)i]) widths[(size_t)i] = minW[(size_t)i];
    }

    sum = 0;
    for (int v : widths) sum += v;
    if (sum < total) {
        const int titleCol = ModelToViewColumn(ColumnTitle);
        if (titleCol >= 0 && titleCol < visibleCount) {
            widths[(size_t)titleCol] += (total - sum);
        } else {
            widths[0] += (total - sum);
        }
    }

    for (int i = 0; i < visibleCount; ++i) {
        result_list.SetColumnWidth(i, widths[(size_t)i]);
    }
}














