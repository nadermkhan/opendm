#include "main_window.hpp"
#include "zip_preview_dialog.hpp"
#include "grabber_dialog.hpp"
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <iostream>

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
        case SessionStatus::PROBING: return "Connecting...";
        case SessionStatus::DOWNLOADING: return "Downloading";
        case SessionStatus::ASSEMBLING: return "Assembling...";
        case SessionStatus::PAUSED: return "Paused";
        case SessionStatus::COMPLETED: return "Completed";
        case SessionStatus::ERROR: return "Error";
    }
    return "Unknown";
}

} // namespace

MainWindow::MainWindow(GtkApplication* app) : app_(app) {
    setup_ui();
    refresh_timer_id_ = g_timeout_add(500, on_tick_refresh, this);
}

MainWindow::~MainWindow() {
    if (refresh_timer_id_ > 0) {
        g_source_remove(refresh_timer_id_);
        refresh_timer_id_ = 0;
    }
}

void MainWindow::present() {
    gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::setup_ui() {
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), "OpenDM - High-Speed Download Accelerator");
    gtk_window_set_default_size(GTK_WINDOW(window_), 960, 600);

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window_), main_vbox);

    // Toolbar / HeaderBar
    setup_toolbar(main_vbox);

    // Paned layout: Sidebar on left, Downloads Table on right
    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 200);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(main_vbox), paned);

    setup_sidebar(paned);
    setup_downloads_view(paned);

    // Bottom Status Bar
    setup_statusbar(main_vbox);

    g_signal_connect_swapped(window_, "destroy", G_CALLBACK(+[](MainWindow* self) {
        if (self->refresh_timer_id_ > 0) {
            g_source_remove(self->refresh_timer_id_);
            self->refresh_timer_id_ = 0;
        }
    }), this);
}

void MainWindow::setup_toolbar(GtkWidget* parent_box) {
    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(toolbar, 8);
    gtk_widget_set_margin_end(toolbar, 8);
    gtk_widget_set_margin_top(toolbar, 6);
    gtk_widget_set_margin_bottom(toolbar, 6);
    gtk_box_append(GTK_BOX(parent_box), toolbar);

    auto make_btn = [&](const char* label, const char* tooltip, auto callback) -> GtkWidget* {
        GtkWidget* btn = gtk_button_new_with_label(label);
        gtk_widget_set_tooltip_text(btn, tooltip);
        g_signal_connect_swapped(btn, "clicked", G_CALLBACK(+callback), this);
        gtk_box_append(GTK_BOX(toolbar), btn);
        return btn;
    };

    make_btn("+ Add URL", "Add a new download URL", [](MainWindow* self) { self->on_add_url_clicked(); });
    make_btn("▶ Resume", "Resume selected download", [](MainWindow* self) { self->on_resume_clicked(); });
    make_btn("⏸ Stop", "Pause selected download", [](MainWindow* self) { self->on_stop_clicked(); });
    make_btn("⏹ Stop All", "Pause all active downloads", [](MainWindow* self) { self->on_stop_all_clicked(); });
    make_btn("✖ Delete", "Remove selected download", [](MainWindow* self) { self->on_delete_clicked(); });

    // Separator
    GtkWidget* sep1 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_append(GTK_BOX(toolbar), sep1);

    make_btn("▶▶ Start Queue", "Start queue processing", [](MainWindow* self) { self->on_start_queue_clicked(); });
    make_btn("⏸⏸ Stop Queue", "Stop queue processing", [](MainWindow* self) { self->on_stop_queue_clicked(); });

    // Separator
    GtkWidget* sep2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_append(GTK_BOX(toolbar), sep2);

    speed_limiter_btn_ = gtk_toggle_button_new_with_label("⚡ Speed Limiter");
    gtk_widget_set_tooltip_text(speed_limiter_btn_, "Toggle bandwidth speed limiter");
    g_signal_connect_swapped(speed_limiter_btn_, "toggled", G_CALLBACK(+[](MainWindow* self) {
        self->on_speed_limiter_toggled(GTK_TOGGLE_BUTTON(self->speed_limiter_btn_));
    }), this);
    gtk_box_append(GTK_BOX(toolbar), speed_limiter_btn_);

    make_btn("⏱ Scheduler", "Configure queue scheduling and automated timing", [](MainWindow* self) {
        self->on_scheduler_clicked();
    });

    make_btn("📦 Preview ZIP", "Inspect remote ZIP archive without downloading", [](MainWindow* self) {
        self->on_zip_preview_clicked();
    });

    make_btn("🕸 Site Grabber", "Crawl website and download media assets", [](MainWindow* self) {
        self->on_site_grabber_clicked();
    });
}

void MainWindow::setup_sidebar(GtkWidget* paned) {
    GtkWidget* frame = gtk_frame_new("Categories");
    gtk_widget_set_size_request(frame, 180, -1);
    gtk_paned_set_start_child(GTK_PANED(paned), frame);

    sidebar_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar_list_), GTK_SELECTION_SINGLE);
    gtk_frame_set_child(GTK_FRAME(frame), sidebar_list_);

    const std::vector<std::string> categories = {
        "All Downloads",
        "Compressed",
        "Documents",
        "Music",
        "Programs",
        "Video",
        "Main Queue"
    };

    for (const auto& cat : categories) {
        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* lbl = gtk_label_new(cat.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_set_margin_start(lbl, 12);
        gtk_widget_set_margin_top(lbl, 6);
        gtk_widget_set_margin_bottom(lbl, 6);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        gtk_list_box_append(GTK_LIST_BOX(sidebar_list_), row);
    }

    g_signal_connect_swapped(sidebar_list_, "row-selected", G_CALLBACK(+[](MainWindow* self, GtkListBoxRow* row) {
        if (!row) return;
        GtkWidget* child = gtk_list_box_row_get_child(row);
        if (GTK_IS_LABEL(child)) {
            const char* text = gtk_label_get_text(GTK_LABEL(child));
            self->on_category_selected(text ? text : "All Downloads");
        }
    }), this);
}

void MainWindow::setup_downloads_view(GtkWidget* paned) {
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_paned_set_end_child(GTK_PANED(paned), scroll);

    downloads_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(downloads_container_, 6);
    gtk_widget_set_margin_end(downloads_container_, 6);
    gtk_widget_set_margin_top(downloads_container_, 6);
    gtk_widget_set_margin_bottom(downloads_container_, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), downloads_container_);
}

void MainWindow::setup_statusbar(GtkWidget* parent_box) {
    GtkWidget* status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(status_box, 12);
    gtk_widget_set_margin_end(status_box, 12);
    gtk_widget_set_margin_top(status_box, 4);
    gtk_widget_set_margin_bottom(status_box, 4);
    gtk_box_append(GTK_BOX(parent_box), status_box);

    statusbar_label_ = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(statusbar_label_), 0.0f);
    gtk_box_append(GTK_BOX(status_box), statusbar_label_);
}

gboolean MainWindow::on_tick_refresh(gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    self->refresh_downloads_list();
    return G_SOURCE_CONTINUE;
}

void MainWindow::refresh_downloads_list() {
    auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();

    // Clear existing children in container
    GtkWidget* child = gtk_widget_get_first_child(downloads_container_);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(downloads_container_), child);
        child = next;
    }

    uint64_t total_speed = 0;
    size_t active_count = 0;

    for (const auto& s : sessions) {
        std::string filename = s->get_filename();
        std::string cat = QueueManager::detect_category(filename);

        if (current_category_ != "All Downloads" && current_category_ != "Main Queue") {
            if (cat != current_category_) {
                continue;
            }
        }

        DownloadProgress prog = s->get_progress();
        if (prog.status == SessionStatus::DOWNLOADING || prog.status == SessionStatus::PROBING) {
            total_speed += prog.speed_bytes_per_sec;
            active_count++;
        }

        // Create a structured row for this download
        GtkWidget* card = gtk_frame_new(nullptr);
        GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(row_box, 8);
        gtk_widget_set_margin_end(row_box, 8);
        gtk_widget_set_margin_top(row_box, 6);
        gtk_widget_set_margin_bottom(row_box, 6);
        gtk_frame_set_child(GTK_FRAME(card), row_box);

        // Filename & Category
        GtkWidget* name_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_size_request(name_vbox, 240, -1);
        GtkWidget* name_lbl = gtk_label_new(filename.c_str());
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
        GtkWidget* cat_lbl = gtk_label_new(("[ " + cat + " ]").c_str());
        gtk_label_set_xalign(GTK_LABEL(cat_lbl), 0.0f);

        gtk_box_append(GTK_BOX(name_vbox), name_lbl);
        gtk_box_append(GTK_BOX(name_vbox), cat_lbl);
        gtk_box_append(GTK_BOX(row_box), name_vbox);

        // Size & Status
        GtkWidget* info_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_size_request(info_vbox, 130, -1);
        GtkWidget* size_lbl = gtk_label_new(format_bytes(prog.total_size).c_str());
        gtk_label_set_xalign(GTK_LABEL(size_lbl), 0.0f);
        GtkWidget* st_lbl = gtk_label_new(status_to_string(prog.status).c_str());
        gtk_label_set_xalign(GTK_LABEL(st_lbl), 0.0f);

        gtk_box_append(GTK_BOX(info_vbox), size_lbl);
        gtk_box_append(GTK_BOX(info_vbox), st_lbl);
        gtk_box_append(GTK_BOX(row_box), info_vbox);

        // Progress bar + Speed & ETA
        GtkWidget* prog_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(prog_vbox, TRUE);

        GtkWidget* pbar = gtk_progress_bar_new();
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbar), prog.progress_fraction);
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(pbar), TRUE);
        std::ostringstream ss_pct;
        ss_pct << std::fixed << std::setprecision(1) << (prog.progress_fraction * 100.0) << "%";
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(pbar), ss_pct.str().c_str());

        std::string rate_text = format_speed(prog.speed_bytes_per_sec) + " | ETA: " + format_eta(prog.eta_seconds);
        GtkWidget* rate_lbl = gtk_label_new(rate_text.c_str());
        gtk_label_set_xalign(GTK_LABEL(rate_lbl), 0.0f);

        gtk_box_append(GTK_BOX(prog_vbox), pbar);
        gtk_box_append(GTK_BOX(prog_vbox), rate_lbl);
        gtk_box_append(GTK_BOX(row_box), prog_vbox);

        // Buttons for this row: Details / Open
        GtkWidget* details_btn = gtk_button_new_with_label("Details");
        std::string sid = s->get_id();
        g_object_set_data_full(G_OBJECT(details_btn), "session_id", strdup(sid.c_str()), free);
        g_signal_connect(details_btn, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer user_data) {
            auto* self = static_cast<MainWindow*>(user_data);
            const char* id_str = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "session_id"));
            if (id_str) self->on_row_activated(id_str);
        }), this);
        gtk_box_append(GTK_BOX(row_box), details_btn);

        // Handle selection on click
        g_object_set_data_full(G_OBJECT(card), "session_id", strdup(sid.c_str()), free);
        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* gesture, int n_press, double x, double y, gpointer user_data) {
            auto* self = static_cast<MainWindow*>(user_data);
            GtkWidget* w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
            const char* id_str = static_cast<const char*>(g_object_get_data(G_OBJECT(w), "session_id"));
            if (id_str) {
                self->selected_session_id_ = id_str;
                if (n_press == 2) {
                    self->on_row_activated(id_str);
                }
            }
        }), this);
        gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(click));

        gtk_box_append(GTK_BOX(downloads_container_), card);
    }

    // Update Status Bar
    std::ostringstream ss_stat;
    ss_stat << "Total downloads: " << sessions.size()
            << " | Active: " << active_count
            << " | Total Speed: " << format_speed(total_speed);
    if (QueueManager::instance().is_global_speed_limit_enabled()) {
        ss_stat << " | ⚡ Limit: " << format_bytes(QueueManager::instance().get_global_speed_limit()) << "/s";
    }
    gtk_label_set_text(GTK_LABEL(statusbar_label_), ss_stat.str().c_str());
}

void MainWindow::on_category_selected(const std::string& category) {
    current_category_ = category;
    refresh_downloads_list();
}

void MainWindow::on_row_activated(const std::string& session_id) {
    selected_session_id_ = session_id;
    auto s = QueueManager::instance().find_session(session_id);
    if (!s) return;

    auto it = active_dialogs_.find(session_id);
    if (it != active_dialogs_.end()) {
        it->second->show();
    } else {
        auto dlg = std::make_shared<ProgressDialog>(s, GTK_WINDOW(window_));
        active_dialogs_[session_id] = dlg;
        dlg->show();
    }
}

void MainWindow::on_add_url_clicked() {
    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Add New Download");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 480, 240);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window_));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    // URL entry
    GtkWidget* url_lbl = gtk_label_new("Download Address (URL):");
    gtk_label_set_xalign(GTK_LABEL(url_lbl), 0.0f);
    gtk_box_append(GTK_BOX(vbox), url_lbl);

    GtkWidget* url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url_entry), "https://...");
    gtk_box_append(GTK_BOX(vbox), url_entry);

    // Path entry
    GtkWidget* path_lbl = gtk_label_new("Save Directory:");
    gtk_label_set_xalign(GTK_LABEL(path_lbl), 0.0f);
    gtk_box_append(GTK_BOX(vbox), path_lbl);

    GtkWidget* path_entry = gtk_entry_new();
    std::string default_dl = (std::filesystem::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string();
    gtk_editable_set_text(GTK_EDITABLE(path_entry), default_dl.c_str());
    gtk_box_append(GTK_BOX(vbox), path_entry);

    // Filename entry
    GtkWidget* fn_lbl = gtk_label_new("Save As (optional):");
    gtk_label_set_xalign(GTK_LABEL(fn_lbl), 0.0f);
    gtk_box_append(GTK_BOX(vbox), fn_lbl);

    GtkWidget* fn_entry = gtk_entry_new();
    gtk_box_append(GTK_BOX(vbox), fn_entry);

    // Buttons
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), btn_box);

    GtkWidget* btn_cancel = gtk_button_new_with_label("Cancel");
    GtkWidget* btn_queue = gtk_button_new_with_label("Download Later");
    GtkWidget* btn_start = gtk_button_new_with_label("Start Download");

    gtk_box_append(GTK_BOX(btn_box), btn_cancel);
    gtk_box_append(GTK_BOX(btn_box), btn_queue);
    gtk_box_append(GTK_BOX(btn_box), btn_start);

    g_signal_connect_swapped(btn_cancel, "clicked", G_CALLBACK(+[](GtkWidget* d) {
        gtk_window_destroy(GTK_WINDOW(d));
    }), dialog);

    struct DialogData {
        MainWindow* self;
        GtkWidget* dialog;
        GtkWidget* url_e;
        GtkWidget* path_e;
        GtkWidget* fn_e;
        bool start_now;
    };

    auto* data_start = new DialogData{this, dialog, url_entry, path_entry, fn_entry, true};
    g_signal_connect_swapped(btn_start, "clicked", G_CALLBACK(+[](DialogData* d) {
        const char* u = gtk_editable_get_text(GTK_EDITABLE(d->url_e));
        const char* p = gtk_editable_get_text(GTK_EDITABLE(d->path_e));
        const char* f = gtk_editable_get_text(GTK_EDITABLE(d->fn_e));
        if (u && strlen(u) > 0) {
            d->self->add_download_url(u, p ? p : "", f ? f : "", d->start_now);
        }
        gtk_window_destroy(GTK_WINDOW(d->dialog));
        delete d;
    }), data_start);

    auto* data_queue = new DialogData{this, dialog, url_entry, path_entry, fn_entry, false};
    g_signal_connect_swapped(btn_queue, "clicked", G_CALLBACK(+[](DialogData* d) {
        const char* u = gtk_editable_get_text(GTK_EDITABLE(d->url_e));
        const char* p = gtk_editable_get_text(GTK_EDITABLE(d->path_e));
        const char* f = gtk_editable_get_text(GTK_EDITABLE(d->fn_e));
        if (u && strlen(u) > 0) {
            d->self->add_download_url(u, p ? p : "", f ? f : "", d->start_now);
        }
        gtk_window_destroy(GTK_WINDOW(d->dialog));
        delete d;
    }), data_queue);

    gtk_window_present(GTK_WINDOW(dialog));
}

void MainWindow::add_download_url(const std::string& url, const std::string& path,
                                  const std::string& filename, bool start_now) {
    std::string dest_dir = path;
    if (dest_dir.empty()) {
        dest_dir = (std::filesystem::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string();
    }

    std::string fname = filename;
    if (fname.empty()) {
        size_t slash = url.find_last_of('/');
        if (slash != std::string::npos && slash + 1 < url.length()) {
            fname = url.substr(slash + 1);
        } else {
            fname = "download.bin";
        }
    }

    std::string full_dest = dest_dir + "/" + fname;
    static uint64_t counter = 1;
    std::string id = "task_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);

    auto session = std::make_shared<DownloadSession>(id, url, full_dest);
    QueueManager::instance().get_main_queue().add_session(session, start_now);

    if (start_now) {
        on_row_activated(id);
    }

    refresh_downloads_list();
}

void MainWindow::on_resume_clicked() {
    if (selected_session_id_.empty()) return;
    auto s = QueueManager::instance().find_session(selected_session_id_);
    if (s) {
        s->resume();
        refresh_downloads_list();
    }
}

void MainWindow::on_stop_clicked() {
    if (selected_session_id_.empty()) return;
    auto s = QueueManager::instance().find_session(selected_session_id_);
    if (s) {
        s->pause();
        refresh_downloads_list();
    }
}

void MainWindow::on_stop_all_clicked() {
    auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();
    for (auto& s : sessions) {
        s->pause();
    }
    refresh_downloads_list();
}

void MainWindow::on_delete_clicked() {
    if (selected_session_id_.empty()) return;
    QueueManager::instance().get_main_queue().remove_session(selected_session_id_);
    selected_session_id_.clear();
    refresh_downloads_list();
}

void MainWindow::on_start_queue_clicked() {
    QueueManager::instance().get_main_queue().start_queue();
    refresh_downloads_list();
}

void MainWindow::on_stop_queue_clicked() {
    QueueManager::instance().get_main_queue().stop_queue();
    refresh_downloads_list();
}

void MainWindow::on_speed_limiter_toggled(GtkToggleButton* btn) {
    gboolean active = gtk_toggle_button_get_active(btn);
    QueueManager::instance().enable_global_speed_limit(active);

    if (active && QueueManager::instance().get_global_speed_limit() == 0) {
        // Default to 500 KB/s if not set
        QueueManager::instance().set_global_speed_limit(500 * 1024);
    }
    refresh_downloads_list();
}

void MainWindow::on_scheduler_clicked() {
    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "OpenDM Queue Scheduler");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 380, 260);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window_));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    GtkWidget* check_enable = gtk_check_button_new_with_label("Enable Scheduled Queue");
    gtk_box_append(GTK_BOX(vbox), check_enable);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(vbox), grid);

    GtkWidget* lbl_start = gtk_label_new("Start time (HH:MM):");
    GtkWidget* ent_start = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(ent_start), "02:00");
    gtk_grid_attach(GTK_GRID(grid), lbl_start, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_start, 1, 0, 1, 1);

    GtkWidget* lbl_stop = gtk_label_new("Stop time (HH:MM):");
    GtkWidget* ent_stop = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(ent_stop), "06:00");
    gtk_grid_attach(GTK_GRID(grid), lbl_stop, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_stop, 1, 1, 1, 1);

    GtkWidget* check_shutdown = gtk_check_button_new_with_label("Turn off computer when done");
    gtk_box_append(GTK_BOX(vbox), check_shutdown);

    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    GtkWidget* btn_save = gtk_button_new_with_label("Apply");
    gtk_box_append(GTK_BOX(btn_box), btn_save);
    gtk_box_append(GTK_BOX(vbox), btn_box);

    g_signal_connect_swapped(btn_save, "clicked", G_CALLBACK(+[](GtkWidget* d) {
        gtk_window_destroy(GTK_WINDOW(d));
    }), dialog);

    gtk_window_present(GTK_WINDOW(dialog));
}

void MainWindow::on_zip_preview_clicked() {
    auto dlg = std::make_shared<ZipPreviewDialog>(GTK_WINDOW(window_));
    dlg->show();
}

void MainWindow::on_site_grabber_clicked() {
    auto dlg = std::make_shared<GrabberDialog>(GTK_WINDOW(window_));
    dlg->show();
}

} // namespace opendm
