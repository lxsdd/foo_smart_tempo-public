#ifndef __BPM_PREFERENCES_PAGE_H__
#define __BPM_PREFERENCES_PAGE_H__

// clang-format off
#include "foobar2000/SDK/foobar2000.h"
#include "foobar2000/helpers/foobar2000+atl.h"
#include "foobar2000/helpers/atl-misc.h"
#include "foobar2000/SDK/coreDarkMode.h"
#include <atlctrls.h>
// clang-format on
#include <memory>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "resource.h"
#include "ibpm_analyzer.h"

namespace smart_tempo::helpers {
struct import_resolver_input_token;
struct import_resolver_result;
}

class bpm_preferences_page : public preferences_page_instance,
                             public CDialogImpl<bpm_preferences_page> {
public:
  static uint64_t NextImportInstanceNonce();
  explicit bpm_preferences_page(preferences_page_callback::ptr callback)
      : m_callback(callback),
        m_importInstanceNonce(NextImportInstanceNonce()),
        m_list_is_dirty(false) {}
  ~bpm_preferences_page() override;

  enum { IDD = IDD_BPM_PREFERENCES };

  t_uint32 get_state();
  void apply();
  void reset();
  fb2k::hwnd_t get_wnd() { return m_hWnd; }

  BEGIN_MSG_MAP_EX(bpm_preferences_page)
  MSG_WM_INITDIALOG(OnInitDialog)
  MSG_WM_SIZE(OnSize)
  MESSAGE_HANDLER_EX(WM_DPICHANGED, OnDpiChanged)
  MESSAGE_HANDLER_EX(WM_APP + 130, OnImportAsyncMessage)
  NOTIFY_HANDLER_EX(ID_CONFIG_PREF_TABS, TCN_SELCHANGE, OnTabChanged)

  COMMAND_HANDLER_EX(ID_CONFIG_BPM_PRECISION, CBN_SELCHANGE, OnComboBoxChange)
  COMMAND_HANDLER_EX(ID_CONFIG_BPM_TAG, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_CONFIDENCE_TAG_NAME, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_GENERIC_ANCHOR_TOKENS, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_SMART_TEMPO_PLAYLIST_NAME, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_CREATE_UNMATCHED_PLAYLIST, BN_CLICKED, OnButtonClicked)
  COMMAND_HANDLER_EX(ID_CONFIG_REVIEW_PLAYLIST_NAME, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_CREATE_REVIEW_PLAYLIST, BN_CLICKED, OnButtonClicked)
  COMMAND_HANDLER_EX(ID_CONFIG_SMART_TEMPO_ROUTING_TAG, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_WORKER_MODE, CBN_SELCHANGE, OnComboBoxChange)
  COMMAND_HANDLER_EX(ID_CONFIG_AUTO_WRITE_TAG, BN_CLICKED, OnButtonClicked)
  COMMAND_HANDLER_EX(ID_CONFIG_WRITE_CONFIDENCE_TAG, BN_CLICKED, OnButtonClicked)
  COMMAND_HANDLER_EX(ID_CONFIG_VERBOSE_LOGGING, BN_CLICKED, OnButtonClicked)
  COMMAND_HANDLER_EX(IDC_BTN_ROUTING_IMPORT, BN_CLICKED, OnButtonClicked)
  MSG_WM_HSCROLL(OnHScroll)
  END_MSG_MAP()

private:
  BOOL OnInitDialog(CWindow wndFocus, LPARAM lInitParam);
  void OnSize(UINT nType, CSize size);
  LRESULT OnDpiChanged(UINT, WPARAM, LPARAM);
  LRESULT OnImportAsyncMessage(UINT, WPARAM, LPARAM);
  void OnEditControlChange(UINT uNotifyCode, int nID, CWindow wndCtl);
  void OnComboBoxChange(UINT uNotifyCode, int nID, CWindow wndCtl);
  void OnButtonClicked(UINT uNotifyCode, int nID, CWindow wndCtl);
  void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar pScrollBar);
  void InitTabLayout();
  void UpdateTabLayout();
  void UpdateRuleListColumnWidths();
  void ShowTabPage(int index);
  LRESULT OnTabChanged(NMHDR *pnmh);
  void InitTooltips();
  void AddTooltip(int controlID, const TCHAR *text);
  void SetDynamicTooltip(int controlID, bool isEnabled, const TCHAR *text);
  void UpdateUnmatchedPlaylistUiState();
  void UpdateReviewPlaylistUiState();
  void UpdateConfidenceTagControls();
  HWND FindControlParent(int controlID, HWND* controlHandle = nullptr) const;
  HWND DialogForControl(int controlID) const;
  HWND ControlHandle(int controlID) const;
  void SetDlgItemTextX(int controlID, const TCHAR *text);
  void SetDlgItemTextUtf8X(int controlID, const char* text);
  pfc::string8 ReadDlgItemTextUtf8X(int controlID) const;
  void CheckDlgButtonX(int controlID, UINT check);
  UINT IsDlgButtonCheckedX(int controlID) const;
  void SetCheckboxFromBool(int controlID, bool checked);
  LRESULT SendDlgItemMessageX(int controlID, UINT msg, WPARAM wParam,
                              LPARAM lParam) const;
  void SetComboSelectionX(int controlID, int index);
  int GetComboSelectionX(int controlID) const;
  void SetControlEnabled(int controlID, bool enabled);
  LRESULT OnRuleListItemChanged(NMHDR *pnmh);
  void OnRuleCommand(int nID);
  pfc::string8 BuildRulesConfigFromList();
  const pfc::string8& GetRulesConfigSnapshot();
  void InvalidateRulesConfigSnapshot();
  void MarkRuleListChanged();
  void MarkRuleListChangedAndShowGenreRules();
  void SaveListToConfig();
  void ParseRulesToList(const char *rulesText);
  void UpdateRuleSelectionUiState();
  bool CheckIfRuleModified();
  void SetImportUiLock(bool locked);
  void PopulateRuleEditorsFromSelection();
  void ImportRulesFromLibrary();
  void StartImportResolveAsync(
      const std::vector<smart_tempo::helpers::import_resolver_input_token>& resolverInput,
      const std::vector<std::string>& rulePatterns,
      const std::string& anchorDisplayText, uint64_t generation);
  void ApplyResolverResultToUi(
      const smart_tempo::helpers::import_resolver_result& resolverResult,
      const std::vector<std::string>& previousRulePatterns);
  void JoinImportWorkerThread(bool requestAbort = true);
  void ImportRulesFromClipboard();
  void ExportRulesToClipboard();
  pfc::string8 BuildRulesExportText();
  void MoveSelectedRule(int delta);
  void InsertOrUpdateRule(bool isUpdate);
  int GetSelectedRuleIndex(bool requireSingleSelection = true);
  std::vector<int> GetSelectedRuleRows();
  struct RuleListRowValues {
    CString genres;
    CString centerBpm;
    CString spreadBpm;
  };
  RuleListRowValues ReadRuleListRow(int row);
  void WriteRuleListRow(int row, const RuleListRowValues& values);
  void SelectRuleRow(int row);
  void ShowGenreRulesTab();
  static bool ParseBpmText(const char *text, double &outValue);
  int ReadBpmWritePrecisionFromUi() const;
  int ReadWorkerModeFromUi() const;
  bool ReadCheckboxFromUi(int controlID) const;
  pfc::string8 ReadBpmTagNameFromUi() const;
  pfc::string8 ReadConfidenceTagNameFromUi() const;
  std::string ReadPlaylistNameFromUi() const;
  std::string ReadReviewPlaylistNameFromUi() const;
  std::string ReadRoutingScriptFromUi() const;
  std::string ReadGenericAnchorTokensFromUi() const;
  struct RuleEditorValues {
    pfc::string8 genresRaw;
    pfc::string8 centerRaw;
    std::string genresDisplay;
  };
  RuleEditorValues ReadRuleEditorValuesFromUi() const;
  static pfc::string8 UnformatRegexForUI(const char *regexStr);
  static pfc::string8 ConvertToRegexForConfig(const char *uiStr);
  bool HasChanged();
  void OnChanged();

  const preferences_page_callback::ptr m_callback;
  CTabCtrl m_tabs;
  class bpm_pref_page_tagging;
  class bpm_pref_page_algo;
  class bpm_pref_page_genres;
  friend class bpm_pref_page_tagging;
  friend class bpm_pref_page_algo;
  friend class bpm_pref_page_genres;
  std::unique_ptr<bpm_pref_page_tagging> m_pageTagging;
  std::unique_ptr<bpm_pref_page_algo> m_pageAlgo;
  std::unique_ptr<bpm_pref_page_genres> m_pageGenres;
  CListViewCtrl m_list_rules;
  bool m_layoutDone = false;
  fb2k::CCoreDarkModeHooks m_dark;
  CToolTipCtrl m_tooltip;
  bool m_ignoreChanges = false;
  bool m_importInProgress = false;
  const uint64_t m_importInstanceNonce;
  std::shared_ptr<std::atomic<bool>> m_importAbortToken;
  std::atomic<uint64_t> m_importGeneration{0};
  uint64_t m_activeImportGeneration = 0;
  std::thread m_importWorkerThread;
  bool m_list_is_dirty;
  bool m_rules_config_cache_valid = false;
  pfc::string8 m_rules_config_cache;
};

class bpm_preferences_page_impl
    : public preferences_page_impl<bpm_preferences_page> {
public:
  const char *get_name();
  GUID get_guid();
  GUID get_parent_guid();
};

#endif // __BPM_PREFERENCES_PAGE_H__
