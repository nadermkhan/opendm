#pragma once

#include <gtk/gtk.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include "progress_dialog.hpp"
#include "download_info_dialog.hpp"
#include "../core/download_queue.hpp"

namespace opendm {

class MainWindow {
public:
    explicit MainWindow(GtkApplication* app);
    ~MainWindow();

    void present();
    GtkWidget* get_widget() const { return window_; }

    void add_download_url(const std::string& url, const std::string& path = "",
                          const std::string& filename = "", bool start_now = true);
    void prompt_download(const DownloadPromptInfo& info);

    bool save_snapshot(const std::string& filename);

private:
    void setup_ui();
    void setup_menubar(GtkWidget* parent_box);
    void setup_toolbar(GtkWidget* parent_box);
    void setup_sidebar(GtkWidget* paned);
    void setup_downloads_view(GtkWidget* paned);
    void setup_statusbar(GtkWidget* parent_box);

    void refresh_downloads_list();
    static gboolean on_tick_refresh(gpointer user_data);

    // Actions
    void on_add_url_clicked();
    void on_resume_clicked();
    void on_stop_clicked();
    void on_stop_all_clicked();
    void on_delete_clicked();
    void on_delete_completed_clicked();
    void on_start_queue_clicked();
    void on_stop_queue_clicked();
    void on_speed_limiter_clicked();
    void on_scheduler_clicked();
    void on_options_clicked(int initial_tab = 0);
    void on_zip_preview_clicked();
    void on_site_grabber_clicked();
    void on_batch_import_clicked();
    void on_batch_export_clicked();
    void on_properties_clicked();
    void on_delete_confirmed(bool delete_file_on_disk);
    void on_sidebar_toggle_clicked();
    void on_category_selected(const std::string& category);
    void on_row_activated(const std::string& session_id);
    void on_row_right_clicked(GtkGestureClick* gesture, int n_press, double x, double y, const std::string& session_id);
    void update_toolbar_sensitivity();

    GtkApplication* app_{nullptr};
    GtkWidget* window_{nullptr};
    GtkWidget* sidebar_vbox_{nullptr};
    GtkWidget* sidebar_list_{nullptr};
    GtkWidget* downloads_container_{nullptr};
    GtkWidget* btn_resume_{nullptr};
    GtkWidget* btn_stop_{nullptr};
    GtkWidget* btn_delete_{nullptr};
    GtkWidget* status_panel_count_{nullptr};
    GtkWidget* status_panel_speed_{nullptr};
    GtkWidget* status_panel_limiter_{nullptr};

    std::string current_category_{"All Downloads"};
    std::string selected_session_id_;
    bool active_context_menu_open_{false};

    // Category tree expansion state: category -> is_expanded
    std::map<std::string, bool> category_expanded_;
    // Category row widgets: category -> GtkWidget* row
    std::map<std::string, GtkWidget*> category_rows_;
    // Category child list: parent -> list of child categories
    std::map<std::string, std::vector<std::string>> category_children_;

    guint refresh_timer_id_{0};

    // Synchronized column size groups between header and row cells
    std::vector<GtkWidget*> header_col_widgets_;
    std::vector<GtkSizeGroup*> col_size_groups_;

    // Tracking active progress dialogs
    std::map<std::string, std::shared_ptr<ProgressDialog>> active_dialogs_;

    std::string last_clipboard_url_;
};

} // namespace opendm
