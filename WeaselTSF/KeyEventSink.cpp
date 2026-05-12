#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <filesystem>
namespace fs = std::filesystem;

static weasel::KeyEvent prevKeyEvent;
static BOOL prevfEaten = FALSE;
static int keyCountToSimulate = 0;

void WeaselTSF::_ProcessKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
  // when _IsKeyboardDisabled don't eat the key,
  // when keyboard closable and keyboard closed, don't eat the key
  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) {
    *pfEaten = FALSE;
    return;
  }

  // if server connection is Not OK, don't eat it.
  if (!_EnsureServerConnected()) {
    *pfEaten = FALSE;
    return;
  }
  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke)) {
    /* Unknown key event */
    *pfEaten = FALSE;
  } else {
    // cheet key code when vertical auto reverse happened, swap up and down
    if (_cand->GetIsReposition()) {
      if (ke.keycode == ibus::Up)
        ke.keycode = ibus::Down;
      else if (ke.keycode == ibus::Down)
        ke.keycode = ibus::Up;
    }
    if (!keyCountToSimulate)
      *pfEaten = (BOOL)m_client.ProcessKeyEvent(ke);

    if (ke.keycode == ibus::Caps_Lock) {
      if (prevKeyEvent.keycode == ibus::Caps_Lock && prevfEaten == TRUE &&
          (ke.mask & ibus::RELEASE_MASK) && (!keyCountToSimulate)) {
        if ((GetKeyState(VK_CAPITAL) & 0x01)) {
          if (_committed || (!*pfEaten && _status.composing)) {
            keyCountToSimulate = 2;
            INPUT inputs[2];
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki = {VK_CAPITAL, 0, 0, 0, 0};
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki = {VK_CAPITAL, 0, KEYEVENTF_KEYUP, 0, 0};
            ::SendInput(sizeof(inputs) / sizeof(INPUT), inputs, sizeof(INPUT));
          }
        }
        *pfEaten = TRUE;
      }
      if (keyCountToSimulate)
        keyCountToSimulate--;
    }

    prevfEaten = *pfEaten;
    prevKeyEvent = ke;
  }
}

STDAPI WeaselTSF::OnSetFocus(BOOL fForeground) {
  if (fForeground) {
    m_client.FocusIn();  // keep IPC semantics; decision is made locally
    BOOL toOpenClose = _isToOpenClose;
    BOOL keyboardOpen = _IsKeyboardOpen();
    bool shouldDisable = _ShouldDisableImeForForegroundApp();
    if (shouldDisable && toOpenClose && keyboardOpen) {
      _RequestImeOpenStateChange(FALSE);
    }
  } else {
    m_client.FocusOut();
    _AbortComposition();
  }

  return S_OK;
}

namespace {
struct KeyChord {
  WPARAM vkey;
  bool ctrl;
  bool shift;
  bool alt;
};

constexpr KeyChord kImeOpenCloseHotkey = {VK_SPACE, true, false, false};

bool IsKeyPressed(const BYTE key_state[256], int vkey) {
  return (key_state[vkey] & 0x80) != 0;
}

fs::path GetWeaselUserDataPath() {
  WCHAR path[MAX_PATH] = {0};
  HKEY hKey = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Rime\\Weasel", 0, KEY_READ,
                    &hKey) == ERROR_SUCCESS) {
    DWORD type = 0;
    DWORD len = sizeof(path);
    if (RegQueryValueExW(hKey, L"RimeUserDir", nullptr, &type, (LPBYTE)path,
                         &len) == ERROR_SUCCESS &&
        type == REG_SZ && path[0]) {
      RegCloseKey(hKey);
      return fs::path(path);
    }
    RegCloseKey(hKey);
  }
  ExpandEnvironmentStringsW(L"%AppData%\\Rime", path, _countof(path));
  return fs::path(path);
}

// Log one line to Rime user dir weasel_disable_ime.log for debugging
// disable_ime / window-title based exceptions.
static void AppendDisableImeLog(const std::wstring& line) {
  (void)line;
  // Logging disabled; re-enable by writing to weasel_disable_ime.log here.
}

// Max last-write time of weasel.yaml / weasel.custom.yaml; 0 if neither exists.
static ULONGLONG GetDisableImeConfigMtime() {
  fs::path user_dir = GetWeaselUserDataPath();
  ULONGLONG max_time = 0;
  for (const wchar_t* name : {L"weasel.yaml", L"weasel.custom.yaml"}) {
    HANDLE h = CreateFileW((user_dir / name).c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
      FILETIME ft = {0};
      if (GetFileTime(h, NULL, NULL, &ft)) {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        if (u.QuadPart > max_time)
          max_time = u.QuadPart;
      }
      CloseHandle(h);
    }
  }
  return max_time;
}

static std::wstring Utf8ToWLower(const std::string& s) {
  if (s.empty())
    return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
  if (n <= 0)
    return L"";
  std::wstring out(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
  std::transform(out.begin(), out.end(), out.begin(), ::towlower);
  return out;
}

static std::wstring Utf8ToW(const std::string& s) {
  if (s.empty())
    return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
  if (n <= 0)
    return L"";
  std::wstring out(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
  return out;
}

struct DisableImeRule {
  bool disable_ime = false;
  std::wstring except_window_title_contains;
};

// Extract value after colon from "key: value", trim and strip surrounding
// quotes.
static std::string GetYamlValueAfterColon(const std::string& trimmed) {
  size_t colon = trimmed.find(':');
  if (colon == std::string::npos)
    return "";
  size_t v = colon + 1;
  while (v < trimmed.size() && (trimmed[v] == ' ' || trimmed[v] == '\t'))
    v++;
  std::string val = trimmed.substr(v);
  while (!val.empty() &&
         (val.back() == ' ' || val.back() == '\t' || val.back() == '\r'))
    val.pop_back();
  if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                          (val.front() == '\'' && val.back() == '\'')))
    val = val.substr(1, val.size() - 2);
  return val;
}

// Parse patch-style: "  app_options/conhost.exe/disable_ime: true"
// or value on next line: "  app_options/.../disable_ime:\n    true"
static void ParsePatchAppOptionsDisableIme(
    const std::string& content,
    std::map<std::wstring, DisableImeRule>& app_rules) {
  const std::string prefix = "app_options/";
  const std::string suffix = "/disable_ime:";
  std::istringstream is(content);
  std::string line;
  while (std::getline(is, line)) {
    size_t pos = line.find(prefix);
    if (pos == std::string::npos)
      continue;
    pos += prefix.size();
    size_t end = line.find(suffix, pos);
    if (end == std::string::npos)
      continue;
    std::string app(line.substr(pos, end - pos));
    size_t val_start = end + suffix.size();
    while (val_start < line.size() &&
           (line[val_start] == ' ' || line[val_start] == '\t'))
      val_start++;
    bool val = (line.find("true", val_start) != std::string::npos);
    if (!val && std::getline(is, line)) {
      size_t s = line.find_first_not_of(" \t");
      if (s != std::string::npos && line.find("true", s) != std::string::npos)
        val = true;
    }
    std::wstring key = Utf8ToWLower(app);
    if (!key.empty() && val)
      app_rules[key] = {true, L""};
  }
}

// Parse "app_options/xxx.exe": key style; disable_ime and
// disable_ime_except_window_title_contains may follow on next lines.
static void ParseQuotedAppOptionsKeys(
    const std::string& content,
    std::map<std::wstring, DisableImeRule>& app_rules) {
  const std::string prefix = "app_options/";
  std::istringstream is(content);
  std::string line;
  std::string current_app;
  while (std::getline(is, line)) {
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos)
      continue;
    std::string trimmed = line.substr(start);
    if (trimmed.empty() || trimmed[0] == '#')
      continue;
    size_t pos = trimmed.find(prefix);
    if (pos != std::string::npos) {
      size_t name_start = pos + prefix.size();
      size_t colon = trimmed.find(':', name_start);
      if (colon != std::string::npos) {
        std::string name = trimmed.substr(name_start, colon - name_start);
        size_t b = name.find_first_not_of(" \t");
        size_t e = name.find_last_not_of(" \t");
        if (b != std::string::npos) {
          size_t len = (e != std::string::npos ? e + 1 : name.size()) - b;
          name = name.substr(b, len);
        }
        while (!name.empty() && name.back() == '"')
          name.pop_back();
        while (name.size() >= 2 && name.front() == '"')
          name = name.substr(1);
        if (!name.empty())
          current_app = name;
      }
      continue;
    }
    size_t colon = trimmed.find(':');
    if (colon == std::string::npos || current_app.empty())
      continue;
    std::string key = trimmed.substr(0, colon);
    size_t key_end = key.find_last_not_of(" \t");
    if (key_end != std::string::npos)
      key = key.substr(0, key_end + 1);
    std::wstring wapp = Utf8ToWLower(current_app);
    if (wapp.empty())
      continue;
    if (key == "disable_ime") {
      bool val = (trimmed.find("true", colon + 1) != std::string::npos);
      if (val)
        app_rules[wapp].disable_ime = true;
    } else if (key == "disable_ime_except_window_title_contains") {
      std::string raw = GetYamlValueAfterColon(trimmed);
      if (!raw.empty())
        app_rules[wapp].except_window_title_contains = Utf8ToW(raw);
    }
  }
}

// Parse app_options block (any indent >= 2 under app_options:) and patch-style.
void ParseAppOptionsDisableIme(
    const std::string& content,
    std::map<std::wstring, DisableImeRule>& app_rules) {
  ParsePatchAppOptionsDisableIme(content, app_rules);
  ParseQuotedAppOptionsKeys(content, app_rules);
  std::istringstream is(content);
  std::string line;
  bool in_app_options = false;
  size_t options_indent = 0;  // indent of "app_options:"
  std::string current_app;
  while (std::getline(is, line)) {
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos)
      continue;
    std::string trimmed = line.substr(start);
    if (trimmed.empty() || trimmed[0] == '#')
      continue;
    // Accept "app_options:" with optional trailing space/cr
    bool is_app_options_key = trimmed.size() >= 12 &&
                              trimmed.compare(0, 12, "app_options:") == 0 &&
                              (trimmed.size() == 12 || trimmed[12] == ' ' ||
                               trimmed[12] == '\t' || trimmed[12] == '\r');
    if (is_app_options_key) {
      in_app_options = true;
      options_indent = start;
      current_app.clear();
      continue;
    }
    if (!in_app_options)
      continue;
    if (start <= options_indent) {
      in_app_options = false;
      continue;
    }
    size_t colon = trimmed.find(':');
    if (colon == std::string::npos)
      continue;
    std::string key = trimmed.substr(0, colon);
    size_t key_end = key.find_last_not_of(" \t");
    if (key_end != std::string::npos)
      key = key.substr(0, key_end + 1);
    if (key == "disable_ime") {
      bool val = (trimmed.find("true", colon + 1) != std::string::npos);
      if (!current_app.empty()) {
        std::wstring wapp = Utf8ToWLower(current_app);
        if (!wapp.empty() && val)
          app_rules[wapp].disable_ime = true;
      }
    } else if (key == "disable_ime_except_window_title_contains") {
      std::string raw = GetYamlValueAfterColon(trimmed);
      if (!current_app.empty() && !raw.empty()) {
        std::wstring wapp = Utf8ToWLower(current_app);
        if (!wapp.empty())
          app_rules[wapp].except_window_title_contains = Utf8ToW(raw);
      }
    } else if (start == options_indent + 2) {
      // First level under app_options: this line is an app name (e.g. cmd.exe)
      current_app = key;
    }
  }
}

void LoadDisableImeRules(std::map<std::wstring, DisableImeRule>& out) {
  out.clear();
  fs::path user_dir = GetWeaselUserDataPath();
  auto read_file = [](const fs::path& p) -> std::string {
    std::ifstream f(p, std::ios::binary);
    if (!f)
      return "";
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
  };
  std::string yaml_base = read_file(user_dir / "weasel.yaml");
  std::string yaml_custom = read_file(user_dir / "weasel.custom.yaml");
  ParseAppOptionsDisableIme(yaml_base, out);
  ParseAppOptionsDisableIme(yaml_custom, out);
}

// Cache for disable_ime rules; refreshed when config file mtime changes.
static std::map<std::wstring, DisableImeRule> s_disableImeRules;
static ULONGLONG s_disableImeRulesMtime = 0;

const std::map<std::wstring, DisableImeRule>& GetCachedDisableImeRules() {
  ULONGLONG mtime = GetDisableImeConfigMtime();
  if (mtime != s_disableImeRulesMtime) {
    LoadDisableImeRules(s_disableImeRules);
    s_disableImeRulesMtime = mtime;
  }
  return s_disableImeRules;
}
}  // namespace

bool WeaselTSF::_ShouldDisableImeForForegroundApp(HWND hwndFromDoc) {
  HWND hwnd = (hwndFromDoc != NULL) ? hwndFromDoc : GetForegroundWindow();
  if (!hwnd)
    return false;

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid)
    return false;

  const DWORD kAccess = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
  HANDLE hProcess = OpenProcess(kAccess, FALSE, pid);
  if (!hProcess)
    return false;

  WCHAR path[MAX_PATH] = {0};
  DWORD size = _countof(path);
  BOOL ok = QueryFullProcessImageNameW(hProcess, 0, path, &size);
  if (!ok || !path[0]) {
    CloseHandle(hProcess);
    return false;
  }

  std::wstring exe(path);
  size_t pos = exe.find_last_of(L"\\/");
  if (pos != std::wstring::npos && pos + 1 < exe.size())
    exe = exe.substr(pos + 1);
  std::wstring exeLower = exe;
  std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(),
                 ::towlower);

  const auto& rules = GetCachedDisableImeRules();
  auto it = rules.find(exeLower);
  if (it == rules.end()) {
    CloseHandle(hProcess);
    return false;
  }
  if (!it->second.disable_ime) {
    CloseHandle(hProcess);
    return false;
  }

  const DisableImeRule& rule = it->second;
  // 窗口特征例外：如 Zmail，
  //    disable_ime_except_window_title_contains: "Zmail"
  if (!rule.except_window_title_contains.empty()) {
    wchar_t title[256] = {0};
    GetWindowTextW(hwnd, title, _countof(title));
    std::wstring title_lower = title;
    std::transform(title_lower.begin(), title_lower.end(), title_lower.begin(),
                   ::towlower);
    std::wstring needle = rule.except_window_title_contains;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
    bool title_matched =
        (!needle.empty() && title_lower.find(needle) != std::wstring::npos);
    if (title_matched) {
      CloseHandle(hProcess);
      return false;
    }
  }

  CloseHandle(hProcess);
  return true;
}

bool WeaselTSF::_MatchesImeCloseHotkey(WPARAM wParam, LPARAM lParam) {
  if (!_isToOpenClose || !_IsKeyboardOpen() ||
      wParam != kImeOpenCloseHotkey.vkey)
    return false;

  KeyInfo kinfo(lParam);
  if (kinfo.isKeyUp)
    return false;

  BYTE key_state[256] = {0};
  GetKeyboardState(key_state);
  return IsKeyPressed(key_state, VK_CONTROL) == kImeOpenCloseHotkey.ctrl &&
         IsKeyPressed(key_state, VK_SHIFT) == kImeOpenCloseHotkey.shift &&
         IsKeyPressed(key_state, VK_MENU) == kImeOpenCloseHotkey.alt;
}

void WeaselTSF::_EnterImeClosingState() {
  if (!_IsKeyboardOpen())
    return;

  // Enter IME_CLOSING before the compartment callback lands, so all icon UI is
  // gated off during the close-request -> compartment-update gap.
  _SetImeOpenState(weasel::IME_CLOSING);
}

/* Some apps sends strange OnTestKeyDown/OnKeyDown combinations:
 *  Some sends OnKeyDown() only. (QQ2012)
 *  Some sends multiple OnTestKeyDown() for a single key event. (MS WORD 2010
 * x64)
 *
 * We assume every key event will eventually cause a OnKeyDown() call.
 * We use _fTestKeyDownPending to omit multiple OnTestKeyDown() calls,
 *  and for OnKeyDown() to check if the key has already been sent to the server.
 */

STDAPI WeaselTSF::OnTestKeyDown(ITfContext* pContext,
                                WPARAM wParam,
                                LPARAM lParam,
                                BOOL* pfEaten) {
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  if (_MatchesImeCloseHotkey(wParam, lParam))
    _EnterImeClosingState();
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyDownPending = TRUE;
  return S_OK;
}

STDAPI WeaselTSF::OnKeyDown(ITfContext* pContext,
                            WPARAM wParam,
                            LPARAM lParam,
                            BOOL* pfEaten) {
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    _fTestKeyDownPending = FALSE;
    *pfEaten = TRUE;
  } else {
    if (_MatchesImeCloseHotkey(wParam, lParam))
      _EnterImeClosingState();
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    _UpdateComposition(pContext);
  }
  return S_OK;
}

STDAPI WeaselTSF::OnTestKeyUp(ITfContext* pContext,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL* pfEaten) {
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyUpPending = TRUE;
  return S_OK;
}

STDAPI WeaselTSF::OnKeyUp(ITfContext* pContext,
                          WPARAM wParam,
                          LPARAM lParam,
                          BOOL* pfEaten) {
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    _fTestKeyUpPending = FALSE;
    *pfEaten = TRUE;
  } else {
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    if (!_async_edit)
      _UpdateComposition(pContext);
  }
  return S_OK;
}

STDAPI WeaselTSF::OnPreservedKey(ITfContext* pContext,
                                 REFGUID rguid,
                                 BOOL* pfEaten) {
  *pfEaten = FALSE;
  return S_OK;
}

BOOL WeaselTSF::_InitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
  HRESULT hr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return FALSE;

  hr = pKeystrokeMgr->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink*)this,
                                         TRUE);

  return (hr == S_OK);
}

void WeaselTSF::_UninitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return;

  pKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
}

BOOL WeaselTSF::_InitPreservedKey() {
  return TRUE;
#if 0
	com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (_pThreadMgr->QueryInterface(pKeystrokeMgr.GetAddressOf()) != S_OK)
	{
		return FALSE;
	}
	TF_PRESERVEDKEY preservedKeyImeMode;

	/* Define SHIFT ONLY for now */
	preservedKeyImeMode.uVKey = VK_SHIFT;
	preservedKeyImeMode.uModifiers = TF_MOD_ON_KEYUP;

	auto hr = pKeystrokeMgr->PreserveKey(
		_tfClientId,
		GUID_IME_MODE_PRESERVED_KEY,
		&preservedKeyImeMode, L"", 0);
	
	return SUCCEEDED(hr);
#endif
}

void WeaselTSF::_UninitPreservedKey() {}
