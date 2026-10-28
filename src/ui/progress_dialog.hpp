#pragma once

#include <gtk/gtk.h>
#include <memory>
#include <string>
#include <vector>
#include "../core/download_session.hpp"

namespace opendm {

struct ConnRowWidgets {
    GtkWidget* row_box{nullptr};
    GtkWidget* lbl_num{nullptr};
    GtkWidget* lbl_downloaded{nullptr};
    GtkWidget* lbl_info{nullptr};
};

class ProgressDialog {
public:
    explicit ProgressDialog(std::shared_ptr<DownloadSession> session, GtkWindow* parent = nullptr);
    ~ProgressDialog();

    void show();
    void close();
    bool save_snapshot(const std::string& filename);
    GtkWidget* get_widget() const { return window_; }

private:
    void setup_ui();
    void setup_status_tab(GtkWidget* notebook);
    void setup_speed_limiter_tab(GtkWidget* notebook);
    void setup_options_tab(GtkWidget* notebook);
    void update_ui();
    void update_connection_table(const std::vector<Chunk>& chunks, SessionStatus status);

    static gboolean on_tick_update(gpointer user_data);
    static void on_draw_visualizer(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer user_data);

    void on_pause_resume_clicked();
    void on_cancel_clicked();
    void on_toggle_details_clicked();
    void on_open_file_clicked();
    void on_open_folder_clicked();
    void on_speed_limiter_toggled();
    void on_speed_limit_changed();

    std::shared_ptr<DownloadSession> session_;
    GtkWindow* parent_{nullptr};

    GtkWidget* window_{nullptr};
    GtkWidget* label_url_{nullptr};
    GtkWidget* label_status_{nullptr};
    GtkWidget* label_file_size_{nullptr};
    GtkWidget* label_downloaded_{nullptr};
    GtkWidget* label_transfer_rate_{nullptr};
    GtkWidget* label_time_left_{nullptr};
    GtkWidget* label_resume_cap_{nullptr};
    GtkWidget* main_progress_bar_{nullptr};

    // Details area (Visualizer bar + Connection table)
    GtkWidget* box_details_{nullptr};
    GtkWidget* visualizer_area_{nullptr};
    GtkWidget* conns_list_box_{nullptr};
    std::vector<ConnRowWidgets> conn_rows_;
    bool details_expanded_{true};

    // Buttons
    GtkWidget* btn_toggle_details_{nullptr};
    GtkWidget* btn_pause_resume_{nullptr};
    GtkWidget* btn_cancel_{nullptr};
    GtkWidget* btn_open_file_{nullptr};
    GtkWidget* btn_open_folder_{nullptr};

    // Speed Limiter tab
    GtkWidget* chk_use_speed_limiter_{nullptr};
    GtkWidget* spin_speed_limit_{nullptr};

    // Options tab checkboxes
    GtkWidget* chk_show_complete_{nullptr};
    GtkWidget* chk_hang_up_{nullptr};
    GtkWidget* chk_turn_off_{nullptr};
    GtkWidget* chk_force_close_{nullptr};

    bool completion_dialog_shown_{false};
    guint timer_id_{0};
};

} // namespace opendm
