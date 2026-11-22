#include "win32_zip_preview_dialog.hpp"
#include "win32_dialogs_common.hpp"
#include "dpi_helper.hpp"
#include "../core/zip_previewer.hpp"
#include "../core/win32_unicode.hpp"
#include <commctrl.h>
#include <shlobj.h>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>

namespace opendm {
namespace win32 {

namespace {

constexpr UINT WM_ZIP_FETCHED = WM_USER + 201;
constexpr UINT WM_ZIP_EXTRACTED = WM_USER + 202;

struct ZipExtractResult {
    bool ok{false};
    std::string error;
};

struct ZipDialogState {
    HWND hwnd{nullptr};
    HWND hEditUrl{nullptr};
    HWND hBtnFetch{nullptr};
    HWND hListView{nullptr};
    HWND hBtnExtract{nullptr};
    HWND hStaticStatus{nullptr};
    HWND hBtnClose{nullptr};

    HFONT hFont{nullptr};
    HFONT hFontBold{nullptr};

    std::string current_url;
    std::vector<ZipEntry> entries;
    bool done{false};
    std::shared_ptr<std::atomic<bool>> alive{std::make_shared<std::atomic<bool>>(true)};
};

LRESULT CALLBACK ZipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ZipDialogState* state = reinterpret_cast<ZipDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<ZipDialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->hwnd = hwnd;
            return 0;
        }
        case WM_ZIP_FETCHED: {
            auto* p_entries = reinterpret_cast<std::vector<ZipEntry>*>(lParam);
            if (p_entries) {
                if (state && state->alive && state->alive->load()) {
                    state->entries = std::move(*p_entries);
                    EnableWindow(state->hBtnFetch, TRUE);
                    ListView_DeleteAllItems(state->hListView);

                    if (state->entries.empty()) {
                        SetWindowTextW(state->hStaticStatus, L"Failed to read central directory or archive empty.");
                        EnableWindow(state->hBtnExtract, FALSE);
                    } else {
                        std::wstring st = L"Found " + std::to_wstring(state->entries.size()) + L" files in remote archive.";
                        SetWindowTextW(state->hStaticStatus, st.c_str());

                        for (size_t i = 0; i < state->entries.size(); ++i) {
                            const auto& e = state->entries[i];
                            std::wstring wfn = sys::utf8_to_wide(e.filename);
                            std::wstring wsz = sys::utf8_to_wide(sys::format_bytes(static_cast<int64_t>(e.uncompressed_size)));
                            std::wstring wcsz = sys::utf8_to_wide(sys::format_bytes(static_cast<int64_t>(e.compressed_size)));

                            LVITEMW lvi = {LVIF_TEXT};
                            lvi.iItem = static_cast<int>(i);
                            lvi.pszText = const_cast<LPWSTR>(wfn.c_str());
                            ListView_InsertItem(state->hListView, &lvi);
                            ListView_SetItemText(state->hListView, static_cast<int>(i), 1, const_cast<LPWSTR>(wsz.c_str()));
                            ListView_SetItemText(state->hListView, static_cast<int>(i), 2, const_cast<LPWSTR>(wcsz.c_str()));
                        }
                        EnableWindow(state->hBtnExtract, TRUE);
                    }
                }
                delete p_entries;
            }
            return 0;
        }
        case WM_ZIP_EXTRACTED: {
            auto* res = reinterpret_cast<ZipExtractResult*>(lParam);
            if (res) {
                if (state && state->alive && state->alive->load()) {
                    EnableWindow(state->hBtnExtract, TRUE);
                    SetWindowTextW(state->hStaticStatus,
                        res->ok ? L"Extraction complete." : L"Extraction failed.");
                    std::wstring msg = res->ok
                        ? L"File extracted successfully!"
                        : sys::utf8_to_wide(res->error.empty() ? "Failed to extract file." : ("Failed to extract file:\n" + res->error));
                    MessageBoxW(hwnd, msg.c_str(), L"ZIP Preview",
                        res->ok ? (MB_OK | MB_ICONINFORMATION) : (MB_OK | MB_ICONERROR));
                }
                delete res;
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 701) { // Fetch Directory
                std::string url = get_window_text_utf8(state->hEditUrl);
                if (url.empty()) return 0;
                state->current_url = url;

                EnableWindow(state->hBtnFetch, FALSE);
                SetWindowTextW(state->hStaticStatus, L"Connecting and fetching ZIP central directory...");

                std::thread([hwnd, alive = state->alive, url]() {
                    auto* result = new std::vector<ZipEntry>();
                    std::string err;
                    ZipPreviewer prev;
                    bool ok = prev.fetch_entries(url, *result, err);
                    if (!ok) {
                        result->clear();
                    }
                    if (alive->load() && IsWindow(hwnd)) {
                        if (!PostMessageW(hwnd, WM_ZIP_FETCHED, ok ? 1 : 0, reinterpret_cast<LPARAM>(result))) {
                            delete result;
                        }
                    } else {
                        delete result;
                    }
                }).detach();
                return 0;
            }
            if (id == 702) { // Extract selected
                int sel = ListView_GetNextItem(state->hListView, -1, LVNI_SELECTED);
                if (sel >= 0 && sel < static_cast<int>(state->entries.size())) {
                    const auto entry = state->entries[sel];

                    std::vector<wchar_t> szFile(4096, L'\0');
                    std::string def_fn = entry.filename;
                    size_t slash = def_fn.find_last_of("/\\");
                    if (slash != std::string::npos) def_fn = def_fn.substr(slash + 1);
                    std::wstring wdfn = sys::utf8_to_wide(def_fn);
                    wcsncpy(szFile.data(), wdfn.c_str(), szFile.size() - 1);

                    OPENFILENAMEW ofn = {sizeof(OPENFILENAMEW)};
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = szFile.data();
                    ofn.nMaxFile = static_cast<DWORD>(szFile.size());
                    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

                    if (GetSaveFileNameW(&ofn)) {
                        std::string out_path = sys::wide_to_utf8(szFile.data());

                        EnableWindow(state->hBtnExtract, FALSE);
                        SetWindowTextW(state->hStaticStatus, L"Extracting file...");
                        std::thread([hwnd, alive = state->alive, url = state->current_url, entry, out_path]() {
                            std::string err;
                            ZipPreviewer prev;
                            bool ok = prev.extract_entry(url, entry, out_path, err);
                            auto* res = new ZipExtractResult{ok, err};
                            if (alive->load() && IsWindow(hwnd)) {
                                if (!PostMessageW(hwnd, WM_ZIP_EXTRACTED, 0, reinterpret_cast<LPARAM>(res))) {
                                    delete res;
                                }
                            } else {
                                delete res;
                            }
                        }).detach();
                    }
                }
                return 0;
            }
            if (id == IDCANCEL || id == 703) {
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
        case WM_DESTROY: {
            if (state && state->alive) state->alive->store(false);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void Win32ZipPreviewDialog::show(HWND parent, const std::string& initial_url) {
    ZipDialogState state;
    state.current_url = initial_url;

    UINT dpi = DpiHelper::get_dpi(parent);
    state.hFont = DpiHelper::create_font(9, FW_NORMAL, false, dpi);
    state.hFontBold = DpiHelper::create_font(9, FW_SEMIBOLD, false, dpi);

    static bool s_registered = false;
    const wchar_t* className = L"OpenDM_ZipPreviewDialog";
    if (!s_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = ZipWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        s_registered = true;
    }

    int dlg_w = DpiHelper::scale(620, dpi);
    int dlg_h = DpiHelper::scale(440, dpi);

    int x = 0, y = 0;
    compute_centered_pos(parent, dlg_w, dlg_h, x, y);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        className, L"Preview Remote ZIP Archive",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlg_w, dlg_h,
        parent, nullptr, GetModuleHandleW(nullptr), &state
    );

    int margin_x = DpiHelper::scale(16, dpi);
    int cur_y = DpiHelper::scale(14, dpi);
    int row_h = DpiHelper::scale(24, dpi);

    // URL row
    HWND hLblUrl = CreateWindowExW(0, L"STATIC", L"Archive URL:", WS_CHILD | WS_VISIBLE,
        margin_x, cur_y + 2, DpiHelper::scale(85, dpi), row_h, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hLblUrl, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    int btn_fetch_w = DpiHelper::scale(110, dpi);
    int url_w = dlg_w - margin_x * 2 - DpiHelper::scale(95, dpi) - btn_fetch_w - DpiHelper::scale(8, dpi) - DpiHelper::scale(16, dpi);

    state.hEditUrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        margin_x + DpiHelper::scale(90, dpi), cur_y, url_w, row_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hEditUrl, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    if (!initial_url.empty()) {
        std::wstring wu = sys::utf8_to_wide(initial_url);
        SetWindowTextW(state.hEditUrl, wu.c_str());
    }

    state.hBtnFetch = CreateWindowExW(0, L"BUTTON", L"Fetch Directory",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        margin_x + DpiHelper::scale(95, dpi) + url_w, cur_y, btn_fetch_w, row_h,
        hwnd, (HMENU)701, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnFetch, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    cur_y += row_h + DpiHelper::scale(12, dpi);

    // ListView
    int list_w = dlg_w - margin_x * 2 - DpiHelper::scale(16, dpi);
    int list_h = DpiHelper::scale(260, dpi);
    state.hListView = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP,
        margin_x, cur_y, list_w, list_h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    SendMessageW(state.hListView, WM_SETFONT, (WPARAM)state.hFont, TRUE);
    ListView_SetExtendedListViewStyle(state.hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW lvc = {LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM};
    lvc.cx = DpiHelper::scale(300, dpi);
    lvc.pszText = (LPWSTR)L"File Name";
    ListView_InsertColumn(state.hListView, 0, &lvc);

    lvc.cx = DpiHelper::scale(120, dpi);
    lvc.pszText = (LPWSTR)L"Size";
    ListView_InsertColumn(state.hListView, 1, &lvc);

    lvc.cx = DpiHelper::scale(120, dpi);
    lvc.pszText = (LPWSTR)L"Compressed";
    ListView_InsertColumn(state.hListView, 2, &lvc);

    cur_y += list_h + DpiHelper::scale(10, dpi);

    // Status label
    state.hStaticStatus = CreateWindowExW(0, L"STATIC", L"Enter URL and click Fetch Directory to inspect ZIP.",
        WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
        margin_x, cur_y, list_w, DpiHelper::scale(18, dpi),
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hStaticStatus, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    cur_y += DpiHelper::scale(24, dpi);

    // Extract and Close buttons
    int btn_w = DpiHelper::scale(160, dpi);
    int btn_h = DpiHelper::scale(26, dpi);

    state.hBtnExtract = CreateWindowExW(0, L"BUTTON", L"Download Selected...",
        WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
        margin_x, cur_y, btn_w, btn_h,
        hwnd, (HMENU)702, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnExtract, WM_SETFONT, (WPARAM)state.hFontBold, TRUE);

    int close_w = DpiHelper::scale(85, dpi);
    state.hBtnClose = CreateWindowExW(0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        dlg_w - margin_x - close_w - DpiHelper::scale(16, dpi), cur_y, close_w, btn_h,
        hwnd, (HMENU)703, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state.hBtnClose, WM_SETFONT, (WPARAM)state.hFont, TRUE);

    if (!initial_url.empty()) {
        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(701, BN_CLICKED), (LPARAM)state.hBtnFetch);
    }

    run_modal_loop(hwnd, parent, state.done);

    if (state.hFont) DeleteObject(state.hFont);
    if (state.hFontBold) DeleteObject(state.hFontBold);
}

} // namespace win32
} // namespace opendm
