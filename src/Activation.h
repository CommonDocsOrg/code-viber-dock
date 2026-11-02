#pragma once

#include <windows.h>

#include <vector>

namespace Activation {

// Restores `hwnd` if minimized and makes it the foreground window. Works from the
// WS_EX_NOACTIVATE dock because the click was delivered to our thread; if the foreground
// lock still refuses, taps VK_MENU via SendInput and retries, then falls back to
// AttachThreadInput / BringWindowToTop.
bool BringToFront(HWND hwnd);

void Minimize(HWND hwnd);

// The dock's click behaviour for an item's windows (first-seen order):
//   one window: foreground -> minimize; minimized -> restore + activate; else activate.
//   several: activate the window after the foreground one (wrapping), or the first one when
//   none of them is foreground.
void ActivateItem(const std::vector<HWND>& windows);

}  // namespace Activation
