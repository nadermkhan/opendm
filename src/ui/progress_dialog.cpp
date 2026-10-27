#include "progress_dialog.hpp"
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
    timer_id_ = g_timeout_add(250, on_tick_update, this);
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
    gtk_window_set_default_size(GTK_WINDOW(window_), 520, 480);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    }

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(main_vbox, 16);
    gtk_widget_set_margin_end(main_vbox, 16);
    gtk_widget_set_margin_top(main_vbox, 16);
    gtk_widget_set_margin_bottom(main_vbox, 16);
    gtk_window_set_child(GTK_WINDOW(window_), main_vbox);

    // Header info: Filename
    label_filename_ = gtk_label_new(("<b>File:</b> " + session_->get_filename()).c_str());
    gtk_label_set_use_markup(GTK_LABEL(label_filename_), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label_filename_), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label_filename_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(main_vbox), label_filename_);

    // Info Grid (IDM-style 2-column info layout)
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_box_append(GTK_BOX(main_vbox), grid);

    auto add_row = [&](int row, const char* title, GtkWidget** out_lbl) {
        GtkWidget* t_lbl = gtk_label_new(title);
        gtk_label_set_xalign(GTK_LABEL(t_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), t_lbl, 0, row, 1, 1);

        *out_lbl = gtk_label_new("-");
        gtk_label_set_xalign(GTK_LABEL(*out_lbl), 0.0f);
        gtk_grid_attach(GTK_GRID(grid), *out_lbl, 1, row, 1, 1);
    };

    add_row(0, "Status:", &label_status_);
    add_row(1, "File size:", &label_file_size_);
    add_row(2, "Downloaded:", &label_downloaded_);
    add_row(3, "Transfer rate:", &label_transfer_rate_);
    add_row(4, "Time left:", &label_time_left_);
    add_row(5, "Resume capability:", &label_resume_cap_);

    // Overall Progress Bar
    main_progress_bar_ = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(main_progress_bar_), TRUE);
    gtk_box_append(GTK_BOX(main_vbox), main_progress_bar_);

    // Chunk Segmentation Area (IDM-style dynamic segment visualization)
    GtkWidget* chunk_frame = gtk_frame_new("Dynamic Chunk Segmentation");
    gtk_box_append(GTK_BOX(main_vbox), chunk_frame);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 150);
    gtk_frame_set_child(GTK_FRAME(chunk_frame), scroll);

    chunk_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(chunk_container_, 8);
    gtk_widget_set_margin_end(chunk_container_, 8);
    gtk_widget_set_margin_top(chunk_container_, 8);
    gtk_widget_set_margin_bottom(chunk_container_, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), chunk_container_);

    // Action Buttons
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(main_vbox), btn_box);

    btn_pause_resume_ = gtk_button_new_with_label("Pause");
    btn_cancel_ = gtk_button_new_with_label("Cancel");
    btn_open_file_ = gtk_button_new_with_label("Open File");
    btn_open_folder_ = gtk_button_new_with_label("Open Folder");

    gtk_widget_set_sensitive(btn_open_file_, FALSE);
    gtk_widget_set_sensitive(btn_open_folder_, FALSE);

    gtk_box_append(GTK_BOX(btn_box), btn_pause_resume_);
    gtk_box_append(GTK_BOX(btn_box), btn_cancel_);
    gtk_box_append(GTK_BOX(btn_box), btn_open_file_);
    gtk_box_append(GTK_BOX(btn_box), btn_open_folder_);

    g_signal_connect_swapped(btn_pause_resume_, "clicked", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_pause_resume_clicked();
    }), this);

    g_signal_connect_swapped(btn_cancel_, "clicked", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_cancel_clicked();
    }), this);

    g_signal_connect_swapped(btn_open_file_, "clicked", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_open_file_clicked();
    }), this);

    g_signal_connect_swapped(btn_open_folder_, "clicked", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_open_folder_clicked();
    }), this);

    g_signal_connect_swapped(window_, "destroy", G_CALLBACK(+[](ProgressDialog* self) {
        if (self->timer_id_ > 0) {
            g_source_remove(self->timer_id_);
            self->timer_id_ = 0;
        }
    }), this);

    update_ui();
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

    // Update Pause / Resume button label
    if (p.status == SessionStatus::PAUSED) {
        gtk_button_set_label(GTK_BUTTON(btn_pause_resume_), "Resume");
    } else if (p.status == SessionStatus::DOWNLOADING || p.status == SessionStatus::PROBING) {
        gtk_button_set_label(GTK_BUTTON(btn_pause_resume_), "Pause");
    } else if (p.status == SessionStatus::COMPLETED) {
        gtk_widget_set_sensitive(btn_pause_resume_, FALSE);
        gtk_widget_set_sensitive(btn_open_file_, TRUE);
        gtk_widget_set_sensitive(btn_open_folder_, TRUE);
    }

    // Dynamic chunk progress bars
    while (chunk_bars_.size() < p.chunks.size()) {
        size_t idx = chunk_bars_.size();
        GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget* lbl = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        GtkWidget* bar = gtk_progress_bar_new();
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), FALSE);

        gtk_box_append(GTK_BOX(row_box), lbl);
        gtk_box_append(GTK_BOX(row_box), bar);
        gtk_box_append(GTK_BOX(chunk_container_), row_box);

        chunk_labels_.push_back(lbl);
        chunk_bars_.push_back(bar);
    }

    for (size_t i = 0; i < p.chunks.size(); ++i) {
        const auto& c = p.chunks[i];
        double fraction = 0.0;
        int64_t total = c.total_size();
        if (total > 0) {
            fraction = std::clamp(static_cast<double>(c.downloaded_bytes) / total, 0.0, 1.0);
        }
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(chunk_bars_[i]), fraction);

        std::string status_sym = (c.status == ChunkStatus::COMPLETED) ? "✓ Done" :
                                 (c.status == ChunkStatus::DOWNLOADING) ? ("↓ " + format_speed(c.current_speed)) :
                                 (c.status == ChunkStatus::FAILED) ? "✗ Failed" : "⏳ Pending";

        std::ostringstream ss_info;
        ss_info << "Chunk #" << c.id << " [" << format_bytes(c.start_offset) << " - "
                << format_bytes(c.end_offset) << "] : " << status_sym << " ("
                << static_cast<int>(fraction * 100) << "%)";
        gtk_label_set_text(GTK_LABEL(chunk_labels_[i]), ss_info.str().c_str());
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

void ProgressDialog::on_open_file_clicked() {
    if (!session_) return;
    std::string path = session_->get_destination_path();
    if (std::filesystem::exists(path)) {
        std::string cmd = "xdg-open \"" + path + "\" &";
        std::system(cmd.c_str());
    }
}

void ProgressDialog::on_open_folder_clicked() {
    if (!session_) return;
    std::string path = session_->get_destination_path();
    std::filesystem::path p(path);
    std::string parent_dir = p.parent_path().string();
    if (parent_dir.empty()) parent_dir = ".";
    std::string cmd = "xdg-open \"" + parent_dir + "\" &";
    std::system(cmd.c_str());
}

} // namespace opendm
