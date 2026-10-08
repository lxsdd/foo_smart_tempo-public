#ifndef __BPM_MANUAL_DIALOG_H__
#define __BPM_MANUAL_DIALOG_H__

#include <string>
#include <vector>

#include "foobar2000/SDK/foobar2000.h"
#include "foobar2000/helpers/foobar2000+atl.h"
#include "foobar2000/SDK/coreDarkMode.h"
#include <atlctrls.h>

#include "resource.h"

void show_manual_bpm_dialog();

class bpm_manual_dialog : public CDialogImpl<bpm_manual_dialog>, 
						  private message_filter_impl_base,
						  private play_callback_impl_base,
						  private playlist_callback
{
public:
	enum { IDD = IDD_BPM_MANUAL_DIALOG };

	LONGLONG time_to_reset_average;
	std::vector<LONGLONG> tap_times;
	LONGLONG timer_resolution;

	BEGIN_MSG_MAP_EX(bpm_manual_dialog)
		MSG_WM_INITDIALOG(OnInitDialog)
		MSG_WM_ACTIVATE(OnActivate)
		MSG_WM_TIMER(OnTimer)
		MSG_WM_HSCROLL(OnHScroll)
		MSG_WM_DESTROY(OnDestroy)
		MESSAGE_HANDLER_EX(WM_DPICHANGED, OnDpiChanged)
		MESSAGE_HANDLER(WM_USER + 101, OnRegainFocus)
		COMMAND_HANDLER_EX(ID_BPM_MANUAL_TAP_BUTTON, BN_CLICKED, OnTapClicked)
		COMMAND_HANDLER_EX(ID_BPM_MANUAL_RESET_BUTTON, BN_CLICKED, OnResetClicked)
		COMMAND_HANDLER_EX(ID_BPM_MANUAL_UPDATE_TAG_BUTTON, BN_CLICKED, OnUpdateFileClicked)
		COMMAND_HANDLER_EX(ID_MANUAL_PREFERENCES_BUTTON, BN_CLICKED, OnPreferencesClicked)
		COMMAND_HANDLER_EX(IDC_CHK_MANUAL_ALWAYS_ON_TOP, BN_CLICKED, OnAlwaysOnTopClicked)
		COMMAND_HANDLER_EX(IDC_METRONOME_STOP, BN_CLICKED, OnMetronomeStop)
		COMMAND_HANDLER_EX(IDC_METRONOME_SYNC, BN_PUSHED, OnMetronomeSync)
		COMMAND_HANDLER_EX(IDC_METRONOME_NUDGE_EARLIER, BN_CLICKED, OnMetronomeNudgeEarlier)
		COMMAND_HANDLER_EX(IDC_METRONOME_NUDGE_LATER, BN_CLICKED, OnMetronomeNudgeLater)
		COMMAND_HANDLER_EX(IDC_METRONOME_METER, CBN_SELCHANGE, OnMetronomeMeterChanged)
		COMMAND_HANDLER_EX(IDCANCEL, BN_CLICKED, OnCancel)
		MSG_WM_CLOSE(OnClose);
	END_MSG_MAP()

	bpm_manual_dialog();

private:
	LRESULT OnInitDialog(CWindow wndFocus, LPARAM lInitParam);
	void OnActivate(UINT nState, BOOL bMinimized, CWindow wndOther);
	LRESULT OnDpiChanged(UINT, WPARAM, LPARAM);
	void RescaleControlsForDpi(UINT oldDpi, UINT newDpi);
	LRESULT OnRegainFocus(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);

	LRESULT OnTapClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
	LRESULT OnResetClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
	LRESULT OnUpdateFileClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
	LRESULT OnPreferencesClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
	LRESULT OnAlwaysOnTopClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
	LRESULT OnMetronomeStop(UINT uNotifyCode, int nID, CWindow wndCtl);
	LRESULT OnMetronomeSync(UINT uNotifyCode, int nID, CWindow wndCtl);
	LRESULT OnMetronomeNudgeEarlier(UINT uNotifyCode, int nID, CWindow wndCtl);
	LRESULT OnMetronomeNudgeLater(UINT uNotifyCode, int nID, CWindow wndCtl);
	LRESULT OnMetronomeMeterChanged(UINT uNotifyCode, int nID, CWindow wndCtl);
	void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar scrollBar);
	void OnTimer(UINT_PTR timerId);
	void OnDestroy();
	LRESULT OnCancel(UINT uNotifyCode, int nID, CWindow wndCtl);
	void OnClose();
	void PostNcDestroy();

	bool pretranslate_message(MSG *p_msg);

	void InitTooltips();
	void AddTooltip(int controlID, const TCHAR* text);
	bool IsAlwaysOnTopEnabled() const;
	void ApplyAlwaysOnTopState();
	void ResetBPM();
	void SetBPM(double bpm);
	void UpdateTapFeedback(bool stable);
	void UpdateTrackInfo();
	void InitMetronomeControls();
	void UpdateMetronomeState();
	static int MeterFromCombo(int selection);
	static int ComboFromMeter(int meter);

	// play_callback_impl_base
	void on_playback_new_track(metadb_handle_ptr p_track) override { UpdateTrackInfo(); }
	void on_playback_stop(play_control::t_stop_reason p_reason) override { UpdateTrackInfo(); }
	
	// playlist_callback
	void on_items_added(t_size p_playlist, t_size p_start, const pfc::list_base_const_t<metadb_handle_ptr> & p_data, const bit_array & p_selection) override { UpdateTrackInfo(); }
	void on_items_reordered(t_size p_playlist, const t_size * p_order, t_size p_count) override { UpdateTrackInfo(); }
	void on_items_removing(t_size p_playlist, const bit_array & p_mask, t_size p_old_count, t_size p_new_count) override {}
	void on_items_removed(t_size p_playlist, const bit_array & p_mask, t_size p_old_count, t_size p_new_count) override { UpdateTrackInfo(); }
	void on_items_selection_change(t_size p_playlist, const bit_array & p_affected, const bit_array & p_state) override { UpdateTrackInfo(); }
	void on_item_focus_change(t_size p_playlist, t_size p_from, t_size p_to) override { UpdateTrackInfo(); }
	void on_items_modified(t_size p_playlist, const bit_array & p_mask) override { UpdateTrackInfo(); }
	void on_items_modified_fromplayback(t_size p_playlist, const bit_array & p_mask, play_control::t_display_level p_level) override {}
	void on_items_replaced(t_size p_playlist, const bit_array & p_mask, const pfc::list_base_const_t<playlist_callback::t_on_items_replaced_entry> & p_data) override { UpdateTrackInfo(); }
	void on_item_ensure_visible(t_size p_playlist, t_size p_idx) override {}
	void on_playlist_activate(t_size p_old, t_size p_new) override { UpdateTrackInfo(); }
	void on_playlist_created(t_size p_index, const char * p_name, t_size p_name_len) override {}
	void on_playlists_reorder(const t_size * p_order, t_size p_count) override {}
	void on_playlists_removing(const bit_array & p_mask, t_size p_old_count, t_size p_new_count) override {}
	void on_playlists_removed(const bit_array & p_mask, t_size p_old_count, t_size p_new_count) override {}
	void on_playlist_renamed(t_size p_index, const char * p_new_name, t_size p_new_name_len) override {}
	void on_default_format_changed() override {}
	void on_playback_order_changed(t_size p_new_index) override {}
	void on_playlist_locked(t_size p_playlist, bool p_locked) override {}

	double m_bpm;
	pfc::string8 m_targetTrackKey;
	bool m_targetTrackInitialized = false;
	UINT m_currentDpi = 96;
	std::wstring m_trackInfoTooltipText;
	fb2k::CCoreDarkModeHooks m_dark;
	CToolTipCtrl m_tooltip;
	unsigned m_metronomeTimerTicks = 0;
	static constexpr UINT_PTR kMetronomeTimer = 1;
	static constexpr UINT kMetronomeTimerIntervalMs = 2;
	static constexpr unsigned kMetronomeStatusRefreshTicks = 125;
};

#endif // __BPM_MANUAL_DIALOG_H__

