#pragma once
// Central policy for candidate / tip panel status icons (中 / A / 半全角等).
// Trays and language bar use separate logic; do not route them here.

#include "WeaselIPCData.h"

namespace weasel {

// Whether the candidate / tip panel should reserve space and draw the status
// icon strip (same rules historically split between StandardLayout and
// WeaselPanel::Refresh).
//
// Layers:
// 1) Unified IME open state: only IME_OPEN may show panel status icons.
// 2) One-shot suppress after ClearComposition (server tray + panel).
// 3) Legacy layout rules (ascii emphasis, composing / idle, aux tips,
//    fullscreen aux exception).
inline bool PanelShowsStatusIcon(const Status& st,
                                 const Context& ctx,
                                 const UIStyle& style) {
  if (st.ime_open_state != IME_OPEN)
    return false;
  if (st.suppress_status_icon)
    return false;

  const bool fullscreen_aux_suppresses_icon =
      (style.layout_type == UIStyle::LAYOUT_HORIZONTAL_FULLSCREEN ||
       style.layout_type == UIStyle::LAYOUT_VERTICAL_FULLSCREEN) &&
      !ctx.aux.empty();
  if (fullscreen_aux_suppresses_icon)
    return false;

  return (st.ascii_mode && !style.inline_preedit) || !st.composing ||
         !ctx.aux.empty();
}

}  // namespace weasel
