#include "progress_dialog.hpp"
#include "ui_assets.hpp"
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <cstdlib>

namespace opendm {

namespace {

std::string format_bytes(int64_t bytes) {
    if (bytes < 0) return "Unknown";
    constexpr double KB = 1024.0;
    constexpr double MB = 1024.0 * KB;
    constexpr double GB = 1024.0 * MB;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (bytes >= GB) {
        ss << (bytes / GB) << " GB";
    } else if (bytes >= MB) {
        ss << (bytes / MB) << " MB";
    } else if (bytes >= KB) {
        ss << (bytes / KB) << " KB";
    } else {
        ss << bytes << " Bytes";
    }
    return ss.str();
}

std::string format_speed(uint64_t bytes_per_sec) {
    if (bytes_per_sec == 0) return "0.00 KB/s";
    return format_bytes(bytes_per_sec) + "/s";
}

std::string format_eta(int64_t eta_sec) {
    if (eta_sec <= 0) return "--:--";
    int64_t hours = eta_sec / 3600;
    int64_t mins = (eta_sec % 3600) / 60;
    int64_t secs = eta_sec % 60;

    std::ostringstream ss;
    if (hours > 0) {
        ss << hours << "h " << mins << "m " << secs << "s";
    } else if (mins > 0) {
        ss << mins << "m " << secs << "s";
    } else {
        ss << secs << "s";
    }
    return ss.str();
}

std::string status_to_string(SessionStatus status) {
    switch (status) {
        case SessionStatus::IDLE: return "Idle";
        case SessionStatus::PROBING: return "Connecting / Probing...";
        case SessionStatus::DOWNLOADING: return "Downloading";
        case SessionStatus::ASSEMBLING: return "Assembling file parts...";
        case SessionStatus::PAUSED: return "Paused";
        case SessionStatus::COMPLETED: return "Completed";
        case SessionStatus::ERROR: return "Error";
    }
    return "Unknown";
}

} // namespace

ProgressDialog::ProgressDialog(std::shared_ptr<DownloadSession> session, GtkWindow* parent)
    : session_(session), parent_(parent) {
    setup_ui();
    timer_id_ = g_timeout_add(150, on_tick_update, this);
}

ProgressDialog::~ProgressDialog() {
    if (timer_id_ > 0) {
        g_source_remove(timer_id_);
        timer_id_ = 0;
    }
}

void ProgressDialog::setup_ui() {
    window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window_), ("OpenDM - " + session_->get_filename()).c_str());
    gtk_window_set_default_size(GTK_WINDOW(window_), 540, 460);
    gtk_window_set_resizable(GTK_WINDOW(window_), TRUE);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    }

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(main_vbox, 12);
    gtk_widget_set_margin_end(main_vbox, 12);
    gtk_widget_set_margin_top(main_vbox, 12);
    gtk_widget_set_margin_bottom(main_vbox, 12);
    gtk_window_set_child(GTK_WINDOW(window_), main_vbox);

    // Notebook tabs matching authentic IDM Dialog 363 & 364
    GtkWidget* notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_box_append(GTK_BOX(main_vbox), notebook);

    setup_status_tab(notebook);
    setup_options_tab(notebook);

    // Bottom action button row
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 6);

    btn_pause_resume_ = gtk_button_new_with_label("Pause");
    btn_cancel_ = gtk_button_new_with_label("Cancel");
    btn_hide_ = gtk_button_new_with_label("Hide");
    btn_open_file_ = gtk_button_new_with_label("Open");
    btn_open_folder_ = gtk_button_new_with_label("Open Folder");

    gtk_widget_set_sensitive(btn_open_file_, FALSE);
    gtk_widget_set_sensitive(btn_open_folder_, FALSE);

    gtk_box_append(GTK_BOX(btn_box), btn_pause_resume_);
    gtk_box_append(GTK_BOX(btn_box), btn_cancel_);
    gtk_box_append(GTK_BOX(btn_box), btn_hide_);
    gtk_box_append(GTK_BOX(btn_box), btn_open_file_);
    gtk_box_append(GTK_BOX(btn_box), btn_open_folder_);
    gtk_box_append(GTK_BOX(main_vbox), btn_box);

    g_signal_connect_swapped(btn_pause_resume_, "clicked", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_pause_resume_clicked();
    }), this);

    g_signal_connect_swapped(btn_cancel_, "clicked", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_cancel_clicked();
    }), this);

    g_signal_connect_swapped(btn_hide_, "clicked", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_hide_clicked();
    }), this);

    g_signal_connect_swapped(btn_open_file_, "clicked", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_open_file_clicked();
    }), this);

    g_signal_connect_swapped(btn_open_folder_, "clicked", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_open_folder_clicked();
    }), this);

    update_ui();
}

void ProgressDialog::setup_status_tab(GtkWidget* notebook) {
    GtkWidget* page_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(page_vbox, 10);
    gtk_widget_set_margin_end(page_vbox, 10);
    gtk_widget_set_margin_top(page_vbox, 10);
    gtk_widget_set_margin_bottom(page_vbox, 10);

    // Header Info Grid (matching Dialog 363)
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_add_css_class(grid, "idm-progress-header");
    gtk_box_append(GTK_BOX(page_vbox), grid);

    auto add_row = [&](int row, const char* label_txt, GtkWidget** out_lbl) {
        GtkWidget* title = gtk_label_new(label_txt);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_widget_set_hexpand(title, FALSE);

        GtkWidget* val = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(val), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(val), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(val), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(val, TRUE);

        gtk_grid_attach(GTK_GRID(grid), title, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), val, 1, row, 1, 1);
        *out_lbl = val;
    };

    add_row(0, "URL:", &label_url_);
    add_row(1, "Status:", &label_status_);
    add_row(2, "File size:", &label_file_size_);
    add_row(3, "Downloaded:", &label_downloaded_);
    add_row(4, "Transfer rate:", &label_transfer_rate_);
    add_row(5, "Time left:", &label_time_left_);
    add_row(6, "Resume capability:", &label_resume_cap_);

    gtk_label_set_text(GTK_LABEL(label_url_), session_->get_url().c_str());

    // Main Progress Bar
    main_progress_bar_ = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(main_progress_bar_), TRUE);
    gtk_box_append(GTK_BOX(page_vbox), main_progress_bar_);

    // Section header: "Download progress by connection"
    GtkWidget* lbl_conns = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(lbl_conns), "<b>Download progress by connection:</b>");
    gtk_label_set_xalign(GTK_LABEL(lbl_conns), 0.0f);
    gtk_widget_set_margin_top(lbl_conns, 4);
    gtk_box_append(GTK_BOX(page_vbox), lbl_conns);

    // Authentic IDM Cairo-drawn Multi-track Visualizer Canvas
    visualizer_area_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(visualizer_area_, -1, 56);
    gtk_widget_set_hexpand(visualizer_area_, TRUE);
    gtk_widget_add_css_class(visualizer_area_, "idm-visualizer-canvas");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(visualizer_area_), on_draw_visualizer, this, nullptr);
    gtk_box_append(GTK_BOX(page_vbox), visualizer_area_);

    GtkWidget* tab_lbl = gtk_label_new("Download status");
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page_vbox, tab_lbl);
}

void ProgressDialog::setup_options_tab(GtkWidget* notebook) {
    GtkWidget* page_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(page_vbox, 14);
    gtk_widget_set_margin_end(page_vbox, 14);
    gtk_widget_set_margin_top(page_vbox, 14);
    gtk_widget_set_margin_bottom(page_vbox, 14);

    GtkWidget* frame = gtk_frame_new("Options on completion");
    GtkWidget* frame_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(frame_box, 12);
    gtk_widget_set_margin_end(frame_box, 12);
    gtk_widget_set_margin_top(frame_box, 12);
    gtk_widget_set_margin_bottom(frame_box, 12);
    gtk_frame_set_child(GTK_FRAME(frame), frame_box);
    gtk_box_append(GTK_BOX(page_vbox), frame);

    chk_show_complete_ = gtk_check_button_new_with_label("Show \"Download complete\" dialog");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_show_complete_), TRUE);
    gtk_box_append(GTK_BOX(frame_box), chk_show_complete_);

    chk_hang_up_ = gtk_check_button_new_with_label("Hang up modem / disconnect when done");
    gtk_box_append(GTK_BOX(frame_box), chk_hang_up_);

    chk_turn_off_ = gtk_check_button_new_with_label("Turn off computer when done");
    gtk_box_append(GTK_BOX(frame_box), chk_turn_off_);

    chk_force_close_ = gtk_check_button_new_with_label("Force processes to close");
    gtk_box_append(GTK_BOX(frame_box), chk_force_close_);

    GtkWidget* tab_lbl = gtk_label_new("Options on completion");
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page_vbox, tab_lbl);
}

void ProgressDialog::on_draw_visualizer(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer user_data) {
    auto* self = static_cast<ProgressDialog*>(user_data);
    if (!self->session_) return;

    DownloadProgress p = self->session_->get_progress();
    int64_t total = p.total_size > 0 ? p.total_size : 1;

    // 1. Sunken dark background
    cairo_set_source_rgb(cr, 0.11, 0.12, 0.13); // #1C1E21
    cairo_paint(cr);

    if (p.chunks.empty()) {
        // Single stream fallback
        double pct_w = std::max(0.0, std::min<double>(width, width * p.progress_fraction));
        cairo_set_source_rgb(cr, 0.05, 0.35, 0.75); // Dark blue
        cairo_rectangle(cr, 0, 0, pct_w, height);
        cairo_fill(cr);
        return;
    }

    size_t chunk_count = p.chunks.size();
    double row_h = static_cast<double>(height) / chunk_count;

    for (size_t i = 0; i < chunk_count; ++i) {
        const auto& c = p.chunks[i];
        double y = i * row_h;
        double h = std::max(row_h - 1.0, 1.0);

        double x_start = (static_cast<double>(c.start_offset) / total) * width;
        double x_end = (static_cast<double>(c.end_offset) / total) * width;
        double w_allocated = std::max(x_end - x_start, 2.0);

        // Allocated chunk boundary track (Dark Gray)
        cairo_set_source_rgb(cr, 0.22, 0.24, 0.26);
        cairo_rectangle(cr, x_start, y, w_allocated, h);
        cairo_fill(cr);

        // Downloaded bytes (Classic IDM Navy Blue #1565C0)
        double x_downloaded = (static_cast<double>(c.start_offset + c.downloaded_bytes) / total) * width;
        double w_downloaded = std::max(x_downloaded - x_start, 0.0);
        cairo_set_source_rgb(cr, 0.08, 0.39, 0.75);
        cairo_rectangle(cr, x_start, y, w_downloaded, h);
        cairo_fill(cr);

        // Active writing head indicator (Electric Lime Green #00E676)
        if (c.status == ChunkStatus::DOWNLOADING) {
            double head_w = std::min(6.0, std::max(w_downloaded, 3.0));
            cairo_set_source_rgb(cr, 0.0, 0.90, 0.46);
            cairo_rectangle(cr, std::max(x_start, x_downloaded - head_w), y, head_w, h);
            cairo_fill(cr);
        }
    }

    // Outer border
    cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
    cairo_stroke(cr);
}

void ProgressDialog::show() {
    gtk_window_present(GTK_WINDOW(window_));
}

void ProgressDialog::close() {
    gtk_window_destroy(GTK_WINDOW(window_));
}

gboolean ProgressDialog::on_tick_update(gpointer user_data) {
    auto* self = static_cast<ProgressDialog*>(user_data);
    self->update_ui();
    return G_SOURCE_CONTINUE;
}

void ProgressDialog::update_ui() {
    if (!session_) return;

    DownloadProgress p = session_->get_progress();

    // Update window title with percentage
    std::ostringstream ss_title;
    ss_title << std::fixed << std::setprecision(1) << (p.progress_fraction * 100.0) << "% of " << session_->get_filename() << " - OpenDM";
    gtk_window_set_title(GTK_WINDOW(window_), ss_title.str().c_str());

    gtk_label_set_text(GTK_LABEL(label_status_), status_to_string(p.status).c_str());
    gtk_label_set_text(GTK_LABEL(label_file_size_), format_bytes(p.total_size).c_str());
    gtk_label_set_text(GTK_LABEL(label_downloaded_),
        (format_bytes(p.downloaded_bytes) + " (" + std::to_string(static_cast<int>(p.progress_fraction * 100)) + "%)").c_str());
    gtk_label_set_text(GTK_LABEL(label_transfer_rate_), format_speed(p.speed_bytes_per_sec).c_str());
    gtk_label_set_text(GTK_LABEL(label_time_left_), format_eta(p.eta_seconds).c_str());
    gtk_label_set_text(GTK_LABEL(label_resume_cap_), (p.total_size > 0 ? "Yes (Range supported)" : "No (Stream)"));

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(main_progress_bar_), p.progress_fraction);
    std::ostringstream ss_pct;
    ss_pct << std::fixed << std::setprecision(1) << (p.progress_fraction * 100.0) << "%";
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(main_progress_bar_), ss_pct.str().c_str());

    // Redraw chunk visualizer
    if (visualizer_area_) {
        gtk_widget_queue_draw(visualizer_area_);
    }

    // Update buttons
    if (p.status == SessionStatus::PAUSED) {
        gtk_button_set_label(GTK_BUTTON(btn_pause_resume_), "Resume");
    } else if (p.status == SessionStatus::DOWNLOADING || p.status == SessionStatus::PROBING) {
        gtk_button_set_label(GTK_BUTTON(btn_pause_resume_), "Pause");
    } else if (p.status == SessionStatus::COMPLETED) {
        gtk_widget_set_sensitive(btn_pause_resume_, FALSE);
        gtk_widget_set_sensitive(btn_open_file_, TRUE);
        gtk_widget_set_sensitive(btn_open_folder_, TRUE);
    }
}

void ProgressDialog::on_pause_resume_clicked() {
    if (!session_) return;
    if (session_->get_status() == SessionStatus::PAUSED) {
        session_->resume();
    } else {
        session_->pause();
    }
    update_ui();
}

void ProgressDialog::on_cancel_clicked() {
    if (session_) {
        session_->cancel();
    }
    close();
}

void ProgressDialog::on_hide_clicked() {
    gtk_widget_set_visible(window_, FALSE);
}

void ProgressDialog::on_open_file_clicked() {
    if (!session_) return;
    std::string cmd = "xdg-open \"" + session_->get_destination_path() + "\" &";
    (void)system(cmd.c_str());
}

void ProgressDialog::on_open_folder_clicked() {
    if (!session_) return;
    std::filesystem::path p(session_->get_destination_path());
    std::string cmd = "xdg-open \"" + p.parent_path().string() + "\" &";
    (void)system(cmd.c_str());
}

} // namespace opendm
