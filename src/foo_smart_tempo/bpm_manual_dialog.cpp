#include "stdafx.h"

#include "bpm_manual_dialog.h"
#include "globals.h"
#include "preferences.h"
#include "format_bpm.h"
#include "file_info_filter_bpm.h"
#include "analysis_telemetry.h"
#include "guid.h"
#include "tag_write_dispatch.h"
#include "ui_scaling.h"
#include "manual_metronome.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>

namespace {
HWND g_manualBpmDialog = nullptr;

constexpr int64_t kMinTapAverageCount = 8;
constexpr int64_t kMaxTapAverageCount = 120;
constexpr int64_t kMinTapResetSeconds = 1;
constexpr int64_t kMaxTapResetSeconds = 60;
constexpr size_t kMinimumStableTapCount = 8;
constexpr double kStablePhaseMadSeconds = 0.035;
constexpr double kStableRecentDeltaRatio = 0.005;

struct tap_estimate {
	double bpm = 0.0;
	double phaseMadSeconds = 0.0;
	double recentDeltaRatio = 0.0;
	bool valid = false;
	bool stable = false;
};

double median(std::vector<double> values) {
	if (values.empty()) return 0.0;
	const size_t middle = values.size() / 2;
	std::nth_element(values.begin(), values.begin() + middle, values.end());
	const double upper = values[middle];
	if ((values.size() % 2) != 0) return upper;
	std::nth_element(values.begin(), values.begin() + middle - 1,
	                 values.begin() + middle);
	return (values[middle - 1] + upper) * 0.5;
}

double estimate_interval_seconds(const std::vector<LONGLONG>& times,
	                             size_t first, size_t last,
	                             LONGLONG timerResolution) {
	if (timerResolution <= 0 || last <= first || last > times.size()) return 0.0;
	std::vector<double> pairwiseIntervals;
	const size_t count = last - first;
	pairwiseIntervals.reserve(count * (count - 1) / 2);
	for (size_t left = first; left + 1 < last; ++left) {
		for (size_t right = left + 1; right < last; ++right) {
			const double elapsed = static_cast<double>(times[right] - times[left]) /
			                       static_cast<double>(timerResolution);
			pairwiseIntervals.push_back(
				elapsed / static_cast<double>(right - left));
		}
	}
	return median(std::move(pairwiseIntervals));
}

tap_estimate estimate_tap_tempo(const std::vector<LONGLONG>& times,
	                            LONGLONG timerResolution) {
	tap_estimate result;
	if (times.size() < 2 || timerResolution <= 0) return result;

	const double interval =
		estimate_interval_seconds(times, 0, times.size(), timerResolution);
	if (!std::isfinite(interval) || interval <= 0.0) return result;

	result.bpm = 60.0 / interval;
	result.valid = std::isfinite(result.bpm) && result.bpm > 0.0;
	if (!result.valid) return result;

	std::vector<double> phaseResiduals;
	phaseResiduals.reserve(times.size());
	std::vector<double> intercepts;
	intercepts.reserve(times.size());
	for (size_t index = 0; index < times.size(); ++index) {
		const double timestampSeconds =
			static_cast<double>(times[index] - times.front()) /
			static_cast<double>(timerResolution);
		intercepts.push_back(timestampSeconds -
		                     interval * static_cast<double>(index));
	}
	const double intercept = median(std::move(intercepts));
	for (size_t index = 0; index < times.size(); ++index) {
		const double timestampSeconds =
			static_cast<double>(times[index] - times.front()) /
			static_cast<double>(timerResolution);
		phaseResiduals.push_back(std::abs(
			timestampSeconds -
			(intercept + interval * static_cast<double>(index))));
	}
	result.phaseMadSeconds = median(std::move(phaseResiduals));

	if (times.size() >= kMinimumStableTapCount) {
		const size_t recentFirst = times.size() - kMinimumStableTapCount;
		const double recentInterval = estimate_interval_seconds(
			times, recentFirst, times.size(), timerResolution);
		if (std::isfinite(recentInterval) && recentInterval > 0.0) {
			const double recentBpm = 60.0 / recentInterval;
			result.recentDeltaRatio =
				std::abs(recentBpm - result.bpm) / result.bpm;
			result.stable =
				result.phaseMadSeconds <= kStablePhaseMadSeconds &&
				result.recentDeltaRatio <= kStableRecentDeltaRatio;
		}
	}
	return result;
}

size_t sanitized_tap_average_count() noexcept {
	return static_cast<size_t>((std::clamp)(
		static_cast<int64_t>(bpm_config_taps_to_average),
		kMinTapAverageCount, kMaxTapAverageCount));
}

int64_t sanitized_tap_reset_seconds() noexcept {
	return (std::clamp)(
		static_cast<int64_t>(bpm_config_seconds_to_reset_average),
		kMinTapResetSeconds, kMaxTapResetSeconds);
}
}

void show_manual_bpm_dialog() {
	if (::IsWindow(g_manualBpmDialog)) {
		::ShowWindow(g_manualBpmDialog,
		             ::IsIconic(g_manualBpmDialog) ? SW_RESTORE : SW_SHOW);
		::SetForegroundWindow(g_manualBpmDialog);
		::BringWindowToTop(g_manualBpmDialog);
		const HWND tapButton =
			::GetDlgItem(g_manualBpmDialog, ID_BPM_MANUAL_TAP_BUTTON);
		if (::IsWindow(tapButton)) ::SetFocus(tapButton);
		return;
	}
	auto dlg = std::make_unique<bpm_manual_dialog>();
	dlg->Create(core_api::get_main_window(), NULL);
	if (dlg->IsWindow()) {
		g_manualBpmDialog = dlg->m_hWnd;
		dlg->ShowWindow(SW_SHOWNORMAL);
		dlg.release();
	}
}

bpm_manual_dialog::bpm_manual_dialog() :
	m_bpm(0.0)
{
	LARGE_INTEGER res;
	QueryPerformanceFrequency(&res);
	timer_resolution = res.QuadPart;
	time_to_reset_average =
		static_cast<LONGLONG>(sanitized_tap_reset_seconds()) * timer_resolution;
}

LRESULT bpm_manual_dialog::OnInitDialog(CWindow wndFocus, LPARAM lInitParam)
{
	(void)wndFocus; (void)lInitParam;
    m_dark.AddDialogWithControls(m_hWnd);
	m_currentDpi = smart_tempo::ui::detect_window_dpi(m_hWnd);

	InitTooltips();
	InitMetronomeControls();
	UpdateTrackInfo();

	CheckDlgButton(IDC_CHK_MANUAL_ALWAYS_ON_TOP, BST_UNCHECKED);
	ApplyAlwaysOnTopState();
	GetDlgItem(ID_BPM_MANUAL_TAP_BUTTON).SetFocus();

	ResetBPM();

	// Register for playback and playlist changes to update the label.
	fb2k::std_api_get<play_callback_manager>()->register_callback(this, play_callback_impl_base::flag_on_playback_new_track | play_callback_impl_base::flag_on_playback_stop, false);
	fb2k::std_api_get<playlist_manager>()->register_callback(this, playlist_callback::flag_all);

	static_api_ptr_t<message_loop>()->add_message_filter(this);

	return 0;
}

void bpm_manual_dialog::OnActivate(UINT nState, BOOL bMinimized, CWindow wndOther)
{
	(void)wndOther; (void)bMinimized;
	if (nState == WA_INACTIVE && IsWindowVisible() && IsAlwaysOnTopEnabled()) {
		// Use a posted message to regain focus. This prevents the synchronous focus steal 
		// from killing button click messages that caused the deactivation.
		PostMessage(WM_USER + 101, 0, 0);
	}
}

LRESULT bpm_manual_dialog::OnDpiChanged(UINT, WPARAM wParam, LPARAM lParam)
{
	const UINT newDpiRaw = HIWORD(static_cast<DWORD>(wParam));
	const UINT newDpi = (newDpiRaw > 0)
		? newDpiRaw
		: smart_tempo::ui::detect_window_dpi(m_hWnd);
	const UINT oldDpi = (m_currentDpi > 0) ? m_currentDpi : 96;

	const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
	if (suggested != nullptr) {
		SetWindowPos(NULL, suggested->left, suggested->top,
		             suggested->right - suggested->left,
		             suggested->bottom - suggested->top,
		             SWP_NOZORDER | SWP_NOACTIVATE);
	}
	RescaleControlsForDpi(oldDpi, newDpi);
	m_currentDpi = newDpi;
	ApplyAlwaysOnTopState();
	return 0;
}

void bpm_manual_dialog::RescaleControlsForDpi(UINT oldDpi, UINT newDpi)
{
	if (oldDpi == 0) oldDpi = 96;
	if (newDpi == 0) newDpi = 96;
	if (oldDpi == newDpi) return;

	// Keep controls proportionally aligned after per-monitor DPI switches.
	constexpr std::array<int, 18> kControlIds = {
		ID_BPM_MANUAL_TAP_BUTTON,
		ID_BPM_MANUAL_RESET_BUTTON,
		ID_BPM_MANUAL_UPDATE_TAG_BUTTON,
		ID_MANUAL_PREFERENCES_BUTTON,
		IDC_CHK_MANUAL_ALWAYS_ON_TOP,
		IDCANCEL,
		ID_BPM_MANUAL_BPM,
		ID_BPM_MANUAL_STABILITY,
		ID_MANUAL_BPM_TAG,
		ID_BPM_MANUAL_TRACK_INFO,
		IDC_METRONOME_GROUP,
		IDC_METRONOME_STOP,
		IDC_METRONOME_SYNC,
		IDC_METRONOME_NUDGE_EARLIER,
		IDC_METRONOME_NUDGE_LATER,
		IDC_METRONOME_METER,
		IDC_METRONOME_VOLUME,
		IDC_METRONOME_STATUS
	};

	for (const int controlId : kControlIds) {
		const HWND control = ::GetDlgItem(m_hWnd, controlId);
		if (control == nullptr) continue;

		RECT rc{};
		::GetWindowRect(control, &rc);
		::MapWindowPoints(HWND_DESKTOP, m_hWnd, reinterpret_cast<LPPOINT>(&rc), 2);

		const int left = smart_tempo::ui::scale_between_dpi(rc.left, oldDpi, newDpi);
		const int top = smart_tempo::ui::scale_between_dpi(rc.top, oldDpi, newDpi);
		const int width =
			smart_tempo::ui::scale_between_dpi(rc.right - rc.left, oldDpi, newDpi);
		const int height =
			smart_tempo::ui::scale_between_dpi(rc.bottom - rc.top, oldDpi, newDpi);

		::SetWindowPos(control, nullptr, left, top, width, height,
		               SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

LRESULT bpm_manual_dialog::OnRegainFocus(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	(void)uMsg; (void)wParam; (void)lParam; (void)bHandled;
	if (IsWindowVisible() && IsAlwaysOnTopEnabled()) {
		ApplyAlwaysOnTopState();
		SetForegroundWindow(m_hWnd);
		CWindow tapButton = GetDlgItem(ID_BPM_MANUAL_TAP_BUTTON);
		if (tapButton.IsWindow() && GetFocus() != tapButton.m_hWnd) {
			tapButton.SetFocus();
		}
	}
	return 0;
}

void bpm_manual_dialog::UpdateTrackInfo()
{
	if (!m_hWnd) return;

	metadb_handle_ptr track;
	bool haveTrack = false;
	
	// Prioritize now playing
	haveTrack = fb2k::std_api_get<playback_control>()->get_now_playing(track);
	
	// Fallback to active playlist focus/selection
	if (!haveTrack) {
		metadb_handle_list sel;
		fb2k::std_api_get<playlist_manager>()->activeplaylist_get_selected_items(sel);
		if (sel.get_count() > 0) {
			track = sel[0];
			haveTrack = true;
		}
	}

	pfc::string_formatter targetTrackKey;
	if (haveTrack && track.is_valid()) {
		targetTrackKey << track->get_path() << "\n"
		               << track->get_subsong_index();
	}
	const bool targetChanged =
		m_targetTrackInitialized &&
		std::strcmp(m_targetTrackKey.get_ptr(), targetTrackKey.get_ptr()) != 0;
	m_targetTrackKey = targetTrackKey;
	m_targetTrackInitialized = true;
	if (targetChanged) ResetBPM();

	pfc::string_formatter bpm_tag_label;
	bpm_tag_label << "BPM will be written to %" << bpm_config_bpm_tag.get().get_ptr() << "% tag.";
	uSetDlgItemText(m_hWnd, ID_MANUAL_BPM_TAG, bpm_tag_label);

	pfc::string_formatter track_info;
	if (haveTrack) {
		metadb_info_container::ptr info = track->get_info_ref();
		if (info.is_valid()) {
			const file_info& fi = info->info();
			const char* artist = fi.meta_get("ARTIST", 0);
			const char* title = fi.meta_get("TITLE", 0);
			
			if (artist && title) {
				track_info << artist << " - " << title;
			} else if (title) {
				track_info << title;
			} else {
				track_info << pfc::string_filename(track->get_path());
			}
		} else {
			track_info << pfc::string_filename(track->get_path());
		}
	} else {
		track_info << "No track playing or selected.";
	}
	
	uSetDlgItemText(m_hWnd, ID_BPM_MANUAL_TRACK_INFO, track_info);

	if (m_tooltip.IsWindow()) {
		TOOLINFO ti = {};
		ti.cbSize = sizeof(ti);
		ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		ti.hwnd = m_hWnd;
		ti.uId = (UINT_PTR)(HWND)GetDlgItem(ID_BPM_MANUAL_TRACK_INFO);
		pfc::stringcvt::string_wide_from_utf8 trackInfoWide(track_info);
		m_trackInfoTooltipText.assign(trackInfoWide.get_ptr());
		ti.lpszText = const_cast<LPWSTR>(m_trackInfoTooltipText.c_str());
		m_tooltip.UpdateTipText(&ti);
	}
}

LRESULT bpm_manual_dialog::OnUpdateFileClicked(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	(void)uNotifyCode; (void)nID; (void)wndCtl;

	metadb_handle_ptr track;
	bool haveTrack = fb2k::std_api_get<playback_control>()->get_now_playing(track);
	if (!haveTrack) {
		metadb_handle_list sel;
		fb2k::std_api_get<playlist_manager>()->activeplaylist_get_selected_items(sel);
		if (sel.get_count() > 0) { track = sel[0]; haveTrack = true; }
	}

	if (haveTrack) {
		if (smart_tempo::verbose_console_logging_enabled()) {
			FB2K_console_formatter() << "foo_smart_tempo: Manual update triggered for " << track->get_path() << " with " << m_bpm << " BPM";
		}

		metadb_handle_list list;
		list.add_item(track);

		auto changedItemCounter = std::make_shared<std::atomic<uint64_t>>(0);
		smart_tempo::tag_write::safe_update_info_async(
			list,
			new service_impl_t<file_info_filter_bpm>(
				track, bpm_config_bpm_tag.get().get_ptr(), m_bpm,
				-1.0, changedItemCounter),
			"manual-tap-update", m_hWnd, true, changedItemCounter);
	} else {
		if (smart_tempo::verbose_console_logging_enabled()) {
			FB2K_console_formatter() << "foo_smart_tempo: Manual update failed - no track playing or selected.";
		}
	}

	return 0;
}

LRESULT bpm_manual_dialog::OnTapClicked(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	(void)uNotifyCode; (void)nID; (void)wndCtl;
	SetForegroundWindow(m_hWnd);

	LARGE_INTEGER time;
	QueryPerformanceCounter(&time);

	const bool restartedAfterPause =
		!tap_times.empty() &&
		((time.QuadPart - tap_times.back()) > time_to_reset_average);
	if (restartedAfterPause) ResetBPM();

	if (tap_times.size() >= sanitized_tap_average_count()) {
		tap_times.erase(tap_times.begin());
	}
	tap_times.push_back(time.QuadPart);

	const tap_estimate estimate = estimate_tap_tempo(tap_times, timer_resolution);
	if (estimate.valid) {
		SetBPM(estimate.bpm);
		UpdateTapFeedback(estimate.stable);
	} else {
		SetBPM(0.0);
		UpdateTapFeedback(false);
	}

	return 0;
}

LRESULT bpm_manual_dialog::OnResetClicked(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	(void)uNotifyCode; (void)nID; (void)wndCtl;
	ResetBPM();
	return 0;
}

LRESULT bpm_manual_dialog::OnPreferencesClicked(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	(void)uNotifyCode; (void)nID; (void)wndCtl;
	fb2k::std_api_get<ui_control>()->show_preferences(guid_bpm_preferences);
	return 0;
}

LRESULT bpm_manual_dialog::OnAlwaysOnTopClicked(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	(void)uNotifyCode; (void)nID; (void)wndCtl;
	ApplyAlwaysOnTopState();
	return 0;
}

int bpm_manual_dialog::MeterFromCombo(int selection)
{
	constexpr int meters[] = {1, 2, 3, 4, 6};
	return selection >= 0 && selection < 5 ? meters[selection] : 4;
}

int bpm_manual_dialog::ComboFromMeter(int meter)
{
	constexpr int meters[] = {1, 2, 3, 4, 6};
	for (int index = 0; index < 5; ++index) {
		if (meters[index] == meter) return index;
	}
	return 3;
}

void bpm_manual_dialog::InitMetronomeControls()
{
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
	meter.SetCurSel(ComboFromMeter(metronome.meter()));
	smart_tempo::manual_metronome::initialize_volume_slider(
		GetDlgItem(IDC_METRONOME_VOLUME), m_hWnd, metronome.volume());

	AddTooltip(IDC_METRONOME_STOP,
		_T("Stop the metronome."));
	AddTooltip(IDC_METRONOME_SYNC,
		_T("Start or restart the metronome now. The immediate click defines beat one. Track playback is controlled separately by foobar2000."));
	AddTooltip(IDC_METRONOME_NUDGE_EARLIER,
		_T("Move every click 10 milliseconds earlier without changing BPM. Use this when the metronome sounds late."));
	AddTooltip(IDC_METRONOME_NUDGE_LATER,
		_T("Move every click 10 milliseconds later without changing BPM. Use this when the metronome sounds early."));
	AddTooltip(IDC_METRONOME_METER,
		_T("Choose an accent pattern only. This does not alter the tapped BPM."));
	AddTooltip(IDC_METRONOME_VOLUME,
		_T("Adjust the metronome click volume independently of the track. Click anywhere on the slider or drag the thumb."));
	UpdateMetronomeState();
	RedrawWindow(nullptr, nullptr,
		RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void bpm_manual_dialog::UpdateMetronomeState()
{
	if (!m_hWnd) return;
	auto& metronome = smart_tempo::manual_metronome::controller::instance();
	const bool active = metronome.is_active_for(m_hWnd);
	const bool validBpm = std::isfinite(m_bpm) && m_bpm >= 40.0 && m_bpm <= 220.0;
	GetDlgItem(IDC_METRONOME_STOP).EnableWindow(active);
	GetDlgItem(IDC_METRONOME_SYNC).EnableWindow(active || validBpm);
	GetDlgItem(IDC_METRONOME_NUDGE_EARLIER).EnableWindow(active);
	GetDlgItem(IDC_METRONOME_NUDGE_LATER).EnableWindow(active);

	pfc::string_formatter status;
	if (active) {
		status << "Running "
		       << pfc::format_float(metronome.bpm(), 0, 2) << " | "
		       << pfc::format_float(
		              metronome.phase_nudge_milliseconds(), 0, 0)
		       << " ms";
	} else if (validBpm) {
		status << "Ready: " << pfc::format_float(m_bpm, 0, 2) << " BPM";
	} else {
		status << "Tap at least twice (40-220 BPM)";
	}
	uSetDlgItemText(m_hWnd, IDC_METRONOME_STATUS, status);
}

LRESULT bpm_manual_dialog::OnMetronomeStop(UINT, int, CWindow)
{
	smart_tempo::manual_metronome::controller::instance().disable(m_hWnd);
	KillTimer(kMetronomeTimer);
	UpdateMetronomeState();
	return 0;
}

LRESULT bpm_manual_dialog::OnMetronomeSync(UINT, int, CWindow)
{
	auto& metronome = smart_tempo::manual_metronome::controller::instance();
	if (metronome.is_active_for(m_hWnd)) {
		metronome.sync_now(m_hWnd);
	} else {
		CComboBox meter(GetDlgItem(IDC_METRONOME_METER));
		CTrackBarCtrl volume(GetDlgItem(IDC_METRONOME_VOLUME));
		metronome.enable(
			m_hWnd, m_bpm, MeterFromCombo(meter.GetCurSel()), volume.GetPos());
	}
	if (metronome.is_active_for(m_hWnd)) {
		SetTimer(kMetronomeTimer, kMetronomeTimerIntervalMs);
	}
	UpdateMetronomeState();
	return 0;
}

LRESULT bpm_manual_dialog::OnMetronomeNudgeEarlier(UINT, int, CWindow)
{
	smart_tempo::manual_metronome::controller::instance().nudge_seconds(m_hWnd, -0.010);
	UpdateMetronomeState();
	return 0;
}

LRESULT bpm_manual_dialog::OnMetronomeNudgeLater(UINT, int, CWindow)
{
	smart_tempo::manual_metronome::controller::instance().nudge_seconds(m_hWnd, 0.010);
	UpdateMetronomeState();
	return 0;
}

LRESULT bpm_manual_dialog::OnMetronomeMeterChanged(UINT, int, CWindow)
{
	CComboBox meter(GetDlgItem(IDC_METRONOME_METER));
	smart_tempo::manual_metronome::controller::instance().set_meter(
		m_hWnd, MeterFromCombo(meter.GetCurSel()));
	UpdateMetronomeState();
	return 0;
}

void bpm_manual_dialog::OnHScroll(UINT, UINT, CScrollBar scrollBar)
{
	if (scrollBar.m_hWnd != GetDlgItem(IDC_METRONOME_VOLUME).m_hWnd) return;
	CTrackBarCtrl volume(GetDlgItem(IDC_METRONOME_VOLUME));
	smart_tempo::manual_metronome::controller::instance().set_volume(
		m_hWnd, volume.GetPos());
}

void bpm_manual_dialog::OnTimer(UINT_PTR timerId)
{
	if (timerId != kMetronomeTimer) return;
	auto& metronome = smart_tempo::manual_metronome::controller::instance();
	if (!metronome.is_active_for(m_hWnd)) {
		KillTimer(kMetronomeTimer);
		UpdateMetronomeState();
		return;
	}
	metronome.tick(m_hWnd);
	if ((m_metronomeTimerTicks++ % kMetronomeStatusRefreshTicks) == 0) {
		UpdateMetronomeState();
	}
}

LRESULT bpm_manual_dialog::OnCancel(UINT uNotifyCode, int nID, CWindow wndCtl)
{
	(void)uNotifyCode; (void)nID; (void)wndCtl;
	DestroyWindow();
	return 0;
}

void bpm_manual_dialog::OnClose()
{
	DestroyWindow();
}

void bpm_manual_dialog::OnDestroy()
{
	KillTimer(kMetronomeTimer);
	smart_tempo::manual_metronome::controller::instance().release(m_hWnd);
}

void bpm_manual_dialog::PostNcDestroy()
{
	if (g_manualBpmDialog == m_hWnd) g_manualBpmDialog = nullptr;
	static_api_ptr_t<message_loop>()->remove_message_filter(this);
	fb2k::std_api_get<play_callback_manager>()->unregister_callback(this);
	fb2k::std_api_get<playlist_manager>()->unregister_callback(this);
	delete this;
}

bool bpm_manual_dialog::pretranslate_message(MSG *p_msg)
{
	if (m_tooltip.IsWindow()) m_tooltip.RelayEvent(p_msg);
	if (m_hWnd != NULL && IsDialogMessage(p_msg)) return true;
	return false;
}


void bpm_manual_dialog::AddTooltip(int controlID, const TCHAR* text)
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

bool bpm_manual_dialog::IsAlwaysOnTopEnabled() const
{
	return m_hWnd != NULL &&
	       ::IsDlgButtonChecked(m_hWnd, IDC_CHK_MANUAL_ALWAYS_ON_TOP) == BST_CHECKED;
}

void bpm_manual_dialog::ApplyAlwaysOnTopState()
{
	if (m_hWnd == NULL) return;
	SetWindowPos(IsAlwaysOnTopEnabled() ? HWND_TOPMOST : HWND_NOTOPMOST,
	             0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void bpm_manual_dialog::InitTooltips()
{
	if (m_tooltip.IsWindow()) return;
	if (!m_tooltip.Create(m_hWnd, NULL, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP)) return;

	m_tooltip.SetMaxTipWidth(500);
	m_tooltip.SetDelayTime(TTDT_INITIAL, 350);
	m_tooltip.Activate(TRUE);

	AddTooltip(ID_BPM_MANUAL_TAP_BUTTON, _T("Tap in time with the beat. The calculated BPM updates from your recent taps."));
	AddTooltip(ID_BPM_MANUAL_RESET_BUTTON, _T("Reset current tap history and BPM value."));
	AddTooltip(ID_BPM_MANUAL_UPDATE_TAG_BUTTON, _T("Write the displayed BPM to the configured tag of the current track."));
	AddTooltip(ID_MANUAL_PREFERENCES_BUTTON, _T("Open the Smart Tempo preferences page."));
	AddTooltip(IDC_CHK_MANUAL_ALWAYS_ON_TOP, _T("Keep this manual BPM dialog above foobar2000 while tapping. Disable for normal window focus behavior."));
	AddTooltip(ID_BPM_MANUAL_BPM, _T("Current manual BPM value using the selected write precision."));
	AddTooltip(ID_BPM_MANUAL_STABILITY, _T("Shows tap timing consistency, not whether you chose the musical half-time or double-time pulse. Stable requires at least eight consistent taps."));
	AddTooltip(ID_MANUAL_BPM_TAG, _T("Shows which tag field will be updated."));
	AddTooltip(ID_BPM_MANUAL_TRACK_INFO, _T("Displays the currently selected track for manual BPM tapping."));
	AddTooltip(IDCANCEL, _T("Close manual BPM calculation dialog."));
}
void bpm_manual_dialog::ResetBPM()
{
	smart_tempo::manual_metronome::controller::instance().disable(m_hWnd);
	KillTimer(kMetronomeTimer);
	tap_times.clear();
	tap_times.reserve(sanitized_tap_average_count());
	SetBPM(0.0);
	UpdateTapFeedback(false);
}

void bpm_manual_dialog::SetBPM(double bpm)
{
	m_bpm = bpm;
	if (m_hWnd) uSetDlgItemText(m_hWnd, ID_BPM_MANUAL_BPM, format_bpm(m_bpm, get_write_precision()));
	if (m_hWnd) {
		smart_tempo::manual_metronome::controller::instance().set_bpm(m_hWnd, bpm);
		UpdateMetronomeState();
	}
}

void bpm_manual_dialog::UpdateTapFeedback(bool stable)
{
	if (!m_hWnd) return;
	pfc::string_formatter feedback;
	if (tap_times.empty()) {
		feedback << "Tap to begin";
	} else if (tap_times.size() == 1) {
		feedback << "1 tap - continue";
	} else if (tap_times.size() < kMinimumStableTapCount) {
		feedback << "Collecting " << tap_times.size() << "/"
		         << kMinimumStableTapCount << " taps";
	} else if (stable) {
		feedback << "Stable (" << tap_times.size() << " taps)";
	} else {
		feedback << "Stabilizing (" << tap_times.size() << " taps)";
	}
	uSetDlgItemText(m_hWnd, ID_BPM_MANUAL_STABILITY, feedback);
}
