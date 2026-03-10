#include "stdafx.h"
#include "WeaselTSF.h"

STDAPI WeaselTSF::OnInitDocumentMgr(ITfDocumentMgr* pDocMgr) {
  return S_OK;
}

STDAPI WeaselTSF::OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr) {
  return S_OK;
}

STDAPI WeaselTSF::OnSetFocus(ITfDocumentMgr* pDocMgrFocus,
                             ITfDocumentMgr* pDocMgrPrevFocus) {
  _InitTextEditSink(pDocMgrFocus);

  com_ptr<ITfDocumentMgr> pCandidateListDocumentMgr;
  com_ptr<ITfContext> pTfContext = _GetUIContextDocument();
  if ((nullptr != pTfContext) &&
      SUCCEEDED(pTfContext->GetDocumentMgr(&pCandidateListDocumentMgr))) {
    if (pCandidateListDocumentMgr != pDocMgrFocus) {
      _HideUI();
    } else {
      _ShowUI();
    }
  }

  // 文档焦点切换时按 app_options/disable_ime 判定是否自动关闭 IME；优先用文档
  // 窗口即时判定，若 IME 仍打开则调度延迟检查（新开进程时用
  // GetForegroundWindow 纠正）。
  HWND hwndFromDoc = _GetWindowFromDocumentMgr(pDocMgrFocus);
  BOOL toOpenClose = _isToOpenClose;
  BOOL keyboardOpen = _IsKeyboardOpen();
  bool shouldDisable = _ShouldDisableImeForForegroundApp(hwndFromDoc);
  if (shouldDisable && toOpenClose && keyboardOpen) {
    _SetKeyboardOpen(FALSE);
    _disableImeClosedByRule = TRUE;
  } else {
    // 如果之前因黑名单规则关闭了 IME，而当前前台应用不在黑名单中，则恢复 IME。
    if (!shouldDisable && toOpenClose && !keyboardOpen &&
        _disableImeClosedByRule) {
      _SetKeyboardOpen(TRUE);
      _disableImeClosedByRule = FALSE;
    } else if (keyboardOpen) {
      _ScheduleDisableImeDeferCheck();
    }
  }

  return S_OK;
}

STDAPI WeaselTSF::OnPushContext(ITfContext* pContext) {
  return S_OK;
}

STDAPI WeaselTSF::OnPopContext(ITfContext* pContext) {
  return S_OK;
}

BOOL WeaselTSF::_InitThreadMgrEventSink() {
  ITfSource* pSource;
  if (_pThreadMgr->QueryInterface(IID_ITfSource, (void**)&pSource) != S_OK)
    return FALSE;
  if (pSource->AdviseSink(IID_ITfThreadMgrEventSink,
                          (ITfThreadMgrEventSink*)this,
                          &_dwThreadMgrEventSinkCookie) != S_OK) {
    _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
    pSource->Release();
    return FALSE;
  }
  pSource->Release();
  return TRUE;
}

void WeaselTSF::_UninitThreadMgrEventSink() {
  ITfSource* pSource;
  if (_dwThreadMgrEventSinkCookie == TF_INVALID_COOKIE)
    return;
  if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfSource, (void**)&pSource))) {
    pSource->UnadviseSink(_dwThreadMgrEventSinkCookie);
    pSource->Release();
  }
  _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
}
