#include "stdafx.h"

#include "bpm_preferences_page.h"
#include "preferences.h"
#include "smart_tempo_helpers.h"
#include "smart_tempo_mapper.h"
#include "ui_scaling.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <atlframe.h>

namespace {
using smart_tempo::helpers::canonicalize_import_token;
using smart_tempo::helpers::casefold_codepoint_less;
using smart_tempo::helpers::import_resolver_input_token;
using smart_tempo::helpers::import_resolver_result;
using smart_tempo::helpers::join_tokens_csv;
using smart_tempo::helpers::normalize_genre_token;
using smart_tempo::helpers::normalize_routing_script_text;
using smart_tempo::helpers::parse_rule_rhs_fields;
using smart_tempo::helpers::resolve_import_genres;
using smart_tempo::helpers::split_import_tokens_library;
using smart_tempo::helpers::tokenize_genre_text;
using smart_tempo::helpers::trim_copy;

constexpr UINT kMsgImportAsync = WM_APP + 130;
constexpr int kRuleColumnGenres = 0;
constexpr int kRuleColumnCenter = 1;
constexpr int kRuleColumnSpread = 2;
constexpr double kNewRuleHalfWidthBpm = 15.0;
constexpr int kSecondsPerWindowChoices[] = {5, 10, 15, 20, 30, 45, 60, 90};
constexpr int kWindowsPerTrackChoices[] = {1, 3, 5, 10, 20, 50};

template <size_t N>
int FindPresetIndex(int value, const int (&choices)[N]) {
  for (size_t i = 0; i < N; ++i) {
    if (choices[i] == value) return static_cast<int>(i);
  }
  return -1;
}


enum class import_async_event : WPARAM {
  resolve_complete = 1,
  worker_error = 2
};

struct import_token_bucket {
  std::string representative_raw;
  size_t occurrence_count = 0;
};

struct import_completion_payload {
  uint64_t generation = 0;
  bool success = false;
  import_resolver_result resolver_result;
  std::vector<std::string> previous_rule_patterns;
  std::string error_message;
};

enum class smart_tempo_message_buttons {
  ok,
  action_cancel,
};

HWND resolve_dialog_owner(HWND preferredOwner) {
  if (preferredOwner != nullptr && ::IsWindow(preferredOwner)) {
    return preferredOwner;
  }
  return core_api::get_main_window();
}

class smart_tempo_message_dialog
    : public CDialogImpl<smart_tempo_message_dialog> {
public:
  enum { IDD = IDD_BPM_OVERWRITE_PROMPT_DIALOG };

  smart_tempo_message_dialog(HWND owner, const wchar_t* title,
                             const wchar_t* promptText,
                             const wchar_t* actionText,
                             const wchar_t* cancelText,
                             smart_tempo_message_buttons buttons)
      : m_owner(resolve_dialog_owner(owner)),
        m_title(title != nullptr ? title : L"Smart Tempo"),
        m_promptText(promptText != nullptr ? promptText : L""),
        m_actionText(actionText != nullptr ? actionText : L"OK"),
        m_cancelText(cancelText != nullptr ? cancelText : L"Cancel"),
        m_buttons(buttons) {}

  BEGIN_MSG_MAP_EX(smart_tempo_message_dialog)
  MSG_WM_INITDIALOG(OnInitDialog)
  COMMAND_ID_HANDLER_EX(IDYES, OnButtonClicked)
  COMMAND_ID_HANDLER_EX(IDNO, OnButtonClicked)
  COMMAND_ID_HANDLER_EX(IDCANCEL, OnButtonClicked)
  MESSAGE_HANDLER_EX(WM_DPICHANGED, OnDpiChanged)
  MSG_WM_CLOSE(OnClose)
  END_MSG_MAP()

private:
  BOOL OnInitDialog(CWindow, LPARAM) {
    m_dark.AddDialogWithControls(m_hWnd);
    SetWindowTextW(m_title.c_str());
    SetDlgItemTextW(IDC_BPM_OVERWRITE_PROMPT_TEXT, m_promptText.c_str());
    SetDlgItemTextW(IDYES, m_actionText.c_str());
    SetDlgItemTextW(IDNO, m_cancelText.c_str());

    HWND actionButton = GetDlgItem(IDYES);
    HWND cancelButton = GetDlgItem(IDNO);
    CWindow closeBtn(GetDlgItem(IDCANCEL));
    if (closeBtn.IsWindow()) closeBtn.ShowWindow(SW_HIDE);

    if (m_buttons == smart_tempo_message_buttons::ok) {
      if (cancelButton != nullptr) ::ShowWindow(cancelButton, SW_HIDE);
      if (actionButton != nullptr) {
        ::SendMessage(actionButton, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
        ::SendMessage(m_hWnd, DM_SETDEFID, IDYES, 0);
        ::SetFocus(actionButton);
        LayoutOkMessageDialog(actionButton);
      }
      CenterWindow(m_owner);
      return FALSE;
    }

    if (actionButton != nullptr) {
      ::SendMessage(actionButton, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
    }
    if (cancelButton != nullptr) {
      ::SendMessage(cancelButton, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
      ::SendMessage(m_hWnd, DM_SETDEFID, IDNO, 0);
      ::SetFocus(cancelButton);
    }

    CenterWindow(m_owner);
    return FALSE;
  }

  void OnButtonClicked(UINT, int buttonID, CWindow) {
    if (m_buttons == smart_tempo_message_buttons::ok) {
      EndDialog(IDOK);
      return;
    }
    EndDialog(buttonID == IDYES ? IDYES : IDNO);
  }

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

  void OnClose() {
    EndDialog(m_buttons == smart_tempo_message_buttons::ok ? IDOK : IDNO);
  }

  int MeasurePromptHeight(int widthPixels) const {
    HDC dc = ::GetDC(m_hWnd);
    if (dc == nullptr) return 0;

    HWND textWindow = GetDlgItem(IDC_BPM_OVERWRITE_PROMPT_TEXT);
    HFONT font = textWindow != nullptr
                     ? reinterpret_cast<HFONT>(
                           ::SendMessage(textWindow, WM_GETFONT, 0, 0))
                     : nullptr;
    HGDIOBJ oldFont = nullptr;
    if (font != nullptr) oldFont = ::SelectObject(dc, font);

    RECT measureRect = {0, 0, widthPixels, 0};
    ::DrawTextW(dc, m_promptText.c_str(), -1, &measureRect,
                DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);

    if (oldFont != nullptr) ::SelectObject(dc, oldFont);
    ::ReleaseDC(m_hWnd, dc);
    return measureRect.bottom - measureRect.top;
  }

  void LayoutOkMessageDialog(HWND actionButton) {
    HWND textWindow = GetDlgItem(IDC_BPM_OVERWRITE_PROMPT_TEXT);
    if (textWindow == nullptr || actionButton == nullptr) return;

    RECT clientRect = {};
    RECT windowRect = {};
    RECT textRect = {};
    RECT buttonRect = {};
    GetClientRect(&clientRect);
    GetWindowRect(&windowRect);
    ::GetWindowRect(textWindow, &textRect);
    ::GetWindowRect(actionButton, &buttonRect);
    ::MapWindowPoints(HWND_DESKTOP, m_hWnd, reinterpret_cast<LPPOINT>(&textRect), 2);
    ::MapWindowPoints(HWND_DESKTOP, m_hWnd,
                      reinterpret_cast<LPPOINT>(&buttonRect), 2);

    const int textLeft = static_cast<int>(textRect.left);
    const int textWidth =
        std::max(1, static_cast<int>(textRect.right - textRect.left));
    const int buttonWidth = buttonRect.right - buttonRect.left;
    const int buttonHeight = buttonRect.bottom - buttonRect.top;
    const int topMargin = std::max(12, static_cast<int>(textRect.top));
    const int gap = std::max(16, topMargin);
    const int bottomMargin = topMargin;
    const int measuredTextHeight =
        std::max(16, MeasurePromptHeight(textWidth));
    const int textHeight = std::max(measuredTextHeight, 16);
    const int targetClientHeight =
        topMargin + textHeight + gap + buttonHeight + bottomMargin;
    const int currentClientHeight =
        static_cast<int>(clientRect.bottom - clientRect.top);

    if (targetClientHeight != currentClientHeight) {
      const int chromeHeight =
          static_cast<int>(windowRect.bottom - windowRect.top) -
          currentClientHeight;
      SetWindowPos(NULL, 0, 0, windowRect.right - windowRect.left,
                   targetClientHeight + chromeHeight,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      GetClientRect(&clientRect);
    }

    const int buttonX =
        std::max(0, (static_cast<int>(clientRect.right - clientRect.left) -
                     buttonWidth) /
                        2);
    const int buttonY = topMargin + textHeight + gap;
    ::SetWindowPos(textWindow, NULL, textLeft, topMargin, textWidth, textHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    ::SetWindowPos(actionButton, NULL, buttonX, buttonY, buttonWidth,
                   buttonHeight, SWP_NOZORDER | SWP_NOACTIVATE);
  }

  HWND m_owner = nullptr;
  std::wstring m_title;
  std::wstring m_promptText;
  std::wstring m_actionText;
  std::wstring m_cancelText;
  smart_tempo_message_buttons m_buttons = smart_tempo_message_buttons::ok;
  fb2k::CCoreDarkModeHooks m_dark;
};

bool show_smart_tempo_confirm_dialog(HWND owner, const wchar_t* title,
                                     const wchar_t* promptText,
                                     const wchar_t* actionText,
                                     const wchar_t* cancelText) {
  smart_tempo_message_dialog dialog(owner, title, promptText, actionText,
                                    cancelText,
                                    smart_tempo_message_buttons::action_cancel);
  return dialog.DoModal(owner) == IDYES;
}

void show_smart_tempo_message_dialog(HWND owner, const wchar_t* title,
                                     const wchar_t* promptText,
                                     const wchar_t* okText = L"OK") {
  smart_tempo_message_dialog dialog(owner, title, promptText, okText, L"",
                                    smart_tempo_message_buttons::ok);
  dialog.DoModal(owner);
}

std::string build_import_wizard_key(const std::string& value) {
  const std::string trimmed = trim_copy(value);
  if (trimmed.empty()) return {};
  const std::string canonical = canonicalize_import_token(trimmed);
  if (!canonical.empty()) return canonical;
  return trimmed;
}

bool import_wizard_display_less(const std::string& aRaw, const std::string& bRaw) {
  const std::string a = trim_copy(aRaw);
  const std::string b = trim_copy(bRaw);
  if (casefold_codepoint_less(a, b)) return true;
  if (casefold_codepoint_less(b, a)) return false;
  return a < b;
}

struct import_completion_key {
  uint64_t instance_nonce = 0;
  uint64_t generation = 0;

  bool operator==(const import_completion_key& other) const noexcept {
    return instance_nonce == other.instance_nonce &&
           generation == other.generation;
  }
};

struct import_completion_key_hash {
  size_t operator()(const import_completion_key& key) const noexcept {
    const size_t hNonce = std::hash<uint64_t>{}(key.instance_nonce);
    const size_t hGen = std::hash<uint64_t>{}(key.generation);
    constexpr size_t kHashMixConstant =
        static_cast<size_t>(0x9e3779b97f4a7c15ull);
    return hNonce ^ (hGen + kHashMixConstant + (hNonce << 6) +
                     (hNonce >> 2));
  }
};

std::mutex g_import_completion_mutex;
std::unordered_map<import_completion_key,
                   std::shared_ptr<import_completion_payload>,
                   import_completion_key_hash>
    g_import_completions;

void store_import_completion(
    uint64_t instanceNonce, uint64_t generation,
    std::shared_ptr<import_completion_payload> payload) {
  std::lock_guard<std::mutex> lock(g_import_completion_mutex);
  g_import_completions[import_completion_key{instanceNonce, generation}] =
      std::move(payload);
}

std::shared_ptr<import_completion_payload> take_import_completion(
    uint64_t instanceNonce, uint64_t generation) {
  std::lock_guard<std::mutex> lock(g_import_completion_mutex);
  const auto it = g_import_completions.find(
      import_completion_key{instanceNonce, generation});
  if (it == g_import_completions.end()) return {};
  auto payload = std::move(it->second);
  g_import_completions.erase(it);
  return payload;
}

void discard_import_completion(uint64_t instanceNonce, uint64_t generation) {
  std::lock_guard<std::mutex> lock(g_import_completion_mutex);
  g_import_completions.erase(import_completion_key{instanceNonce, generation});
}

void discard_import_completions_for_instance(uint64_t instanceNonce) {
  std::lock_guard<std::mutex> lock(g_import_completion_mutex);
  for (auto it = g_import_completions.begin();
       it != g_import_completions.end();) {
    if (it->first.instance_nonce == instanceNonce) {
      it = g_import_completions.erase(it);
    } else {
      ++it;
    }
  }
}

void format_routing_values_from_item(
    const metadb_handle_ptr& item,
    const service_ptr_t<titleformat_object>& routingScript,
    pfc::string8& routedValues) {
  routedValues.reset();
  if (!item.is_valid() || !routingScript.is_valid()) return;

  metadb_info_container::ptr infoRef;
  if (item->get_info_ref(infoRef) && infoRef.is_valid()) {
    item->format_title_from_external_info(infoRef->info(), nullptr,
                                          routedValues, routingScript, nullptr);
    return;
  }

  file_info_impl fallbackInfo;
  if (item->get_info(fallbackInfo)) {
    item->format_title_from_external_info(fallbackInfo, nullptr, routedValues,
                                          routingScript, nullptr);
  }
}

using smart_tempo::ui::scale_for_window_dpi;

pfc::string8 sanitize_tag_field_name(const char* rawValue, const char* fallbackValue) {
  const std::string trimmed = trim_copy(rawValue != nullptr ? rawValue : "");
  if (trimmed.empty()) return pfc::string8(fallbackValue);

  return pfc::string8(trimmed.c_str());
}

const TCHAR* tag_name_write_disabled_tooltip() {
  return _T("Enable Write tag to edit and use this tag name.");
}

const TCHAR* confidence_tag_name_enabled_tooltip() {
  return _T("Tag name used to write confidence (for example BPM_CONFIDENCE).");
}

const TCHAR* write_confidence_enabled_tooltip() {
  return _T("If enabled, confidence values are written to file tags.");
}

CString list_item_text(CListViewCtrl& list, int item, int subItem) {
  TCHAR buffer[2048] = {};
  list.GetItemText(item, subItem, buffer, _countof(buffer));
  return CString(buffer);
}

bool read_utf8_text_from_clipboard(HWND owner, pfc::string8& outText) {
  outText.reset();
  if (!::OpenClipboard(owner)) return false;

  bool ok = false;
  HGLOBAL hData = ::GetClipboardData(CF_UNICODETEXT);
  if (hData != nullptr) {
    const wchar_t* wideText = static_cast<const wchar_t*>(::GlobalLock(hData));
    if (wideText != nullptr) {
      const pfc::stringcvt::string_utf8_from_wide utf8(wideText);
      outText = utf8;
      ::GlobalUnlock(hData);
      ok = true;
    }
  }

  ::CloseClipboard();
  return ok;
}

bool write_utf8_text_to_clipboard(HWND owner, const char* utf8Text) {
  if (utf8Text == nullptr) return false;
  if (!::OpenClipboard(owner)) return false;

  bool ok = false;
  const pfc::stringcvt::string_wide_from_utf8 wideText(utf8Text);
  const wchar_t* source = wideText.get_ptr();
  const size_t charCount = std::wcslen(source) + 1;
  const size_t byteCount = charCount * sizeof(wchar_t);

  HGLOBAL hData = ::GlobalAlloc(GMEM_MOVEABLE, byteCount);
  if (hData != nullptr) {
    void* memory = ::GlobalLock(hData);
    if (memory != nullptr) {
      std::memcpy(memory, source, byteCount);
      ::GlobalUnlock(hData);
      if (::EmptyClipboard()) {
        if (::SetClipboardData(CF_UNICODETEXT, hData) != nullptr) {
          ok = true;
          hData = nullptr;  // ownership transferred to the system clipboard
        }
      }
    }
  }

  if (hData != nullptr) ::GlobalFree(hData);
  ::CloseClipboard();
  return ok;
}

bool is_manual_list_delimiter(char ch) noexcept {
  return ch == ';' || ch == ',' || ch == '/' || ch == '|';
}

std::vector<std::string> split_manual_list_tokens(std::string_view input) {
  std::vector<std::string> out;
  std::string current;
  current.reserve(input.size());
  for (const char ch : input) {
    if (is_manual_list_delimiter(ch)) {
      const std::string trimmed = trim_copy(current);
      if (!trimmed.empty()) out.push_back(trimmed);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  const std::string trimmed = trim_copy(current);
  if (!trimmed.empty()) out.push_back(trimmed);
  return out;
}

std::string join_semicolon_tokens(const std::vector<std::string>& tokens) {
  std::string out;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (i != 0) out += "; ";
    out += tokens[i];
  }
  return out;
}

std::string smooth_token_list_for_display(std::string_view input) {
  const auto rawTokens = split_manual_list_tokens(input);
  std::unordered_set<std::string> seenCanonical;
  std::vector<std::string> out;
  out.reserve(rawTokens.size());
  for (const auto& token : rawTokens) {
    const std::string canonical = canonicalize_import_token(token);
    if (canonical.empty()) continue;
    if (!seenCanonical.emplace(canonical).second) continue;
    const std::string display = trim_copy(token);
    if (display.empty()) continue;
    out.push_back(display);
  }
  return join_semicolon_tokens(out);
}

std::string smooth_anchor_tokens_for_display(std::string_view input) {
  return smooth_token_list_for_display(input);
}

std::string smooth_rule_genres_for_display(std::string_view input) {
  return smooth_token_list_for_display(input);
}

class routing_import_wizard_dialog
    : public CDialogImpl<routing_import_wizard_dialog>,
      public CDialogResize<routing_import_wizard_dialog> {
public:
  enum { IDD = IDD_BPM_ROUTING_IMPORT_DIALOG };

  explicit routing_import_wizard_dialog(std::vector<pfc::string8> values) {
    m_values.reserve(values.size());
    for (const auto& v : values) m_values.emplace_back(v.get_ptr());
  }

  const std::vector<std::string>& selected_values() const noexcept {
    return m_selected_values;
  }

  BEGIN_MSG_MAP_EX(routing_import_wizard_dialog)
  MSG_WM_INITDIALOG(OnInitDialog)
  MESSAGE_HANDLER_EX(WM_DPICHANGED, OnDpiChanged)
  NOTIFY_HANDLER_EX(IDC_IMPORT_VALUES_LIST, LVN_ITEMCHANGED, OnListItemChanged)
  NOTIFY_HANDLER_EX(IDC_IMPORT_VALUES_LIST, LVN_COLUMNCLICK, OnColumnClick)
  COMMAND_ID_HANDLER_EX(IDC_BTN_IMPORT_SELECT_ALL, OnSelectAll)
  COMMAND_ID_HANDLER_EX(IDC_BTN_IMPORT_CLEAR_ALL, OnClearAll)
  COMMAND_ID_HANDLER_EX(IDOK, OnApply)
  COMMAND_ID_HANDLER_EX(IDCANCEL, OnCancel)
  CHAIN_MSG_MAP(CDialogResize<routing_import_wizard_dialog>)
  END_MSG_MAP()

  BEGIN_DLGRESIZE_MAP(routing_import_wizard_dialog)
  DLGRESIZE_CONTROL(IDC_STATIC_IMPORT_HINT, DLSZ_SIZE_X)
  DLGRESIZE_CONTROL(IDC_IMPORT_VALUES_LIST, DLSZ_SIZE_X | DLSZ_SIZE_Y)
  DLGRESIZE_CONTROL(IDC_BTN_IMPORT_SELECT_ALL, DLSZ_MOVE_X | DLSZ_MOVE_Y)
  DLGRESIZE_CONTROL(IDC_BTN_IMPORT_CLEAR_ALL, DLSZ_MOVE_X | DLSZ_MOVE_Y)
  DLGRESIZE_CONTROL(IDOK, DLSZ_MOVE_X | DLSZ_MOVE_Y)
  DLGRESIZE_CONTROL(IDCANCEL, DLSZ_MOVE_Y)
  END_DLGRESIZE_MAP()

private:
  void UpdateListColumnWidth() {
    if (!m_list.IsWindow()) return;
    m_list.SetColumnWidth(0, scale_for_window_dpi(m_hWnd, 320));
  }

  void SelectListRow(int row) {
    if (!m_list.IsWindow() || row < 0) return;
    m_list.SetItemState(row, LVIS_SELECTED | LVIS_FOCUSED,
                        LVIS_SELECTED | LVIS_FOCUSED);
    m_list.EnsureVisible(row, FALSE);
  }

  void SetAllListChecks(BOOL checked) {
    if (!m_list.IsWindow()) return;
    const int count = m_list.GetItemCount();
    for (int i = 0; i < count; ++i) {
      m_list.SetCheckState(i, checked);
    }
  }

  void RebuildListFromValues(const std::unordered_set<std::string>& checkedKeys = {},
                             const std::string* focusKey = nullptr) {
    if (!m_list.IsWindow()) return;
    m_list.DeleteAllItems();

    int selectedRow = -1;
    for (const auto& value : m_values) {
      const std::string key = build_import_wizard_key(value);
      const pfc::stringcvt::string_wide_from_utf8 valueW(value.c_str());
      const int row = m_list.InsertItem(m_list.GetItemCount(), valueW.get_ptr());
      m_list.SetCheckState(row, checkedKeys.find(key) != checkedKeys.end() ? TRUE
                                                                            : FALSE);
      if (focusKey != nullptr && selectedRow < 0 && *focusKey == key) {
        selectedRow = row;
      }
    }

    if (selectedRow >= 0) {
      SelectListRow(selectedRow);
    }
    UpdateListColumnWidth();
  }

  BOOL OnInitDialog(CWindow, LPARAM) {
    m_dark.AddDialogWithControls(m_hWnd);
    m_list.Attach(GetDlgItem(IDC_IMPORT_VALUES_LIST));
    if (m_list.IsWindow()) {
      m_list.ModifyStyle(0, LVS_REPORT | LVS_SHOWSELALWAYS);
      m_list.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                      LVS_EX_CHECKBOXES);
      while (m_list.DeleteColumn(0)) {
      }
      m_list.InsertColumn(0, _T("Library Tag Value"), LVCFMT_LEFT,
                          scale_for_window_dpi(m_hWnd, 320));
      RebuildListFromValues();
    }
    DlgResize_Init(false, true, WS_THICKFRAME | WS_CLIPCHILDREN);

    HWND applyButton = GetDlgItem(IDOK);
    HWND cancelButton = GetDlgItem(IDCANCEL);
    if (applyButton != nullptr) {
      ::SendMessage(applyButton, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
    }
    if (cancelButton != nullptr) {
      ::SendMessage(cancelButton, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
      ::SendMessage(m_hWnd, DM_SETDEFID, IDCANCEL, 0);
      ::SetFocus(cancelButton);
    }

    CenterWindow(resolve_dialog_owner(GetParent()));
    return FALSE;
  }

  LRESULT OnDpiChanged(UINT, WPARAM, LPARAM lParam) {
    const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
    if (suggested != nullptr) {
      SetWindowPos(NULL, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    }
    UpdateListColumnWidth();
    return 0;
  }

  LRESULT OnListItemChanged(NMHDR* pnmh) {
    if (!m_list.IsWindow() || pnmh == nullptr) return 0;
    const auto* pNMLV = reinterpret_cast<const NMLISTVIEW*>(pnmh);
    if (pNMLV == nullptr || pNMLV->iItem < 0) return 0;

    static bool s_updating = false;
    if (s_updating) return 0;

    const UINT oldStateImage = pNMLV->uOldState & LVIS_STATEIMAGEMASK;
    const UINT newStateImage = pNMLV->uNewState & LVIS_STATEIMAGEMASK;
    if (oldStateImage == newStateImage) return 0;
    if ((pNMLV->uNewState & LVIS_SELECTED) == 0) return 0;

    const BOOL newChecked = m_list.GetCheckState(pNMLV->iItem) ? TRUE : FALSE;
    s_updating = true;
    for (int i = m_list.GetNextItem(-1, LVNI_SELECTED); i != -1;
         i = m_list.GetNextItem(i, LVNI_SELECTED)) {
      if (i == pNMLV->iItem) continue;
      m_list.SetCheckState(i, newChecked);
    }
    s_updating = false;
    return 0;
  }

  LRESULT OnColumnClick(NMHDR* pnmh) {
    if (!m_list.IsWindow() || pnmh == nullptr) return 0;
    const auto* pNMLV = reinterpret_cast<const NMLISTVIEW*>(pnmh);
    if (pNMLV == nullptr) return 0;

    const int clickedColumn = pNMLV->iSubItem;
    if (clickedColumn == m_sort_column) {
      m_sort_ascending = !m_sort_ascending;
    } else {
      m_sort_column = clickedColumn;
      m_sort_ascending = true;
    }

    std::unordered_set<std::string> checkedKeys;
    std::string selectedKey;
    const int count = m_list.GetItemCount();
    checkedKeys.reserve((size_t)count);
    for (int row = 0; row < count; ++row) {
      const pfc::stringcvt::string_utf8_from_wide text(
          list_item_text(m_list, row, 0).GetString());
      const std::string key = build_import_wizard_key(text.get_ptr());
      if (key.empty()) continue;
      if (m_list.GetCheckState(row)) checkedKeys.emplace(key);
      if (selectedKey.empty() &&
          (m_list.GetItemState(row, LVIS_SELECTED) & LVIS_SELECTED)) {
        selectedKey = key;
      }
    }

    std::ranges::sort(m_values, [this](const std::string& a,
                                       const std::string& b) {
      if (m_sort_ascending) return import_wizard_display_less(a, b);
      return import_wizard_display_less(b, a);
    });

    const std::string* focus = selectedKey.empty() ? nullptr : &selectedKey;
    RebuildListFromValues(checkedKeys, focus);
    return 0;
  }

  void OnSelectAll(UINT, int, CWindow) {
    SetAllListChecks(TRUE);
  }

  void OnClearAll(UINT, int, CWindow) {
    SetAllListChecks(FALSE);
  }

  void OnApply(UINT, int, CWindow) {
    m_selected_values.clear();
    if (m_list.IsWindow()) {
      const int count = m_list.GetItemCount();
      for (int i = 0; i < count; ++i) {
        if (!m_list.GetCheckState(i)) continue;
        const CString value = list_item_text(m_list, i, 0);
        const pfc::stringcvt::string_utf8_from_wide valueUtf8(value.GetString());
        const std::string cleaned = trim_copy(valueUtf8.get_ptr());
        if (cleaned.empty()) continue;
        m_selected_values.push_back(cleaned);
      }
    }
    if (m_selected_values.empty()) {
      show_smart_tempo_message_dialog(
          m_hWnd, L"Information",
          L"Please check at least one item to import.");
      return;
    }
    EndDialog(IDOK);
  }

  void OnCancel(UINT, int, CWindow) { EndDialog(IDCANCEL); }

  std::vector<std::string> m_values;
  std::vector<std::string> m_selected_values;
  int m_sort_column = -1;
  bool m_sort_ascending = true;
  CListViewCtrl m_list;
  fb2k::CCoreDarkModeHooks m_dark;
};
} // namespace

class bpm_preferences_page::bpm_pref_page_tagging
    : public CDialogImpl<bpm_pref_page_tagging> {
public:
  enum { IDD = IDD_BPM_PREF_TAGGING };

  void SetParent(bpm_preferences_page* parent) { m_parent = parent; }

  BEGIN_MSG_MAP_EX(bpm_pref_page_tagging)
  COMMAND_HANDLER_EX(ID_CONFIG_BPM_PRECISION, CBN_SELCHANGE, OnComboBoxChange)
  COMMAND_HANDLER_EX(ID_CONFIG_BPM_TAG, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_CONFIDENCE_TAG_NAME, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_AUTO_WRITE_TAG, BN_CLICKED, OnButtonClicked)
  COMMAND_HANDLER_EX(ID_CONFIG_WRITE_CONFIDENCE_TAG, BN_CLICKED, OnButtonClicked)
  END_MSG_MAP()

private:
  void OnEditControlChange(UINT uNotifyCode, int nID, CWindow wndCtl) {
    if (m_parent) m_parent->OnEditControlChange(uNotifyCode, nID, wndCtl);
  }
  void OnComboBoxChange(UINT uNotifyCode, int nID, CWindow wndCtl) {
    if (m_parent) m_parent->OnComboBoxChange(uNotifyCode, nID, wndCtl);
  }
  void OnButtonClicked(UINT uNotifyCode, int nID, CWindow wndCtl) {
    if (m_parent) m_parent->OnButtonClicked(uNotifyCode, nID, wndCtl);
  }
  bpm_preferences_page* m_parent = nullptr;
};

class bpm_preferences_page::bpm_pref_page_algo
    : public CDialogImpl<bpm_pref_page_algo> {
public:
  enum { IDD = IDD_BPM_PREF_ALGO };

  void SetParent(bpm_preferences_page* parent) { m_parent = parent; }

  BEGIN_MSG_MAP_EX(bpm_pref_page_algo)
  COMMAND_HANDLER_EX(ID_CONFIG_GENERIC_ANCHOR_TOKENS, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_SMART_TEMPO_PLAYLIST_NAME, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_CREATE_UNMATCHED_PLAYLIST, BN_CLICKED, OnButtonClicked)
  COMMAND_HANDLER_EX(ID_CONFIG_REVIEW_PLAYLIST_NAME, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_CREATE_REVIEW_PLAYLIST, BN_CLICKED, OnButtonClicked)
  COMMAND_HANDLER_EX(ID_CONFIG_SMART_TEMPO_ROUTING_TAG, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(ID_CONFIG_WORKER_MODE, CBN_SELCHANGE, OnComboBoxChange)
  COMMAND_HANDLER_EX(ID_CONFIG_ANALYSIS_SECONDS, CBN_SELCHANGE, OnComboBoxChange)
  COMMAND_HANDLER_EX(ID_CONFIG_ANALYSIS_PASSES, CBN_SELCHANGE, OnComboBoxChange)
  COMMAND_HANDLER_EX(ID_CONFIG_VERBOSE_LOGGING, BN_CLICKED, OnButtonClicked)
  COMMAND_HANDLER_EX(IDC_BTN_ROUTING_IMPORT, BN_CLICKED, OnButtonClicked)
  MSG_WM_HSCROLL(OnHScroll)
  END_MSG_MAP()

private:
  void OnEditControlChange(UINT uNotifyCode, int nID, CWindow wndCtl) {
    if (m_parent) m_parent->OnEditControlChange(uNotifyCode, nID, wndCtl);
  }
  void OnComboBoxChange(UINT uNotifyCode, int nID, CWindow wndCtl) {
    if (m_parent) m_parent->OnComboBoxChange(uNotifyCode, nID, wndCtl);
  }
  void OnButtonClicked(UINT uNotifyCode, int nID, CWindow wndCtl) {
    if (m_parent) m_parent->OnButtonClicked(uNotifyCode, nID, wndCtl);
  }
  void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar pScrollBar) {
    if (m_parent) m_parent->OnHScroll(nSBCode, nPos, pScrollBar);
  }

  bpm_preferences_page* m_parent = nullptr;
};

class bpm_preferences_page::bpm_pref_page_genres
    : public CDialogImpl<bpm_pref_page_genres> {
public:
  enum { IDD = IDD_BPM_PREF_GENRES };

  void SetParent(bpm_preferences_page* parent) { m_parent = parent; }

  BEGIN_MSG_MAP_EX(bpm_pref_page_genres)
  NOTIFY_HANDLER_EX(IDC_LIST_GENRE_RULES, LVN_ITEMCHANGED, OnRuleListItemChanged)
  COMMAND_HANDLER_EX(IDC_EDIT_RULE_GENRES, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(IDC_EDIT_RULE_MIN, EN_CHANGE, OnEditControlChange)
  COMMAND_HANDLER_EX(IDC_BTN_RULE_ADD, BN_CLICKED, OnRuleButtonClicked)
  COMMAND_HANDLER_EX(IDC_BTN_RULE_UPDATE, BN_CLICKED, OnRuleButtonClicked)
  COMMAND_HANDLER_EX(IDC_BTN_RULE_REMOVE, BN_CLICKED, OnRuleButtonClicked)
  COMMAND_HANDLER_EX(IDC_BTN_RULE_UP, BN_CLICKED, OnRuleButtonClicked)
  COMMAND_HANDLER_EX(IDC_BTN_RULE_DOWN, BN_CLICKED, OnRuleButtonClicked)
  COMMAND_HANDLER_EX(IDC_BTN_RULE_IMPORT_CLIPBOARD, BN_CLICKED, OnRuleButtonClicked)
  COMMAND_HANDLER_EX(IDC_BTN_RULE_EXPORT_CLIPBOARD, BN_CLICKED, OnRuleButtonClicked)
  END_MSG_MAP()

private:
  LRESULT OnRuleListItemChanged(NMHDR* pnmh) {
    if (m_parent) return m_parent->OnRuleListItemChanged(pnmh);
    return 0;
  }

  void OnEditControlChange(UINT uNotifyCode, int nID, CWindow wndCtl) {
    if (m_parent) m_parent->OnEditControlChange(uNotifyCode, nID, wndCtl);
  }

  void OnComboBoxChange(UINT uNotifyCode, int nID, CWindow wndCtl) {
    if (m_parent) m_parent->OnComboBoxChange(uNotifyCode, nID, wndCtl);
  }

  void OnRuleButtonClicked(UINT uNotifyCode, int nID, CWindow wndCtl) {
    (void)uNotifyCode;
    (void)wndCtl;
    if (m_parent) m_parent->OnRuleCommand(nID);
  }

  bpm_preferences_page* m_parent = nullptr;
};

bpm_preferences_page::~bpm_preferences_page() {
  if (m_importAbortToken) {
    m_importAbortToken->store(true, std::memory_order_relaxed);
  }
  JoinImportWorkerThread(true);
  discard_import_completions_for_instance(m_importInstanceNonce);
}

void bpm_preferences_page::JoinImportWorkerThread(bool requestAbort) {
  if (requestAbort && m_importAbortToken) {
    m_importAbortToken->store(true, std::memory_order_relaxed);
  }
  if (m_importWorkerThread.joinable()) {
    m_importWorkerThread.join();
  }
  if (requestAbort) {
    discard_import_completions_for_instance(m_importInstanceNonce);
  }
  m_importAbortToken.reset();
}

uint64_t bpm_preferences_page::NextImportInstanceNonce() {
  static std::atomic<uint64_t> g_import_instance_nonce{1};
  return g_import_instance_nonce.fetch_add(1, std::memory_order_relaxed);
}

HWND bpm_preferences_page::FindControlParent(int controlID,
                                             HWND* controlHandle) const {
  const auto probeParent = [controlID, controlHandle](HWND parent) -> HWND {
    if (parent == nullptr) return nullptr;
    HWND control = ::GetDlgItem(parent, controlID);
    if (!control) return nullptr;
    if (controlHandle != nullptr) *controlHandle = control;
    return parent;
  };

  if (HWND parent = probeParent(m_hWnd)) return parent;
  if (m_pageTagging && m_pageTagging->IsWindow()) {
    if (HWND parent = probeParent(m_pageTagging->m_hWnd)) return parent;
  }
  if (m_pageAlgo && m_pageAlgo->IsWindow()) {
    if (HWND parent = probeParent(m_pageAlgo->m_hWnd)) return parent;
  }
  if (m_pageGenres && m_pageGenres->IsWindow()) {
    if (HWND parent = probeParent(m_pageGenres->m_hWnd)) return parent;
  }
  return nullptr;
}

HWND bpm_preferences_page::DialogForControl(int controlID) const {
  if (HWND parent = FindControlParent(controlID)) return parent;
  return m_hWnd;
}

HWND bpm_preferences_page::ControlHandle(int controlID) const {
  HWND control = nullptr;
  FindControlParent(controlID, &control);
  return control;
}

void bpm_preferences_page::SetDlgItemTextX(int controlID, const TCHAR* text) {
  ::SetDlgItemText(DialogForControl(controlID), controlID, text);
}

void bpm_preferences_page::SetDlgItemTextUtf8X(int controlID, const char* text) {
  uSetDlgItemText(DialogForControl(controlID), controlID,
                  text != nullptr ? text : "");
}

pfc::string8 bpm_preferences_page::ReadDlgItemTextUtf8X(int controlID) const {
  return uGetDlgItemText(DialogForControl(controlID), controlID);
}

void bpm_preferences_page::CheckDlgButtonX(int controlID, UINT check) {
  ::CheckDlgButton(DialogForControl(controlID), controlID, check);
}

UINT bpm_preferences_page::IsDlgButtonCheckedX(int controlID) const {
  return ::IsDlgButtonChecked(DialogForControl(controlID), controlID);
}

void bpm_preferences_page::SetCheckboxFromBool(int controlID, bool checked) {
  CheckDlgButtonX(controlID, checked ? BST_CHECKED : BST_UNCHECKED);
}

LRESULT bpm_preferences_page::SendDlgItemMessageX(int controlID, UINT msg, WPARAM wParam,
                                                  LPARAM lParam) const {
  return ::SendDlgItemMessage(DialogForControl(controlID), controlID, msg, wParam, lParam);
}

void bpm_preferences_page::SetComboSelectionX(int controlID, int index) {
  SendDlgItemMessageX(controlID, CB_SETCURSEL, (WPARAM)index, 0);
}

int bpm_preferences_page::GetComboSelectionX(int controlID) const {
  return (int)SendDlgItemMessageX(controlID, CB_GETCURSEL, 0, 0);
}

void bpm_preferences_page::SetControlEnabled(int controlID, bool enabled) {
  if (HWND h = ControlHandle(controlID)) {
    ::EnableWindow(h, enabled ? TRUE : FALSE);
  }
}

void bpm_preferences_page::reset() {
  if (m_importInProgress) {
    show_smart_tempo_message_dialog(
        m_hWnd, L"Import in Progress",
        L"An import is currently running. Please wait for it to finish before "
        L"resetting settings.");
    return;
  }

  if (!show_smart_tempo_confirm_dialog(
          m_hWnd, L"Confirm Smart Tempo Reset",
          L"Reset Smart Tempo preferences to their default values?\n\n"
          L"This restores tag names, routing script, fallback settings, "
          L"analysis defaults, and genre rules for this page.",
          L"Reset", L"Cancel")) {
    return;
  }

  m_ignoreChanges = true;

  SetComboSelectionX(ID_CONFIG_BPM_PRECISION,
                     get_default_bpm_write_precision());
  SetDlgItemTextX(ID_CONFIG_BPM_TAG, _T("BPM"));
  SetDlgItemTextX(ID_CONFIG_CONFIDENCE_TAG_NAME, _T("BPM_CONFIDENCE"));
  SetDlgItemTextX(ID_CONFIG_SMART_TEMPO_PLAYLIST_NAME, _T("Smart Tempo: Unmatched"));
  SetDlgItemTextX(ID_CONFIG_REVIEW_PLAYLIST_NAME,
                  _T("Smart Tempo: Needs BPM Review"));
  SetDlgItemTextX(
      ID_CONFIG_SMART_TEMPO_ROUTING_TAG,
      pfc::stringcvt::string_wide_from_utf8(get_default_routing_script_text()).get_ptr());
  {
    const std::string defaultAnchorsDisplay =
        smooth_anchor_tokens_for_display(
            get_default_generic_anchor_tokens_text());
    const pfc::stringcvt::string_wide_from_utf8 anchorsW(
        defaultAnchorsDisplay.c_str());
    SetDlgItemTextX(ID_CONFIG_GENERIC_ANCHOR_TOKENS, anchorsW.get_ptr());
  }
  SetComboSelectionX(ID_CONFIG_WORKER_MODE, get_default_analysis_worker_mode());
  SetComboSelectionX(ID_CONFIG_ANALYSIS_SECONDS,
                     FindPresetIndex(get_default_analysis_seconds_to_read(), kSecondsPerWindowChoices));
  SetComboSelectionX(ID_CONFIG_ANALYSIS_PASSES,
                     FindPresetIndex(get_default_analysis_sample_passes(), kWindowsPerTrackChoices));
  SetCheckboxFromBool(ID_CONFIG_CREATE_UNMATCHED_PLAYLIST, true);
  SetCheckboxFromBool(ID_CONFIG_CREATE_REVIEW_PLAYLIST, true);
  UpdateUnmatchedPlaylistUiState();
  UpdateReviewPlaylistUiState();
  SetDlgItemTextX(IDC_EDIT_RULE_GENRES, _T(""));
  SetDlgItemTextX(IDC_EDIT_RULE_MIN, _T(""));
  SetCheckboxFromBool(ID_CONFIG_AUTO_WRITE_TAG, false);
  SetCheckboxFromBool(ID_CONFIG_WRITE_CONFIDENCE_TAG, false);
  SetCheckboxFromBool(ID_CONFIG_VERBOSE_LOGGING, false);
  if (m_list_rules.IsWindow()) {
    m_list_rules.DeleteAllItems();
  }
  ParseRulesToList(get_default_genre_rules_text());
  UpdateRuleSelectionUiState();
  InvalidateRulesConfigSnapshot();
  m_list_is_dirty = true;
  m_ignoreChanges = false;
  UpdateConfidenceTagControls();
  if (m_callback != nullptr) m_callback->on_state_changed();
}

BOOL bpm_preferences_page::OnInitDialog(CWindow wndFocus, LPARAM lInitParam) {
  (void)wndFocus;
  (void)lInitParam;
  m_ignoreChanges = true;

  m_dark.AddDialogWithControls(m_hWnd);
  m_tabs = GetDlgItem(ID_CONFIG_PREF_TABS);
  if (m_tabs.IsWindow()) {
    if (m_pageTagging == nullptr) m_pageTagging = std::make_unique<bpm_pref_page_tagging>();
    if (m_pageAlgo == nullptr) m_pageAlgo = std::make_unique<bpm_pref_page_algo>();
    if (m_pageGenres == nullptr) m_pageGenres = std::make_unique<bpm_pref_page_genres>();
    m_pageTagging->SetParent(this);
    m_pageAlgo->SetParent(this);
    m_pageGenres->SetParent(this);
    if (!m_pageTagging->IsWindow()) m_pageTagging->Create(m_hWnd);
    if (!m_pageAlgo->IsWindow()) m_pageAlgo->Create(m_hWnd);
    if (!m_pageGenres->IsWindow()) m_pageGenres->Create(m_hWnd);
    m_dark.AddDialogWithControls(m_pageTagging->m_hWnd);
    m_dark.AddDialogWithControls(m_pageAlgo->m_hWnd);
    m_dark.AddDialogWithControls(m_pageGenres->m_hWnd);
  }

  m_list_rules.Attach(ControlHandle(IDC_LIST_GENRE_RULES));
  if (m_list_rules.IsWindow()) {
    m_list_rules.ModifyStyle(LVS_SINGLESEL, LVS_REPORT | LVS_NOSORTHEADER);
    m_list_rules.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    while (m_list_rules.DeleteColumn(0)) {
    }
    m_list_rules.InsertColumn(kRuleColumnGenres, _T("Genre Mapping Rule"), LVCFMT_LEFT,
                              scale_for_window_dpi(m_hWnd, 120));
    m_list_rules.InsertColumn(kRuleColumnCenter, _T("Center BPM"), LVCFMT_LEFT,
                              scale_for_window_dpi(m_hWnd, 72));
    // Spread is an internal weak-prior parameter. It is persisted losslessly
    // but deliberately remains outside the user-facing editor.
    m_list_rules.InsertColumn(kRuleColumnSpread, _T(""), LVCFMT_LEFT, 0);
    UpdateRuleListColumnWidths();
  }

  CComboBox write_precision_box(ControlHandle(ID_CONFIG_BPM_PRECISION));
  write_precision_box.AddString(_T("Nearest 1"));
  write_precision_box.AddString(_T("1 Decimal"));
  write_precision_box.AddString(_T("2 Decimals"));
  write_precision_box.SetCurSel(
      clamp_bpm_write_precision((int)bpm_config_bpm_write_precision));

  CComboBox worker_mode_box(ControlHandle(ID_CONFIG_WORKER_MODE));
  if (worker_mode_box.IsWindow()) {
    if (worker_mode_box.GetCount() == 0) {
      worker_mode_box.AddString(_T("Max"));
      worker_mode_box.AddString(_T("Balanced"));
      worker_mode_box.AddString(_T("Conservative"));
    }
    const int workerMode = clamp_analysis_worker_mode((int)bpm_config_worker_mode);
    worker_mode_box.SetCurSel(workerMode);
  }


  CComboBox seconds_box(ControlHandle(ID_CONFIG_ANALYSIS_SECONDS));
  if (seconds_box.IsWindow()) {
    for (const int seconds : kSecondsPerWindowChoices) {
      CString text;
      text.Format(_T("%d seconds"), seconds);
      seconds_box.AddString(text);
    }
    seconds_box.SetCurSel(FindPresetIndex(
        clamp_analysis_seconds_to_read((int)bpm_config_analysis_seconds_to_read),
        kSecondsPerWindowChoices));
  }
  CComboBox passes_box(ControlHandle(ID_CONFIG_ANALYSIS_PASSES));
  if (passes_box.IsWindow()) {
    for (const int passes : kWindowsPerTrackChoices) {
      CString text;
      text.Format(_T("%d windows"), passes);
      passes_box.AddString(text);
    }
    passes_box.SetCurSel(FindPresetIndex(
        clamp_analysis_sample_passes((int)bpm_config_analysis_sample_passes),
        kWindowsPerTrackChoices));
  }

  const pfc::string8 bpmTagText =
      sanitize_tag_field_name(bpm_config_bpm_tag.get().get_ptr(), "BPM");
  const pfc::string8 confidenceTagText =
      sanitize_tag_field_name(bpm_config_confidence_tag_name.get().get_ptr(),
                              "BPM_CONFIDENCE");
  SetDlgItemTextUtf8X(ID_CONFIG_BPM_TAG, bpmTagText.get_ptr());
  SetDlgItemTextUtf8X(ID_CONFIG_CONFIDENCE_TAG_NAME,
                      confidenceTagText.get_ptr());
  SetDlgItemTextUtf8X(
      ID_CONFIG_SMART_TEMPO_PLAYLIST_NAME,
      trim_copy(cfg_smart_tempo_playlist_name.get().get_ptr()).c_str());
  SetDlgItemTextUtf8X(
      ID_CONFIG_REVIEW_PLAYLIST_NAME,
      trim_copy(cfg_smart_tempo_review_playlist_name.get().get_ptr()).c_str());
  SetDlgItemTextUtf8X(
      ID_CONFIG_SMART_TEMPO_ROUTING_TAG,
      normalize_routing_script_text(cfg_smart_tempo_routing_tag.get().get_ptr())
          .c_str());
  {
    const std::string displayAnchors = smooth_anchor_tokens_for_display(
        cfg_smart_tempo_generic_anchor_tokens.get().get_ptr());
    SetDlgItemTextUtf8X(ID_CONFIG_GENERIC_ANCHOR_TOKENS,
                        displayAnchors.c_str());
  }
  SetCheckboxFromBool(ID_CONFIG_AUTO_WRITE_TAG, bpm_config_auto_write_tag);
  SetCheckboxFromBool(ID_CONFIG_WRITE_CONFIDENCE_TAG,
                      bpm_config_write_confidence_tag);
  SetCheckboxFromBool(ID_CONFIG_CREATE_UNMATCHED_PLAYLIST,
                      cfg_smart_tempo_create_unmatched_playlist);
  SetCheckboxFromBool(ID_CONFIG_CREATE_REVIEW_PLAYLIST,
                      cfg_smart_tempo_create_review_playlist);
  UpdateUnmatchedPlaylistUiState();
  UpdateReviewPlaylistUiState();
  SetCheckboxFromBool(ID_CONFIG_VERBOSE_LOGGING,
                      cfg_smart_tempo_verbose_logging);
  ParseRulesToList(cfg_smart_tempo_genre_rules.get());
  UpdateRuleSelectionUiState();

  InitTooltips();
  InitTabLayout();
  ShowTabPage(0);
  UpdateConfidenceTagControls();

  m_ignoreChanges = false;
  m_list_is_dirty = false;
  return TRUE;
}

void bpm_preferences_page::OnSize(UINT nType, CSize size) {
  (void)nType;
  if (m_tabs.IsWindow()) {
    m_tabs.SetWindowPos(NULL, 0, 0, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateTabLayout();
  }
  UpdateRuleListColumnWidths();
}

LRESULT bpm_preferences_page::OnDpiChanged(UINT, WPARAM, LPARAM lParam) {
  const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
  if (suggested != nullptr) {
    SetWindowPos(NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                 suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
  }

  if (m_tabs.IsWindow()) {
    CRect client;
    GetClientRect(&client);
    m_tabs.SetWindowPos(NULL, 0, 0, client.Width(), client.Height(),
                        SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateTabLayout();
  }
  UpdateRuleListColumnWidths();
  UpdateConfidenceTagControls();
  return 0;
}

void bpm_preferences_page::UpdateRuleListColumnWidths() {
  if (!m_list_rules.IsWindow()) return;

  CRect listRect;
  m_list_rules.GetClientRect(&listRect);

  const int w = listRect.Width();
  const int centerColumn = scale_for_window_dpi(m_hWnd, 72);
  const int scrollbar = ::GetSystemMetrics(SM_CXVSCROLL);
  const int columnPadding = scale_for_window_dpi(m_hWnd, 6);
  int col0 = w - centerColumn - scrollbar - columnPadding;
  const int minCol0 = scale_for_window_dpi(m_hWnd, 100);
  if (col0 < minCol0) col0 = minCol0;

  m_list_rules.SetColumnWidth(kRuleColumnGenres, col0);
  m_list_rules.SetColumnWidth(kRuleColumnCenter, centerColumn);
  m_list_rules.SetColumnWidth(kRuleColumnSpread, 0);
}

void bpm_preferences_page::InitTabLayout() {
  if (m_layoutDone || !m_tabs.IsWindow()) return;

  m_tabs.DeleteAllItems();
  TCITEM item = {};
  item.mask = TCIF_TEXT;
  item.pszText = const_cast<LPTSTR>(_T("Tagging"));
  m_tabs.InsertItem(0, &item);
  item.pszText = const_cast<LPTSTR>(_T("Algorithm"));
  m_tabs.InsertItem(1, &item);
  item.pszText = const_cast<LPTSTR>(_T("Genre Rules"));
  m_tabs.InsertItem(2, &item);
  m_tabs.SetCurSel(0);

  UpdateTabLayout();
  m_layoutDone = true;
}

void bpm_preferences_page::UpdateTabLayout() {
  if (!m_tabs.IsWindow()) return;

  CRect pageRect;
  m_tabs.GetClientRect(&pageRect);
  m_tabs.AdjustRect(FALSE, &pageRect);
  m_tabs.MapWindowPoints(m_hWnd, &pageRect);

  if (m_pageTagging && m_pageTagging->IsWindow()) {
    m_pageTagging->SetWindowPos(HWND_TOP, pageRect.left, pageRect.top, pageRect.Width(),
                                pageRect.Height(), SWP_NOACTIVATE);
  }
  if (m_pageAlgo && m_pageAlgo->IsWindow()) {
    m_pageAlgo->SetWindowPos(HWND_TOP, pageRect.left, pageRect.top, pageRect.Width(),
                             pageRect.Height(), SWP_NOACTIVATE);
  }
  if (m_pageGenres && m_pageGenres->IsWindow()) {
    m_pageGenres->SetWindowPos(HWND_TOP, pageRect.left, pageRect.top, pageRect.Width(),
                               pageRect.Height(), SWP_NOACTIVATE);
  }
}

void bpm_preferences_page::ShowTabPage(int index) {
  const bool showTagging = (index == 0);
  const bool showAlgo = (index == 1);
  const bool showGenres = (index == 2);
  if (m_pageTagging && m_pageTagging->IsWindow()) {
    m_pageTagging->ShowWindow(showTagging ? SW_SHOW : SW_HIDE);
  }
  if (m_pageAlgo && m_pageAlgo->IsWindow()) {
    m_pageAlgo->ShowWindow(showAlgo ? SW_SHOW : SW_HIDE);
  }
  if (m_pageGenres && m_pageGenres->IsWindow()) {
    m_pageGenres->ShowWindow(showGenres ? SW_SHOW : SW_HIDE);
  }
}

LRESULT bpm_preferences_page::OnTabChanged(NMHDR*) {
  if (!m_tabs.IsWindow()) return 0;
  ShowTabPage(m_tabs.GetCurSel());
  return 0;
}

void bpm_preferences_page::OnEditControlChange(UINT uNotifyCode, int nID, CWindow wndCtl) {
  (void)uNotifyCode;
  (void)wndCtl;
  if (m_importInProgress || m_ignoreChanges) return;
  if (nID == IDC_EDIT_RULE_GENRES || nID == IDC_EDIT_RULE_MIN) {
    CheckIfRuleModified();
    return;
  }
  OnChanged();
}

void bpm_preferences_page::OnComboBoxChange(UINT uNotifyCode, int nID, CWindow wndCtl) {
  (void)uNotifyCode;
  (void)wndCtl;
  if (m_importInProgress || m_ignoreChanges) return;
  OnChanged();
}

void bpm_preferences_page::OnButtonClicked(UINT uNotifyCode, int nID, CWindow wndCtl) {
  (void)uNotifyCode;
  (void)wndCtl;
  if (m_importInProgress || m_ignoreChanges) return;
  if (nID == IDC_BTN_ROUTING_IMPORT) {
    ImportRulesFromLibrary();
    return;
  }
  if (nID == ID_CONFIG_WRITE_CONFIDENCE_TAG) {
    UpdateConfidenceTagControls();
  }
  if (nID == ID_CONFIG_CREATE_UNMATCHED_PLAYLIST) {
    UpdateUnmatchedPlaylistUiState();
  }
  if (nID == ID_CONFIG_CREATE_REVIEW_PLAYLIST) {
    UpdateReviewPlaylistUiState();
  }
  OnChanged();
}

void bpm_preferences_page::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar pScrollBar) {
  (void)nSBCode;
  (void)nPos;
  (void)pScrollBar;
  if (m_importInProgress || m_ignoreChanges) return;
}

bool bpm_preferences_page::ParseBpmText(const char* text, double& outValue) {
  if (text == nullptr) return false;
  const std::string trimmed = trim_copy(text);
  if (trimmed.empty()) return false;

  char* end = nullptr;
  const double val = std::strtod(trimmed.c_str(), &end);
  if (end == nullptr || *end != '\0') return false;
  if (!(val > 0.0)) return false;
  outValue = val;
  return true;
}


int bpm_preferences_page::ReadBpmWritePrecisionFromUi() const {
  return clamp_bpm_write_precision(
      GetComboSelectionX(ID_CONFIG_BPM_PRECISION));
}

int bpm_preferences_page::ReadWorkerModeFromUi() const {
  return clamp_analysis_worker_mode(
      GetComboSelectionX(ID_CONFIG_WORKER_MODE));
}

int bpm_preferences_page::ReadAnalysisSecondsFromUi() const {
  const int index = GetComboSelectionX(ID_CONFIG_ANALYSIS_SECONDS);
  return index >= 0 && index < static_cast<int>(std::size(kSecondsPerWindowChoices))
             ? kSecondsPerWindowChoices[index]
             : get_default_analysis_seconds_to_read();
}

int bpm_preferences_page::ReadAnalysisPassesFromUi() const {
  const int index = GetComboSelectionX(ID_CONFIG_ANALYSIS_PASSES);
  return index >= 0 && index < static_cast<int>(std::size(kWindowsPerTrackChoices))
             ? kWindowsPerTrackChoices[index]
             : get_default_analysis_sample_passes();
}

bool bpm_preferences_page::ReadCheckboxFromUi(int controlID) const {
  return IsDlgButtonCheckedX(controlID) == BST_CHECKED;
}

pfc::string8 bpm_preferences_page::ReadBpmTagNameFromUi() const {
  return sanitize_tag_field_name(
      ReadDlgItemTextUtf8X(ID_CONFIG_BPM_TAG).get_ptr(),
      "BPM");
}

pfc::string8 bpm_preferences_page::ReadConfidenceTagNameFromUi() const {
  return sanitize_tag_field_name(
      ReadDlgItemTextUtf8X(ID_CONFIG_CONFIDENCE_TAG_NAME).get_ptr(),
      "BPM_CONFIDENCE");
}

std::string bpm_preferences_page::ReadPlaylistNameFromUi() const {
  const pfc::string8 playlistNameRaw =
      ReadDlgItemTextUtf8X(ID_CONFIG_SMART_TEMPO_PLAYLIST_NAME);
  return trim_copy(playlistNameRaw.get_ptr());
}

std::string bpm_preferences_page::ReadReviewPlaylistNameFromUi() const {
  const pfc::string8 playlistNameRaw =
      ReadDlgItemTextUtf8X(ID_CONFIG_REVIEW_PLAYLIST_NAME);
  return trim_copy(playlistNameRaw.get_ptr());
}

std::string bpm_preferences_page::ReadRoutingScriptFromUi() const {
  return normalize_routing_script_text(
      ReadDlgItemTextUtf8X(ID_CONFIG_SMART_TEMPO_ROUTING_TAG).get_ptr());
}

std::string bpm_preferences_page::ReadGenericAnchorTokensFromUi() const {
  return smooth_anchor_tokens_for_display(
      ReadDlgItemTextUtf8X(ID_CONFIG_GENERIC_ANCHOR_TOKENS).get_ptr());
}

bpm_preferences_page::RuleEditorValues
bpm_preferences_page::ReadRuleEditorValuesFromUi() const {
  RuleEditorValues values;
  values.genresRaw = ReadDlgItemTextUtf8X(IDC_EDIT_RULE_GENRES);
  values.centerRaw = ReadDlgItemTextUtf8X(IDC_EDIT_RULE_MIN);
  values.genresDisplay =
      smooth_rule_genres_for_display(values.genresRaw.get_ptr());
  return values;
}

pfc::string8 bpm_preferences_page::UnformatRegexForUI(const char* regexStr) {
  const std::string trimmed = trim_copy(regexStr != nullptr ? regexStr : "");
  if (trimmed.empty()) return pfc::string8();

  const auto parsedTokens = tokenize_genre_text(trimmed, true, true);
  if (parsedTokens.empty()) return pfc::string8(trimmed.c_str());

  std::string out;
  for (size_t i = 0; i < parsedTokens.size(); ++i) {
    if (i != 0) out += "; ";
    out += parsedTokens[i].original;
  }
  return pfc::string8(out.c_str());
}

pfc::string8 bpm_preferences_page::ConvertToRegexForConfig(const char* uiStr) {
  const std::string trimmed = trim_copy(uiStr != nullptr ? uiStr : "");
  if (trimmed.empty()) return pfc::string8();

  const auto parsedTokens = tokenize_genre_text(trimmed, true, true);
  if (parsedTokens.empty()) return pfc::string8();

  std::string canonical;
  for (size_t i = 0; i < parsedTokens.size(); ++i) {
    if (i != 0) canonical += "; ";
    canonical += parsedTokens[i].original;
  }
  return pfc::string8(canonical.c_str());
}

void bpm_preferences_page::ParseRulesToList(const char* rulesText) {
  if (!m_list_rules.IsWindow()) return;
  InvalidateRulesConfigSnapshot();
  m_list_rules.DeleteAllItems();

  std::istringstream stream(rulesText != nullptr ? rulesText : "");
  std::string line;
  while (std::getline(stream, line)) {
    const std::string trimmed = trim_copy(line);
    if (trimmed.empty()) continue;
    if (trimmed[0] == '#' || trimmed[0] == ';') continue;
    if (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/') continue;

    const size_t eqPos = trimmed.find('=');
    if (eqPos == std::string::npos) continue;
    const std::string left = trim_copy(std::string_view(trimmed).substr(0, eqPos));
    const std::string right = trim_copy(std::string_view(trimmed).substr(eqPos + 1));
    if (left.empty() || right.empty()) continue;

    smart_tempo::helpers::parsed_rule_rhs_fields parsedRhs;
    if (!parse_rule_rhs_fields(right, parsedRhs)) continue;

    char centerText[32] = {};
    char spreadText[32] = {};
    std::snprintf(centerText, sizeof(centerText), "%.2f", parsedRhs.center_bpm);
    std::snprintf(spreadText, sizeof(spreadText), "%.2f", parsedRhs.spread_bpm);

    const pfc::string8 leftUi = UnformatRegexForUI(left.c_str());
    const pfc::stringcvt::string_wide_from_utf8 leftW(leftUi.get_ptr());
    const pfc::stringcvt::string_wide_from_utf8 centerW(centerText);
    const pfc::stringcvt::string_wide_from_utf8 spreadW(spreadText);

    RuleListRowValues values;
    values.genres = leftW.get_ptr();
    values.centerBpm = centerW.get_ptr();
    values.spreadBpm = spreadW.get_ptr();
    const int row = m_list_rules.InsertItem(m_list_rules.GetItemCount(), leftW.get_ptr());
    WriteRuleListRow(row, values);
  }
  m_rules_config_cache = BuildRulesConfigFromList();
  m_rules_config_cache_valid = true;
}

void bpm_preferences_page::UpdateRuleSelectionUiState() {
  if (!m_list_rules.IsWindow()) return;

  const int selectedCount = m_list_rules.GetSelectedCount();
  const bool hasAnySelection = selectedCount > 0;
  const bool hasSingleSelection = selectedCount == 1;
  const bool hasMultiSelection = selectedCount > 1;

  const int selectedRow = GetSelectedRuleIndex();
  const bool canMoveUp = hasSingleSelection && selectedRow > 0;
  const bool canMoveDown =
      hasSingleSelection && selectedRow >= 0 &&
      selectedRow < (m_list_rules.GetItemCount() - 1);

  const bool enableEditors = !hasMultiSelection;
  SetControlEnabled(IDC_EDIT_RULE_GENRES, enableEditors);
  SetControlEnabled(IDC_EDIT_RULE_MIN, enableEditors);
  SetControlEnabled(IDC_BTN_RULE_ADD, enableEditors);

  SetControlEnabled(IDC_BTN_RULE_UPDATE, false);
  SetControlEnabled(IDC_BTN_RULE_UP, canMoveUp);
  SetControlEnabled(IDC_BTN_RULE_DOWN, canMoveDown);
  SetControlEnabled(IDC_BTN_RULE_REMOVE, hasAnySelection);

  if (hasSingleSelection) {
    PopulateRuleEditorsFromSelection();
    CheckIfRuleModified();
  }
}

bool bpm_preferences_page::CheckIfRuleModified() {
  if (!m_list_rules.IsWindow()) return false;
  const int selected = GetSelectedRuleIndex();
  if (selected < 0) {
    SetControlEnabled(IDC_BTN_RULE_UPDATE, false);
    return false;
  }

  const RuleListRowValues row = ReadRuleListRow(selected);
  const pfc::stringcvt::string_utf8_from_wide rowGenresUtf8(
      row.genres.GetString());
  const pfc::stringcvt::string_utf8_from_wide rowCenterUtf8(
      row.centerBpm.GetString());

  const RuleEditorValues ui = ReadRuleEditorValuesFromUi();
  const std::string rowGenres =
      smooth_rule_genres_for_display(rowGenresUtf8.get_ptr());
  bool modified = (ui.genresDisplay != rowGenres);

  if (!modified) {
    double uiCenter = 0.0;
    double rowCenter = 0.0;
    if (ParseBpmText(ui.centerRaw.get_ptr(), uiCenter) &&
        ParseBpmText(rowCenterUtf8.get_ptr(), rowCenter)) {
      modified = std::abs(uiCenter - rowCenter) > 1e-9;
    } else {
      modified = true;
    }
  }

  SetControlEnabled(IDC_BTN_RULE_UPDATE, modified);
  return modified;
}

void bpm_preferences_page::ImportRulesFromLibrary() {
  if (m_importInProgress || !m_list_rules.IsWindow()) return;

  const pfc::string8 routingScriptPref =
      ReadDlgItemTextUtf8X(ID_CONFIG_SMART_TEMPO_ROUTING_TAG);
  const std::string routingScriptText = normalize_routing_script_text(routingScriptPref.get_ptr());

  const pfc::string8 anchorInputText =
      ReadDlgItemTextUtf8X(ID_CONFIG_GENERIC_ANCHOR_TOKENS);
  const std::string anchorDisplayText =
      smooth_anchor_tokens_for_display(anchorInputText.get_ptr());
  if (trim_copy(anchorInputText.get_ptr()) != anchorDisplayText) {
    const bool prevIgnore = m_ignoreChanges;
    m_ignoreChanges = true;
    SetDlgItemTextUtf8X(ID_CONFIG_GENERIC_ANCHOR_TOKENS,
                        anchorDisplayText.c_str());
    m_ignoreChanges = prevIgnore;
  }

  SetImportUiLock(true);
  JoinImportWorkerThread();
  const uint64_t generation =
      m_importGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
  m_activeImportGeneration = generation;

  try {
    service_ptr_t<titleformat_object> routingScript;
    if (!titleformat_compiler::get()->compile(routingScript,
                                              routingScriptText.c_str())) {
      FB2K_console_formatter()
          << "foo_smart_tempo: [Routing] invalid routing script during import"
          << " -> falling back to %genre%: " << routingScriptText.c_str();
      titleformat_compiler::get()->compile_force(routingScript, "%genre%");
    }

    metadb_handle_list allItems;
    static_api_ptr_t<library_manager> libraryApi;
    libraryApi->get_all_items(allItems);

    // Import collect intentionally stays synchronous on the UI thread (stability trade-off
    // for foobar2000 SDK object lifetime/hook compatibility).
    std::unordered_map<std::string, import_token_bucket> buckets;
    const t_size total = allItems.get_count();
    for (t_size i = 0; i < total; ++i) {
      const auto& item = allItems[i];
      if (!item.is_valid()) continue;

      pfc::string8 routed;
      format_routing_values_from_item(item, routingScript, routed);
      const auto rawTokens = split_import_tokens_library(routed.get_ptr());
      for (const auto& rawToken : rawTokens) {
        const std::string trimmedRaw = trim_copy(rawToken);
        if (trimmedRaw.empty()) continue;
        const std::string canonical = canonicalize_import_token(trimmedRaw);
        if (canonical.empty()) continue;

        auto& bucket = buckets[canonical];
        ++bucket.occurrence_count;
        if (bucket.representative_raw.empty() ||
            casefold_codepoint_less(trimmedRaw, bucket.representative_raw)) {
          bucket.representative_raw = trimmedRaw;
        }
      }
    }

    std::vector<pfc::string8> wizardValues;
    wizardValues.reserve(buckets.size());
    for (const auto& [canonicalToken, bucket] : buckets) {
      (void)canonicalToken;
      if (bucket.representative_raw.empty()) continue;
      wizardValues.emplace_back(bucket.representative_raw.c_str());
    }
    std::sort(wizardValues.begin(), wizardValues.end(),
              [](const pfc::string8& a, const pfc::string8& b) {
                return casefold_codepoint_less(a.get_ptr(), b.get_ptr());
              });

    if (wizardValues.empty()) {
      show_smart_tempo_message_dialog(
          m_hWnd,
          L"Import Result",
          L"Import completed, but no non-empty routed genre tokens were found "
          L"in your media library.\n\nPlease check your routing script or "
          L"source tags and try again.");
      SetImportUiLock(false);
      return;
    }

    routing_import_wizard_dialog wizard(std::move(wizardValues));
    HWND modalOwner = ::GetAncestor(m_hWnd, GA_ROOT);
    if (modalOwner == NULL) modalOwner = core_api::get_main_window();
    if (wizard.DoModal(modalOwner) != IDOK) {
      SetImportUiLock(false);
      return;
    }

    const auto& selectedValues = wizard.selected_values();
    if (selectedValues.empty()) {
      SetImportUiLock(false);
      return;
    }

    std::unordered_set<std::string> selectedCanonical;
    std::vector<import_resolver_input_token> resolverInput;
    resolverInput.reserve(selectedValues.size());
    for (const auto& selectedRaw : selectedValues) {
      const std::string trimmedRaw = trim_copy(selectedRaw);
      if (trimmedRaw.empty()) continue;
      const std::string canonical = canonicalize_import_token(trimmedRaw);
      if (canonical.empty()) continue;
      if (!selectedCanonical.emplace(canonical).second) continue;

      import_resolver_input_token token;
      auto it = buckets.find(canonical);
      if (it == buckets.end()) {
        token.representative_raw = trimmedRaw;
        token.occurrence_count = 1;
      } else {
        token.representative_raw = it->second.representative_raw.empty()
                                       ? trimmedRaw
                                       : it->second.representative_raw;
        token.occurrence_count =
            (it->second.occurrence_count == 0) ? 1 : it->second.occurrence_count;
      }
      resolverInput.push_back(std::move(token));
    }

    if (resolverInput.empty()) {
      SetImportUiLock(false);
      return;
    }

    const int existingCount = m_list_rules.GetItemCount();
    std::vector<std::string> rulePatterns;
    rulePatterns.reserve((size_t)existingCount);
    for (int row = 0; row < existingCount; ++row) {
      const RuleListRowValues ruleRow = ReadRuleListRow(row);
      const pfc::stringcvt::string_utf8_from_wide existingPattern(
          ruleRow.genres.GetString());
      rulePatterns.push_back(trim_copy(existingPattern.get_ptr()));
    }

    StartImportResolveAsync(resolverInput, rulePatterns, anchorDisplayText,
                            generation);
  } catch (const std::exception& e) {
    JoinImportWorkerThread();
    SetImportUiLock(false);
    FB2K_console_formatter()
        << "foo_smart_tempo[semantic_resolver]: import scan failed: "
        << e.what();
  } catch (...) {
    JoinImportWorkerThread();
    SetImportUiLock(false);
    FB2K_console_formatter()
        << "foo_smart_tempo[semantic_resolver]: import scan failed: unknown error.";
  }
}

void bpm_preferences_page::StartImportResolveAsync(
    const std::vector<import_resolver_input_token>& resolverInput,
    const std::vector<std::string>& rulePatterns,
    const std::string& anchorDisplayText, uint64_t generation) {
  if (generation != m_activeImportGeneration) return;
  JoinImportWorkerThread();
  auto abortToken = std::make_shared<std::atomic<bool>>(false);
  m_importAbortToken = abortToken;
  const uint64_t instanceNonce = m_importInstanceNonce;
  const HWND targetWnd = m_hWnd;
  try {
    m_importWorkerThread = std::thread([instanceNonce, targetWnd, resolverInput,
                                        rulePatterns, anchorDisplayText,
                                        abortToken, generation]() {
          auto completion = std::make_shared<import_completion_payload>();
          completion->generation = generation;
          try {
            if (abortToken->load(std::memory_order_relaxed)) return;
            completion->previous_rule_patterns = rulePatterns;
            if (abortToken->load(std::memory_order_relaxed)) return;
            completion->resolver_result = resolve_import_genres(
                resolverInput, rulePatterns, anchorDisplayText, abortToken.get());
            if (abortToken->load(std::memory_order_relaxed)) return;
            completion->success = true;
          } catch (const std::exception& e) {
            if (abortToken->load(std::memory_order_relaxed)) return;
            completion->success = false;
            completion->error_message = e.what();
          } catch (...) {
            if (abortToken->load(std::memory_order_relaxed)) return;
            completion->success = false;
            completion->error_message = "Unknown resolver worker failure.";
          }

          store_import_completion(instanceNonce, generation, completion);
          const import_async_event event =
              completion->success ? import_async_event::resolve_complete
                                  : import_async_event::worker_error;
          const bool posted =
              ::IsWindow(targetWnd) &&
              !!::PostMessage(targetWnd, kMsgImportAsync,
                              static_cast<WPARAM>(event),
                              static_cast<LPARAM>(generation));
          if (!posted) {
            fb2k::inMainThread([instanceNonce, targetWnd, event, generation]() {
              if (!::IsWindow(targetWnd)) {
                discard_import_completion(instanceNonce, generation);
                return;
              }
              if (::PostMessage(targetWnd, kMsgImportAsync,
                                static_cast<WPARAM>(event),
                                static_cast<LPARAM>(generation))) {
                return;
              }
              FB2K_console_formatter()
                  << "foo_smart_tempo[semantic_resolver]: async completion dispatch failed; payload discarded.";
              discard_import_completion(instanceNonce, generation);
            });
          }
        });
  } catch (const std::exception& e) {
    SetImportUiLock(false);
    FB2K_console_formatter()
        << "foo_smart_tempo[semantic_resolver]: failed to start resolver thread: "
        << e.what();
  }
}

LRESULT bpm_preferences_page::OnImportAsyncMessage(UINT, WPARAM wParam, LPARAM lParam) {
  const auto event = static_cast<import_async_event>(wParam);
  const uint64_t generation = static_cast<uint64_t>(lParam);
  if (generation == 0) return 0;

  if (generation != m_activeImportGeneration) {
    discard_import_completion(m_importInstanceNonce, generation);
    return 0;
  }

  auto payload = take_import_completion(m_importInstanceNonce, generation);
  if (!payload) {
    JoinImportWorkerThread(false);
    SetImportUiLock(false);
    FB2K_console_formatter()
        << "foo_smart_tempo[semantic_resolver]: async completion payload missing.";
    return 0;
  }

  switch (event) {
    case import_async_event::resolve_complete: {
      JoinImportWorkerThread(false);
      if (!payload->success) {
        SetImportUiLock(false);
        FB2K_console_formatter()
            << "foo_smart_tempo[semantic_resolver]: import worker failed: "
            << (payload->error_message.empty() ? "Unknown worker error."
                                               : payload->error_message.c_str());
        show_smart_tempo_message_dialog(
            m_hWnd, L"Smart Tempo",
            L"Import failed. See console for details.");
        return 0;
      }
      ApplyResolverResultToUi(payload->resolver_result,
                              payload->previous_rule_patterns);
      SetImportUiLock(false);
      return 0;
    }
    case import_async_event::worker_error: {
      JoinImportWorkerThread(false);
      SetImportUiLock(false);
      const char* errorText =
          (!payload->error_message.empty()) ? payload->error_message.c_str()
                                                  : "Unknown worker error.";
      FB2K_console_formatter()
          << "foo_smart_tempo[semantic_resolver]: import worker failed: "
          << errorText;
      show_smart_tempo_message_dialog(
          m_hWnd, L"Smart Tempo",
          L"Import failed. See console for details.");
      return 0;
    }
    default:
      break;
  }
  return 0;
}

void bpm_preferences_page::ApplyResolverResultToUi(
    const import_resolver_result& resolverResult,
    const std::vector<std::string>& previousRulePatterns) {
  bool anyRuleChanged = false;
  bool genericTokensChanged = false;
  int firstAffectedRow = -1;
  const size_t assignCount =
      std::min(previousRulePatterns.size(),
               resolverResult.updated_rule_patterns.size());
  for (size_t i = 0; i < assignCount; ++i) {
    if (resolverResult.updated_rule_patterns[i] ==
        previousRulePatterns[i]) {
      continue;
    }
    const pfc::stringcvt::string_wide_from_utf8 updatedW(
        resolverResult.updated_rule_patterns[i].c_str());
    m_list_rules.SetItemText((int)i, 0, updatedW.get_ptr());
    if (firstAffectedRow < 0) firstAffectedRow = (int)i;
    anyRuleChanged = true;
  }

  if (firstAffectedRow >= 0) {
    SelectRuleRow(firstAffectedRow);
    PopulateRuleEditorsFromSelection();
  }

  if (!resolverResult.generic_tokens_to_add.empty()) {
    const std::string previousGenericTokens =
        ReadGenericAnchorTokensFromUi();
    std::vector<std::string> mergedTokens =
        split_manual_list_tokens(previousGenericTokens);
    std::unordered_set<std::string> seenCanonical;
    for (const auto& token : mergedTokens) {
      const std::string canonical = canonicalize_import_token(token);
      if (!canonical.empty()) seenCanonical.emplace(canonical);
    }
    for (const auto& token : resolverResult.generic_tokens_to_add) {
      const std::string canonical = canonicalize_import_token(token);
      if (canonical.empty() || !seenCanonical.emplace(canonical).second) {
        continue;
      }
      mergedTokens.push_back(trim_copy(token));
    }

    const std::string updatedGenericTokens =
        join_semicolon_tokens(mergedTokens);
    if (updatedGenericTokens != previousGenericTokens) {
      const bool previousIgnoreChanges = m_ignoreChanges;
      m_ignoreChanges = true;
      SetDlgItemTextUtf8X(ID_CONFIG_GENERIC_ANCHOR_TOKENS,
                          updatedGenericTokens.c_str());
      m_ignoreChanges = previousIgnoreChanges;
      genericTokensChanged = true;
    }
  }

  const auto& telemetry = resolverResult.telemetry;
  const size_t unresolvedDiscarded = (telemetry.initially_unresolved >= telemetry.generic_fallbacks)
                                         ? (telemetry.initially_unresolved - telemetry.generic_fallbacks)
                                         : 0;
  const long long checksum = (long long)telemetry.deduplicated +
                             (long long)telemetry.stage_a_matches +
                             (long long)telemetry.stage_b_matches +
                             (long long)telemetry.stage_c_matches +
                             (long long)telemetry.initially_unresolved;
  const std::string selectedCatchAllText =
      (resolverResult.selected_catch_all_rule_index >= 0)
          ? std::to_string(resolverResult.selected_catch_all_rule_index)
          : std::string("none");
  FB2K_console_formatter()
      << "foo_smart_tempo[semantic_resolver]: selected_catch_all="
      << selectedCatchAllText.c_str()
      << ", anchor_token_count=" << resolverResult.anchor_token_count
      << ", GenresAnalyzed=" << telemetry.genres_analyzed
      << ", Deduplicated=" << telemetry.deduplicated
      << ", StageA=" << telemetry.stage_a_matches
      << ", StageB=" << telemetry.stage_b_matches
      << ", StageC=" << telemetry.stage_c_matches
      << ", InitiallyUnresolved=" << telemetry.initially_unresolved
      << ", GenericFallbacks=" << telemetry.generic_fallbacks
      << ", GenericTokensAdded=" << telemetry.generic_tokens_added
      << ", Unresolved_Discarded=" << unresolvedDiscarded
      << ", checksum=" << checksum;
  if (!telemetry.unresolved_discarded.empty()) {
    const std::string unresolvedList = join_tokens_csv(telemetry.unresolved_discarded);
    FB2K_console_formatter()
        << "foo_smart_tempo[semantic_resolver]: Unresolved_Discarded="
        << unresolvedList.c_str();
  }

  UpdateRuleSelectionUiState();
  if (anyRuleChanged) {
    MarkRuleListChangedAndShowGenreRules();
  } else if (genericTokensChanged) {
    OnChanged();
  }
}

void bpm_preferences_page::PopulateRuleEditorsFromSelection() {
  if (!m_list_rules.IsWindow()) return;
  const int selected = GetSelectedRuleIndex(false);
  if (selected < 0) return;

  const RuleListRowValues row = ReadRuleListRow(selected);

  const bool prevIgnore = m_ignoreChanges;
  m_ignoreChanges = true;
  SetDlgItemTextX(IDC_EDIT_RULE_GENRES, row.genres.GetString());
  SetDlgItemTextX(IDC_EDIT_RULE_MIN, row.centerBpm.GetString());
  m_ignoreChanges = prevIgnore;
}

pfc::string8 bpm_preferences_page::BuildRulesConfigFromList() {
  if (!m_list_rules.IsWindow()) return pfc::string8();

  pfc::string_formatter out;
  const int total = m_list_rules.GetItemCount();
  for (int i = 0; i < total; ++i) {
    const RuleListRowValues row = ReadRuleListRow(i);

    const pfc::stringcvt::string_utf8_from_wide c0Ui(row.genres.GetString());
    const pfc::stringcvt::string_utf8_from_wide c1(row.centerBpm.GetString());
    const pfc::stringcvt::string_utf8_from_wide c2(row.spreadBpm.GetString());
    const pfc::string8 c0Regex = ConvertToRegexForConfig(c0Ui.get_ptr());
    if (c0Regex.is_empty()) continue;
    double centerBpm = 0.0;
    double spreadBpm = 0.0;
    if (!ParseBpmText(c1.get_ptr(), centerBpm) ||
        !ParseBpmText(c2.get_ptr(), spreadBpm) || centerBpm <= spreadBpm) {
      continue;
    }
    out << c0Regex.get_ptr() << " = center "
        << pfc::format_float(centerBpm, 0, 2) << ", spread "
        << pfc::format_float(spreadBpm, 0, 2);
    out << "\r\n";
  }

  return pfc::string8(out.get_ptr());
}

const pfc::string8& bpm_preferences_page::GetRulesConfigSnapshot() {
  if (!m_rules_config_cache_valid) {
    m_rules_config_cache = BuildRulesConfigFromList();
    m_rules_config_cache_valid = true;
  }
  return m_rules_config_cache;
}

void bpm_preferences_page::InvalidateRulesConfigSnapshot() {
  m_rules_config_cache_valid = false;
  m_rules_config_cache.reset();
}

void bpm_preferences_page::MarkRuleListChanged() {
  InvalidateRulesConfigSnapshot();
  m_list_is_dirty = true;
  OnChanged();
}

void bpm_preferences_page::MarkRuleListChangedAndShowGenreRules() {
  InvalidateRulesConfigSnapshot();
  m_list_is_dirty = true;
  ShowGenreRulesTab();
  OnChanged();
}

void bpm_preferences_page::SaveListToConfig() {
  m_rules_config_cache = BuildRulesConfigFromList();
  m_rules_config_cache_valid = true;
  cfg_smart_tempo_genre_rules = m_rules_config_cache.get_ptr();
}

int bpm_preferences_page::GetSelectedRuleIndex(bool requireSingleSelection) {
  if (!m_list_rules.IsWindow()) return -1;
  if (requireSingleSelection && m_list_rules.GetSelectedCount() != 1) return -1;
  return m_list_rules.GetNextItem(-1, LVNI_SELECTED);
}

std::vector<int> bpm_preferences_page::GetSelectedRuleRows() {
  std::vector<int> selectedRows;
  if (!m_list_rules.IsWindow()) return selectedRows;
  for (int row = m_list_rules.GetNextItem(-1, LVNI_SELECTED); row != -1;
       row = m_list_rules.GetNextItem(row, LVNI_SELECTED)) {
    selectedRows.push_back(row);
  }
  return selectedRows;
}

bpm_preferences_page::RuleListRowValues
bpm_preferences_page::ReadRuleListRow(int row) {
  RuleListRowValues values;
  if (!m_list_rules.IsWindow() || row < 0) return values;
  values.genres = list_item_text(m_list_rules, row, kRuleColumnGenres);
  values.centerBpm = list_item_text(m_list_rules, row, kRuleColumnCenter);
  values.spreadBpm = list_item_text(m_list_rules, row, kRuleColumnSpread);
  return values;
}

void bpm_preferences_page::WriteRuleListRow(
    int row, const RuleListRowValues& values) {
  if (!m_list_rules.IsWindow() || row < 0) return;
  m_list_rules.SetItemText(row, kRuleColumnGenres, values.genres.GetString());
  m_list_rules.SetItemText(row, kRuleColumnCenter,
                           values.centerBpm.GetString());
  m_list_rules.SetItemText(row, kRuleColumnSpread,
                           values.spreadBpm.GetString());
}

void bpm_preferences_page::SelectRuleRow(int row) {
  if (!m_list_rules.IsWindow() || row < 0) return;
  m_list_rules.SetItemState(row, LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
  m_list_rules.EnsureVisible(row, FALSE);
}

void bpm_preferences_page::ShowGenreRulesTab() {
  if (!m_tabs.IsWindow()) return;
  m_tabs.SetCurSel(2);
  ShowTabPage(2);
}

void bpm_preferences_page::InsertOrUpdateRule(bool isUpdate) {
  const RuleEditorValues ui = ReadRuleEditorValuesFromUi();
  if (ui.genresDisplay.empty()) return;

  double centerVal = 0.0;
  if (!ParseBpmText(ui.centerRaw.get_ptr(), centerVal) || centerVal <= 1.0) {
    return;
  }

  RuleListRowValues previousValues;
  double halfWidth = kNewRuleHalfWidthBpm;
  if (isUpdate) {
    const int selected = GetSelectedRuleIndex(false);
    if (selected < 0) return;
    previousValues = ReadRuleListRow(selected);
    const pfc::stringcvt::string_utf8_from_wide previousSpread(
        previousValues.spreadBpm.GetString());
    ParseBpmText(previousSpread.get_ptr(), halfWidth);
  }
  halfWidth = (std::min)(halfWidth, centerVal - 1.0);

  char centerBuf[32] = {};
  char spreadBuf[32] = {};
  std::snprintf(centerBuf, sizeof(centerBuf), "%.2f", centerVal);
  std::snprintf(spreadBuf, sizeof(spreadBuf), "%.2f", halfWidth);

  const pfc::stringcvt::string_wide_from_utf8 genresW(ui.genresDisplay.c_str());
  const pfc::stringcvt::string_wide_from_utf8 centerW(centerBuf);
  const pfc::stringcvt::string_wide_from_utf8 spreadW(spreadBuf);
  RuleListRowValues rowValues;
  rowValues.genres = genresW.get_ptr();
  rowValues.centerBpm = centerW.get_ptr();
  rowValues.spreadBpm = spreadW.get_ptr();

  int row = -1;
  if (isUpdate) {
    row = GetSelectedRuleIndex(false);
    if (row < 0) return;
  } else {
    row = m_list_rules.InsertItem(m_list_rules.GetItemCount(), genresW.get_ptr());
  }

  WriteRuleListRow(row, rowValues);
  SelectRuleRow(row);

  MarkRuleListChanged();
}

void bpm_preferences_page::MoveSelectedRule(int delta) {
  const int selected = GetSelectedRuleIndex(false);
  if (selected < 0) return;
  const int target = selected + delta;
  if (target < 0 || target >= m_list_rules.GetItemCount()) return;

  const RuleListRowValues selectedValues = ReadRuleListRow(selected);
  const RuleListRowValues targetValues = ReadRuleListRow(target);
  WriteRuleListRow(selected, targetValues);
  WriteRuleListRow(target, selectedValues);

  m_list_rules.SetItemState(selected, 0, LVIS_SELECTED | LVIS_FOCUSED);
  SelectRuleRow(target);
  MarkRuleListChanged();
}

LRESULT bpm_preferences_page::OnRuleListItemChanged(NMHDR* pnmh) {
  if (m_ignoreChanges) return 0;
  const NMLISTVIEW* pnmv = reinterpret_cast<NMLISTVIEW*>(pnmh);
  if (pnmv != nullptr && (pnmv->uChanged & LVIF_STATE)) {
    UpdateRuleSelectionUiState();
  }
  return 0;
}

void bpm_preferences_page::OnRuleCommand(int nID) {
  if (m_importInProgress || !m_list_rules.IsWindow()) return;

  switch (nID) {
    case IDC_BTN_RULE_ADD:
      InsertOrUpdateRule(false);
      break;
    case IDC_BTN_RULE_UPDATE:
      if (!CheckIfRuleModified()) break;
      InsertOrUpdateRule(true);
      break;
    case IDC_BTN_RULE_REMOVE: {
      std::vector<int> selectedRows = GetSelectedRuleRows();
      if (!selectedRows.empty()) {
        std::sort(selectedRows.begin(), selectedRows.end(),
                  [](int a, int b) { return a > b; });
        for (const int row : selectedRows) {
          m_list_rules.DeleteItem(row);
        }
        MarkRuleListChanged();
      }
      break;
    }
    case IDC_BTN_RULE_UP:
      MoveSelectedRule(-1);
      break;
    case IDC_BTN_RULE_DOWN:
      MoveSelectedRule(1);
      break;
    case IDC_BTN_RULE_IMPORT_CLIPBOARD:
      ImportRulesFromClipboard();
      break;
    case IDC_BTN_RULE_EXPORT_CLIPBOARD:
      ExportRulesToClipboard();
      break;
    default:
      break;
  }
  UpdateRuleSelectionUiState();
}

void bpm_preferences_page::ImportRulesFromClipboard() {
  pfc::string8 clipboardText;
  if (!read_utf8_text_from_clipboard(m_hWnd, clipboardText) ||
      trim_copy(clipboardText.get_ptr()).empty()) {
    show_smart_tempo_message_dialog(
        m_hWnd,
        L"Smart Tempo",
        L"Clipboard is empty or does not contain plain text genre rules.");
    return;
  }

  if (!show_smart_tempo_confirm_dialog(
          m_hWnd, L"Import Genre Rules",
          L"This will replace your current genre rules with rules parsed from "
          L"the clipboard.\n\nExisting rules on this page will be overwritten.",
          L"Import", L"Cancel")) {
    return;
  }

  struct ParsedRule {
    pfc::string8 genres_ui;
    pfc::string8 center_text;
    pfc::string8 spread_text;
  };

  std::vector<ParsedRule> parsedRules;
  size_t invalidLines = 0;
  std::istringstream stream(clipboardText.get_ptr());
  std::string line;
  while (std::getline(stream, line)) {
    const std::string trimmed = trim_copy(line);
    if (trimmed.empty()) continue;
    if (trimmed[0] == '#' || trimmed[0] == ';') continue;
    if (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/') continue;

    const size_t eqPos = trimmed.find('=');
    if (eqPos == std::string::npos) {
      ++invalidLines;
      continue;
    }

    const std::string left = trim_copy(std::string_view(trimmed).substr(0, eqPos));
    const std::string right = trim_copy(std::string_view(trimmed).substr(eqPos + 1));
    if (left.empty() || right.empty()) {
      ++invalidLines;
      continue;
    }

    smart_tempo::helpers::parsed_rule_rhs_fields parsedRhs;
    if (!parse_rule_rhs_fields(right, parsedRhs)) {
      ++invalidLines;
      continue;
    }
    char centerText[32] = {};
    char spreadText[32] = {};
    std::snprintf(centerText, sizeof(centerText), "%.2f", parsedRhs.center_bpm);
    std::snprintf(spreadText, sizeof(spreadText), "%.2f", parsedRhs.spread_bpm);

    const pfc::string8 regexPattern = ConvertToRegexForConfig(left.c_str());
    if (regexPattern.is_empty()) {
      ++invalidLines;
      continue;
    }

    ParsedRule row;
    row.genres_ui = UnformatRegexForUI(regexPattern.get_ptr());
    row.center_text = centerText;
    row.spread_text = spreadText;
    parsedRules.emplace_back(std::move(row));
  }

  if (parsedRules.empty()) {
    show_smart_tempo_message_dialog(
        m_hWnd, L"Smart Tempo",
        L"Import failed: no valid rule lines were found in the clipboard.");
    return;
  }

  m_list_rules.DeleteAllItems();
  for (const auto& row : parsedRules) {
    const pfc::stringcvt::string_wide_from_utf8 genresW(row.genres_ui.get_ptr());
    const pfc::stringcvt::string_wide_from_utf8 centerW(row.center_text.get_ptr());
    const pfc::stringcvt::string_wide_from_utf8 spreadW(row.spread_text.get_ptr());
    RuleListRowValues rowValues;
    rowValues.genres = genresW.get_ptr();
    rowValues.centerBpm = centerW.get_ptr();
    rowValues.spreadBpm = spreadW.get_ptr();

    const int item = m_list_rules.InsertItem(m_list_rules.GetItemCount(), genresW.get_ptr());
    WriteRuleListRow(item, rowValues);
  }

  if (m_list_rules.GetItemCount() > 0) {
    SelectRuleRow(0);
  }
  PopulateRuleEditorsFromSelection();
  UpdateRuleSelectionUiState();
  MarkRuleListChangedAndShowGenreRules();

  CString infoMsg;
  infoMsg.Format(
      _T("Import successful: %u genre centers imported, %u invalid lines skipped."),
      static_cast<unsigned>(parsedRules.size()),
      static_cast<unsigned>(invalidLines));
  show_smart_tempo_message_dialog(m_hWnd, L"Smart Tempo", infoMsg.GetString());
}

pfc::string8 bpm_preferences_page::BuildRulesExportText() {
  if (!m_list_rules.IsWindow()) return pfc::string8();

  pfc::string_formatter out;
  const int total = m_list_rules.GetItemCount();
  for (int i = 0; i < total; ++i) {
    const RuleListRowValues row = ReadRuleListRow(i);
    const pfc::stringcvt::string_utf8_from_wide genres(
        row.genres.GetString());
    const pfc::stringcvt::string_utf8_from_wide centerVal(
        row.centerBpm.GetString());
    const pfc::stringcvt::string_utf8_from_wide spreadVal(
        row.spreadBpm.GetString());
    if (trim_copy(genres.get_ptr()).empty()) continue;

    double centerBpm = 0.0;
    double spreadBpm = 0.0;
    if (!ParseBpmText(centerVal.get_ptr(), centerBpm) ||
        !ParseBpmText(spreadVal.get_ptr(), spreadBpm) ||
        centerBpm <= spreadBpm) {
      continue;
    }

    char centerBuf[32] = {};
    char spreadBuf[32] = {};
    std::snprintf(centerBuf, sizeof(centerBuf), "%.2f", centerBpm);
    std::snprintf(spreadBuf, sizeof(spreadBuf), "%.2f", spreadBpm);
    out << genres.get_ptr() << " = center " << centerBuf << ", spread "
        << spreadBuf << "\r\n";
  }
  return pfc::string8(out.get_ptr());
}

void bpm_preferences_page::ExportRulesToClipboard() {
  const pfc::string8 exportText = BuildRulesExportText();
  if (trim_copy(exportText.get_ptr()).empty()) {
    show_smart_tempo_message_dialog(
        m_hWnd, L"Smart Tempo", L"There are no genre rules to export.");
    return;
  }

  if (!write_utf8_text_to_clipboard(m_hWnd, exportText.get_ptr())) {
    show_smart_tempo_message_dialog(
        m_hWnd, L"Smart Tempo",
        L"Export failed: unable to write to clipboard.");
    return;
  }

  show_smart_tempo_message_dialog(
      m_hWnd, L"Smart Tempo", L"Genre rules exported to clipboard.");
}

void bpm_preferences_page::SetImportUiLock(bool locked) {
  if (m_importInProgress == locked) return;
  m_importInProgress = locked;

  const int controlsToLock[] = {
      ID_CONFIG_PREF_TABS,
      ID_CONFIG_SMART_TEMPO_ROUTING_TAG,
      IDC_BTN_ROUTING_IMPORT,
      ID_CONFIG_GENERIC_ANCHOR_TOKENS,
      ID_CONFIG_SMART_TEMPO_PLAYLIST_NAME,
      ID_CONFIG_CREATE_UNMATCHED_PLAYLIST,
      ID_CONFIG_REVIEW_PLAYLIST_NAME,
      ID_CONFIG_CREATE_REVIEW_PLAYLIST,
      ID_CONFIG_WORKER_MODE,
      ID_CONFIG_ANALYSIS_SECONDS,
      ID_CONFIG_ANALYSIS_PASSES,
      IDC_LIST_GENRE_RULES,
      IDC_EDIT_RULE_GENRES,
      IDC_EDIT_RULE_MIN,
      IDC_BTN_RULE_ADD,
      IDC_BTN_RULE_UPDATE,
      IDC_BTN_RULE_REMOVE,
      IDC_BTN_RULE_UP,
      IDC_BTN_RULE_DOWN,
      IDC_BTN_RULE_IMPORT_CLIPBOARD,
      IDC_BTN_RULE_EXPORT_CLIPBOARD};

  for (const int controlID : controlsToLock) {
    SetControlEnabled(controlID, !locked);
  }

  if (!locked) {
    UpdateRuleSelectionUiState();
  }
  UpdateUnmatchedPlaylistUiState();
  UpdateReviewPlaylistUiState();
}

void bpm_preferences_page::UpdateUnmatchedPlaylistUiState() {
  const bool createPlaylist =
      ReadCheckboxFromUi(ID_CONFIG_CREATE_UNMATCHED_PLAYLIST);
  SetControlEnabled(ID_CONFIG_SMART_TEMPO_PLAYLIST_NAME,
                    !m_importInProgress && createPlaylist);
}

void bpm_preferences_page::UpdateReviewPlaylistUiState() {
  const bool createPlaylist =
      ReadCheckboxFromUi(ID_CONFIG_CREATE_REVIEW_PLAYLIST);
  SetControlEnabled(ID_CONFIG_REVIEW_PLAYLIST_NAME,
                    !m_importInProgress && createPlaylist);
}

void bpm_preferences_page::UpdateConfidenceTagControls() {
  const bool writeConfidence =
      ReadCheckboxFromUi(ID_CONFIG_WRITE_CONFIDENCE_TAG);
  SetControlEnabled(ID_CONFIG_CONFIDENCE_TAG_NAME, writeConfidence);
  SetControlEnabled(IDC_STATIC_CONFIDENCE_TAG, true);

  SetDynamicTooltip(
      ID_CONFIG_CONFIDENCE_TAG_NAME, writeConfidence,
      writeConfidence ? confidence_tag_name_enabled_tooltip()
                      : tag_name_write_disabled_tooltip());
}

void bpm_preferences_page::apply() {
  if (m_importInProgress) {
    show_smart_tempo_message_dialog(
        m_hWnd, L"Import in Progress",
        L"An import is currently running. Please wait for it to finish before "
        L"applying changes.");
    return;
  }

  bpm_config_bpm_tag = ReadBpmTagNameFromUi();
  bpm_config_confidence_tag_name = ReadConfidenceTagNameFromUi();
  cfg_smart_tempo_playlist_name = ReadPlaylistNameFromUi().c_str();
  cfg_smart_tempo_create_unmatched_playlist =
      ReadCheckboxFromUi(ID_CONFIG_CREATE_UNMATCHED_PLAYLIST);
  cfg_smart_tempo_review_playlist_name =
      ReadReviewPlaylistNameFromUi().c_str();
  cfg_smart_tempo_create_review_playlist =
      ReadCheckboxFromUi(ID_CONFIG_CREATE_REVIEW_PLAYLIST);
  cfg_smart_tempo_routing_tag = ReadRoutingScriptFromUi().c_str();
  cfg_smart_tempo_generic_anchor_tokens = ReadGenericAnchorTokensFromUi().c_str();
  bpm_config_bpm_write_precision = ReadBpmWritePrecisionFromUi();
  bpm_config_auto_write_tag = ReadCheckboxFromUi(ID_CONFIG_AUTO_WRITE_TAG);
  bpm_config_write_confidence_tag =
      ReadCheckboxFromUi(ID_CONFIG_WRITE_CONFIDENCE_TAG);
  bpm_config_worker_mode = ReadWorkerModeFromUi();
  bpm_config_analysis_seconds_to_read = ReadAnalysisSecondsFromUi();
  bpm_config_analysis_sample_passes = ReadAnalysisPassesFromUi();
  cfg_smart_tempo_verbose_logging =
      ReadCheckboxFromUi(ID_CONFIG_VERBOSE_LOGGING);
  SaveListToConfig();
  m_list_is_dirty = false;
}

t_uint32 bpm_preferences_page::get_state() {
  t_uint32 state = preferences_state::dark_mode_supported;
  if (!m_importInProgress) state |= preferences_state::resettable;
  if (HasChanged()) state |= preferences_state::changed;
  return state;
}

bool bpm_preferences_page::HasChanged() {
  if (m_list_is_dirty) return true;
  if (m_list_rules.IsWindow()) {
    const pfc::string8& currentRules = GetRulesConfigSnapshot();
    if (strcmp(cfg_smart_tempo_genre_rules.get().get_ptr(), currentRules.get_ptr()) != 0)
      return true;
  }

  const pfc::string8 uiBpmTag = ReadBpmTagNameFromUi();
  const pfc::string8 uiConfidenceTag = ReadConfidenceTagNameFromUi();

  if (strcmp(bpm_config_bpm_tag.get().get_ptr(), uiBpmTag.get_ptr()) != 0)
    return true;
  if (strcmp(bpm_config_confidence_tag_name.get().get_ptr(),
             uiConfidenceTag.get_ptr()) != 0)
    return true;
  {
    const std::string cfgPlaylist =
        trim_copy(cfg_smart_tempo_playlist_name.get().get_ptr());
    if (cfgPlaylist != ReadPlaylistNameFromUi()) return true;
  }
  if (cfg_smart_tempo_create_unmatched_playlist !=
      ReadCheckboxFromUi(ID_CONFIG_CREATE_UNMATCHED_PLAYLIST))
    return true;
  {
    const std::string cfgReviewPlaylist =
        trim_copy(cfg_smart_tempo_review_playlist_name.get().get_ptr());
    if (cfgReviewPlaylist != ReadReviewPlaylistNameFromUi()) return true;
  }
  if (cfg_smart_tempo_create_review_playlist !=
      ReadCheckboxFromUi(ID_CONFIG_CREATE_REVIEW_PLAYLIST))
    return true;
  {
    const std::string cfgRouting =
        normalize_routing_script_text(cfg_smart_tempo_routing_tag.get().get_ptr());
    if (cfgRouting != ReadRoutingScriptFromUi()) return true;
  }
  {
    const std::string cfgAnchorCsv = smooth_anchor_tokens_for_display(
        cfg_smart_tempo_generic_anchor_tokens.get().get_ptr());
    if (ReadGenericAnchorTokensFromUi() != cfgAnchorCsv) return true;
  }
  if (clamp_bpm_write_precision((int)bpm_config_bpm_write_precision) !=
      ReadBpmWritePrecisionFromUi())
    return true;
  if (bpm_config_auto_write_tag != ReadCheckboxFromUi(ID_CONFIG_AUTO_WRITE_TAG))
    return true;
  if (bpm_config_write_confidence_tag !=
      ReadCheckboxFromUi(ID_CONFIG_WRITE_CONFIDENCE_TAG))
    return true;
  if ((int)bpm_config_worker_mode != ReadWorkerModeFromUi()) return true;
  if (clamp_analysis_seconds_to_read((int)bpm_config_analysis_seconds_to_read) !=
      ReadAnalysisSecondsFromUi()) return true;
  if (clamp_analysis_sample_passes((int)bpm_config_analysis_sample_passes) !=
      ReadAnalysisPassesFromUi()) return true;
  if (cfg_smart_tempo_verbose_logging !=
      ReadCheckboxFromUi(ID_CONFIG_VERBOSE_LOGGING))
    return true;
  return false;
}

void bpm_preferences_page::AddTooltip(int controlID, const TCHAR* text) {
  if (!m_tooltip.IsWindow()) return;
  HWND ctrlHwnd = ControlHandle(controlID);
  if (!ctrlHwnd) return;
  HWND parentHwnd = DialogForControl(controlID);

  TOOLINFO ti = {};
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  ti.hwnd = parentHwnd;
  ti.uId = (UINT_PTR)ctrlHwnd;
  ti.lpszText = const_cast<LPTSTR>(text);
  m_tooltip.AddTool(&ti);
}

void bpm_preferences_page::SetDynamicTooltip(int controlID, bool isEnabled,
                                             const TCHAR* text) {
  if (!m_tooltip.IsWindow()) return;
  HWND ctrlHwnd = ControlHandle(controlID);
  if (!ctrlHwnd) return;
  HWND parentHwnd = DialogForControl(controlID);
  if (!parentHwnd) return;

  TOOLINFO ti = {};
  ti.cbSize = sizeof(ti);
  ti.hwnd = parentHwnd;
  ti.uId = (UINT_PTR)ctrlHwnd;
  m_tooltip.DelTool(&ti);
  ti.uId = (UINT_PTR)controlID;
  m_tooltip.DelTool(&ti);

  TOOLINFO add = {};
  add.cbSize = sizeof(add);
  add.uFlags = TTF_SUBCLASS;
  add.hwnd = parentHwnd;
  add.lpszText = const_cast<LPTSTR>(text);

  if (isEnabled) {
    add.uFlags |= TTF_IDISHWND;
    add.uId = (UINT_PTR)ctrlHwnd;
  } else {
    RECT rc{};
    ::GetWindowRect(ctrlHwnd, &rc);
    ::MapWindowPoints(HWND_DESKTOP, parentHwnd, reinterpret_cast<LPPOINT>(&rc), 2);
    add.uId = (UINT_PTR)controlID;
    add.rect = rc;
  }

  m_tooltip.AddTool(&add);
}

void bpm_preferences_page::InitTooltips() {
  if (m_tooltip.IsWindow()) return;
  if (!m_tooltip.Create(m_hWnd, NULL, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP)) return;

  m_tooltip.SetMaxTipWidth(560);
  m_tooltip.SetDelayTime(TTDT_INITIAL, 350);
  m_tooltip.Activate(TRUE);

  AddTooltip(ID_CONFIG_BPM_TAG, _T("Tag field name used for BPM write/read operations."));
  AddTooltip(ID_CONFIG_BPM_PRECISION, _T("How BPM values are rounded when written."));
  AddTooltip(ID_CONFIG_CONFIDENCE_TAG_NAME,
             _T("Tag name used when confidence values are written."));
  AddTooltip(ID_CONFIG_WRITE_CONFIDENCE_TAG,
             _T("Write confidence values to the confidence tag field."));
  AddTooltip(ID_CONFIG_AUTO_WRITE_TAG,
             _T("Write BPM tags immediately after analysis without the results dialog."));
  AddTooltip(ID_CONFIG_SMART_TEMPO_ROUTING_TAG,
             _T("Title formatting script used for smart routing.\n")
             _T("Recommended default: $if(%genre%,%genre%[; %style%],$if2(%style%,?))\n")
             _T("GENRE is treated as the main route, while STYLE remains available as a detailed fallback.\n")
             _T("Use %style% first only if your STYLE field is curated as the primary musical category."));
  AddTooltip(ID_CONFIG_GENERIC_ANCHOR_TOKENS,
             _T("Advanced: exact routing tokens that are too broad to provide a Soft Genre Center.\n")
             _T("Specific genre matches are preferred over these tokens. A generic-only match still counts as routed, but its Center BPM is disabled and MIR decides without a genre prior.\n")
             _T("The library importer adds unresolved catch-all values to this list so they cannot silently inherit a Genre Center. Changing it can therefore change final routing behavior.\n")
             _T("Separate words with a semicolon (;).\n")
             _T("Examples: Beat; Club; Dance; EDM"));
  AddTooltip(IDC_BTN_ROUTING_IMPORT,
             _T("Import values from the configured routing script across your whole library, ")
             _T("then map selected values onto the existing curated Genre Rules. ")
             _T("The import does not invent new Center BPM values."));
  AddTooltip(ID_CONFIG_SMART_TEMPO_PLAYLIST_NAME,
             _T("Playlist name used for tracks whose routing tags do not match any Genre Rule.\n")
             _T("If left blank, Smart Tempo uses 'Smart Tempo: Unmatched'.\n")
             _T("Disable 'Create playlist' if you do not want unmatched tracks collected."));
  AddTooltip(ID_CONFIG_CREATE_UNMATCHED_PLAYLIST,
             _T("Creates or reuses the configured playlist and adds unmatched routing cases.\n")
             _T("When disabled, unmatched tracks still use the fallback scenario but no playlist is created."));
  AddTooltip(ID_CONFIG_REVIEW_PLAYLIST_NAME,
             _T("Playlist name for tracks whose MIR analysis intentionally produced no writable BPM.\n")
             _T("If left blank, Smart Tempo uses 'Smart Tempo: Needs BPM Review'."));
  AddTooltip(ID_CONFIG_CREATE_REVIEW_PLAYLIST,
             _T("Creates or reuses the configured playlist and adds only MIR review-hold/no-write tracks.\n")
             _T("Reanalyzed tracks are reconciled instead of duplicated: unresolved tracks remain once, while resolved tracks are removed.\n")
             _T("It does not add ordinary tracks when automatic tag writing is disabled."));
  AddTooltip(ID_CONFIG_ANALYSIS_SECONDS,
             _T("Length of each MIR analysis window. Longer windows add rhythmic context but cost more decode/CPU time. Default: 20 seconds."));
  AddTooltip(ID_CONFIG_ANALYSIS_PASSES,
             _T("Number of spaced windows sampled from each track. Fewer windows speed up scans but may reduce evidence. Unknown-length tracks use one. Default: 50."));
  AddTooltip(ID_CONFIG_WORKER_MODE,
             _T("Controls analysis parallelism.\n")
             _T("Max uses all logical CPU cores for fastest library scans.\n")
             _T("Balanced leaves one core free. Conservative also caps workers at 8."));
  AddTooltip(ID_CONFIG_VERBOSE_LOGGING,
             _T("Outputs detailed diagnostic data to the foobar2000 console. ")
             _T("Keep disabled for a cleaner console."));
  AddTooltip(IDC_LIST_GENRE_RULES,
             _T("Master list of genre-to-BPM mapping rules."));
  AddTooltip(IDC_EDIT_RULE_GENRES,
             _T("Enter tags or genres that should map to this rule.\n")
             _T("The import feature splits and routes library tags using your generic routing tokens.\n")
             _T("Separate items with a semicolon (;).\n")
             _T("Examples: Deep House; Techno; Drum & Bass"));
  AddTooltip(IDC_EDIT_RULE_MIN,
             _T("Soft genre center used only to resolve ambiguous measured MIR candidates. ")
             _T("It never creates, rounds, or clamps a BPM value."));
  AddTooltip(IDC_BTN_RULE_ADD, _T("Add a new mapping rule."));
  AddTooltip(IDC_BTN_RULE_UPDATE, _T("Update the selected mapping rule."));
  AddTooltip(IDC_BTN_RULE_REMOVE, _T("Remove the selected mapping rule."));
  AddTooltip(IDC_BTN_RULE_UP, _T("Move selected rule up."));
  AddTooltip(IDC_BTN_RULE_DOWN, _T("Move selected rule down."));
  AddTooltip(IDC_BTN_RULE_IMPORT_CLIPBOARD,
              _T("Imports a genre rules list from the clipboard. ")
              _T("Preferred format: Genres = Center BPM. Exact exports may also carry an internal spread; it is restored automatically and is not a required UI field. ")
              _T("Spread is the curated tolerance scale used by guarded Soft Center comparisons between already measured MIR candidates; it is not a hard BPM range or a generated tempo. ")
              _T("Legacy rule formats remain import-compatible."));
  AddTooltip(IDC_BTN_RULE_EXPORT_CLIPBOARD,
             _T("Exports the current genre rules list to the clipboard. ")
             _T("The exported spread preserves each rule's internal Soft Center tolerance exactly for lossless backup and import."));
}

void bpm_preferences_page::OnChanged() { m_callback->on_state_changed(); }

const char* bpm_preferences_page_impl::get_name() { return "Smart Tempo"; }

GUID bpm_preferences_page_impl::get_guid() { return guid_bpm_preferences; }

GUID bpm_preferences_page_impl::get_parent_guid() {
  return preferences_page::guid_tools;
}

static preferences_page_factory_t<bpm_preferences_page_impl> g_bpm_preferences_page_impl;
