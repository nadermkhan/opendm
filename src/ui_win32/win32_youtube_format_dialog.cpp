#include "win32_youtube_format_dialog.hpp"
#include "win32_dialogs_common.hpp"
#include "dpi_helper.hpp"
#include "../core/win32_unicode.hpp"
#include "../core/media_muxer.hpp"
#include "../core/download_session.hpp"
#include <commctrl.h>
#include <vector>

namespace opendm {
namespace win32 {

namespace {

struct YtDialogState {
    HWND hwnd{nullptr};
    HWND hListView{nullptr};
    HWND hBtnDownload{nullptr};
    HWND hBtnAudioOnly{nullptr};
    HWND hBtnCancel{nullptr};

    HFONT hFont{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontTitle{nullptr};

    YouTubeVideoInfo info;
    std::vector<YouTubeFormat> displayed_formats;
    YoutubeFormatResult result;
    bool has_ffmpeg{false};
    bool done{false};
};

std::string sanitize_fname(const std::string& title, const std::string& ext) {
    std::string base = DownloadSession::sanitize_filename(title);
    if (base.empty() || base == "download.bin") base = "youtube_video";
    if (!ext.empty()) {
        std::string lower_ext = sys::ascii_tolower(ext);
        std::string tail = "." + lower_ext;
        if (base.size() < tail.size() ||
            sys::ascii_tolower(base.substr(base.size() - tail.size())) != tail) {
            base += tail;
        }
    }
    return base;
}

std::string ext_for_container(const std::string& container, const std::string& def) {
    std::string lc = sys::ascii_tolower(container);
    if (lc == "mp4" || lc == "m4a" || lc == "webm" || lc == "mkv") return lc;
    return def;
}

LRESULT CALLBACK YtWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    YtDialogState* state = reinterpret_cast<YtDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<YtDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->hwnd = hwnd;
            return 0;
        }
        case WM_NOTIFY: {
            NMHDR* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (state && nmhdr && nmhdr->hwndFrom == state->hListView && nmhdr->code == NM_DBLCLK) {
                SendMessageW(hwnd, WM_COMMAND, 801, 0);
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 801) { // Download Selected Format
                int sel = ListView_GetNextItem(state->hListView, -1, LVNI_SELECTED);
                if (sel < 0 && !state->displayed_formats.empty()) {
                    sel = 0; // Default to first format if none highlighted
                }
                if (sel >= 0 && sel < static_cast<int>(state->displayed_formats.size())) {
                    const auto& vfmt = state->displayed_formats[sel];
                    if (!vfmt.is_progressive && !state->info.audio_formats.empty() &&
                        vfmt.container != "MP4" && !state->has_ffmpeg) {
                        MessageBoxW(
                            hwnd,
                            L"This WebM video requires FFmpeg in PATH or the application directory to mux with audio.\n"
                            L"Please select an MP4 format or Download Audio Only.",
                            L"FFmpeg Required for WebM Muxing",
                            MB_OK | MB_ICONWARNING
                        );
                        return 0;
                    }
                    state->result.video_url = vfmt.url;
                    state->result.user_agent = vfmt.user_agent;
                    std::string out_ext = (vfmt.container == "MP4") ? "mp4" : "mkv";
                    state->result.filename = sanitize_fname(state->info.title, out_ext);
                    if (!vfmt.is_progressive && !state->info.audio_formats.empty()) {
                        YouTubeFormat chosen_audio = state->info.best_audio;
                        for (const auto& af : state->info.audio_formats) {
                            if (vfmt.container == "WEBM" && af.container == "WEBM") {
                                chosen_audio = af;
                                break;
                            } else if (vfmt.container == "MP4" && (af.container == "M4A" || af.container == "MP4")) {
                                chosen_audio = af;
                                break;
                            }
                        }
                        state->result.audio_url = chosen_audio.url;
                        state->result.is_muxed = true;
                    }
                    state->result.accepted = true;
                } else if (!state->info.progressive_formats.empty()) {
                    const auto& pfmt = state->info.progressive_formats.front();
                    state->result.video_url = pfmt.url;
                    state->result.user_agent = pfmt.user_agent;
                    state->result.filename = sanitize_fname(state->info.title, ext_for_container(pfmt.container, "mp4"));
                    state->result.accepted = true;
                }
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == 802) { // Download Audio Only
                if (!state->info.audio_formats.empty()) {
                    const auto& afmt = state->info.best_audio;
                    state->result.video_url = afmt.url;
                    state->result.user_agent = afmt.user_agent;
                    state->result.filename = sanitize_fname(state->info.title, ext_for_container(afmt.container, "m4a"));
                    state->result.accepted = true;
                }
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL || id == 803) {
                state->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            state->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

YoutubeFormatResult Win32YoutubeFormatDialog::show(HWND parent, const YouTubeVideoInfo& info) {
    YtDialogState state;
    state.info = info;
    state.has_ffmpeg = !MediaMuxer::find_ffmpeg().empty();

    UINT dpi = DpiHelper::get_dpi(parent);
    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);
    state.hFontTitle = DpiHelper::create_font(11, FW_BOLD, false, dpi);

    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_YtDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = YtWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(600, dpi);
    int dlg_h = DpiHelper::scale(400, dpi);

    int x = 0, y = 0;
    compute_centered_pos(parent, dlg_w, dlg_h, x, y);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        className, L"YouTube Video Stream Options",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandleW(nullptr), &state
    );

    int margin_x = DpiHelper::scale(16, dpi);
    int cur_y = DpiHelper::scale(14, dpi);
    int content_w = dlg_w - margin_x * 2 - DpiHelper::scale(16, dpi);

    // Title label
    std::wstring wtitle = sys::utf8_to_wide(info.title);
    HWND hTitle = CreateWindowExW(0, L"STATIC", wtitle.c_str(),
        WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
        margin_x, cur_y, content_w, DpiHelper::scale(22, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hTitle, WM_SETFONT, (WPARAM)state.hFontTitle, TRUE);

    cur_y += DpiHelper::scale(24, dpi);

    // Author & Duration
    std::string sub = "Author: " + info.author + "   Duration: " + YouTubeEngine::format_duration(info.duration_seconds);
    std::wstring wsub = sys::utf8_to_wide(sub);
    HWND hSub = CreateWindowExW(0, L"STATIC", wsub.c_str(),
        WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
        margin_x, cur_y, content_w, DpiHelper::scale(18, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hSub, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    cur_y += DpiHelper::scale(26, dpi);

    // ListView of formats
    int list_h = DpiHelper::scale(220, dpi);
    state.hListView = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
        margin_x, cur_y, content_w, list_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(state.hListView, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    ListView_SetExtendedListViewStyle(state.hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW lvc = {LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM};
    lvc.cx = DpiHelper::scale(140, dpi);
    lvc.pszText = (LPWSTR)L"Quality";
    ListView_InsertColumn(state.hListView, 0, &lvc);

    lvc.cx = DpiHelper::scale(90, dpi);
    lvc.pszText = (LPWSTR)L"Container";
    ListView_InsertColumn(state.hListView, 1, &lvc);

    lvc.cx = DpiHelper::scale(120, dpi);
    lvc.pszText = (LPWSTR)L"Bitrate";
    ListView_InsertColumn(state.hListView, 2, &lvc);

    lvc.cx = DpiHelper::scale(180, dpi);
    lvc.pszText = (LPWSTR)L"Audio Track";
    ListView_InsertColumn(state.hListView, 3, &lvc);

    state.displayed_formats = !info.video_formats.empty() ? info.video_formats : info.progressive_formats;

    for (size_t i = 0; i < state.displayed_formats.size(); ++i) {
        const auto& f = state.displayed_formats[i];
        std::wstring wq = sys::utf8_to_wide(f.quality_label);
        std::wstring wc = sys::utf8_to_wide(f.container);
        std::string br = YouTubeEngine::format_bitrate(f.bitrate);
        std::wstring wbr = sys::utf8_to_wide(br);
        std::wstring wa = f.is_progressive
            ? L"Included (Progressive)"
            : ((!state.has_ffmpeg && f.container != "MP4")
                ? L"Requires FFmpeg (WebM Mux)"
                : (L"Auto-Mux Audio (" + std::to_wstring(info.best_audio.bitrate / 1000) + L" kbps)"));

        LVITEMW lvi = {LVIF_TEXT};
        lvi.iItem = static_cast<int>(i);
        lvi.pszText = (LPWSTR)wq.c_str();
        ListView_InsertItem(state.hListView, &lvi);
        ListView_SetItemText(state.hListView, static_cast<int>(i), 1, (LPWSTR)wc.c_str());
        ListView_SetItemText(state.hListView, static_cast<int>(i), 2, (LPWSTR)wbr.c_str());
        ListView_SetItemText(state.hListView, static_cast<int>(i), 3, (LPWSTR)wa.c_str());
    }

    if (!state.displayed_formats.empty()) {
        ListView_SetItemState(state.hListView, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    cur_y += list_h + DpiHelper::scale(16, dpi);

    // Buttons
    int btn_w = DpiHelper::scale(140, dpi);
    int btn_h = DpiHelper::scale(26, dpi);

    state.hBtnDownload = CreateWindowExW(0, L"BUTTON", L"Download Video",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        margin_x, cur_y, btn_w, btn_h,
        hwnd, (HMENU)801, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnDownload, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    state.hBtnAudioOnly = CreateWindowExW(0, L"BUTTON", L"Download Audio Only",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        margin_x + btn_w + DpiHelper::scale(8, dpi), cur_y, btn_w + DpiHelper::scale(10, dpi), btn_h,
        hwnd, (HMENU)802, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnAudioOnly, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    int cancel_w = DpiHelper::scale(85, dpi);
    state.hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        dlg_w - margin_x - cancel_w - DpiHelper::scale(16, dpi), cur_y, cancel_w, btn_h,
        hwnd, (HMENU)803, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnCancel, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    SetFocus(state.hBtnDownload);

    run_modal_loop(hwnd, parent, state.done);

    if (state.hFont) DeleteObject(state.hFont);
    if (state.hFontBold) DeleteObject(state.hFontBold);
    if (state.hFontTitle) DeleteObject(state.hFontTitle);

    return state.result;
}

} // namespace win32
} // namespace opendm
