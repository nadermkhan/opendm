#pragma once

#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include <string>

namespace opendm {
namespace win32 {

// Reads dynamic-length UTF-8 text from any Win32 edit/control without fixed-buffer truncation.
std::string get_window_text_utf8(HWND hwnd);

// Computes centered work-area coordinates relative to parent (or nearest active monitor).
void compute_centered_pos(HWND parent, int w, int h, int& out_x, int& out_y);

// Brings a popup dialog into foreground focus over other apps (e.g. browsers), optionally topmost.
void bring_popup_to_foreground(HWND hwnd, bool make_topmost = true);

// Runs a modal message loop until done == true, hwnd closes, or WM_QUIT arrives.
// Re-enables parent and re-posts WM_QUIT if WM_QUIT aborted the modal loop.
void run_modal_loop(HWND hwnd, HWND parent, bool& done);

// Looks up a GDI+ encoder CLSID (zero-initializes *pClsid, returns >= 0 on success).
int get_gdiplus_encoder_clsid(const WCHAR* format, CLSID* pClsid);

} // namespace win32
} // namespace opendm
