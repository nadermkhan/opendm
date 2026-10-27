#pragma once

#include <gtk/gtk.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include "progress_dialog.hpp"
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

private:
    void setup_ui();
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
    void on_start_queue_clicked();
    void on_stop_queue_clicked();
    void on_speed_limiter_toggled(GtkToggleButton* btn);
    void on_scheduler_clicked();
    void on_zip_preview_clicked();
    void on_site_grabber_clicked();
    void on_category_selected(const std::string& category);
    void on_row_activated(const std::string& session_id);

    GtkApplication* app_{nullptr};
    GtkWidget* window_{nullptr};
    GtkWidget* sidebar_list_{nullptr};
    GtkWidget* downloads_container_{nullptr};
    GtkWidget* statusbar_label_{nullptr};
    GtkWidget* speed_limiter_btn_{nullptr};

    std::string current_category_{"All Downloads"};
    std::string selected_session_id_;

    guint refresh_timer_id_{0};

    // Tracking active progress dialogs
    std::map<std::string, std::shared_ptr<ProgressDialog>> active_dialogs_;
};

} // namespace opendm
