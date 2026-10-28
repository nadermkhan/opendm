#include "progress_dialog.hpp"
#include "download_complete_dialog.hpp"
#include "ui_assets.hpp"
#include "../core/post_download.hpp"
#include <gio/gio.h>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <cmath>

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
    if (bytes_per_sec == 0) return "0.00 KB/sec";
    return format_bytes(bytes_per_sec) + "/sec";
}

std::string format_eta(int64_t eta_sec) {
    if (eta_sec <= 0) return "--:--";
    int64_t hours = eta_sec / 3600;
    int64_t mins = (eta_sec % 3600) / 60;
    int64_t secs = eta_sec % 60;

    std::ostringstream ss;
    if (hours > 0) {
        ss << hours << " hr " << mins << " min";
    } else if (mins > 0) {
        ss << mins << " min " << secs << " sec";
    } else {
        ss << secs << " sec";
    }
    return ss.str();
}

std::string status_to_string(SessionStatus status) {
    switch (status) {
        case SessionStatus::IDLE: return "Idle";
        case SessionStatus::PROBING: return "Connecting / Probing...";
        case SessionStatus::DOWNLOADING: return "Receiving data...";
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
    gtk_window_set_default_size(GTK_WINDOW(window_), 520, 510);
    gtk_window_set_resizable(GTK_WINDOW(window_), TRUE);
    gtk_widget_add_css_class(window_, "idm-progress-window");
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    }

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(main_vbox, 8);
    gtk_widget_set_margin_end(main_vbox, 8);
    gtk_widget_set_margin_top(main_vbox, 8);
    gtk_widget_set_margin_bottom(main_vbox, 8);
    gtk_window_set_child(GTK_WINDOW(window_), main_vbox);

    // 3 Tabs matching authentic IDM: Download status, Speed Limiter, Options on completion
    GtkWidget* notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_box_append(GTK_BOX(main_vbox), notebook);

    setup_status_tab(notebook);
    setup_speed_limiter_tab(notebook);
    setup_options_tab(notebook);

    g_signal_connect(window_, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
        auto* self = static_cast<ProgressDialog*>(data);
        self->close();
        return FALSE;
    }), this);

    update_ui();
}

void ProgressDialog::setup_status_tab(GtkWidget* notebook) {
    GtkWidget* page_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(page_vbox, 10);
    gtk_widget_set_margin_end(page_vbox, 10);
    gtk_widget_set_margin_top(page_vbox, 8);
    gtk_widget_set_margin_bottom(page_vbox, 8);

    // 1. Metadata Inset Frame (matching IDM Dialog 363)
    GtkWidget* meta_frame = gtk_frame_new(nullptr);
    gtk_widget_add_css_class(meta_frame, "idm-metadata-frame");

    GtkWidget* meta_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(meta_box, 12);
    gtk_widget_set_margin_end(meta_box, 12);
    gtk_widget_set_margin_top(meta_box, 8);
    gtk_widget_set_margin_bottom(meta_box, 8);
    gtk_frame_set_child(GTK_FRAME(meta_frame), meta_box);
    gtk_box_append(GTK_BOX(page_vbox), meta_frame);

    // Full URL row
    label_url_ = gtk_label_new(session_->get_url().c_str());
    gtk_label_set_xalign(GTK_LABEL(label_url_), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(label_url_), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label_url_), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(label_url_, "idm-url-label");
    gtk_box_append(GTK_BOX(meta_box), label_url_);

    // 2-column info grid
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_widget_add_css_class(grid, "idm-meta-grid");
    gtk_box_append(GTK_BOX(meta_box), grid);

    auto add_row = [&](int row, const char* label_txt, GtkWidget** out_lbl, bool is_status = false) {
        GtkWidget* title = gtk_label_new(label_txt);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_widget_add_css_class(title, "idm-meta-title");
        gtk_widget_set_hexpand(title, FALSE);

        GtkWidget* val = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(val), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(val), TRUE);
        gtk_widget_set_hexpand(val, TRUE);
        gtk_widget_add_css_class(val, is_status ? "idm-status-blue" : "idm-meta-val");

        gtk_grid_attach(GTK_GRID(grid), title, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), val, 1, row, 1, 1);
        *out_lbl = val;
    };

    add_row(0, "Status", &label_status_, true);
    add_row(1, "File size", &label_file_size_);
    add_row(2, "Downloaded", &label_downloaded_);
    add_row(3, "Transfer rate", &label_transfer_rate_);
    add_row(4, "Time left", &label_time_left_);
    add_row(5, "Resume capability", &label_resume_cap_);

    // 2. Main Progress Bar (thick authentic IDM green progress bar)
    main_progress_bar_ = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(main_progress_bar_), FALSE);
    gtk_widget_set_size_request(main_progress_bar_, -1, 16);
    gtk_widget_add_css_class(main_progress_bar_, "idm-main-progress-bar");
    gtk_box_append(GTK_BOX(page_vbox), main_progress_bar_);

    // 3. Middle Action Buttons Row (Left: << Hide details, Right: Pause / Cancel / Open / Close)
    GtkWidget* btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(btn_row, 2);
    gtk_widget_set_margin_bottom(btn_row, 2);

    btn_toggle_details_ = gtk_button_new_with_label("<< Hide details");
    gtk_widget_add_css_class(btn_toggle_details_, "idm-dialog-btn");
    gtk_widget_add_css_class(btn_toggle_details_, "idm-btn-details");
    gtk_box_append(GTK_BOX(btn_row), btn_toggle_details_);

    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(btn_row), spacer);

    btn_pause_resume_ = gtk_button_new_with_label("Pause");
    btn_cancel_ = gtk_button_new_with_label("Cancel");
    btn_open_file_ = gtk_button_new_with_label("Open");
    btn_open_folder_ = gtk_button_new_with_label("Open Folder");

    gtk_widget_add_css_class(btn_pause_resume_, "idm-dialog-btn");
    gtk_widget_add_css_class(btn_cancel_, "idm-dialog-btn");
    gtk_widget_add_css_class(btn_open_file_, "idm-dialog-btn");
    gtk_widget_add_css_class(btn_open_folder_, "idm-dialog-btn");

    gtk_widget_set_visible(btn_open_file_, FALSE);
    gtk_widget_set_visible(btn_open_folder_, FALSE);

    gtk_box_append(GTK_BOX(btn_row), btn_pause_resume_);
    gtk_box_append(GTK_BOX(btn_row), btn_cancel_);
    gtk_box_append(GTK_BOX(btn_row), btn_open_file_);
    gtk_box_append(GTK_BOX(btn_row), btn_open_folder_);
    gtk_box_append(GTK_BOX(page_vbox), btn_row);

    g_signal_connect_swapped(btn_toggle_details_, "clicked", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_toggle_details_clicked();
    }), this);

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

    // 4. Details Section (Start positions & download progress by connections)
    box_details_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(page_vbox), box_details_);

    GtkWidget* lbl_conns_title = gtk_label_new("Start positions and download progress by connections");
    gtk_label_set_xalign(GTK_LABEL(lbl_conns_title), 0.5f);
    gtk_widget_add_css_class(lbl_conns_title, "idm-conns-title");
    gtk_box_append(GTK_BOX(box_details_), lbl_conns_title);

    // Authentic IDM Multi-connection Segmented Visualizer Bar
    visualizer_area_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(visualizer_area_, -1, 18);
    gtk_widget_set_hexpand(visualizer_area_, TRUE);
    gtk_widget_add_css_class(visualizer_area_, "idm-visualizer-canvas");
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(visualizer_area_), on_draw_visualizer, this, nullptr);
    gtk_box_append(GTK_BOX(box_details_), visualizer_area_);

    // Connection List Table
    GtkWidget* table_frame = gtk_frame_new(nullptr);
    gtk_widget_add_css_class(table_frame, "idm-table-border-frame");
    gtk_box_append(GTK_BOX(box_details_), table_frame);

    GtkWidget* table_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_frame_set_child(GTK_FRAME(table_frame), table_vbox);

    // Table Header Row
    GtkWidget* hdr_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(hdr_box, "idm-conns-header");
    gtk_box_append(GTK_BOX(table_vbox), hdr_box);

    GtkWidget* h_num = gtk_label_new("N.");
    gtk_label_set_xalign(GTK_LABEL(h_num), 0.0f);
    gtk_widget_set_size_request(h_num, 45, -1);
    gtk_widget_set_margin_start(h_num, 8);
    gtk_widget_add_css_class(h_num, "idm-conns-hcell");
    gtk_box_append(GTK_BOX(hdr_box), h_num);

    GtkWidget* h_down = gtk_label_new("Downloaded");
    gtk_label_set_xalign(GTK_LABEL(h_down), 0.0f);
    gtk_widget_set_size_request(h_down, 140, -1);
    gtk_widget_add_css_class(h_down, "idm-conns-hcell");
    gtk_box_append(GTK_BOX(hdr_box), h_down);

    GtkWidget* h_info = gtk_label_new("Info");
    gtk_label_set_xalign(GTK_LABEL(h_info), 0.0f);
    gtk_widget_set_hexpand(h_info, TRUE);
    gtk_widget_add_css_class(h_info, "idm-conns-hcell");
    gtk_box_append(GTK_BOX(hdr_box), h_info);

    // Scrolled Connection Rows List
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_widget_set_size_request(scrolled, -1, 135);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_add_css_class(scrolled, "idm-conns-scrolled");
    gtk_box_append(GTK_BOX(table_vbox), scrolled);

    conns_list_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(conns_list_box_, "idm-conns-table");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), conns_list_box_);

    GtkWidget* tab_lbl = gtk_label_new("Download status");
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page_vbox, tab_lbl);
}

void ProgressDialog::setup_speed_limiter_tab(GtkWidget* notebook) {
    GtkWidget* page_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(page_vbox, 14);
    gtk_widget_set_margin_end(page_vbox, 14);
    gtk_widget_set_margin_top(page_vbox, 14);
    gtk_widget_set_margin_bottom(page_vbox, 14);

    GtkWidget* frame = gtk_frame_new("Speed Limiter");
    gtk_widget_add_css_class(frame, "idm-options-frame");
    GtkWidget* frame_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(frame_box, 14);
    gtk_widget_set_margin_end(frame_box, 14);
    gtk_widget_set_margin_top(frame_box, 14);
    gtk_widget_set_margin_bottom(frame_box, 14);
    gtk_frame_set_child(GTK_FRAME(frame), frame_box);
    gtk_box_append(GTK_BOX(page_vbox), frame);

    chk_use_speed_limiter_ = gtk_check_button_new_with_label("Use Speed Limiter");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_use_speed_limiter_), session_->is_speed_limit_enabled());
    gtk_box_append(GTK_BOX(frame_box), chk_use_speed_limiter_);

    GtkWidget* spin_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* lbl_spin = gtk_label_new("Maximum download speed:");
    gtk_box_append(GTK_BOX(spin_row), lbl_spin);

    uint64_t cur_limit = session_->get_speed_limit();
    double init_kb = cur_limit > 0 ? static_cast<double>(cur_limit / 1024) : 500.0;
    spin_speed_limit_ = gtk_spin_button_new_with_range(10.0, 100000.0, 50.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_speed_limit_), init_kb);
    gtk_box_append(GTK_BOX(spin_row), spin_speed_limit_);

    GtkWidget* lbl_unit = gtk_label_new("KBytes/sec");
    gtk_box_append(GTK_BOX(spin_row), lbl_unit);
    gtk_box_append(GTK_BOX(frame_box), spin_row);

    g_signal_connect_swapped(chk_use_speed_limiter_, "toggled", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_speed_limiter_toggled();
    }), this);

    g_signal_connect_swapped(spin_speed_limit_, "value-changed", G_CALLBACK(+[](ProgressDialog* self) {
        self->on_speed_limit_changed();
    }), this);

    GtkWidget* tab_lbl = gtk_label_new("Speed Limiter");
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page_vbox, tab_lbl);
}

void ProgressDialog::setup_options_tab(GtkWidget* notebook) {
    GtkWidget* page_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(page_vbox, 14);
    gtk_widget_set_margin_end(page_vbox, 14);
    gtk_widget_set_margin_top(page_vbox, 14);
    gtk_widget_set_margin_bottom(page_vbox, 14);

    GtkWidget* frame = gtk_frame_new("Options on completion");
    gtk_widget_add_css_class(frame, "idm-options-frame");
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

void ProgressDialog::on_draw_visualizer(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer user_data) {
    auto* self = static_cast<ProgressDialog*>(user_data);
    if (!self || !self->session_) return;

    DownloadProgress p = self->session_->get_progress();
    int64_t total = p.total_size > 0 ? p.total_size : 1;

    // 1. Recessed groove background matching IDM
    cairo_set_source_rgb(cr, 0.94, 0.95, 0.96); // #F0F2F5 (Silver/light recessed background)
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    // Beveled sunken borders
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, 0.50, 0.53, 0.56);
    cairo_move_to(cr, 0.5, height - 0.5);
    cairo_line_to(cr, 0.5, 0.5);
    cairo_line_to(cr, width - 0.5, 0.5);
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 0.85, 0.87, 0.90);
    cairo_move_to(cr, width - 0.5, 0.5);
    cairo_line_to(cr, width - 0.5, height - 0.5);
    cairo_line_to(cr, 0.5, height - 0.5);
    cairo_stroke(cr);

    if (p.chunks.empty()) {
        // Single stream fallback
        double pct_w = std::max(0.0, std::min<double>(width - 2.0, (width - 2.0) * p.progress_fraction));
        if (pct_w > 0.0) {
            cairo_pattern_t* pat = cairo_pattern_create_linear(0, 1, 0, height - 1);
            cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.35, 0.68, 0.95);
            cairo_pattern_add_color_stop_rgb(pat, 0.4, 0.12, 0.45, 0.82);
            cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.06, 0.28, 0.65);
            cairo_set_source(cr, pat);
            cairo_rectangle(cr, 1, 1, pct_w, height - 2);
            cairo_fill(cr);
            cairo_pattern_destroy(pat);
        }
        return;
    }

    double content_w = width - 2.0;
    double bar_h = height - 2.0;

    // Pass 1: Draw all glossy cylindrical blue download blocks on the single continuous bar
    for (const auto& c : p.chunks) {
        if (c.downloaded_bytes <= 0) continue;

        double x_start = 1.0 + (static_cast<double>(c.start_offset) / total) * content_w;
        double x_downloaded = 1.0 + (static_cast<double>(c.start_offset + c.downloaded_bytes) / total) * content_w;
        double seg_w = std::max(1.0, x_downloaded - x_start);

        // Cylindrical glossy blue gradient matching IDM
        cairo_pattern_t* pat = cairo_pattern_create_linear(0, 1, 0, bar_h + 1);
        cairo_pattern_add_color_stop_rgb(pat, 0.00, 0.38, 0.70, 0.96); // Bright cyan-blue sheen
        cairo_pattern_add_color_stop_rgb(pat, 0.35, 0.14, 0.48, 0.86); // Vibrant rich blue
        cairo_pattern_add_color_stop_rgb(pat, 0.75, 0.08, 0.38, 0.76); // Solid royal blue
        cairo_pattern_add_color_stop_rgb(pat, 1.00, 0.05, 0.25, 0.60); // Dark bottom bevel
        cairo_set_source(cr, pat);
        cairo_rectangle(cr, x_start, 1, seg_w, bar_h);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
    }

    // Pass 2: Draw thin red start position lines on top for each connection
    for (const auto& c : p.chunks) {
        double x_start = 1.0 + (static_cast<double>(c.start_offset) / total) * content_w;

        // Vivid red line marking the connection's start position
        cairo_set_source_rgb(cr, 0.88, 0.08, 0.08); // Vivid Red #E01414
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, std::floor(x_start) + 0.5, 1);
        cairo_line_to(cr, std::floor(x_start) + 0.5, height - 1);
        cairo_stroke(cr);
    }
}

void ProgressDialog::update_connection_table(const std::vector<Chunk>& chunks, SessionStatus status) {
    if (!conns_list_box_) return;

    if (chunks.size() != conn_rows_.size()) {
        // Clear and rebuild connection row widgets
        GtkWidget* child = gtk_widget_get_first_child(conns_list_box_);
        while (child) {
            GtkWidget* next = gtk_widget_get_next_sibling(child);
            gtk_box_remove(GTK_BOX(conns_list_box_), child);
            child = next;
        }
        conn_rows_.clear();

        for (size_t i = 0; i < chunks.size(); ++i) {
            GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(row_box, "idm-conns-row");
            if (i % 2 == 1) {
                gtk_widget_add_css_class(row_box, "even");
            }

            GtkWidget* lbl_num = gtk_label_new(std::to_string(i + 1).c_str());
            gtk_label_set_xalign(GTK_LABEL(lbl_num), 0.0f);
            gtk_widget_set_size_request(lbl_num, 45, -1);
            gtk_widget_set_margin_start(lbl_num, 8);
            gtk_widget_add_css_class(lbl_num, "idm-conns-cell");
            gtk_box_append(GTK_BOX(row_box), lbl_num);

            GtkWidget* lbl_down = gtk_label_new(format_bytes(chunks[i].downloaded_bytes).c_str());
            gtk_label_set_xalign(GTK_LABEL(lbl_down), 0.0f);
            gtk_widget_set_size_request(lbl_down, 140, -1);
            gtk_widget_add_css_class(lbl_down, "idm-conns-cell");
            gtk_box_append(GTK_BOX(row_box), lbl_down);

            std::string info_text = "Receiving data...";
            if (chunks[i].status == ChunkStatus::COMPLETED) {
                info_text = "Finished";
            } else if (chunks[i].downloaded_bytes == 0) {
                info_text = "Send GET...";
            }

            GtkWidget* lbl_info = gtk_label_new(info_text.c_str());
            gtk_label_set_xalign(GTK_LABEL(lbl_info), 0.0f);
            gtk_widget_set_hexpand(lbl_info, TRUE);
            gtk_widget_add_css_class(lbl_info, "idm-conns-cell");
            gtk_box_append(GTK_BOX(row_box), lbl_info);

            gtk_box_append(GTK_BOX(conns_list_box_), row_box);
            conn_rows_.push_back({row_box, lbl_num, lbl_down, lbl_info});
        }
    } else {
        // Fast in-place label update
        for (size_t i = 0; i < chunks.size(); ++i) {
            gtk_label_set_text(GTK_LABEL(conn_rows_[i].lbl_downloaded), format_bytes(chunks[i].downloaded_bytes).c_str());

            std::string info_text = "Receiving data...";
            if (status == SessionStatus::PAUSED) {
                info_text = "Paused";
            } else if (status == SessionStatus::COMPLETED || chunks[i].status == ChunkStatus::COMPLETED) {
                info_text = "Finished";
            } else if (chunks[i].status == ChunkStatus::DOWNLOADING) {
                info_text = (chunks[i].downloaded_bytes == 0) ? "Send GET..." : "Receiving data...";
            } else if (chunks[i].status == ChunkStatus::PENDING) {
                info_text = "Connecting...";
            } else if (chunks[i].status == ChunkStatus::FAILED) {
                info_text = "Error";
            }
            gtk_label_set_text(GTK_LABEL(conn_rows_[i].lbl_info), info_text.c_str());
        }
    }
}

void ProgressDialog::show() {
    gtk_window_present(GTK_WINDOW(window_));
}

void ProgressDialog::close() {
    if (timer_id_ > 0) {
        g_source_remove(timer_id_);
        timer_id_ = 0;
    }
    if (window_) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

bool ProgressDialog::save_snapshot(const std::string& filename) {
    if (!window_) return false;

    GtkWidget* target = gtk_window_get_child(GTK_WINDOW(window_));
    if (!target) target = window_;

    int w = gtk_widget_get_width(target);
    int h = gtk_widget_get_height(target);
    if (w <= 0) w = 520;
    if (h <= 0) h = 460;

    GdkPaintable* paintable = gtk_widget_paintable_new(target);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), static_cast<double>(w), static_cast<double>(h));
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);

    bool ok = false;
    if (node) {
        GtkNative* native = gtk_widget_get_native(target);
        GskRenderer* renderer = native ? gtk_native_get_renderer(native) : nullptr;
        if (renderer && gsk_renderer_is_realized(renderer)) {
            graphene_rect_t bounds;
            graphene_rect_init(&bounds, 0.0f, 0.0f, (float)w, (float)h);
            GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &bounds);
            if (texture) {
                ok = gdk_texture_save_to_png(texture, filename.c_str());
                g_object_unref(texture);
            }
        }
        gsk_render_node_unref(node);
    }
    g_object_unref(paintable);
    return ok;
}

gboolean ProgressDialog::on_tick_update(gpointer user_data) {
    auto* self = static_cast<ProgressDialog*>(user_data);
    if (!self || !self->window_ || !GTK_IS_WIDGET(self->window_)) {
        return G_SOURCE_REMOVE;
    }
    self->update_ui();
    return G_SOURCE_CONTINUE;
}

void ProgressDialog::update_ui() {
    if (!session_ || !window_ || !GTK_IS_WIDGET(window_)) return;

    DownloadProgress p = session_->get_progress();

    // Window title: Percentage + filename matching IDM
    std::ostringstream ss_title;
    ss_title << std::fixed << std::setprecision(1) << (p.progress_fraction * 100.0) << "% " << session_->get_filename();
    gtk_window_set_title(GTK_WINDOW(window_), ss_title.str().c_str());

    // Status line: classic IDM format
    gtk_label_set_text(GTK_LABEL(label_status_), status_to_string(p.status).c_str());
    gtk_label_set_text(GTK_LABEL(label_file_size_), format_bytes(p.total_size).c_str());

    // Downloaded line matching IDM: "4,360 MB ( 24,57 % )"
    std::ostringstream ss_down;
    ss_down << format_bytes(p.downloaded_bytes) << " ( " 
            << std::fixed << std::setprecision(2) << (p.progress_fraction * 100.0) << " % )";
    gtk_label_set_text(GTK_LABEL(label_downloaded_), ss_down.str().c_str());

    gtk_label_set_text(GTK_LABEL(label_transfer_rate_), format_speed(p.speed_bytes_per_sec).c_str());
    gtk_label_set_text(GTK_LABEL(label_time_left_), format_eta(p.eta_seconds).c_str());
    gtk_label_set_text(GTK_LABEL(label_resume_cap_), (p.total_size > 0 ? "Yes" : "No"));

    // Main green progress bar
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(main_progress_bar_), p.progress_fraction);

    // Queue redraw of connection visualizer bar
    if (visualizer_area_) {
        gtk_widget_queue_draw(visualizer_area_);
    }

    // Update connection table
    update_connection_table(p.chunks, p.status);

    // Dynamic button state management matching IDM
    if (p.status == SessionStatus::COMPLETED) {
        gtk_widget_set_visible(btn_pause_resume_, FALSE);
        gtk_widget_set_visible(btn_cancel_, TRUE);
        gtk_button_set_label(GTK_BUTTON(btn_cancel_), "Close");
        gtk_widget_set_visible(btn_open_file_, TRUE);
        gtk_widget_set_visible(btn_open_folder_, TRUE);

        if (timer_id_ > 0) {
            g_source_remove(timer_id_);
            timer_id_ = 0;
        }

        if (!completion_dialog_shown_) {
            completion_dialog_shown_ = true;
            if (chk_show_complete_ && gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_show_complete_))) {
                DownloadCompleteDialog::show(parent_ ? parent_ : GTK_WINDOW(window_), session_);
            }
        }

        if (chk_turn_off_ && gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_turn_off_))) {
            PostDownloadManager::instance().execute_completion_action(CompletionAction::SHUTDOWN_SYSTEM);
        }
    } else {
        gtk_widget_set_visible(btn_open_file_, FALSE);
        gtk_widget_set_visible(btn_open_folder_, FALSE);
        gtk_widget_set_visible(btn_pause_resume_, TRUE);
        gtk_widget_set_visible(btn_cancel_, TRUE);
        gtk_button_set_label(GTK_BUTTON(btn_cancel_), "Cancel");

        if (p.status == SessionStatus::PAUSED) {
            gtk_button_set_label(GTK_BUTTON(btn_pause_resume_), "Resume");
        } else {
            gtk_button_set_label(GTK_BUTTON(btn_pause_resume_), "Pause");
        }
    }
}

void ProgressDialog::on_toggle_details_clicked() {
    details_expanded_ = !details_expanded_;
    gtk_widget_set_visible(box_details_, details_expanded_ ? TRUE : FALSE);
    gtk_button_set_label(GTK_BUTTON(btn_toggle_details_), details_expanded_ ? "<< Hide details" : "Details >>");

    if (window_) {
        if (details_expanded_) {
            gtk_window_set_default_size(GTK_WINDOW(window_), 520, 510);
        } else {
            gtk_window_set_default_size(GTK_WINDOW(window_), 520, 260);
        }
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
    if (session_ && session_->get_status() != SessionStatus::COMPLETED) {
        session_->pause();
    }
    close();
}

void ProgressDialog::on_open_file_clicked() {
    if (!session_) return;
    std::string path = session_->get_destination_path();
    std::string uri = "file://" + path;
    g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
}

void ProgressDialog::on_open_folder_clicked() {
    if (!session_) return;
    std::filesystem::path p(session_->get_destination_path());
    std::string uri = "file://" + p.parent_path().string();
    g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
}

void ProgressDialog::on_speed_limiter_toggled() {
    if (!session_ || !chk_use_speed_limiter_) return;
    bool active = gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_use_speed_limiter_));
    session_->enable_speed_limit(active);
}

void ProgressDialog::on_speed_limit_changed() {
    if (!session_ || !spin_speed_limit_) return;
    int kb = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_speed_limit_));
    session_->set_speed_limit(static_cast<uint64_t>(kb) * 1024);
}

} // namespace opendm
