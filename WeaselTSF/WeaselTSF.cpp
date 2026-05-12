#include "stdafx.h"

#include <WeaselIPCData.h>
#include <thread>
#include <shellapi.h>
#include <tlhelp32.h>
#include "WeaselTSF.h"
#include "CandidateList.h"
#include "LanguageBar.h"
#include "Compartment.h"
#include "ResponseParser.h"

static void error_message(const WCHAR* msg) {
  static DWORD next_tick = 0;
  DWORD now = GetTickCount();
  if (now > next_tick) {
    next_tick = now + 10000;  // (ms)
    MessageBox(NULL, msg, get_weasel_ime_name().c_str(), MB_ICONERROR | MB_OK);
  }
}

WeaselTSF::WeaselTSF() {
  _cRef = 1;

  _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;

  _dwTextEditSinkCookie = TF_INVALID_COOKIE;
  _dwTextLayoutSinkCookie = TF_INVALID_COOKIE;
  _dwThreadFocusSinkCookie = TF_INVALID_COOKIE;
  _fTestKeyDownPending = FALSE;
  _fTestKeyUpPending = FALSE;

  _fCUASWorkaroundTested = _fCUASWorkaroundEnabled = FALSE;
  _hwndDisableImeDefer = NULL;
  _disableImeClosedByRule = FALSE;

  _cand = new CCandidateList(this);
  m_imeOpenState = weasel::IME_OPEN;

  DllAddRef();
}

WeaselTSF::~WeaselTSF() {
  DllRelease();
}

STDAPI WeaselTSF::QueryInterface(REFIID riid, void** ppvObject) {
  if (ppvObject == NULL)
    return E_INVALIDARG;

  *ppvObject = NULL;

  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_ITfTextInputProcessor))
    *ppvObject = (ITfTextInputProcessor*)this;
  else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    *ppvObject = (ITfTextInputProcessorEx*)this;
  else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    *ppvObject = (ITfThreadMgrEventSink*)this;
  else if (IsEqualIID(riid, IID_ITfTextEditSink))
    *ppvObject = (ITfTextEditSink*)this;
  else if (IsEqualIID(riid, IID_ITfTextLayoutSink))
    *ppvObject = (ITfTextLayoutSink*)this;
  else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    *ppvObject = (ITfKeyEventSink*)this;
  else if (IsEqualIID(riid, IID_ITfCompositionSink))
    *ppvObject = (ITfCompositionSink*)this;
  else if (IsEqualIID(riid, IID_ITfEditSession))
    *ppvObject = (ITfEditSession*)this;
  else if (IsEqualIID(riid, IID_ITfThreadFocusSink))
    *ppvObject = (ITfThreadFocusSink*)this;
  else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider))
    *ppvObject = (ITfDisplayAttributeProvider*)this;

  if (*ppvObject) {
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

STDAPI_(ULONG) WeaselTSF::AddRef() {
  return ++_cRef;
}

STDAPI_(ULONG) WeaselTSF::Release() {
  LONG cr = --_cRef;

  assert(_cRef >= 0);

  if (_cRef == 0)
    delete this;

  return cr;
}

STDAPI WeaselTSF::Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId) {
  return ActivateEx(pThreadMgr, tfClientId, 0U);
}

STDAPI WeaselTSF::Deactivate() {
  m_client.EndSession();

  _InitTextEditSink(com_ptr<ITfDocumentMgr>());
  _UninitDisableImeDefer();

  _UninitThreadMgrEventSink();

  _UninitKeyEventSink();
  _UninitPreservedKey();

  _UninitLanguageBar();

  _UninitCompartment();

  _UninitThreadMgrEventSink();

  _pThreadMgr = NULL;

  _tfClientId = TF_CLIENTID_NULL;

  _cand->DestroyAll();
  m_imeOpenState = weasel::IME_CLOSED;

  return S_OK;
}

STDAPI WeaselTSF::ActivateEx(ITfThreadMgr* pThreadMgr,
                             TfClientId tfClientId,
                             DWORD dwFlags) {
  com_ptr<ITfDocumentMgr> pDocMgrFocus;
  _activateFlags = dwFlags;

  _pThreadMgr = pThreadMgr;
  _tfClientId = tfClientId;

  if (!_InitThreadMgrEventSink())
    goto ExitError;

  if ((_pThreadMgr->GetFocus(&pDocMgrFocus) == S_OK) &&
      (pDocMgrFocus != NULL)) {
    _InitTextEditSink(pDocMgrFocus);
  }

  if (!_InitKeyEventSink())
    goto ExitError;

  // if (!_InitDisplayAttributeGuidAtom())
  //	goto ExitError;
  //	some app might init failed because it not provide DisplayAttributeInfo,
  // like some opengl stuff
  _InitDisplayAttributeGuidAtom();

  if (!_InitPreservedKey())
    goto ExitError;

  if (!_InitLanguageBar())
    goto ExitError;

  if (!_IsKeyboardOpen())
    _SetKeyboardOpen(TRUE);

  if (!_InitCompartment())
    goto ExitError;
  if (!_InitThreadFocusSink())
    goto ExitError;

  _EnsureServerConnected();
  _SetImeOpenState(_IsKeyboardOpen() ? weasel::IME_OPEN : weasel::IME_CLOSED);

  return S_OK;

ExitError:
  Deactivate();
  return E_FAIL;
}

STDMETHODIMP WeaselTSF::OnSetThreadFocus() {
  std::wstring _ToggleImeOnOpenClose{};
  RegGetStringValue(HKEY_CURRENT_USER, L"Software\\Rime\\weasel",
                    L"ToggleImeOnOpenClose", _ToggleImeOnOpenClose);
  _isToOpenClose = (_ToggleImeOnOpenClose == L"yes");
  if (m_client.Echo()) {
    m_client.ProcessKeyEvent(0);
    weasel::ResponseParser parser(NULL, NULL, &_status, NULL, &_cand->style());
    bool ok = m_client.GetResponseData(std::ref(parser));
    if (ok)
      _UpdateLanguageBar(_status);
  }
  _SetImeOpenState(_IsKeyboardOpen() ? weasel::IME_OPEN : weasel::IME_CLOSED);
  return S_OK;
}
STDMETHODIMP WeaselTSF::OnKillThreadFocus() {
  _AbortComposition();
  return S_OK;
}
BOOL WeaselTSF::_InitThreadFocusSink() {
  com_ptr<ITfSource> pSource;
  if (FAILED(_pThreadMgr->QueryInterface(&pSource)))
    return FALSE;
  if (FAILED(pSource->AdviseSink(IID_ITfThreadFocusSink,
                                 (ITfThreadFocusSink*)this,
                                 &_dwThreadFocusSinkCookie)))
    return FALSE;
  return TRUE;
}
void WeaselTSF::_UninitThreadFocusSink() {
  com_ptr<ITfSource> pSource;
  if (FAILED(_pThreadMgr->QueryInterface(&pSource)))
    return;
  if (FAILED(pSource->UnadviseSink(_dwThreadFocusSinkCookie)))
    return;
}

STDMETHODIMP WeaselTSF::OnActivated(REFCLSID clsid,
                                    REFGUID guidProfile,
                                    BOOL isActivated) {
  if (!IsEqualCLSID(clsid, c_clsidTextService)) {
    return S_OK;
  }

  if (isActivated) {
    _SetImeOpenState(weasel::IME_OPEN);
    _ShowLanguageBar(TRUE);
    _UpdateLanguageBar(_status);
  } else {
    _SetImeOpenState(weasel::IME_CLOSED);
    _DeleteCandidateList();
    _ShowLanguageBar(FALSE);
  }
  return S_OK;
}

void WeaselTSF::_Reconnect() {
  m_client.Disconnect();
  m_client.Connect(NULL);
  m_client.StartSession();
  weasel::ResponseParser parser(NULL, NULL, &_status, NULL, &_cand->style());
  bool ok = m_client.GetResponseData(std::ref(parser));
  if (ok) {
    _UpdateLanguageBar(_status);
  }
  m_client.SetImeOpenState(m_imeOpenState);
}

static unsigned int retry = 0;

bool WeaselTSF::_EnsureServerConnected() {
  if (!m_client.Echo()) {
    _Reconnect();
    retry++;
    if (retry >= 6) {
      HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerExclusiveMutex");
      const auto count_server_process = []() -> int {
        int count = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
          return 0;
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
          do {
            if (_wcsicmp(pe.szExeFile, L"WeaselServer.exe") == 0)
              count++;
          } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        return count;
      };
      if (!m_client.Echo() && GetLastError() != ERROR_ALREADY_EXISTS &&
          !count_server_process()) {
        std::wstring dir = _GetRootDir();
        std::thread th([dir, this]() {
          ShellExecuteW(NULL, L"open", (dir + L"\\start_service.bat").c_str(),
                        NULL, dir.c_str(), SW_HIDE);
          // wait 500ms, then reconnect
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          _Reconnect();
        });
        th.detach();
      }
      if (hMutex) {
        CloseHandle(hMutex);
      }
      retry = 0;
    }
    return (m_client.Echo() != 0);
  } else {
    return true;
  }
}

namespace {
const UINT_PTR DISABLE_IME_DEFER_TIMER_ID = 1;
// 尽量压缩延迟关闭 IME 的感知时间，仅用于文档窗口句柄暂不可用的兜底场景。
const UINT DISABLE_IME_DEFER_MS = 120;
const wchar_t DISABLE_IME_DEFER_CLASS[] = L"WeaselTSF_DisableImeDefer";
}  // namespace

LRESULT CALLBACK WeaselTSF::_DisableImeDeferWndProc(HWND hwnd,
                                                    UINT msg,
                                                    WPARAM wParam,
                                                    LPARAM lParam) {
  WeaselTSF* pThis =
      reinterpret_cast<WeaselTSF*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  if (msg == WM_CREATE && lParam != 0) {
    void* lp = reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams;
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(lp));
    return 0;
  }
  if (msg == WM_TIMER && wParam == DISABLE_IME_DEFER_TIMER_ID &&
      pThis != NULL) {
    KillTimer(hwnd, DISABLE_IME_DEFER_TIMER_ID);
    pThis->_OnDisableImeDeferTimer();
    return 0;
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

void WeaselTSF::_ScheduleDisableImeDeferCheck() {
  if (_hwndDisableImeDefer == NULL) {
    static ATOM reg = 0;
    if (reg == 0) {
      WNDCLASSEXW wc = {0};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = _DisableImeDeferWndProc;
      wc.hInstance = GetModuleHandle(NULL);
      wc.lpszClassName = DISABLE_IME_DEFER_CLASS;
      reg = RegisterClassExW(&wc);
      if (reg == 0)
        return;
    }
    _hwndDisableImeDefer =
        CreateWindowExW(0, DISABLE_IME_DEFER_CLASS, NULL, 0, 0, 0, 0, 0,
                        HWND_MESSAGE, NULL, GetModuleHandle(NULL), this);
    if (_hwndDisableImeDefer == NULL)
      return;
  }
  SetTimer(_hwndDisableImeDefer, DISABLE_IME_DEFER_TIMER_ID,
           DISABLE_IME_DEFER_MS, NULL);
}

void WeaselTSF::_OnDisableImeDeferTimer() {
  BOOL toOpenClose = _isToOpenClose;
  BOOL keyboardOpen = _IsKeyboardOpen();
  bool shouldDisable = _ShouldDisableImeForForegroundApp(NULL);
  if (shouldDisable && toOpenClose && keyboardOpen) {
    _RequestImeOpenStateChange(FALSE);
    _disableImeClosedByRule = TRUE;
  }
}

void WeaselTSF::_UninitDisableImeDefer() {
  if (_hwndDisableImeDefer != NULL) {
    KillTimer(_hwndDisableImeDefer, DISABLE_IME_DEFER_TIMER_ID);
    DestroyWindow(_hwndDisableImeDefer);
    _hwndDisableImeDefer = NULL;
  }
}

void WeaselTSF::_SetImeOpenState(weasel::ImeOpenState state) {
  if (m_imeOpenState == state)
    return;
  m_imeOpenState = state;
  if (state != weasel::IME_OPEN)
    _cand->HidePanelForClosedKeyboard();
  if (m_client.Echo())
    m_client.SetImeOpenState(state);
}

HRESULT WeaselTSF::_RequestImeOpenStateChange(BOOL fOpen) {
  if (!fOpen)
    _EnterImeClosingState();
  return _SetKeyboardOpen(fOpen);
}

void WeaselTSF::_ClearCompositionForUi() {
  _suppressStatusIconForNextPaint = true;
  m_client.ClearComposition();
}
