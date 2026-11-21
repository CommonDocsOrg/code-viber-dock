#pragma once

#include <windows.h>

#include <vector>

namespace Activation {

// Restores `hwnd` if minimized (using Windows task switching if SW_RESTORE is denied)
// and makes it the foreground window. The user's dock click lets us request the
// foreground from a WS_EX_NOACTIVATE window. If refused, tap VK_MENU via SendInput and
// retry, then fall back to AttachThreadInput / BringWindowToTop.
bool BringToFront(HWND hwnd);

void Minimize(HWND hwnd);

// The dock's click behaviour for an item's windows (first-seen order):
//   one window: foreground -> minimize; minimized -> restore + activate; else activate.
//   several: activate the window after the foreground one (wrapping), or the first one when
//   none of them is foreground.
void ActivateItem(const std::vector<HWND>& windows);

// The mouse wheel over an item: brings the window after (forward) / before the foreground one to
// the front, wrapping; the first / last one when none of them is foreground. One window: it is
// brought to the front (never minimized).
void CycleItem(const std::vector<HWND>& windows, bool forward);

}  // namespace Activation
