#include "main_window.hpp"
#include "progress_dialog.hpp"
#include "zip_preview_dialog.hpp"
#include "grabber_dialog.hpp"
#include "add_url_dialog.hpp"
#include "ui_assets.hpp"
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

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

const char* get_file_icon_name(const std::string& filename) {
    std::string ext = "";
    size_t dot = filename.find_last_of('.');
    if (dot != std::string::npos) {
        ext = filename.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    if (ext == "zip" || ext == "rar" || ext == "7z" || ext == "tar" || ext == "gz" || ext == "iso")
        return "cat_compressed_16.png";
    if (ext == "mp4" || ext == "mkv" || ext == "avi" || ext == "mov" || ext == "flv" || ext == "webm")
        return "cat_video_16.png";
    if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "aac" || ext == "m4a" || ext == "ogg")
        return "cat_music_16.png";
    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "txt" || ext == "epub" || ext == "xls")
        return "cat_documents_16.png";
    if (ext == "exe" || ext == "deb" || ext == "rpm" || ext == "AppImage" || ext == "bin" || ext == "sh")
        return "cat_programs_16.png";

    return "cat_all_16.png";
}

} // namespace

MainWindow::MainWindow(GtkApplication* app) : app_(app) {
    UiAssets::apply_theme();
    setup_ui();
    refresh_timer_id_ = g_timeout_add(400, on_tick_refresh, this);
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
    gtk_window_set_title(GTK_WINDOW(window_), "Internet Download Manager 6.43 (OpenDM)");
    gtk_window_set_default_size(GTK_WINDOW(window_), 980, 600);

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window_), main_vbox);

    setup_menubar(main_vbox);
    setup_toolbar(main_vbox);

    // Paned view: Sidebar (Left) + Downloads (Right)
    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 180);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(main_vbox), paned);

    setup_sidebar(paned);
    setup_downloads_view(paned);
    setup_statusbar(main_vbox);

    refresh_downloads_list();
}

void MainWindow::setup_menubar(GtkWidget* parent_box) {
    GMenu* menubar_model = g_menu_new();

    // 1. Tasks Menu
    GMenu* tasks_menu = g_menu_new();
    g_menu_append(tasks_menu, "Add new download (Ctrl+N)", "win.add_url");
    g_menu_append(tasks_menu, "Add batch download (.ef2)...", "win.batch_import");
    g_menu_append(tasks_menu, "Run site grabber...", "win.site_grabber");
    g_menu_append(tasks_menu, "Exit", "app.quit");
    g_menu_append_submenu(menubar_model, "Tasks", G_MENU_MODEL(tasks_menu));

    // 2. File Menu
    GMenu* file_menu = g_menu_new();
    g_menu_append(file_menu, "Resume Download", "win.resume");
    g_menu_append(file_menu, "Stop Download", "win.stop");
    g_menu_append(file_menu, "Remove Selected", "win.delete");
    g_menu_append_submenu(menubar_model, "File", G_MENU_MODEL(file_menu));

    // 3. Downloads Menu
    GMenu* dl_menu = g_menu_new();
    g_menu_append(dl_menu, "Pause All", "win.stop_all");
    g_menu_append(dl_menu, "Delete All Completed", "win.delete_completed");
    g_menu_append(dl_menu, "Scheduler...", "win.scheduler");
    g_menu_append(dl_menu, "Start Queue", "win.start_queue");
    g_menu_append(dl_menu, "Stop Queue", "win.stop_queue");
    g_menu_append(dl_menu, "Speed Limiter", "win.speed_limiter");
    g_menu_append_submenu(menubar_model, "Downloads", G_MENU_MODEL(dl_menu));

    // 4. View Menu
    GMenu* view_menu = g_menu_new();
    g_menu_append(view_menu, "Preview Remote ZIP...", "win.zip_preview");
    g_menu_append_submenu(menubar_model, "View", G_MENU_MODEL(view_menu));

    // 5. Help Menu
    GMenu* help_menu = g_menu_new();
    g_menu_append(help_menu, "About OpenDM", "win.about");
    g_menu_append_submenu(menubar_model, "Help", G_MENU_MODEL(help_menu));

    GtkWidget* menubar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(menubar_model));
    gtk_box_append(GTK_BOX(parent_box), menubar);

    // Register actions on window
    auto add_win_action = [&](const char* name, void (*cb)(GSimpleAction*, GVariant*, gpointer)) {
        GSimpleAction* act = g_simple_action_new(name, nullptr);
        g_signal_connect(act, "activate", G_CALLBACK(cb), this);
        g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(act));
    };

    add_win_action("add_url", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_add_url_clicked(); });
    add_win_action("resume", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_resume_clicked(); });
    add_win_action("stop", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_clicked(); });
    add_win_action("stop_all", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_all_clicked(); });
    add_win_action("delete", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_delete_clicked(); });
    add_win_action("delete_completed", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_delete_completed_clicked(); });
    add_win_action("start_queue", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_start_queue_clicked(); });
    add_win_action("stop_queue", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_queue_clicked(); });
    add_win_action("speed_limiter", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_speed_limiter_clicked(); });
    add_win_action("scheduler", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_scheduler_clicked(); });
    add_win_action("zip_preview", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_zip_preview_clicked(); });
    add_win_action("site_grabber", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_site_grabber_clicked(); });
    add_win_action("about", +[](GSimpleAction*, GVariant*, gpointer) {
        std::cout << "OpenDM - IDM Architecture Reimplementation v1.0.0" << std::endl;
    });
}

void MainWindow::setup_toolbar(GtkWidget* parent_box) {
    GtkWidget* tb_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(tb_box, "idm-toolbar-box");
    gtk_box_append(GTK_BOX(parent_box), tb_box);

    auto add_sep = [&]() {
        GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
        gtk_widget_add_css_class(sep, "idm-toolbar-separator");
        gtk_box_append(GTK_BOX(tb_box), sep);
    };

    // Group 1: Add URL
    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_add_url.png", "Add URL", "Add new download address",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_add_url_clicked(); }), this));

    add_sep();

    // Group 2: Resume, Stop, Stop All
    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_resume.png", "Resume", "Resume selected download",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_resume_clicked(); }), this));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_stop.png", "Stop", "Stop selected download",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_clicked(); }), this));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_stop_all.png", "Stop All", "Stop all active downloads",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_all_clicked(); }), this));

    add_sep();

    // Group 3: Delete, Delete Completed
    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_delete.png", "Delete", "Delete selected download from list",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_delete_clicked(); }), this));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_delete_completed.png", "Delete Comp.", "Delete all completed downloads from list",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_delete_completed_clicked(); }), this));

    add_sep();

    // Group 4: Options, Scheduler, Start Queue, Stop Queue
    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_options.png", "Options", "OpenDM Options and Preferences",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_scheduler_clicked(); }), this));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_scheduler.png", "Scheduler", "Configure download schedules and queues",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_scheduler_clicked(); }), this));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_start_queue.png", "Start Queue", "Start processing download queue",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_start_queue_clicked(); }), this));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_stop_queue.png", "Stop Queue", "Stop download queue",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_queue_clicked(); }), this));

    add_sep();

    // Group 5: Site Grabber & Remote ZIP Previewer
    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_grabber.png", "Grabber", "Open IDM Site Grabber spider wizard",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_site_grabber_clicked(); }), this));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_export.png", "Preview ZIP", "Inspect remote ZIP archive without downloading",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_zip_preview_clicked(); }), this));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "toolbar_speed_limiter.png", "Speed Limiter", "Toggle bandwidth limiter",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_speed_limiter_clicked(); }), this));
}

void MainWindow::setup_sidebar(GtkWidget* paned) {
    GtkWidget* sidebar_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar_vbox, "idm-sidebar");

    // Header label
    GtkWidget* header = gtk_label_new("Categories");
    gtk_widget_add_css_class(header, "idm-category-header");
    gtk_label_set_xalign(GTK_LABEL(header), 0.0f);
    gtk_box_append(GTK_BOX(sidebar_vbox), header);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(sidebar_vbox), scroll);

    sidebar_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar_list_), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), sidebar_list_);

    struct CatDef {
        const char* name;
        const char* icon;
        int indent;
    };

    std::vector<CatDef> categories = {
        {"All Downloads", "cat_all_16.png", 0},
        {"Compressed", "cat_compressed_16.png", 16},
        {"Documents", "cat_documents_16.png", 16},
        {"Music", "cat_music_16.png", 16},
        {"Programs", "cat_programs_16.png", 16},
        {"Video", "cat_video_16.png", 16},
        {"Unfinished", "cat_all_16.png", 0},
        {"Finished", "cat_all_16.png", 0},
        {"Queues", "cat_queues_16.png", 0},
        {"Main download queue", "cat_queues_16.png", 16}
    };

    for (const auto& cat : categories) {
        GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(row_box, "idm-tree-row");
        gtk_widget_set_margin_start(row_box, 8 + cat.indent);

        GtkWidget* icon = UiAssets::create_image(cat.icon, 16, 16, "folder");
        GtkWidget* lbl = gtk_label_new(cat.name);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);

        gtk_box_append(GTK_BOX(row_box), icon);
        gtk_box_append(GTK_BOX(row_box), lbl);

        GtkWidget* row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
        g_object_set_data_full(G_OBJECT(row), "cat_name", g_strdup(cat.name), g_free);

        gtk_list_box_append(GTK_LIST_BOX(sidebar_list_), row);
    }

    g_signal_connect(sidebar_list_, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer d) {
        if (!row) return;
        const char* cat_name = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "cat_name"));
        if (cat_name) {
            static_cast<MainWindow*>(d)->on_category_selected(cat_name);
        }
    }), this);

    gtk_paned_set_start_child(GTK_PANED(paned), sidebar_vbox);
}

void MainWindow::setup_downloads_view(GtkWidget* paned) {
    GtkWidget* right_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // Header row with authentic IDM columns
    GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(header_box, "idm-table-header");

    auto add_col_hdr = [&](const char* title, int width, bool expand = false) {
        GtkWidget* lbl = gtk_label_new(title);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        if (width > 0) gtk_widget_set_size_request(lbl, width, -1);
        if (expand) gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(header_box), lbl);
    };

    add_col_hdr("  File Name", 260, true);
    add_col_hdr("Size", 90);
    add_col_hdr("Status", 130);
    add_col_hdr("Time Left", 90);
    add_col_hdr("Transfer rate", 110);
    add_col_hdr("Last Try Date", 130);
    add_col_hdr("Description", 180);

    gtk_box_append(GTK_BOX(right_vbox), header_box);

    // Scrollable downloads list
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(right_vbox), scroll);

    downloads_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), downloads_container_);

    gtk_paned_set_end_child(GTK_PANED(paned), right_vbox);
}

void MainWindow::setup_statusbar(GtkWidget* parent_box) {
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(bar, "idm-statusbar");
    gtk_box_append(GTK_BOX(parent_box), bar);

    status_panel_count_ = gtk_label_new("Ready");
    gtk_widget_add_css_class(status_panel_count_, "idm-status-panel");
    gtk_label_set_xalign(GTK_LABEL(status_panel_count_), 0.0f);
    gtk_widget_set_hexpand(status_panel_count_, TRUE);
    gtk_box_append(GTK_BOX(bar), status_panel_count_);

    status_panel_speed_ = gtk_label_new("Total Speed: 0.00 KB/s");
    gtk_widget_add_css_class(status_panel_speed_, "idm-status-panel");
    gtk_box_append(GTK_BOX(bar), status_panel_speed_);

    status_panel_limiter_ = gtk_label_new("⚡ Speed Limiter: OFF");
    gtk_widget_add_css_class(status_panel_limiter_, "idm-status-panel");
    gtk_box_append(GTK_BOX(bar), status_panel_limiter_);
}

void MainWindow::on_category_selected(const std::string& category) {
    current_category_ = category;
    refresh_downloads_list();
}

gboolean MainWindow::on_tick_refresh(gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    self->refresh_downloads_list();
    return G_SOURCE_CONTINUE;
}

void MainWindow::refresh_downloads_list() {
    auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();

    // Remove existing row children
    GtkWidget* child = gtk_widget_get_first_child(downloads_container_);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(downloads_container_), child);
        child = next;
    }

    uint64_t total_system_speed = 0;
    size_t active_count = 0;

    for (const auto& s : sessions) {
        auto prog = s->get_progress();
        if (prog.status == SessionStatus::DOWNLOADING) {
            total_system_speed += prog.speed_bytes_per_sec;
            active_count++;
        }

        // Apply category filter
        if (current_category_ == "Finished" && prog.status != SessionStatus::COMPLETED) continue;
        if (current_category_ == "Unfinished" && prog.status == SessionStatus::COMPLETED) continue;
        if (current_category_ != "All Downloads" && current_category_ != "Finished" && current_category_ != "Unfinished" && current_category_ != "Queues" && current_category_ != "Main download queue") {
            std::string icon = get_file_icon_name(s->get_filename());
            std::string cat_lower = current_category_;
            std::transform(cat_lower.begin(), cat_lower.end(), cat_lower.begin(), ::tolower);
            if (icon.find(cat_lower) == std::string::npos) continue;
        }

        // Create row box matching columns
        GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(row_box, "idm-download-row");

        if (s->get_id() == selected_session_id_) {
            gtk_widget_add_css_class(row_box, "idm-download-row:selected");
        }

        // Col 0: Icon + Name
        GtkWidget* col0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_hexpand(col0, TRUE);
        GtkWidget* f_icon = UiAssets::create_image(get_file_icon_name(s->get_filename()), 16, 16, "document-save");
        GtkWidget* lbl_name = gtk_label_new(s->get_filename().c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl_name), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(col0), f_icon);
        gtk_box_append(GTK_BOX(col0), lbl_name);
        gtk_box_append(GTK_BOX(row_box), col0);

        auto add_col_text = [&](const std::string& txt, int width) {
            GtkWidget* lbl = gtk_label_new(txt.c_str());
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_size_request(lbl, width, -1);
            gtk_box_append(GTK_BOX(row_box), lbl);
        };

        // Col 1: Size
        add_col_text(format_bytes(prog.total_size), 90);

        // Col 2: Status
        std::string status_str = "Idle";
        if (prog.status == SessionStatus::COMPLETED) status_str = "Complete";
        else if (prog.status == SessionStatus::DOWNLOADING) {
            status_str = "Downloading (" + std::to_string(static_cast<int>(prog.progress_fraction * 100)) + "%)";
        } else if (prog.status == SessionStatus::PAUSED) status_str = "Paused";
        else if (prog.status == SessionStatus::ASSEMBLING) status_str = "Rebuilding...";
        else if (prog.status == SessionStatus::ERROR) status_str = "Error";
        add_col_text(status_str, 130);

        // Col 3: Time Left
        add_col_text(format_eta(prog.eta_seconds), 90);

        // Col 4: Speed
        add_col_text(format_speed(prog.speed_bytes_per_sec), 110);

        // Col 5: Last Try Date
        add_col_text("Today", 130);

        // Col 6: Description (Domain)
        std::string domain = s->get_url();
        size_t proto = domain.find("://");
        if (proto != std::string::npos) {
            domain = domain.substr(proto + 3);
            size_t slash = domain.find('/');
            if (slash != std::string::npos) domain = domain.substr(0, slash);
        }
        add_col_text(domain, 180);

        // Click gesture for row selection and double-click activation
        GtkGesture* click = gtk_gesture_click_new();
        g_object_set_data_full(G_OBJECT(click), "session_id", g_strdup(s->get_id().c_str()), g_free);

        g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* gesture, int n_press, double, double, gpointer d) {
            auto* self = static_cast<MainWindow*>(d);
            const char* sid = static_cast<const char*>(g_object_get_data(G_OBJECT(gesture), "session_id"));
            if (sid) {
                self->selected_session_id_ = sid;
                if (n_press == 2) {
                    self->on_row_activated(sid);
                }
            }
        }), this);

        gtk_widget_add_controller(row_box, GTK_EVENT_CONTROLLER(click));
        gtk_box_append(GTK_BOX(downloads_container_), row_box);
    }

    // Update status bar panels
    std::string count_str = "Total: " + std::to_string(sessions.size()) + " files (" +
                            std::to_string(active_count) + " downloading)";
    gtk_label_set_text(GTK_LABEL(status_panel_count_), count_str.c_str());
    gtk_label_set_text(GTK_LABEL(status_panel_speed_), ("Total Speed: " + format_speed(total_system_speed)).c_str());
}

void MainWindow::on_add_url_clicked() {
    AddUrlDialog::show(GTK_WINDOW(window_), "", [this](const AddUrlResult& res) {
        if (!res.accepted || res.url.empty()) return;

        std::string filename = res.filename;
        if (filename.empty()) {
            size_t slash = res.url.find_last_of('/');
            if (slash != std::string::npos && slash + 1 < res.url.length()) {
                filename = res.url.substr(slash + 1);
                size_t q = filename.find('?');
                if (q != std::string::npos) filename = filename.substr(0, q);
            } else {
                filename = "download.bin";
            }
        }

        std::string dest_path = res.path + "/" + filename;
        static uint64_t counter = 1;
        std::string id = "task_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);

        auto session = std::make_shared<DownloadSession>(id, res.url, dest_path);
        if (res.use_auth && !res.username.empty()) {
            session->set_header("Authorization", "Basic " + res.username + ":" + res.password);
        }

        QueueManager::instance().get_main_queue().add_session(session, res.start_immediately);
        refresh_downloads_list();

        if (res.start_immediately) {
            on_row_activated(id);
        }
    });
}

void MainWindow::add_download_url(const std::string& url, const std::string& path,
                                  const std::string& filename, bool start_now) {
    std::string dest_dir = path.empty() ? (fs::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string() : path;
    std::string fname = filename;
    if (fname.empty()) {
        size_t slash = url.find_last_of('/');
        if (slash != std::string::npos && slash + 1 < url.length()) {
            fname = url.substr(slash + 1);
            size_t q = fname.find('?');
            if (q != std::string::npos) fname = fname.substr(0, q);
        } else {
            fname = "download.bin";
        }
    }

    std::string dest_path = dest_dir + "/" + fname;
    static uint64_t counter = 1;
    std::string id = "task_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);

    auto session = std::make_shared<DownloadSession>(id, url, dest_path);
    QueueManager::instance().get_main_queue().add_session(session, start_now);
    refresh_downloads_list();

    if (start_now) {
        on_row_activated(id);
    }
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

void MainWindow::on_delete_completed_clicked() {
    auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();
    for (auto& s : sessions) {
        if (s->get_status() == SessionStatus::COMPLETED) {
            QueueManager::instance().get_main_queue().remove_session(s->get_id());
        }
    }
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

void MainWindow::on_speed_limiter_clicked() {
    static bool limiter_active = false;
    limiter_active = !limiter_active;

    auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();
    for (auto& s : sessions) {
        s->enable_speed_limit(limiter_active);
        if (limiter_active) {
            s->set_speed_limit(500 * 1024); // 500 KB/s default
        }
    }

    gtk_label_set_text(GTK_LABEL(status_panel_limiter_),
        limiter_active ? "⚡ Speed Limiter: ON (500 KB/s)" : "⚡ Speed Limiter: OFF");
}

void MainWindow::on_scheduler_clicked() {
    std::cout << "[OpenDM] Scheduler dialog clicked." << std::endl;
}

void MainWindow::on_zip_preview_clicked() {
    auto* dlg = new ZipPreviewDialog(GTK_WINDOW(window_));
    dlg->show();
}

void MainWindow::on_site_grabber_clicked() {
    auto* dlg = new GrabberDialog(GTK_WINDOW(window_));
    dlg->show();
}

void MainWindow::on_row_activated(const std::string& session_id) {
    auto session = QueueManager::instance().find_session(session_id);
    if (!session) return;

    if (session->get_status() == SessionStatus::COMPLETED) {
        // Open completed file
        std::string cmd = "xdg-open \"" + session->get_destination_path() + "\" &";
        (void)system(cmd.c_str());
        return;
    }

    // Open authentic Progress Dialog
    auto it = active_dialogs_.find(session_id);
    if (it != active_dialogs_.end() && it->second) {
        it->second->show();
    } else {
        auto dlg = std::make_shared<ProgressDialog>(session, GTK_WINDOW(window_));
        active_dialogs_[session_id] = dlg;
        dlg->show();
    }
}

} // namespace opendm
