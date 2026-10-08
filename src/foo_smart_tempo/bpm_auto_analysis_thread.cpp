#include "stdafx.h"
#include "foo_smart_tempo.h"

#include "bpm_auto_analysis_thread.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <string>

#include "analysis_telemetry.h"
#include "bpm_analyzer_factory.h"
#include "bpm_result_dialog.h"
#include "experimental_feature_flags.h"
#include "format_bpm.h"
#include "mir_candidate_pipeline.h"
#include "modern_bpm_analyzer.h"
#include "preferences.h"
#include "smart_tempo_helpers.h"
#include "smart_tempo_mapper.h"

namespace {
class null_threaded_status : public threaded_process_status {};

using smart_tempo::helpers::normalize_routing_script_text;
using smart_tempo::helpers::tokenize_genre_text;

pfc::string8 make_track_identifier(metadb_handle_ptr item,
                                   const file_info_impl* info) {
  if (info != nullptr) {
    const char* artist = info->meta_get("ARTIST", 0);
    const char* title = info->meta_get("TITLE", 0);
    if (artist != nullptr && artist[0] != '\0' && title != nullptr &&
        title[0] != '\0') {
      pfc::string_formatter out;
      out << artist << " - " << title;
      return out;
    }
    if (title != nullptr && title[0] != '\0') {
      return pfc::string8(title);
    }
  }

  if (item.is_valid()) {
    const pfc::string8 fileName = pfc::string_filename_ext(item->get_path());
    if (!fileName.is_empty()) return fileName;
    const char* path = item->get_path();
    if (path != nullptr && path[0] != '\0') return pfc::string8(path);
  }

  return pfc::string8("<unknown>");
}

std::string escape_routing_log_value(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string join_normalized_genre_tokens(
    const std::vector<smart_tempo::helpers::genre_token_info>& tokens) {
  if (tokens.empty()) return "<empty>";

  std::string out;
  for (const auto& token : tokens) {
    if (token.normalized.empty()) continue;
    if (!out.empty()) out += "; ";
    out += token.normalized;
  }
  return out.empty() ? std::string("<empty>") : out;
}

service_ptr_t<titleformat_object> compile_routing_script_with_warning(
    const std::string& scriptText, const char* context) {
  service_ptr_t<titleformat_object> script;
  if (titleformat_compiler::get()->compile(script, scriptText.c_str())) {
    return script;
  }

  FB2K_console_formatter()
      << "foo_smart_tempo: [Routing] invalid routing script"
      << (context != nullptr ? context : "")
      << " -> falling back to %genre%: " << scriptText.c_str();
  titleformat_compiler::get()->compile_force(script, "%genre%");
  return script;
}

struct review_playlist_reconcile_stats {
  t_size removed_occurrences = 0;
  t_size added_tracks = 0;
  bool removal_blocked = false;
};

review_playlist_reconcile_stats reconcile_review_playlist(
    playlist_manager* playlistMan, t_size playlist,
    metadb_handle_list_cref analyzedTracks,
    metadb_handle_list_cref reviewHoldTracks) {
  review_playlist_reconcile_stats stats;
  if (playlistMan == nullptr) return stats;

  metadb_handle_list analyzedUnique;
  analyzedUnique.add_items(analyzedTracks);
  analyzedUnique.sort_by_pointer_remove_duplicates();

  metadb_handle_list existing;
  playlistMan->playlist_get_all_items(playlist, existing);
  bit_array_bittable removeMask(existing.get_count());
  for (t_size index = 0; index < existing.get_count(); ++index) {
    if (analyzedUnique.bsearch_by_pointer(existing[index]) != SIZE_MAX) {
      removeMask.set(index, true);
      ++stats.removed_occurrences;
    }
  }

  if (stats.removed_occurrences > 0 &&
      !playlistMan->playlist_remove_items(playlist, removeMask)) {
    stats.removal_blocked = true;
    stats.removed_occurrences = 0;
  }

  metadb_handle_list toAdd;
  toAdd.add_items(reviewHoldTracks);
  toAdd.sort_by_pointer_remove_duplicates();

  if (stats.removal_blocked) {
    // A locked playlist cannot be reconciled, but it must not receive another
    // copy of an item that is already present.
    existing.sort_by_pointer_remove_duplicates();
    bit_array_bittable alreadyPresent(toAdd.get_count());
    for (t_size index = 0; index < toAdd.get_count(); ++index) {
      alreadyPresent.set(
          index, existing.bsearch_by_pointer(toAdd[index]) != SIZE_MAX);
    }
    toAdd.remove_mask(alreadyPresent);
  }

  if (toAdd.get_count() > 0 &&
      playlistMan->playlist_add_items_filter(playlist, toAdd, false)) {
    stats.added_tracks = toAdd.get_count();
  }
  return stats;
}

class bpm_overwrite_prompt_dialog
    : public CDialogImpl<bpm_overwrite_prompt_dialog> {
public:
  enum { IDD = IDD_BPM_OVERWRITE_PROMPT_DIALOG };

  bpm_overwrite_prompt_dialog(const wchar_t *promptText, const wchar_t* yesText,
                              const wchar_t* noText, bool allowCancel)
      : m_promptText(promptText != nullptr ? promptText : L""),
        m_yesText(yesText != nullptr ? yesText : L"Yes"),
        m_noText(noText != nullptr ? noText : L"No"),
        m_allowCancel(allowCancel) {}

  BEGIN_MSG_MAP_EX(bpm_overwrite_prompt_dialog)
  MSG_WM_INITDIALOG(OnInitDialog)
  COMMAND_ID_HANDLER_EX(IDYES, OnButtonClicked)
  COMMAND_ID_HANDLER_EX(IDNO, OnButtonClicked)
  COMMAND_ID_HANDLER_EX(IDCANCEL, OnButtonClicked)
  MESSAGE_HANDLER_EX(WM_DPICHANGED, OnDpiChanged)
  MSG_WM_CLOSE(OnClose)
  END_MSG_MAP()

private:
  BOOL OnInitDialog(CWindow, LPARAM) {
    m_dark_mode_hooks.AddDialogWithControls(m_hWnd);
    SetWindowTextW(L"Smart Tempo Analysis");
    SetDlgItemTextW(IDC_BPM_OVERWRITE_PROMPT_TEXT, m_promptText.c_str());
    SetDlgItemTextW(IDYES, m_yesText.c_str());
    SetDlgItemTextW(IDNO, m_noText.c_str());
    SetDlgItemTextW(IDCANCEL, L"Cancel");

    if (!m_allowCancel) {
      CWindow cancelBtn(GetDlgItem(IDCANCEL));
      if (cancelBtn.IsWindow())
        cancelBtn.ShowWindow(SW_HIDE);
    }

    HWND yesButton = GetDlgItem(IDYES);
    HWND noButton = GetDlgItem(IDNO);
    if (yesButton != nullptr) {
      ::SendMessage(yesButton, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
    }
    if (noButton != nullptr) {
      ::SendMessage(noButton, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
      ::SendMessage(m_hWnd, DM_SETDEFID, IDNO, 0);
      ::SetFocus(noButton);
    }

    CenterWindow(core_api::get_main_window());
    return FALSE;
  }

  void OnButtonClicked(UINT, int buttonID, CWindow) { EndDialog(buttonID); }

  LRESULT OnDpiChanged(UINT, WPARAM, LPARAM lParam) {
    const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
    if (suggested != nullptr) {
      SetWindowPos(NULL, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return 0;
  }

  void OnClose() { EndDialog(m_allowCancel ? IDCANCEL : IDNO); }

  std::wstring m_promptText;
  std::wstring m_yesText;
  std::wstring m_noText;
  bool m_allowCancel = true;
  fb2k::CCoreDarkModeHooks m_dark_mode_hooks;
};
} // namespace

bpm_auto_analysis_thread::bpm_auto_analysis_thread(
    metadb_handle_list_cref p_tracks,
    bool p_measuredCandidateInspectionMode)
    : m_measuredCandidateInspectionMode(
          p_measuredCandidateInspectionMode) {
  m_tracks.add_items(p_tracks);
  m_infos.set_size(m_tracks.get_count());
}

bool bpm_auto_analysis_thread::start() {
  bool anyBpmMissing = false;
  bool anyBpmExisting = false;
  bool rescanExisting = false;
  bool canceledByUser = false;
  const pfc::string8 bpmTag = bpm_config_bpm_tag.get();

  {
    bit_array_bittable mask(m_tracks.get_size());

    for (t_size index = 0; index < m_tracks.get_size(); index++) {
      if (m_tracks[index]->get_info(m_infos[index])) {
        const bool hasBpm = m_infos[index].meta_exists(bpmTag.get_ptr());
        if (hasBpm)
          anyBpmExisting = true;
        else
          anyBpmMissing = true;
      } else {
        mask.set(index, true);
      }
    }

    m_tracks.remove_mask(mask);
    m_infos.remove_mask(mask);
  }

  if (anyBpmExisting && !m_measuredCandidateInspectionMode) {
    pfc::string_formatter prompt;
    if (anyBpmMissing) {
      prompt << "Some selected tracks already contain %" << bpmTag.get_ptr()
             << "% data.\n\n"
             << "Choose whether to analyse all selected tracks or skip tracks "
                "that already have existing values.";
      pfc::stringcvt::string_wide_from_utf8 promptW(prompt);
      const INT_PTR response =
          bpm_overwrite_prompt_dialog(promptW.get_ptr(), L"Analyze all",
                                      L"Skip existing", true)
              .DoModal(core_api::get_main_window());
      canceledByUser = (response == IDCANCEL);
      rescanExisting = (response == IDYES);
    } else {
      prompt << "All selected tracks already contain %" << bpmTag.get_ptr()
             << "% data.\n\n"
             << "Choose whether to analyse the selected tracks again or cancel.";
      pfc::stringcvt::string_wide_from_utf8 promptW(prompt);
      const INT_PTR response =
          bpm_overwrite_prompt_dialog(promptW.get_ptr(), L"Analyze again",
                                      L"Cancel", false)
              .DoModal(core_api::get_main_window());
      rescanExisting = (response == IDYES);
    }
  }

  if (canceledByUser) {
    return false;
  }

  if (anyBpmExisting && !rescanExisting &&
      !m_measuredCandidateInspectionMode) {
    // User chose to skip re-scanning files that already have BPM data.
    bit_array_bittable mask(m_tracks.get_size());
    for (t_size index = 0; index < m_tracks.get_size(); ++index) {
      mask.set(index, m_infos[index].meta_exists(bpmTag.get_ptr()));
    }
    m_tracks.remove_mask(mask);
    m_infos.remove_mask(mask);
  }

  if (m_tracks.get_count() == 0) {
    return false;
  }

  Create(core_api::get_main_window(), NULL);
  if (IsWindow()) {
    ShowWindow(SW_SHOWNORMAL);
    return true;
  }
  return false;
}
BOOL bpm_auto_analysis_thread::OnInitDialog(CWindow wndFocus,
                                            LPARAM lInitParam) {
  (void)wndFocus;
  (void)lInitParam;
  m_dark.AddDialogWithControls(m_hWnd);

  SetWindowText(_T("Analyzing..."));
  uSetDlgItemText(m_hWnd, ID_BPM_PROGRESS_FILE, "Analyzing...");

  const t_size total = m_tracks.get_count();
  m_bpm_results.assign(total, 0.0);
  m_confidence_results.assign(total, 0.0);
  m_routed_genre_results.assign(total, pfc::string8());
  m_method_results.assign(total, pfc::string8());
  m_error_results.assign(total, pfc::string8());
  m_suppress_write_results.assign(total, 0);
  m_routing_context_results.assign(total, {});
  m_measured_bpm_candidate_results.assign(total, {});
  m_completedAudioMillis.store(0, std::memory_order_relaxed);
  m_totalAudioMillis.store(1000, std::memory_order_relaxed);

  CProgressBarCtrl bar(GetDlgItem(ID_BPM_PROGRESS_TOTAL));
  bar.SetRange32(0, 1000);
  bar.SetPos(0);

  m_startedAt = std::chrono::steady_clock::now();
  m_pausedSeconds = 0.0;
  m_pauseLatched = false;
  m_etaDisplayedRemaining = 0.0;
  m_smoothedTotalTimeSeconds = 0.0;
  m_etaUiInitialized = false;
  m_etaLastProjectionDone = 0;
  m_etaLastProjectionElapsed = 0.0;
  m_etaLastUiUpdate = m_startedAt;
  m_workerCount.store(0, std::memory_order_relaxed);
  m_workerSlotsReady.store(false, std::memory_order_relaxed);
  for (auto& bin : m_trackDurationHistogram) {
    bin.store(0, std::memory_order_relaxed);
  }
  m_lastLayoutLines = static_cast<size_t>(-1);
  m_lastLayoutClientWidth = -1;
  m_lastLayoutClientHeight = -1;
  m_lastActiveCount = static_cast<size_t>(-1);
  m_lastFileLine.reset();
  m_lastInfoLine.reset();
  InitLayoutMetrics();
  InitTooltips();
  UpdateStatusLine();
  static_api_ptr_t<message_loop>()->add_message_filter(this);

  SetTimer(1, 100);

  m_workerThread = std::thread([this]() {
    try {
      WorkerMain();
    } catch (const pfc::exception& e) {
      FB2K_console_formatter()
          << "foo_smart_tempo: WorkerMain failed with pfc::exception: "
          << e.what();
      m_wasAborted = true;
      NotifyWorkerDoneMainThread(true);
    } catch (...) {
      try {
        throw;
      } catch (const std::exception& e) {
        FB2K_console_formatter()
            << "foo_smart_tempo: WorkerMain failed with std::exception: "
            << e.what();
      } catch (...) {
        FB2K_console_formatter()
            << "foo_smart_tempo: WorkerMain failed with unknown exception.";
      }
      m_wasAborted = true;
      NotifyWorkerDoneMainThread(true);
    }
  });
  return TRUE;
}

void bpm_auto_analysis_thread::WorkerMain() {
  const auto analysisStart = std::chrono::steady_clock::now();
  const t_size total = m_tracks.get_count();
  if (total == 0) {
    m_processedTrackCount = 0;
    m_analysisElapsedSeconds = 0.0;
    NotifyWorkerDoneMainThread(false);
    return;
  }

  ModernBpmAnalyzer::reset_runtime_stats();
  m_doneAudioMillis.store(0, std::memory_order_relaxed);
  m_completedAudioMillis.store(0, std::memory_order_relaxed);
  constexpr double kProgressDecodeSafetyMarginSeconds = 0.5;
  const int progressSecondsToRead = get_default_analysis_seconds_to_read();
  const int progressSamplePasses = get_default_analysis_sample_passes();
  uint64_t totalAudioMillis = 0;
  for (t_size i = 0; i < total; ++i) {
    const double lengthSec = m_tracks[i]->get_length();
    double estimatedAnalysisSeconds =
        static_cast<double>(progressSecondsToRead);
    int estimatedPasses = progressSamplePasses;
    if (std::isfinite(lengthSec) && lengthSec > 0.0) {
      estimatedAnalysisSeconds =
          (std::min)(estimatedAnalysisSeconds, lengthSec);
      if (lengthSec > kProgressDecodeSafetyMarginSeconds) {
        estimatedAnalysisSeconds =
            (std::min)(estimatedAnalysisSeconds,
                       lengthSec - kProgressDecodeSafetyMarginSeconds);
      }
    } else {
      estimatedPasses = 1;
    }
    if (estimatedAnalysisSeconds > 0.0 && estimatedPasses > 0) {
      const double ms =
          estimatedAnalysisSeconds * static_cast<double>(estimatedPasses) *
          1000.0;
      if (ms >= static_cast<double>((std::numeric_limits<uint64_t>::max)() - totalAudioMillis)) {
        totalAudioMillis = (std::numeric_limits<uint64_t>::max)();
        break;
      }
      totalAudioMillis += static_cast<uint64_t>(ms + 0.5);
    }
  }
  if (totalAudioMillis == 0) totalAudioMillis = 1000;
  m_totalAudioMillis.store(totalAudioMillis, std::memory_order_relaxed);

  if (smart_tempo::verbose_console_logging_enabled()) {
    FB2K_console_formatter()
        << "foo_smart_tempo prefs (MIR Engine): writePrecision="
        << (int)bpm_config_bpm_write_precision
        << ", secondsPerSample=" << progressSecondsToRead
        << ", samplesPerSong=" << progressSamplePasses
        << ", offsetRange=20-80%"
        << ", engine=Hodgkinson/MIR"
        << ", confidence=evidence";
  }

  const unsigned hwRaw = std::thread::hardware_concurrency();
  const unsigned hw = (hwRaw > 0u) ? hwRaw : 1u;
  const unsigned totalU = static_cast<unsigned>(total);
  unsigned workerLimit = hw;
  const unsigned leaveOneCore = (hw > 1u) ? (hw - 1u) : 1u;
  const int workerMode = clamp_analysis_worker_mode((int)bpm_config_worker_mode);
  if (workerMode == (int)ANALYSIS_WORKER_BALANCED) {
    workerLimit = leaveOneCore;
  } else if (workerMode == (int)ANALYSIS_WORKER_CONSERVATIVE) {
    workerLimit = std::min(8u, leaveOneCore);
  }
  const unsigned workers = std::max(1u, std::min(workerLimit, totalU));
  m_workerCount.store(workers, std::memory_order_relaxed);
  pfc::string8 rules = cfg_smart_tempo_genre_rules.get();
  pfc::string8 genericAnchors = cfg_smart_tempo_generic_anchor_tokens.get();
  const auto rulesSnapshot =
      SmartTempoMapper::create_rules_snapshot_from_string(
          rules.get_ptr(), genericAnchors.get_ptr());
  const size_t loadedRuleCount = SmartTempoMapper::get_rule_count(rulesSnapshot);
  const std::string routingScriptText =
      normalize_routing_script_text(cfg_smart_tempo_routing_tag.get().get_ptr());

  // policy unmatched analysis is route-independent. Keep one fixed production
  // rail for DSP safety instead of exposing legacy hard-range controls.
  constexpr double fallbackMinBpm = 40.0;
  constexpr double fallbackMaxBpm = 220.0;
  const bool verboseRoutingLogs =
      smart_tempo::verbose_console_logging_enabled();
  std::atomic<uint64_t> routeMatchedCount{0};
  std::atomic<uint64_t> routeFallbackCount{0};

  FB2K_console_formatter()
      << "foo_smart_tempo: [Routing] config rules_loaded="
      << static_cast<uint64_t>(loadedRuleCount)
      << ", production_rail=" << format_float_locale(fallbackMinBpm, 2)
      << "-" << format_float_locale(fallbackMaxBpm, 2)
      << ", workers=" << workers << "/" << hw
      << ", worker_mode=" << workerMode;

  m_workerSlotCount = workers;
  m_workerSlots.reset(new worker_live_slot[m_workerSlotCount]);
  for (size_t i = 0; i < m_workerSlotCount; ++i) {
    m_workerSlots[i].index.store((t_size)-1, std::memory_order_relaxed);
    m_workerSlots[i].started_at_millis.store(0, std::memory_order_relaxed);
  }
  m_workerSlotsReady.store(true, std::memory_order_release);

  std::vector<std::thread> pool;
  pool.reserve(workers);

  for (unsigned w = 0; w < workers; ++w) {
    pool.emplace_back([this, total, w, routingScriptText,
                       fallbackMinBpm, fallbackMaxBpm,
                       loadedRuleCount, &routeMatchedCount,
                       &routeFallbackCount, verboseRoutingLogs,
                       rulesSnapshot]() {
      try {
        null_threaded_status status;
        auto analyzer = BpmAnalyzerFactory::create(
            &m_doneAudioMillis,
            &m_pauseRequested,
            &m_pauseCv,
            &m_pauseMutex);
        if (!analyzer) {
          FB2K_console_formatter()
              << "foo_smart_tempo: Failed to initialize BPM analyzer worker";
          return;
        }
        service_ptr_t<titleformat_object> routingScript =
            compile_routing_script_with_warning(routingScriptText, " during analysis");

        metadb_handle_list local_unmatched;
        for (;;) {
        if (m_abort.is_aborting())
          break;

        if (m_pauseRequested.load(std::memory_order_acquire)) {
          std::unique_lock<std::mutex> lk(m_pauseMutex);
          m_pauseCv.wait(lk, [this]() {
            return !m_pauseRequested.load() || m_abort.is_aborting();
          });
        }
        if (m_abort.is_aborting())
          break;

        const t_size index = m_nextIndex.fetch_add(1);
        if (index >= total)
          break;

        if (m_workerSlots && w < m_workerSlotCount) {
          m_workerSlots[w].started_at_millis.store(
              SteadyMillis(), std::memory_order_relaxed);
          m_workerSlots[w].index.store(index, std::memory_order_release);
        }

          try {
          const metadb_handle_ptr p_item = m_tracks[index];
          file_info_impl info;
          metadb_info_container::ptr infoRef;
          bool routeMatched = false;
          double routeMinBpm = fallbackMinBpm;
          double routeMaxBpm = fallbackMaxBpm;
          double routeCenterBpm = (routeMinBpm + routeMaxBpm) * 0.5;
          double routeSpreadBpm = (routeMaxBpm - routeMinBpm) * 0.5;

          bool gotInfo = false;
          if (p_item->get_info_ref(infoRef) && infoRef.is_valid()) {
            info = infoRef->info();
            gotInfo = true;
          } else {
            gotInfo = p_item->get_info(info);
          }
          const pfc::string8 trackIdentifier =
              make_track_identifier(p_item, gotInfo ? &info : nullptr);

          pfc::string8 routedValues;
          if (routingScript.is_valid() && gotInfo) {
            p_item->format_title_from_external_info(info, nullptr, routedValues,
                                                    routingScript, nullptr);
          }
          const auto routedParsedTokens =
              tokenize_genre_text(routedValues.get_ptr(), true, false);
          std::vector<std::string> routedTokens;
          routedTokens.reserve(routedParsedTokens.size());
          for (const auto& token : routedParsedTokens) {
            routedTokens.push_back(token.original);
          }
          std::vector<std::string> routedMapperInputs;
          if (!routedValues.is_empty()) {
            // Preserve the user's routing-script field/token order for the
            // mapper. Passing flattened tokens would reset all positions to 0
            // and make specificity win over the entered order again.
            routedMapperInputs.emplace_back(routedValues.get_ptr());
          }
          const std::string routedSourceLog =
              routedValues.is_empty()
                  ? std::string("<empty>")
                  : escape_routing_log_value(routedValues.get_ptr());
          const std::string routedNormalizedLog =
              join_normalized_genre_tokens(routedParsedTokens);
          pfc::string8 resolvedRouteValue;
          SmartTempoMapper::MatchDebugInfo routeDebug;
          const std::vector<SmartTempoMapper::RouteMatchInfo> routeMatches =
              SmartTempoMapper::get_route_matches_for_tag_values_snapshot(
                  rulesSnapshot, routedMapperInputs, 8);
          {
            if (!routeMatches.empty()) {
              const auto& primaryRoute = routeMatches.front();
              routeMinBpm = primaryRoute.min_bpm;
              routeMaxBpm = primaryRoute.max_bpm;
              routeCenterBpm = primaryRoute.center_bpm;
              routeSpreadBpm = primaryRoute.spread_bpm;
              resolvedRouteValue = primaryRoute.resolved_value.c_str();
              routeDebug = primaryRoute.debug;
              routeMatched = true;
            }
          }
          if (routeMatched && routeDebug.is_generic_match) {
            // Generic routing classifies the library value but supplies no
            // musical prior. Run the analyzer on the same production rail as
            // an unmatched track and expose no synthetic center in telemetry.
            routeMinBpm = fallbackMinBpm;
            routeMaxBpm = fallbackMaxBpm;
            routeCenterBpm = 0.0;
            routeSpreadBpm = 0.0;
          }
          if (routeMatched) {
            routeMatchedCount.fetch_add(1, std::memory_order_relaxed);
            if (loadedRuleCount == 0) {
              FB2K_console_formatter()
                  << "foo_smart_tempo: [" << trackIdentifier.get_ptr()
                  << "] routing warning: matched rule while rules_loaded=0";
            }
            if (verboseRoutingLogs) {
              FB2K_console_formatter()
                  << "foo_smart_tempo: [" << trackIdentifier.get_ptr()
                  << "] routing decision: source_genres=\"" << routedSourceLog.c_str()
                  << "\", normalized_genres=\"" << routedNormalizedLog.c_str()
                  << "\", matched_rule=" << routeDebug.matched_rule.c_str()
                  << ", matched_token=" << routeDebug.matched_token.c_str()
                  << ", specificity_score=words=" << routeDebug.specificity_words
                  << ", norm_len=" << routeDebug.specificity_norm_len
                  << ", rule_index=" << routeDebug.rule_index
                  << ", is_generic_match=" << (routeDebug.is_generic_match ? 1 : 0)
                  << ", candidates_considered=" << routeDebug.candidates_considered;
            }
          } else {
            routeFallbackCount.fetch_add(1, std::memory_order_relaxed);
            if (verboseRoutingLogs) {
              FB2K_console_formatter()
                  << "foo_smart_tempo: [" << trackIdentifier.get_ptr()
                  << "] routing decision: source_genres=\"" << routedSourceLog.c_str()
                  << "\", normalized_genres=\"" << routedNormalizedLog.c_str()
                  << "\", matched_rule=<none>, matched_token=<none>"
                  << ", specificity_score=words=0, norm_len=0, rule_index=0"
                  << ", is_generic_match=0, candidates_considered="
                  << routeDebug.candidates_considered;
            }
          }
          if (resolvedRouteValue.is_empty()) {
            if (!routedTokens.empty()) {
              resolvedRouteValue = routedTokens.front().c_str();
            } else if (!routedValues.is_empty()) {
              resolvedRouteValue = routedValues;
            } else {
              resolvedRouteValue = "Unmatched";
            }
          }
          m_routed_genre_results[index] = resolvedRouteValue;
          if (!routeMatched) {
            routeMinBpm = fallbackMinBpm;
            routeMaxBpm = fallbackMaxBpm;
            routeCenterBpm = (routeMinBpm + routeMaxBpm) * 0.5;
            routeSpreadBpm = (routeMaxBpm - routeMinBpm) * 0.5;
            local_unmatched.add_item(p_item);
          }

          IBpmAnalyzer::RoutingLogContext routingContext;
          routingContext.source_genres = routedSourceLog.c_str();
          routingContext.normalized_genres = routedNormalizedLog.c_str();
          routingContext.matched_rule =
              routeMatched ? routeDebug.matched_rule.c_str() : "<none>";
          routingContext.matched_token =
              routeMatched ? routeDebug.matched_token.c_str() : "<none>";
          routingContext.rule_index = routeMatched ? routeDebug.rule_index : 0;
          routingContext.specificity_words =
              routeMatched ? routeDebug.specificity_words : 0;
          routingContext.specificity_norm_len =
              routeMatched ? routeDebug.specificity_norm_len : 0;
          routingContext.route_matched = routeMatched;
          routingContext.is_generic_match =
              routeMatched && routeDebug.is_generic_match;
          routingContext.candidates_considered = routeDebug.candidates_considered;
          routingContext.primary_min_bpm = routeMinBpm;
          routingContext.primary_max_bpm = routeMaxBpm;
          routingContext.primary_center_bpm = routeCenterBpm;
          routingContext.primary_spread_bpm = routeSpreadBpm;
          m_routing_context_results[index].assign(routingContext);
          m_bpm_results[index] = analyzer->analyze(
              p_item, status, m_abort, routeMinBpm, routeMaxBpm,
              trackIdentifier.get_ptr(), &routingContext,
              m_measuredCandidateInspectionMode);
          const auto &diag = analyzer->get_diagnostics();
          m_confidence_results[index] = diag.confidence;
          m_method_results[index] = diag.method;
          m_error_results[index] = diag.error_reason;
          m_suppress_write_results[index] = diag.suppress_write ? 1 : 0;
          m_measured_bpm_candidate_results[index] =
              diag.measured_bpm_candidates;

          const double finalOutputBpmRaw = m_bpm_results[index];
          pfc::string8 confidenceText;
          if (std::isfinite(diag.confidence) && diag.confidence >= 0.0) {
            confidenceText = format_float_locale(diag.confidence, 1);
          } else {
            confidenceText = "n/a";
          }
          const std::string decisionNoteText =
              diag.decision_note.is_empty()
                  ? std::string()
                  : escape_routing_log_value(diag.decision_note.get_ptr());
          if (std::isfinite(finalOutputBpmRaw) && finalOutputBpmRaw > 0.0) {
            const format_bpm finalOutputBpm(finalOutputBpmRaw, get_write_precision());
            FB2K_console_formatter()
                << "foo_smart_tempo: [" << trackIdentifier.get_ptr()
                << "] Final Output: " << finalOutputBpm << " BPM"
                << ", confidence=" << confidenceText.get_ptr()
                << ", uncertain=" << (diag.is_uncertain ? 1 : 0)
                << ", policy_reason="
                << (diag.policy_reason.is_empty() ? "<none>" : diag.policy_reason.get_ptr())
                << ", decision_class="
                << (diag.decision_class.is_empty() ? "unknown" : diag.decision_class.get_ptr())
                << ", decision_note=\""
                << decisionNoteText.c_str()
                << "\"";
          } else if (diag.suppress_write || diag.is_uncertain) {
            FB2K_console_formatter()
                << "foo_smart_tempo: [" << trackIdentifier.get_ptr()
                << "] Final Output: no writable BPM"
                << " (existing BPM tag left unchanged)"
                << ", confidence=" << confidenceText.get_ptr()
                << ", uncertain=" << (diag.is_uncertain ? 1 : 0)
                << ", policy_reason="
                << (diag.policy_reason.is_empty() ? "<none>" : diag.policy_reason.get_ptr())
                << ", decision_class="
                << (diag.decision_class.is_empty() ? "unknown" : diag.decision_class.get_ptr())
                << ", decision_note=\""
                << decisionNoteText.c_str()
                << "\"";
          }
          } catch (const pfc::exception& e) {
            m_bpm_results[index] = 0.0;
            m_confidence_results[index] = 0.0;
            m_routed_genre_results[index] = "Unmatched";
            m_method_results[index] = "Exception";
            m_error_results[index] = e.what();
            m_suppress_write_results[index] = 1;
            m_measured_bpm_candidate_results[index].clear();
            FB2K_console_formatter()
                << "foo_smart_tempo: Worker pfc::exception on "
                << pfc::string_filename_ext(m_tracks[index]->get_path()) << ": "
                << e.what();
          } catch (...) {
            m_bpm_results[index] = 0.0;
            m_confidence_results[index] = 0.0;
            m_routed_genre_results[index] = "Unmatched";
            m_method_results[index] = "Exception";
            m_suppress_write_results[index] = 1;
            try {
              throw;
            } catch (const std::exception& e) {
              m_error_results[index] = e.what();
              FB2K_console_formatter()
                  << "foo_smart_tempo: Worker exception on "
                  << pfc::string_filename_ext(m_tracks[index]->get_path()) << ": "
                  << e.what();
            } catch (...) {
            m_error_results[index] = "Unknown worker exception";
              FB2K_console_formatter()
                  << "foo_smart_tempo: Unknown worker exception on "
                  << pfc::string_filename_ext(m_tracks[index]->get_path());
            }
            m_measured_bpm_candidate_results[index].clear();
          }
          if (m_workerSlots && w < m_workerSlotCount) {
            const uint64_t startedMillis =
                m_workerSlots[w].started_at_millis.load(std::memory_order_relaxed);
            const uint64_t finishedMillis = SteadyMillis();
            if (startedMillis > 0 && finishedMillis >= startedMillis) {
              RecordTrackWallTime(finishedMillis - startedMillis);
            }
          }
          m_doneCount.fetch_add(1);
          const double processedLengthSec = m_tracks[index]->get_length();
          if (std::isfinite(processedLengthSec) && processedLengthSec > 0.0) {
            const double ms = processedLengthSec * 1000.0;
            if (ms > 0.0) {
              m_completedAudioMillis.fetch_add(static_cast<uint64_t>(ms + 0.5),
                                               std::memory_order_relaxed);
            }
          }

          if (m_workerSlots && w < m_workerSlotCount) {
            m_workerSlots[w].index.store((t_size)-1, std::memory_order_release);
            m_workerSlots[w].started_at_millis.store(0,
                                                     std::memory_order_relaxed);
          }
        }
        if (local_unmatched.get_count() > 0) {
          std::lock_guard<std::mutex> unmatchedLock(m_unmatchedTracksMutex);
          m_unmatched_tracks.add_items(local_unmatched);
        }
      } catch (const pfc::exception& e) {
        FB2K_console_formatter()
            << "foo_smart_tempo: worker setup/execution failed (pfc::exception), worker="
            << static_cast<uint64_t>(w) << ", error=" << e.what();
      } catch (...) {
        try {
          throw;
        } catch (const std::exception& e) {
          FB2K_console_formatter()
              << "foo_smart_tempo: worker setup/execution failed (std::exception), worker="
              << static_cast<uint64_t>(w) << ", error=" << e.what();
        } catch (...) {
          FB2K_console_formatter()
              << "foo_smart_tempo: worker setup/execution failed (unknown exception), worker="
              << static_cast<uint64_t>(w);
        }
      }
    });
  }

  for (auto &t : pool) {
    if (t.joinable())
      t.join();
  }

  const ModernBpmAnalyzer::RuntimeStats runtimeStats =
      ModernBpmAnalyzer::query_runtime_stats();
  const double analyzedTracks =
      static_cast<double>((std::max<uint64_t>)(1, runtimeStats.analyze_calls));
  const double avgDecodeMs =
      (runtimeStats.decode_seconds * 1000.0) / analyzedTracks;
  const double avgOnsetMs =
      (runtimeStats.onset_seconds * 1000.0) / analyzedTracks;
  const double avgTotalMs =
      (runtimeStats.total_seconds * 1000.0) / analyzedTracks;
  const bool detailedStageTimingAvailable =
      runtimeStats.decode_seconds > 0.0 || runtimeStats.onset_seconds > 0.0;
  const double avgMirCoreMs =
      (std::max)(0.0, avgTotalMs - avgDecodeMs - avgOnsetMs);
  const double avgTempoAllocsPerTrack =
      static_cast<double>(runtimeStats.tempo_allocations) / analyzedTracks;
  const double elapsedSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    analysisStart)
          .count();
  const double tracksPerSecond =
      elapsedSeconds > 0.0
          ? static_cast<double>(runtimeStats.analyze_calls) / elapsedSeconds
          : 0.0;
  const double avgWallMsPerTrack =
      (elapsedSeconds * 1000.0) / analyzedTracks;

  {
    pfc::string_formatter stats;
    stats << "foo_smart_tempo: [Worker Stats] tracks="
          << runtimeStats.analyze_calls
          << " workers=" << workers << "/" << hw
          << " worker_mode=" << workerMode
          << " tempoState=deep-reset-cache"
          << " tempoAllocs=" << runtimeStats.tempo_allocations
          << " avgTempoAllocsPerTrack="
          << format_float_locale(avgTempoAllocsPerTrack, 3);
    // Detailed stage timers are intentionally verbose-only because per-hop
    // timing measurably slows large scans. Standard runs retain reliable total
    // and wall-clock timing without pretending absent stage samples are zero.
    if (detailedStageTimingAvailable) {
      stats << " stageTiming=detailed"
            << " avgDecodeMs=" << format_float_locale(avgDecodeMs, 2)
            << " avgOnsetMs=" << format_float_locale(avgOnsetMs, 2)
            << " avgMirCoreMs=" << format_float_locale(avgMirCoreMs, 2);
    } else {
      stats << " stageTiming=total-only";
    }
    stats << " avgTotalMs=" << format_float_locale(avgTotalMs, 2)
          << " elapsedSec=" << format_float_locale(elapsedSeconds, 2)
          << " tracksPerSec=" << format_float_locale(tracksPerSecond, 3)
          << " avgWallMsPerTrack=" << format_float_locale(avgWallMsPerTrack, 2)
          << " engine=hodgkinson-mir"
          << " primarySelector=measured-candidate"
          << " fullBoardPolicy=soft-center"
          << " materialRiskCandidates="
          << runtimeStats.hodgkinson_material_risk_candidates
          << " materialRiskReviewHolds="
          << runtimeStats.hodgkinson_material_risk_review_holds
          << " primaryAutoCandidates="
          << runtimeStats.mir_primary_auto_candidates
          << " primaryReviewHolds="
          << runtimeStats.mir_primary_review_holds
          << " primarySparseAcapellaReviewHolds="
          << runtimeStats
                 .mir_primary_sparse_acapella_review_holds
          << " primaryRuntimeApplied="
          << runtimeStats.mir_primary_runtime_applied
          << " policyWriterOverrides="
          << runtimeStats.mir_policy_writer_overrides
          << " policyReviewHoldReleases="
          << runtimeStats
                 .mir_policy_review_hold_promotions
          << " policyLocalExactUpgrades="
          << runtimeStats
                 .mir_policy_keep_current_micro_upgrades
          << " policyReviewHolds="
          << runtimeStats.mir_policy_review_holds
          << " policyFamilyConflictReviewHolds="
          << runtimeStats
                 .mir_policy_family_conflict_review_holds
          << " engineFeatures=probe:"
          << (smart_tempo::experimental::kEnableHodgkinsonTatumProbe ? 1 : 0)
          << ",aliasPulse:"
          << (smart_tempo::experimental::kEnableHodgkinsonAliasPulseEvidence ? 1
                                                                            : 0)
          << ",continuousRefine:"
          << (smart_tempo::experimental::
                      kEnableHodgkinsonContinuousRefinementEvidence
                  ? 1
                  : 0)
          << ",segmentTopK:"
          << (smart_tempo::experimental::kEnableHodgkinsonSegmentTopKTelemetry
                  ? 1
                  : 0)
          << ",materialRisk:"
          << (smart_tempo::experimental::kEnableHodgkinsonMaterialRiskEvidence
                  ? 1
                  : 0)
          << ",partialBar:"
          << ((smart_tempo::mir_pipeline::
                   kEnableMirPolicyPartialBarFamilyConflictRecovery ||
               smart_tempo::experimental::kEnableHodgkinsonPartialBarProbe)
                  ? 1
                  : 0)
          << ",partialBarTopKExact:"
          << (smart_tempo::experimental::
                      kEnableHodgkinsonPartialBarTopKExactTelemetry
                  ? 1
                  : 0)
          << ",primarySelector:"
          << (smart_tempo::experimental::
                      kEnableMirPrimaryTelemetry
                  ? 1
                  : 0)
          << ",primaryRuntime:"
          << (smart_tempo::experimental::
                      kEnableMirPrimaryRuntime
                  ? 1
                  : 0)
          << ",softCenterPolicy:"
          << (smart_tempo::experimental::kEnableMirPolicyRuntime
                  ? 1
                  : 0)
          << ",policyTelemetry:"
          << (smart_tempo::experimental::kEnableMirSoftCenterTelemetry
                  ? 1
                  : 0)
          << ",policyBoard:"
          << (verboseRoutingLogs ? 1 : 0)
          << " routeMatched=" << routeMatchedCount.load(std::memory_order_relaxed)
          << " routeFallback=" << routeFallbackCount.load(std::memory_order_relaxed);
    FB2K_console_formatter() << stats.get_ptr();
  }

  m_processedTrackCount = m_doneCount.load();
  m_analysisElapsedSeconds = elapsedSeconds;
  m_wasAborted = m_abort.is_aborting();
  NotifyWorkerDoneMainThread(m_wasAborted);
}

void bpm_auto_analysis_thread::NotifyWorkerDoneMainThread(bool wasAborted) {
  const HWND wnd = m_hWnd;
  const WPARAM abortedParam = wasAborted ? 1 : 0;
  fb2k::inMainThread([wnd, abortedParam]() {
    if (::IsWindow(wnd)) {
      ::PostMessage(wnd, WM_APP + 120, abortedParam, 0);
    }
  });
}

void bpm_auto_analysis_thread::OnTimer(UINT_PTR id) {
  if (id != 1)
    return;
  UpdateStatusLine();
}

void bpm_auto_analysis_thread::AddTooltip(int controlID, const TCHAR *text) {
  if (!m_tooltip.IsWindow())
    return;
  CWindow ctrl(GetDlgItem(controlID));
  if (!ctrl.IsWindow())
    return;

  TOOLINFO ti = {};
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  ti.hwnd = m_hWnd;
  ti.uId = (UINT_PTR)ctrl.m_hWnd;
  ti.lpszText = const_cast<LPTSTR>(text);
  m_tooltip.AddTool(&ti);
}

void bpm_auto_analysis_thread::UpdateTooltip(int controlID,
                                             const TCHAR *text) {
  if (!m_tooltip.IsWindow())
    return;
  CWindow ctrl(GetDlgItem(controlID));
  if (!ctrl.IsWindow())
    return;

  TOOLINFO ti = {};
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  ti.hwnd = m_hWnd;
  ti.uId = (UINT_PTR)ctrl.m_hWnd;
  ti.lpszText = const_cast<LPTSTR>(text);
  m_tooltip.UpdateTipText(&ti);
}

void bpm_auto_analysis_thread::SetProgressInfo(const char* text,
                                               const char* tooltipText) {
  const char* safeText = text != nullptr ? text : "";
  const char* safeTooltip = tooltipText != nullptr ? tooltipText : "";
  if (strcmp(m_lastInfoLine.get_ptr(), safeText) != 0) {
    uSetDlgItemText(m_hWnd, ID_BPM_PROGRESS_INFO, safeText);
    m_lastInfoLine = safeText;
  }

  pfc::stringcvt::string_wide_from_utf8 tooltipWide(safeTooltip);
  const std::wstring nextTooltip(tooltipWide);
  if (m_progressInfoTooltip != nextTooltip) {
    m_progressInfoTooltip = nextTooltip;
    UpdateTooltip(ID_BPM_PROGRESS_INFO, m_progressInfoTooltip.c_str());
  }
}

void bpm_auto_analysis_thread::InitTooltips() {
  if (m_tooltip.IsWindow())
    return;
  if (!m_tooltip.Create(m_hWnd, NULL, NULL,
                        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP))
    return;

  m_tooltip.SetMaxTipWidth(560);
  m_tooltip.SetDelayTime(TTDT_INITIAL, 350);
  m_tooltip.Activate(TRUE);

  AddTooltip(ID_BPM_PROGRESS_FILE, _T("Live list of currently analyzed files ")
                                   _T("(one line per active worker)."));
  AddTooltip(ID_BPM_PROGRESS_TOTAL,
             _T("Deterministic total progress across the full analysis job."));
  AddTooltip(ID_BPM_PROGRESS_INFO,
             _T("Current analysis state and timing details."));
  AddTooltip(ID_BPM_PROGRESS_PAUSE,
             _T("Pause or resume all active analysis workers."));
  AddTooltip(ID_BPM_PROGRESS_ABORT,
             _T("Abort the current analysis job immediately."));
}
void bpm_auto_analysis_thread::InitLayoutMetrics() {
  CRect client;
  GetClientRect(&client);

  CRect fileRect, barRect, infoRect, pauseRect, abortRect;
  GetDlgItem(ID_BPM_PROGRESS_FILE).GetWindowRect(&fileRect);
  GetDlgItem(ID_BPM_PROGRESS_TOTAL).GetWindowRect(&barRect);
  GetDlgItem(ID_BPM_PROGRESS_INFO).GetWindowRect(&infoRect);
  GetDlgItem(ID_BPM_PROGRESS_PAUSE).GetWindowRect(&pauseRect);
  GetDlgItem(ID_BPM_PROGRESS_ABORT).GetWindowRect(&abortRect);
  ScreenToClient(&fileRect);
  ScreenToClient(&barRect);
  ScreenToClient(&infoRect);
  ScreenToClient(&pauseRect);
  ScreenToClient(&abortRect);

  m_layout.marginLeft = fileRect.left;
  m_layout.marginRight = client.right - abortRect.right;
  m_layout.marginTop = fileRect.top;
  m_layout.marginBottom = client.bottom - pauseRect.bottom;
  m_layout.fileToBarGap = barRect.top - fileRect.bottom;
  m_layout.barToBottomRowGap = pauseRect.top - barRect.bottom;
  m_layout.barHeight = barRect.Height();
  m_layout.infoHeight = infoRect.Height();
  m_layout.buttonWidth = pauseRect.Width();
  m_layout.buttonHeight = pauseRect.Height();
  m_layout.buttonGap = abortRect.left - pauseRect.right;
  m_layout.infoToButtonsGap = pauseRect.left - infoRect.right;
  m_layout.minClientHeight = client.Height();

  RECT gapFileToBarDlu = {0, 0, 4, 4};
  RECT gapBarToRowDlu = {0, 0, 6, 5};
  RECT gapInfoToButtonsDlu = {0, 0, 6, 4};
  MapDialogRect(&gapFileToBarDlu);
  MapDialogRect(&gapBarToRowDlu);
  MapDialogRect(&gapInfoToButtonsDlu);

  const int minFileToBarGap =
      (std::max)(1, static_cast<int>(gapFileToBarDlu.right));
  const int minBarToRowGap =
      (std::max)(1, static_cast<int>(gapBarToRowDlu.bottom));
  const int minInfoToButtonsGap =
      (std::max)(1, static_cast<int>(gapInfoToButtonsDlu.right));

  if (m_layout.fileToBarGap < minFileToBarGap)
    m_layout.fileToBarGap = minFileToBarGap;
  if (m_layout.barToBottomRowGap < minBarToRowGap)
    m_layout.barToBottomRowGap = minBarToRowGap;
  if (m_layout.infoToButtonsGap < minInfoToButtonsGap)
    m_layout.infoToButtonsGap = minInfoToButtonsGap;
}

int bpm_auto_analysis_thread::MeasureDialogTextLineHeight() const {
  int lineHeight = 10;
  HDC dc = ::GetDC(m_hWnd);
  if (dc != NULL) {
    HFONT font = (HFONT)::SendMessage(m_hWnd, WM_GETFONT, 0, 0);
    HFONT old = NULL;
    if (font != NULL)
      old = (HFONT)::SelectObject(dc, font);
    TEXTMETRIC tm = {};
    if (::GetTextMetrics(dc, &tm))
      lineHeight = tm.tmHeight + tm.tmExternalLeading;
    if (old != NULL)
      ::SelectObject(dc, old);
    ::ReleaseDC(m_hWnd, dc);
  }
  if (lineHeight < 10)
    lineHeight = 10;
  return lineHeight;
}

void bpm_auto_analysis_thread::ApplyDynamicLayout(size_t activeLineCount,
                                                  bool force) {
  CRect client;
  GetClientRect(&client);
  const int clientWidth = client.Width();
  const int clientHeight = client.Height();
  const size_t lineCountSize = (activeLineCount > 0) ? activeLineCount : 1;

  if (!force && lineCountSize == m_lastLayoutLines &&
      clientWidth == m_lastLayoutClientWidth &&
      clientHeight == m_lastLayoutClientHeight) {
    return;
  }
  m_lastLayoutLines = lineCountSize;
  m_lastLayoutClientWidth = clientWidth;
  m_lastLayoutClientHeight = clientHeight;

  const int lineCount = static_cast<int>(lineCountSize);
  const int lineHeight = MeasureDialogTextLineHeight();
  const int contentWidth =
      clientWidth - m_layout.marginLeft - m_layout.marginRight;

  const int fileY = m_layout.marginTop;
  const int fileH = lineCount * lineHeight + 4;
  const int barY = fileY + fileH + m_layout.fileToBarGap;
  const int rowY = barY + m_layout.barHeight + m_layout.barToBottomRowGap;

  const int abortX = clientWidth - m_layout.marginRight - m_layout.buttonWidth;
  const int pauseX = abortX - m_layout.buttonGap - m_layout.buttonWidth;

  const int infoX = m_layout.marginLeft;
  int infoW = pauseX - m_layout.infoToButtonsGap - infoX;
  if (infoW < 20)
    infoW = 20;
  const int infoY = rowY + (m_layout.buttonHeight - m_layout.infoHeight) / 2;

  int targetClientHeight = rowY + m_layout.buttonHeight + m_layout.marginBottom;
  if (targetClientHeight < m_layout.minClientHeight)
    targetClientHeight = m_layout.minClientHeight;

  CRect windowRect;
  GetWindowRect(&windowRect);
  const int currentWindowHeight = windowRect.Height();
  const int nonClientHeight = currentWindowHeight - clientHeight;
  const int targetWindowHeight = targetClientHeight + nonClientHeight;

  if (targetWindowHeight > currentWindowHeight) {
    SetWindowPos(NULL, 0, 0, windowRect.Width(), targetWindowHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
  }

  CWindow fileCtl(GetDlgItem(ID_BPM_PROGRESS_FILE));
  CWindow barCtl(GetDlgItem(ID_BPM_PROGRESS_TOTAL));
  CWindow infoCtl(GetDlgItem(ID_BPM_PROGRESS_INFO));
  CWindow pauseCtl(GetDlgItem(ID_BPM_PROGRESS_PAUSE));
  CWindow abortCtl(GetDlgItem(ID_BPM_PROGRESS_ABORT));

  const UINT posFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW;
  fileCtl.SetWindowPos(NULL, m_layout.marginLeft, fileY, contentWidth, fileH,
                       posFlags);
  barCtl.SetWindowPos(NULL, m_layout.marginLeft, barY, contentWidth,
                      m_layout.barHeight, posFlags);
  infoCtl.SetWindowPos(NULL, infoX, infoY, infoW, m_layout.infoHeight,
                       posFlags);
  pauseCtl.SetWindowPos(NULL, pauseX, rowY, m_layout.buttonWidth,
                        m_layout.buttonHeight, posFlags);
  abortCtl.SetWindowPos(NULL, abortX, rowY, m_layout.buttonWidth,
                        m_layout.buttonHeight, posFlags);

  if (targetWindowHeight < currentWindowHeight) {
    SetWindowPos(NULL, 0, 0, windowRect.Width(), targetWindowHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
  }

  InvalidateRect(NULL, TRUE);
}

void bpm_auto_analysis_thread::UpdateStatusLine() {
  if (!::IsWindow(m_hWnd))
    return;

  const t_size total = m_tracks.get_count();
  const t_size done = m_doneCount.load();
  const double completedAudioSeconds =
      static_cast<double>(m_doneAudioMillis.load(std::memory_order_relaxed)) /
      1000.0;
  const double totalAudioSeconds =
      static_cast<double>(m_totalAudioMillis.load(std::memory_order_relaxed)) /
      1000.0;

  constexpr bool showLiveList = false;

  pfc::string_formatter fileLine;
  size_t activeCount = 0;
  size_t activeWorkerCount = 0;
  double oldestActiveSeconds = 0.0;
  const uint64_t nowMillis = SteadyMillis();

  const bool workerSlotsReady =
      m_workerSlotsReady.load(std::memory_order_acquire);
  for (size_t worker = 0;
       workerSlotsReady && worker < m_workerSlotCount; ++worker) {
    const t_size idx =
        m_workerSlots[worker].index.load(std::memory_order_acquire);
    if (idx >= m_tracks.get_count()) continue;
    ++activeWorkerCount;
    const uint64_t startedMillis =
        m_workerSlots[worker].started_at_millis.load(std::memory_order_relaxed);
    if (startedMillis > 0 && nowMillis >= startedMillis) {
      oldestActiveSeconds =
          (std::max)(oldestActiveSeconds,
                     static_cast<double>(nowMillis - startedMillis) / 1000.0);
    }
    if (showLiveList) {
      if (activeCount > 0)
        fileLine << "\n";
      fileLine << pfc::string_filename_ext(m_tracks[idx]->get_path());
      ++activeCount;
    }
  }

  pfc::string8 fileText;
  if (showLiveList) {
    fileText = fileLine;
    if (fileText.is_empty())
      fileText = "Analyzing...";
    if (activeCount == 0)
      activeCount = 1;
  } else {
    fileText = "Analyzing...";
    activeCount = 1;
  }

  if (strcmp(m_lastFileLine.get_ptr(), fileText.get_ptr()) != 0) {
    uSetDlgItemText(m_hWnd, ID_BPM_PROGRESS_FILE, fileText);
    m_lastFileLine = fileText;
  }
  if (m_lastActiveCount != activeCount) {
    ApplyDynamicLayout(activeCount, false);
    m_lastActiveCount = activeCount;
  }

  double byAudio = (totalAudioSeconds > 0.0)
                       ? (completedAudioSeconds / totalAudioSeconds)
                       : 0.0;
  double byTracks = (total > 0) ? ((double)done / (double)total) : 1.0;
  const size_t workerCount =
      (std::max)(m_workerCount.load(std::memory_order_relaxed),
                 workerSlotsReady ? m_workerSlotCount : size_t{0});
  const t_size activeTail =
      (done < total && workerCount > 0)
          ? (std::min)(static_cast<t_size>(workerCount), total - done)
          : 0;
  const double activeTrackWindow =
      (total > 0) ? (static_cast<double>(activeTail) / static_cast<double>(total))
                  : 0.0;
  // Decode progress can run ahead of the expensive MIR segment aggregation.
  // Keep per-track movement alive, but reserve part of each active worker slot
  // for post-decode MIR work so the batch bar and ETA do not finish early.
  const double activeDecodeCap = byTracks + activeTrackWindow * 0.85;
  double overall = (std::max)(byTracks, (std::min)(byAudio, activeDecodeCap));
  if (overall < 0.0)
    overall = 0.0;
  if (overall > 1.0)
    overall = 1.0;

  CProgressBarCtrl bar(GetDlgItem(ID_BPM_PROGRESS_TOTAL));
  constexpr double kProgressBeforeCompleteCap = 0.985;
  const double displayedOverall =
      (done < total && overall > kProgressBeforeCompleteCap)
          ? kProgressBeforeCompleteCap
          : overall;
  bar.SetPos((int)(displayedOverall * 1000.0));

  if (m_pauseRequested.load()) {
    SetProgressInfo(
        "Paused...",
        "Analysis is paused. Elapsed time and ETA are frozen until you resume.");
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(now - m_startedAt).count() -
                   m_pausedSeconds;
  if (m_pauseLatched)
    elapsed -= std::chrono::duration<double>(now - m_pauseBegan).count();
  if (elapsed < 0.0)
    elapsed = 0.0;

  if (total <= 10) {
    pfc::string_formatter info;
    pfc::string_formatter infoTooltip;
    info << "Time elapsed: " << FormatClock(elapsed) << " | ";
    infoTooltip << "Time elapsed: " << FormatClock(elapsed) << ". ";
    if (done == 0) {
      info << "Analyzing " << static_cast<uint64_t>(total) << " tracks...";
      infoTooltip << "Analyzing a small batch of "
                  << static_cast<uint64_t>(total)
                  << " tracks. A numeric ETA is intentionally omitted because "
                     "too few completion samples are available.";
    } else if (done < total) {
      info << "Processing... (" << static_cast<uint64_t>(done) << "/"
           << static_cast<uint64_t>(total) << " completed)";
      infoTooltip << static_cast<uint64_t>(done) << " of "
                  << static_cast<uint64_t>(total)
                  << " tracks are complete. A numeric ETA is intentionally "
                     "omitted for this small batch.";
    } else {
      info << "Done.";
      infoTooltip << "Analysis complete.";
    }
    SetProgressInfo(info.get_ptr(), infoTooltip.get_ptr());
    return;
  }

  // A single fast worker wave is not a representative throughput sample for a
  // parallel MIR batch. Two waves balance useful startup time and stability in
  // both the 150-track focus run and the accepted 7,398-track completion
  // replay. Update the projection only when another track completes; otherwise
  // a slow wave makes the ETA climb despite there being no new timing evidence.
  const t_size normalizedWorkerCount =
      static_cast<t_size>((std::max)(size_t{1}, workerCount));
  const t_size etaWarmupCompletions =
      (std::max)(static_cast<t_size>(16),
                 static_cast<t_size>(normalizedWorkerCount * 2));
  const bool numericEtaSupported =
      total > etaWarmupCompletions + normalizedWorkerCount;
  const bool throughputReady =
      numericEtaSupported && done >= etaWarmupCompletions && elapsed >= 10.0 &&
      done < total;
  const bool etaMature = throughputReady;

  const bool refreshEtaNow =
      !m_etaUiInitialized || done >= total ||
      std::chrono::duration_cast<std::chrono::seconds>(now - m_etaLastUiUpdate)
              .count() >=
          1;
  if (refreshEtaNow) {
    if (done >= total) {
      m_smoothedTotalTimeSeconds = elapsed;
      m_etaDisplayedRemaining = 0.0;
      m_etaUiInitialized = true;
    } else if (etaMature) {
      const bool hasNewCompletionSample =
          !m_etaUiInitialized || done > m_etaLastProjectionDone;
      if (hasNewCompletionSample) {
        const double averageCompletionInterval =
            elapsed / static_cast<double>(done);
        double rawTotalTime =
            elapsed + static_cast<double>(total - done) *
                          averageCompletionInterval;
        if (!std::isfinite(rawTotalTime) || rawTotalTime < elapsed)
          rawTotalTime = elapsed;

        if (!m_etaUiInitialized || m_smoothedTotalTimeSeconds <= elapsed) {
          m_smoothedTotalTimeSeconds = rawTotalTime;
        } else {
          double alpha = 0.15;
          if (total >= 128) alpha = 0.10;
          if (total >= 512) alpha = 0.08;

          double projectedTotal =
              alpha * rawTotalTime +
              (1.0 - alpha) * m_smoothedTotalTimeSeconds;
          const double sampleElapsed =
              (std::max)(1.0, elapsed - m_etaLastProjectionElapsed);
          const double maxProjectionCorrection =
              (std::clamp)(sampleElapsed * 2.0, 5.0, 30.0);
          projectedTotal =
              (std::clamp)(projectedTotal,
                           m_smoothedTotalTimeSeconds - maxProjectionCorrection,
                           m_smoothedTotalTimeSeconds + maxProjectionCorrection);
          m_smoothedTotalTimeSeconds =
              (std::max)(elapsed, projectedTotal);
        }
        m_etaLastProjectionDone = done;
        m_etaLastProjectionElapsed = elapsed;
      }
      m_etaDisplayedRemaining =
          (std::max)(0.0, m_smoothedTotalTimeSeconds - elapsed);
      m_etaUiInitialized = true;
    } else {
      m_etaDisplayedRemaining = 0.0;
      m_etaUiInitialized = false;
      m_etaLastProjectionDone = done;
      m_etaLastProjectionElapsed = elapsed;
    }
    m_etaLastUiUpdate = now;
  } else {
    return;
  }

  pfc::string_formatter info;
  pfc::string_formatter infoTooltip;
  info << "Time elapsed: " << FormatClock(elapsed) << ", ";
  infoTooltip << "Time elapsed: " << FormatClock(elapsed) << ". ";
  const bool useTwoWaveTail = total >= normalizedWorkerCount * 6;
  const t_size etaTailCompletions =
      normalizedWorkerCount * (useTwoWaveTail ? 2 : 1);
  const bool inQueueTail =
      workerCount > 0 && done < total &&
      done >= total - (std::min)(etaTailCompletions, total);
  const double trackP95Seconds = EstimateTrackP95Seconds();
  const double measuredTrackSeconds =
      (done > 0 && workerCount > 0)
          ? elapsed * static_cast<double>(workerCount) /
                static_cast<double>(done)
          : 0.0;
  const double longTrackThresholdSeconds =
      (std::clamp)((std::max)(trackP95Seconds, measuredTrackSeconds) * 1.5,
                   45.0, 300.0);
  const bool longRunningTail =
      inQueueTail && activeWorkerCount > 0 &&
      oldestActiveSeconds >= longTrackThresholdSeconds;
  if (done >= total) {
    info << "Done.";
    infoTooltip << "Analysis complete.";
  } else if (longRunningTail) {
    info << "finishing long-running track...";
    infoTooltip << "Finishing the remaining long-running track.";
  } else if (inQueueTail && done > 0 &&
             (!etaMature || m_etaDisplayedRemaining <= 45.0)) {
    info << "finishing...";
    infoTooltip << "Finishing the remaining active tracks.";
  } else if (!etaMature) {
    if (numericEtaSupported) {
      const auto completedWarmup =
          static_cast<uint64_t>((std::min)(done, etaWarmupCompletions));
      const auto requiredWarmup =
          static_cast<uint64_t>(etaWarmupCompletions);
      const auto activeWorkers = static_cast<uint64_t>(workerCount);
      info << "ETA calibrating: "
           << completedWarmup << "/" << requiredWarmup << " ("
           << activeWorkers << " workers)...";
      infoTooltip << "The remaining-time estimate is calibrating from "
                  << completedWarmup << " of " << requiredWarmup
                  << " completed tracks across " << activeWorkers
                  << " active workers. This calibrates only the ETA, not the "
                     "BPM analysis.";
    } else {
      info << "Estimating time...";
      infoTooltip << "The job is too small for a stable numeric remaining-time "
                     "estimate.";
    }
  } else {
    info << "Time remaining: " << FormatClock(m_etaDisplayedRemaining);
    infoTooltip << "Estimated time remaining: "
                << FormatClock(m_etaDisplayedRemaining) << ".";
  }

  SetProgressInfo(info.get_ptr(), infoTooltip.get_ptr());
}
pfc::string8 bpm_auto_analysis_thread::FormatClock(double seconds) {
  if (seconds < 0.0)
    seconds = 0.0;
  // Saturate to keep formatting arithmetic well-defined and overflow-safe.
  seconds = (std::min)(seconds, 359999.0);
  const auto total = static_cast<unsigned>(seconds + 0.5);
  const unsigned h = total / 3600;
  const unsigned m = (total % 3600) / 60;
  const unsigned s = total % 60;
  pfc::string_formatter out;
  if (h > 0)
    out << h << ":";
  out << pfc::format_int(m, 2) << ":" << pfc::format_int(s, 2);
  return out;
}

uint64_t bpm_auto_analysis_thread::SteadyMillis() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void bpm_auto_analysis_thread::RecordTrackWallTime(uint64_t elapsedMillis) {
  static constexpr std::array<uint64_t, kTrackDurationBinCount - 1>
      kUpperBoundsMillis = {5000,  10000, 15000, 20000, 30000, 45000,
                           60000, 90000, 120000, 180000, 300000};
  size_t bin = 0;
  while (bin < kUpperBoundsMillis.size() &&
         elapsedMillis > kUpperBoundsMillis[bin]) {
    ++bin;
  }
  m_trackDurationHistogram[bin].fetch_add(1, std::memory_order_relaxed);
}

double bpm_auto_analysis_thread::EstimateTrackP95Seconds() const {
  static constexpr std::array<double, kTrackDurationBinCount>
      kUpperBoundsSeconds = {5.0,  10.0, 15.0, 20.0, 30.0, 45.0,
                             60.0, 90.0, 120.0, 180.0, 300.0, 600.0};
  uint64_t total = 0;
  for (const auto& bin : m_trackDurationHistogram) {
    total += bin.load(std::memory_order_relaxed);
  }
  if (total == 0) return 0.0;

  const uint64_t target = (total * 95 + 99) / 100;
  uint64_t cumulative = 0;
  for (size_t i = 0; i < m_trackDurationHistogram.size(); ++i) {
    cumulative +=
        m_trackDurationHistogram[i].load(std::memory_order_relaxed);
    if (cumulative >= target) return kUpperBoundsSeconds[i];
  }
  return kUpperBoundsSeconds.back();
}

void bpm_auto_analysis_thread::ShiftActiveWorkerStarts(uint64_t pauseMillis) {
  if (pauseMillis == 0 ||
      !m_workerSlotsReady.load(std::memory_order_acquire) || !m_workerSlots) {
    return;
  }
  for (size_t worker = 0; worker < m_workerSlotCount; ++worker) {
    const t_size idx =
        m_workerSlots[worker].index.load(std::memory_order_acquire);
    if (idx >= m_tracks.get_count()) continue;
    const uint64_t started =
        m_workerSlots[worker].started_at_millis.load(std::memory_order_relaxed);
    if (started > 0) {
      m_workerSlots[worker].started_at_millis.fetch_add(
          pauseMillis, std::memory_order_relaxed);
    }
  }
}

LRESULT bpm_auto_analysis_thread::OnPauseClicked(UINT uNotifyCode, int nID,
                                                 CWindow wndCtl) {
  (void)uNotifyCode;
  (void)nID;
  (void)wndCtl;
  const bool paused = !m_pauseRequested.load();
  m_pauseRequested.store(paused);
  if (paused) {
    m_pauseBegan = std::chrono::steady_clock::now();
    m_pauseLatched = true;
  } else if (m_pauseLatched) {
    const double pauseSeconds = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() -
                                    m_pauseBegan)
                                    .count();
    m_pausedSeconds += pauseSeconds;
    ShiftActiveWorkerStarts(static_cast<uint64_t>(pauseSeconds * 1000.0 + 0.5));
    m_pauseLatched = false;
    m_pauseCv.notify_all();
  }
  m_etaUiInitialized = false;
  m_etaLastProjectionDone = m_doneCount.load(std::memory_order_relaxed);
  m_etaLastProjectionElapsed = 0.0;
  m_etaLastUiUpdate = std::chrono::steady_clock::now();
  m_smoothedTotalTimeSeconds = 0.0;
  m_etaDisplayedRemaining = 0.0;
  SetDlgItemText(ID_BPM_PROGRESS_PAUSE, paused ? _T("Resume") : _T("Pause"));
  UpdateStatusLine();
  return 0;
}

void bpm_auto_analysis_thread::RequestAbort() {
  if (m_abortRequested.exchange(true))
    return;
  if (m_pauseLatched) {
    m_pausedSeconds += std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - m_pauseBegan)
                           .count();
    m_pauseLatched = false;
  }
  m_pauseRequested.store(false);
  m_pauseCv.notify_all();
  m_abort.abort();
  GetDlgItem(ID_BPM_PROGRESS_ABORT).EnableWindow(FALSE);
  GetDlgItem(ID_BPM_PROGRESS_PAUSE).EnableWindow(FALSE);
  SetProgressInfo(
      "Cancelling...",
      "Cancellation requested. Waiting for active workers to stop safely.");
}

LRESULT bpm_auto_analysis_thread::OnAbortClicked(UINT uNotifyCode, int nID,
                                                 CWindow wndCtl) {
  (void)uNotifyCode;
  (void)nID;
  (void)wndCtl;
  RequestAbort();
  return 0;
}

void bpm_auto_analysis_thread::OnClose() { RequestAbort(); }

LRESULT bpm_auto_analysis_thread::OnDpiChanged(UINT, WPARAM, LPARAM lParam) {
  const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
  if (suggested != nullptr) {
    SetWindowPos(NULL, suggested->left, suggested->top,
                 suggested->right - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }

  InitLayoutMetrics();
  const size_t lines =
      (m_lastActiveCount == static_cast<size_t>(-1)) ? 1 : m_lastActiveCount;
  ApplyDynamicLayout(lines, true);
  UpdateStatusLine();
  return 0;
}

LRESULT bpm_auto_analysis_thread::OnWorkerDone(UINT, WPARAM wp, LPARAM) {
  m_wasAborted = (wp != 0);
  KillTimer(1);
  UpdateStatusLine();

  if (m_workerThread.joinable())
    m_workerThread.join();

  if (!m_wasAborted && core_api::assert_main_thread()) {
    if (!m_measuredCandidateInspectionMode &&
        cfg_smart_tempo_create_unmatched_playlist &&
        m_unmatched_tracks.get_count() > 0) {
      static_api_ptr_t<playlist_manager> playlistMan;
      pfc::string8 playlistName =
          smart_tempo::helpers::trim_copy(cfg_smart_tempo_playlist_name.get().get_ptr()).c_str();
      if (playlistName.is_empty()) playlistName = "Smart Tempo: Unmatched";
      const t_size playlist =
          playlistMan->find_or_create_playlist(playlistName.get_ptr());
      if (playlist != SIZE_MAX) {
        playlistMan->playlist_add_items_filter(playlist, m_unmatched_tracks,
                                               false);
      }
    }

    metadb_handle_list reviewHoldTracks;
    if (!m_measuredCandidateInspectionMode &&
        cfg_smart_tempo_create_review_playlist) {
      for (t_size index = 0;
           index < m_tracks.get_count() && index < m_suppress_write_results.size();
           ++index) {
        if (m_suppress_write_results[index] != 0) {
          reviewHoldTracks.add_item(m_tracks[index]);
        }
      }
    }

    if (!m_measuredCandidateInspectionMode &&
        cfg_smart_tempo_create_review_playlist) {
      static_api_ptr_t<playlist_manager> playlistMan;
      pfc::string8 playlistName = smart_tempo::helpers::trim_copy(
          cfg_smart_tempo_review_playlist_name.get().get_ptr()).c_str();
      if (playlistName.is_empty()) playlistName = "Smart Tempo: Needs BPM Review";
      const t_size playlist = reviewHoldTracks.get_count() > 0
          ? playlistMan->find_or_create_playlist(playlistName.get_ptr())
          : playlistMan->find_playlist(playlistName.get_ptr());
      if (playlist != SIZE_MAX) {
        const auto stats = reconcile_review_playlist(
            playlistMan.get_ptr(), playlist, m_tracks, reviewHoldTracks);
        FB2K_console_formatter()
            << "foo_smart_tempo: [Review Playlist] analyzed="
            << m_tracks.get_count() << ", holds="
            << reviewHoldTracks.get_count() << ", removed="
            << stats.removed_occurrences << ", added="
            << stats.added_tracks << ", removal_blocked="
            << (stats.removal_blocked ? 1 : 0) << ", playlist=\""
            << playlistName.get_ptr() << "\"";
      }
    }

    if (m_measuredCandidateInspectionMode) {
      if (m_tracks.get_count() == 1 &&
          !m_measured_bpm_candidate_results.empty()) {
        const bool hasCurrentResult =
            !m_suppress_write_results.empty() &&
            m_suppress_write_results[0] == 0 &&
            !m_bpm_results.empty() &&
            std::isfinite(m_bpm_results[0]) &&
            m_bpm_results[0] > 0.0;
        const double currentResultBpm =
            hasCurrentResult ? m_bpm_results[0] : 0.0;
        const file_info_impl* info =
            m_infos.get_size() > 0 ? &m_infos[0] : nullptr;
        show_direct_measured_bpm_candidate_dialog(
            m_tracks[0], std::move(m_measured_bpm_candidate_results[0]),
            make_track_identifier(m_tracks[0], info),
            currentResultBpm, hasCurrentResult);
      }
    } else {
      auto dlg = std::make_unique<bpm_result_dialog>(
          m_tracks, m_infos, m_bpm_results, m_confidence_results,
          m_routed_genre_results, m_method_results, m_error_results,
          m_suppress_write_results, m_routing_context_results,
          m_measured_bpm_candidate_results,
          m_processedTrackCount,
          m_analysisElapsedSeconds);
      dlg->Create(core_api::get_main_window(), NULL);
      if (dlg->IsWindow()) {
        dlg->ShowWindow(SW_SHOW);
        dlg.release();
      }
    }
  }

  DestroyWindow();
  return 0;
}

void bpm_auto_analysis_thread::PostNcDestroy() {
  static_api_ptr_t<message_loop>()->remove_message_filter(this);
  KillTimer(1);
  m_pauseRequested.store(false);
  m_pauseCv.notify_all();
  if (m_workerThread.joinable())
    m_workerThread.join();
  delete this;
}

bool bpm_auto_analysis_thread::pretranslate_message(MSG *p_msg) {
  if (m_tooltip.IsWindow())
    m_tooltip.RelayEvent(p_msg);
  if (m_hWnd != NULL && IsDialogMessage(p_msg))
    return true;
  return false;
}
