#pragma once

#include <windows.h>
#include <commctrl.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <unordered_set>
#include "../ipc/ipc_server.hpp"
#include "../core/download_queue.hpp"
#include "win32_progress_dialog.hpp"

namespace opendm {
namespace win32 {

class Win32MainWindow {
public:
    explicit Win32MainWindow(HINSTANCE hInstance = nullptr);
    ~Win32MainWindow();

    void present();
    void restore_and_present();
    HWND get_hwnd() const { return hwnd_; }

    void add_download_url(const std::string& url, const std::string& path = "",
                          const std::string& filename = "", bool start_now = true);
    void prompt_download(const DownloadPromptInfo& info);
    // Thread-safe: queues a download prompt to be shown on the UI thread.
    void post_prompt_download(const DownloadPromptInfo& info);
    // Thread-safe: restores and brings main window to foreground on the UI thread.
    void post_present();

    bool save_snapshot(const std::string& filename);

    void on_add_url();
    void on_resume();
    void on_stop();
    void on_stop_all();
    void on_delete();
    void on_delete_completed();
    void on_start_queue();
    void on_stop_queue();
    void on_speed_limiter();
    void on_scheduler();
    void on_options(int initial_tab = 0);
    void on_site_grabber();
    void on_zip_preview();
    void on_batch_import();
    void on_batch_export();
    void on_properties();
    void on_open_file();
    void on_open_folder();
    void on_copy_url();
    void show_progress_dialog(std::shared_ptr<DownloadSession> session);
    std::shared_ptr<DownloadSession> get_selected_session();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void setup_ui();
    void setup_menubar();
    void setup_toolbar();
    void setup_sidebar();
    void setup_downloads_view();
    void setup_statusbar();
    void update_layout();

    void refresh_downloads_list();
    void update_toolbar_sensitivity();
    void apply_dpi_scaling(UINT new_dpi);
    void drain_completed_notices();
    bool extract_youtube_with_progress(const std::string& url, struct YouTubeVideoInfo& out_info, std::string& out_err);

    HINSTANCE hInstance_{nullptr};
    HWND hwnd_{nullptr};
    HWND hToolbar_{nullptr};
    HWND hTreeView_{nullptr};
    HWND hListView_{nullptr};
    HWND hStatusbar_{nullptr};

    HFONT hFont_{nullptr};
    HFONT hFontBold_{nullptr};
    HIMAGELIST himlToolbar_{nullptr};
    HIMAGELIST himlToolbarDisabled_{nullptr};
    HIMAGELIST himlTree_{nullptr};
    HIMAGELIST himlList_{nullptr};

    UINT dpi_{96};
    UINT timer_id_{0};

    int sidebar_width_{220};
    bool sidebar_visible_{true};
    bool dragging_splitter_{false};

    std::string current_category_{"All Downloads"};
    std::string selected_session_id_;
    std::vector<std::string> visible_session_ids_;
    std::map<std::string, int> ext_icon_map_;

    std::map<std::string, HTREEITEM> tree_items_;
    std::map<std::string, std::shared_ptr<Win32ProgressDialog>> active_progress_dialogs_;

    // Re-entrancy guards (modal MessageBox loops keep dispatching WM_TIMER / keys)
    bool delete_in_progress_{false};
    bool refreshing_list_{false};
    bool completion_modal_active_{false};
    bool prompt_in_progress_{false};
    bool closing_{false};
    std::deque<DownloadPromptInfo> prompt_queue_;
    std::deque<std::pair<std::string, uint64_t>> completed_notices_;
    std::unordered_set<std::string> completed_notified_ids_;
    // Last text rendered per row/column, to avoid redundant ListView updates
    std::vector<std::vector<std::wstring>> row_cache_;
    std::vector<int> row_icon_cache_;
};

} // namespace win32
} // namespace opendm
