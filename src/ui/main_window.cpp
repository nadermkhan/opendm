#include "main_window.hpp"
#include "progress_dialog.hpp"
#include "properties_dialog.hpp"
#include "download_complete_dialog.hpp"
#include "zip_preview_dialog.hpp"
#include "grabber_dialog.hpp"
#include "add_url_dialog.hpp"
#include "about_dialog.hpp"
#include "options_dialog.hpp"
#include "youtube_format_dialog.hpp"
#include "ui_assets.hpp"
#include "../core/batch_importer.hpp"
#include "../core/site_logon.hpp"
#include "../core/youtube_engine.hpp"
#include <gdk/gdkkeysyms.h>
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
        return "package-x-generic-symbolic";
    if (ext == "mp4" || ext == "mkv" || ext == "avi" || ext == "mov" || ext == "flv" || ext == "webm")
        return "video-x-generic-symbolic";
    if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "aac" || ext == "m4a" || ext == "ogg")
        return "audio-x-generic-symbolic";
    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "txt" || ext == "epub" || ext == "xls")
        return "x-office-document-symbolic";
    if (ext == "exe" || ext == "deb" || ext == "rpm" || ext == "AppImage" || ext == "bin" || ext == "sh")
        return "application-x-executable-symbolic";

    return "document-save-symbolic";
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
    for (auto* sg : col_size_groups_) {
        if (sg) g_object_unref(sg);
    }
    col_size_groups_.clear();
}

void MainWindow::present() {
    gtk_window_present(GTK_WINDOW(window_));
}

bool MainWindow::save_snapshot(const std::string& filename) {
    if (!window_) {
        std::cerr << "save_snapshot: window_ is null" << std::endl;
        return false;
    }

    GtkWidget* target = gtk_window_get_child(GTK_WINDOW(window_));
    if (!target) target = window_;

    GdkPaintable* paintable = gtk_widget_paintable_new(target);
    int w = gtk_widget_get_width(target);
    int h = gtk_widget_get_height(target);
    if (w <= 0) w = 980;
    if (h <= 0) h = 560;

    std::cerr << "save_snapshot: widget size is " << w << "x" << h << std::endl;

    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), static_cast<double>(w), static_cast<double>(h));
    GskRenderNode* node = gtk_snapshot_to_node(snapshot);
    if (!node) {
        node = gtk_snapshot_free_to_node(snapshot);
    }

    bool ok = false;
    if (node) {
        GtkNative* native = gtk_widget_get_native(target);
        GskRenderer* renderer = native ? gtk_native_get_renderer(native) : nullptr;
        std::cerr << "save_snapshot: native=" << native << ", renderer=" << renderer << std::endl;
        if (renderer && gsk_renderer_is_realized(renderer)) {
            graphene_rect_t bounds;
            graphene_rect_init(&bounds, 0.0f, 0.0f, (float)w, (float)h);
            GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &bounds);
            if (texture) {
                ok = gdk_texture_save_to_png(texture, filename.c_str());
                std::cerr << "save_snapshot: gdk_texture_save_to_png returned " << ok << std::endl;
                g_object_unref(texture);
            } else {
                std::cerr << "save_snapshot: render_texture failed" << std::endl;
            }
        } else {
            std::cerr << "save_snapshot: renderer not realized" << std::endl;
        }
        gsk_render_node_unref(node);
    } else {
        std::cerr << "save_snapshot: snapshot_free_to_node returned null" << std::endl;
    }
    g_object_unref(paintable);
    return ok;
}

void MainWindow::setup_ui() {
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), "OpenDM");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1260, 660);
    gtk_widget_add_css_class(window_, "idm-main-window");

    g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        auto* self = static_cast<MainWindow*>(d);
        if (self->refresh_timer_id_ > 0) {
            g_source_remove(self->refresh_timer_id_);
            self->refresh_timer_id_ = 0;
        }
    }), this);

    GtkEventController* key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer user_data) -> gboolean {
        auto* self = static_cast<MainWindow*>(user_data);
        bool ctrl = (state & GDK_CONTROL_MASK) != 0;

        if (ctrl && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
            self->on_add_url_clicked();
            return GDK_EVENT_STOP;
        }
        if (keyval == GDK_KEY_Delete) {
            self->on_delete_clicked();
            return GDK_EVENT_STOP;
        }
        if (keyval == GDK_KEY_space) {
            if (!self->selected_session_id_.empty()) {
                auto s = QueueManager::instance().find_session(self->selected_session_id_);
                if (s) {
                    auto prog = s->get_progress();
                    if (prog.status == SessionStatus::DOWNLOADING || prog.status == SessionStatus::PROBING) {
                        self->on_stop_clicked();
                    } else if (prog.status == SessionStatus::PAUSED || prog.status == SessionStatus::IDLE || prog.status == SessionStatus::ERROR) {
                        self->on_resume_clicked();
                    }
                }
            }
            return GDK_EVENT_STOP;
        }
        return GDK_EVENT_PROPAGATE;
    }), this);
    gtk_widget_add_controller(window_, key_ctrl);

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window_), main_vbox);

    setup_menubar(main_vbox);
    setup_toolbar(main_vbox);

    // Paned view: Sidebar (Left) + Downloads Table (Right)
    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 260);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_widget_set_hexpand(paned, TRUE);
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
    g_menu_append(tasks_menu, "Export batch download (.ef2)...", "win.batch_export");
    g_menu_append(tasks_menu, "Run site grabber...", "win.site_grabber");
    g_menu_append(tasks_menu, "Preview remote ZIP archive...", "win.zip_preview");
    g_menu_append(tasks_menu, "Options...", "win.options");
    g_menu_append(tasks_menu, "Exit", "app.quit");
    g_menu_append_submenu(menubar_model, "Tasks", G_MENU_MODEL(tasks_menu));

    // 2. File Menu
    GMenu* file_menu = g_menu_new();
    g_menu_append(file_menu, "Resume Download", "win.resume");
    g_menu_append(file_menu, "Stop Download", "win.stop");
    g_menu_append(file_menu, "Remove Selected", "win.delete");
    g_menu_append(file_menu, "Properties...", "win.properties");
    g_menu_append_submenu(menubar_model, "File", G_MENU_MODEL(file_menu));

    // 3. Downloads Menu
    GMenu* dl_menu = g_menu_new();
    g_menu_append(dl_menu, "Pause All", "win.stop_all");
    g_menu_append(dl_menu, "Delete All Completed", "win.delete_completed");
    g_menu_append(dl_menu, "Scheduler...", "win.scheduler");
    g_menu_append(dl_menu, "Start Queue", "win.start_queue");
    g_menu_append(dl_menu, "Stop Queue", "win.stop_queue");
    g_menu_append(dl_menu, "Speed Limiter", "win.speed_limiter");
    g_menu_append(dl_menu, "Options...", "win.options");
    g_menu_append_submenu(menubar_model, "Downloads", G_MENU_MODEL(dl_menu));

    // 4. View Menu
    GMenu* view_menu = g_menu_new();
    g_menu_append(view_menu, "Categories", "win.toggle_sidebar");
    g_menu_append(view_menu, "Preview Remote ZIP...", "win.zip_preview");
    g_menu_append_submenu(menubar_model, "View", G_MENU_MODEL(view_menu));

    // 5. Help Menu
    GMenu* help_menu = g_menu_new();
    g_menu_append(help_menu, "About OpenDM", "win.about");
    g_menu_append(help_menu, "GitHub Repository...", "win.github");
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
    add_win_action("batch_import", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_batch_import_clicked(); });
    add_win_action("batch_export", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_batch_export_clicked(); });
    add_win_action("properties", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_properties_clicked(); });
    add_win_action("resume", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_resume_clicked(); });
    add_win_action("stop", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_clicked(); });
    add_win_action("stop_all", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_all_clicked(); });
    add_win_action("delete", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_delete_clicked(); });
    add_win_action("delete_completed", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_delete_completed_clicked(); });
    add_win_action("start_queue", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_start_queue_clicked(); });
    add_win_action("stop_queue", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_queue_clicked(); });
    add_win_action("speed_limiter", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_speed_limiter_clicked(); });
    add_win_action("scheduler", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_scheduler_clicked(); });
    add_win_action("options", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_options_clicked(0); });
    add_win_action("zip_preview", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_zip_preview_clicked(); });
    add_win_action("site_grabber", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_site_grabber_clicked(); });
    add_win_action("toggle_sidebar", +[](GSimpleAction*, GVariant*, gpointer d) { static_cast<MainWindow*>(d)->on_sidebar_toggle_clicked(); });
    add_win_action("github", +[](GSimpleAction*, GVariant*, gpointer) {
        g_app_info_launch_default_for_uri("https://github.com/nadermkhan/", nullptr, nullptr);
    });
    add_win_action("about", +[](GSimpleAction*, GVariant*, gpointer d) {
        auto* self = static_cast<MainWindow*>(d);
        AboutDialog::show(GTK_WINDOW(self->get_widget()));
    });
    add_win_action("appreciation", +[](GSimpleAction*, GVariant*, gpointer d) {
        auto* self = static_cast<MainWindow*>(d);
        AboutDialog::show(GTK_WINDOW(self->get_widget()));
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

    // Group 1: Primary Add & Download controls
    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "list-add-symbolic", "Add URL", "Add new download address (Ctrl+N)",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_add_url_clicked(); }), this, true));

    btn_resume_ = UiAssets::create_toolbar_button(
        "media-playback-start-symbolic", "Resume", "Resume selected download",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_resume_clicked(); }), this);
    gtk_box_append(GTK_BOX(tb_box), btn_resume_);

    btn_stop_ = UiAssets::create_toolbar_button(
        "media-playback-pause-symbolic", "Pause", "Pause selected download",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_clicked(); }), this);
    gtk_box_append(GTK_BOX(tb_box), btn_stop_);

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "media-playback-stop-symbolic", "Stop All", "Stop all active downloads",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_all_clicked(); }), this));

    add_sep();

    // Group 2: Delete & Clean
    btn_delete_ = UiAssets::create_toolbar_button(
        "edit-delete-symbolic", "Delete", "Delete selected download from list",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_delete_clicked(); }), this);
    gtk_box_append(GTK_BOX(tb_box), btn_delete_);

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "user-trash-symbolic", "Clean", "Delete all completed downloads from list",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_delete_completed_clicked(); }), this));

    add_sep();

    // Group 3: Queue & Schedule
    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "media-playlist-repeat-symbolic", "Start Queue", "Start processing download queue",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_start_queue_clicked(); }), this, false, true));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "media-playback-stop-symbolic", "Stop Queue", "Stop download queue",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_stop_queue_clicked(); }), this, false, true));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "alarm-symbolic", "Scheduler", "Configure download schedules and queues",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_scheduler_clicked(); }), this));

    add_sep();

    // Group 4: Tools & Options
    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "preferences-system-symbolic", "Options", "OpenDM Options and Preferences",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_options_clicked(0); }), this));

    gtk_box_append(GTK_BOX(tb_box), UiAssets::create_toolbar_button(
        "network-transmit-receive-symbolic", "Grabber", "Open Site Grabber spider wizard",
        G_CALLBACK(+[](GtkButton*, gpointer d) { static_cast<MainWindow*>(d)->on_site_grabber_clicked(); }), this));

    update_toolbar_sensitivity();
}

void MainWindow::setup_sidebar(GtkWidget* paned) {
    sidebar_vbox_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(sidebar_vbox_, "idm-sidebar-frame");
    gtk_widget_set_size_request(sidebar_vbox_, 250, -1);

    // Header label + Close '×' button
    GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(header_box, "idm-category-header-box");

    GtkWidget* header_lbl = gtk_label_new("Categories");
    gtk_widget_add_css_class(header_lbl, "idm-category-header-label");
    gtk_label_set_xalign(GTK_LABEL(header_lbl), 0.0f);
    gtk_widget_set_hexpand(header_lbl, TRUE);
    gtk_box_append(GTK_BOX(header_box), header_lbl);

    GtkWidget* close_btn = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close_btn, "flat");
    gtk_widget_add_css_class(close_btn, "idm-sidebar-close-btn");
    gtk_widget_set_tooltip_text(close_btn, "Hide Categories");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        static_cast<MainWindow*>(d)->on_sidebar_toggle_clicked();
    }), this);
    gtk_box_append(GTK_BOX(header_box), close_btn);

    gtk_box_append(GTK_BOX(sidebar_vbox_), header_box);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(sidebar_vbox_), scroll);

    sidebar_list_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(sidebar_list_, "idm-category-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), sidebar_list_);

    // Setup hierarchy data
    category_expanded_["All Downloads"] = true;
    category_expanded_["Unfinished"] = false;
    category_expanded_["Finished"] = false;
    category_expanded_["Queues"] = false;

    category_children_["All Downloads"] = {"Compressed", "Documents", "Music", "Programs", "Video"};
    category_children_["Queues"] = {"Main download queue"};

    struct CatRowInfo {
        std::string name;
        std::string icon;
        bool is_parent;
        std::string parent;
    };

    std::vector<CatRowInfo> items = {
        {"All Downloads", "folder-download-symbolic", true, ""},
        {"Compressed", "package-x-generic-symbolic", false, "All Downloads"},
        {"Documents", "x-office-document-symbolic", false, "All Downloads"},
        {"Music", "audio-x-generic-symbolic", false, "All Downloads"},
        {"Programs", "application-x-executable-symbolic", false, "All Downloads"},
        {"Video", "video-x-generic-symbolic", false, "All Downloads"},
        {"Unfinished", "folder-symbolic", true, ""},
        {"Finished", "emblem-ok-symbolic", true, ""},
        {"Grabber projects", "network-transmit-receive-symbolic", false, ""},
        {"Queues", "media-playlist-repeat-symbolic", true, ""},
        {"Main download queue", "media-playlist-repeat-symbolic", false, "Queues"}
    };

    for (const auto& item : items) {
        GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(row_box, "idm-tree-row");
        if (item.name == current_category_) {
            gtk_widget_add_css_class(row_box, "selected");
        }

        if (item.is_parent) {
            bool exp = category_expanded_[item.name];
            GtkWidget* arrow_btn = gtk_button_new_with_label(exp ? "▾" : "▸");
            gtk_widget_add_css_class(arrow_btn, "flat");
            gtk_widget_add_css_class(arrow_btn, "idm-tree-arrow-btn");
            
            // Store category name on arrow
            g_object_set_data_full(G_OBJECT(arrow_btn), "cat_name", g_strdup(item.name.c_str()), g_free);
            g_signal_connect(arrow_btn, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer d) {
                auto* self = static_cast<MainWindow*>(d);
                const char* cname = static_cast<const char*>(g_object_get_data(G_OBJECT(btn), "cat_name"));
                if (!cname) return;
                bool new_exp = !self->category_expanded_[cname];
                self->category_expanded_[cname] = new_exp;
                gtk_button_set_label(btn, new_exp ? "▾" : "▸");
                for (const auto& child : self->category_children_[cname]) {
                    auto it = self->category_rows_.find(child);
                    if (it != self->category_rows_.end()) {
                        gtk_widget_set_visible(it->second, new_exp);
                    }
                }
            }), this);
            gtk_box_append(GTK_BOX(row_box), arrow_btn);
        } else {
            // Indent spacer
            GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_set_size_request(spacer, item.parent.empty() ? 16 : 28, -1);
            gtk_box_append(GTK_BOX(row_box), spacer);
        }

        GtkWidget* icon = UiAssets::create_image(item.icon, 16, 16, "folder-symbolic");
        gtk_box_append(GTK_BOX(row_box), icon);

        GtkWidget* lbl = gtk_label_new(item.name.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(row_box), lbl);

        // Click gesture on category row
        GtkGesture* click = gtk_gesture_click_new();
        g_object_set_data_full(G_OBJECT(click), "cat_name", g_strdup(item.name.c_str()), g_free);
        g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* gesture, int, double, double, gpointer d) {
            auto* self = static_cast<MainWindow*>(d);
            const char* cname = static_cast<const char*>(g_object_get_data(G_OBJECT(gesture), "cat_name"));
            if (cname) {
                self->on_category_selected(cname);
            }
        }), this);
        gtk_widget_add_controller(row_box, GTK_EVENT_CONTROLLER(click));

        category_rows_[item.name] = row_box;

        // Initial visibility
        if (!item.parent.empty()) {
            gtk_widget_set_visible(row_box, category_expanded_[item.parent]);
        }

        gtk_box_append(GTK_BOX(sidebar_list_), row_box);
    }

    gtk_paned_set_start_child(GTK_PANED(paned), sidebar_vbox_);
}

void MainWindow::setup_downloads_view(GtkWidget* paned) {
    GtkWidget* right_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(right_frame, "idm-table-frame");
    gtk_widget_set_hexpand(right_frame, TRUE);

    // Header row matching authentic IDM columns
    GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(header_box, "idm-table-header");
    gtk_widget_set_hexpand(header_box, TRUE);

    header_col_widgets_.clear();

    // Col 0: File Name (Header box with indent matching row icon)
    GtkWidget* col0_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* hdr_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(hdr_spacer, 20, -1);
    gtk_box_append(GTK_BOX(col0_hdr), hdr_spacer);

    GtkWidget* hdr_name = gtk_label_new("File Name");
    gtk_widget_add_css_class(hdr_name, "idm-col-header");
    gtk_label_set_xalign(GTK_LABEL(hdr_name), 0.0f);
    gtk_label_set_width_chars(GTK_LABEL(hdr_name), 20);
    gtk_widget_set_hexpand(hdr_name, TRUE);
    gtk_box_append(GTK_BOX(col0_hdr), hdr_name);

    gtk_widget_set_hexpand(col0_hdr, TRUE);
    gtk_box_append(GTK_BOX(header_box), col0_hdr);

    auto add_hdr_col = [&](const char* title, int width_chars) {
        GtkWidget* lbl = gtk_label_new(title);
        gtk_widget_add_css_class(lbl, "idm-col-header");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        if (width_chars > 0) {
            gtk_label_set_width_chars(GTK_LABEL(lbl), width_chars);
        }
        gtk_box_append(GTK_BOX(header_box), lbl);
        header_col_widgets_.push_back(lbl);
    };

    add_hdr_col("Size", 8);        // Index 0 in header_col_widgets_
    add_hdr_col("Status", 9);      // Index 1
    add_hdr_col("Time Left", 9);   // Index 2
    add_hdr_col("Speed", 9);       // Index 3
    add_hdr_col("Date Added", 9);  // Index 4
    add_hdr_col("Domain", 11);     // Index 5

    gtk_box_append(GTK_BOX(right_frame), header_box);

    // Scrollable downloads list
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(right_frame), scroll);

    downloads_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(downloads_container_, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), downloads_container_);

    gtk_paned_set_end_child(GTK_PANED(paned), right_frame);
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

    status_panel_limiter_ = gtk_button_new_with_label("Speed Limiter: OFF");
    gtk_widget_add_css_class(status_panel_limiter_, "flat");
    gtk_widget_add_css_class(status_panel_limiter_, "idm-status-panel-btn");
    g_signal_connect(status_panel_limiter_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        static_cast<MainWindow*>(d)->on_speed_limiter_clicked();
    }), this);
    gtk_box_append(GTK_BOX(bar), status_panel_limiter_);
}

void MainWindow::on_sidebar_toggle_clicked() {
    if (sidebar_vbox_) {
        bool vis = gtk_widget_get_visible(sidebar_vbox_);
        gtk_widget_set_visible(sidebar_vbox_, !vis);
    }
}

void MainWindow::on_category_selected(const std::string& category) {
    current_category_ = category;
    for (auto& [name, widget] : category_rows_) {
        if (name == category) {
            gtk_widget_add_css_class(widget, "selected");
        } else {
            gtk_widget_remove_css_class(widget, "selected");
        }
    }
    refresh_downloads_list();
}

gboolean MainWindow::on_tick_refresh(gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (!self->active_context_menu_open_) {
        self->refresh_downloads_list();
    }
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

    // Recreate column size groups for synchronized column widths
    for (auto* sg : col_size_groups_) {
        if (sg) g_object_unref(sg);
    }
    col_size_groups_.clear();
    for (size_t i = 0; i < header_col_widgets_.size(); ++i) {
        GtkSizeGroup* sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
        gtk_size_group_add_widget(sg, header_col_widgets_[i]);
        col_size_groups_.push_back(sg);
    }

    uint64_t total_system_speed = 0;
    size_t active_count = 0;
    size_t rendered_count = 0;

    for (const auto& s : sessions) {
        auto prog = s->get_progress();
        if (prog.status == SessionStatus::DOWNLOADING) {
            total_system_speed += prog.speed_bytes_per_sec;
            active_count++;
        }

        // Apply category filter
        if (current_category_ == "Finished" && prog.status != SessionStatus::COMPLETED) continue;
        if (current_category_ == "Unfinished" && prog.status == SessionStatus::COMPLETED) continue;
        if (current_category_ == "Grabber projects") {
            if (s->get_category() != "Grabber projects") continue;
        } else if (current_category_ != "All Downloads" && current_category_ != "Finished" && current_category_ != "Unfinished" && current_category_ != "Queues" && current_category_ != "Main download queue") {
            std::string cat = s->get_category();
            if (cat.empty()) cat = QueueManager::detect_category(s->get_filename());
            if (cat != current_category_) continue;
        }

        rendered_count++;

        // Create row box matching columns
        GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_add_css_class(row_box, "idm-download-row");
        gtk_widget_set_hexpand(row_box, TRUE);
        g_object_set_data_full(G_OBJECT(row_box), "session_id", g_strdup(s->get_id().c_str()), g_free);

        if (s->get_id() == selected_session_id_) {
            gtk_widget_add_css_class(row_box, "selected");
        }

        // Col 0: File Name (Icon + text)
        GtkWidget* col0 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(col0, "idm-download-cell");
        gtk_widget_set_hexpand(col0, TRUE);
        GtkWidget* f_icon = UiAssets::create_image(get_file_icon_name(s->get_filename()), 20, 20, "document-save-symbolic");
        GtkWidget* lbl_name = gtk_label_new(s->get_filename().c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0f);
        gtk_label_set_width_chars(GTK_LABEL(lbl_name), 20);
        gtk_label_set_ellipsize(GTK_LABEL(lbl_name), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(col0), f_icon);
        gtk_box_append(GTK_BOX(col0), lbl_name);
        gtk_box_append(GTK_BOX(row_box), col0);

        auto add_col_cell = [&](size_t col_idx, const std::string& txt, int width_chars, const char* badge_class = nullptr) {
            GtkWidget* lbl = gtk_label_new(txt.c_str());
            gtk_widget_add_css_class(lbl, "idm-download-cell");
            if (badge_class) {
                gtk_widget_add_css_class(lbl, "idm-badge");
                gtk_widget_add_css_class(lbl, badge_class);
            }
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            if (width_chars > 0) {
                gtk_label_set_width_chars(GTK_LABEL(lbl), width_chars);
            }
            if (col_idx < col_size_groups_.size() && col_size_groups_[col_idx]) {
                gtk_size_group_add_widget(col_size_groups_[col_idx], lbl);
            }
            gtk_box_append(GTK_BOX(row_box), lbl);
        };

        // Col 1: Size
        add_col_cell(0, format_bytes(prog.total_size), 8);

        // Col 2: Status with modern badge
        std::string status_str = "Queued";
        const char* badge_class = "idm-badge-queued";
        if (prog.status == SessionStatus::COMPLETED) {
            status_str = "Complete";
            badge_class = "idm-badge-complete";
        } else if (prog.status == SessionStatus::DOWNLOADING) {
            status_str = std::to_string(static_cast<int>(prog.progress_fraction * 100)) + "%";
            badge_class = "idm-badge-downloading";
        } else if (prog.status == SessionStatus::PAUSED) {
            status_str = "Paused";
            badge_class = "idm-badge-paused";
        } else if (prog.status == SessionStatus::ASSEMBLING) {
            status_str = "Rebuilding";
            badge_class = "idm-badge-downloading";
        } else if (prog.status == SessionStatus::ERROR) {
            status_str = "Error";
            badge_class = "idm-badge-paused";
        }
        add_col_cell(1, status_str, 9, badge_class);

        // Col 3: Time left
        add_col_cell(2, format_eta(prog.eta_seconds), 9);

        // Col 4: Transfer rate
        add_col_cell(3, format_speed(prog.speed_bytes_per_sec), 9);

        // Col 5: Last Try Date
        add_col_cell(4, "Today", 9);

        // Col 6: Description (Domain)
        std::string domain = s->get_url();
        size_t proto = domain.find("://");
        if (proto != std::string::npos) {
            domain = domain.substr(proto + 3);
            size_t slash = domain.find('/');
            if (slash != std::string::npos) domain = domain.substr(0, slash);
        }
        add_col_cell(5, domain, 11);

        // Click gesture for row selection, right-click context menu, and double-click activation
        GtkGesture* click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); // Listen to all mouse buttons
        g_object_set_data_full(G_OBJECT(click), "session_id", g_strdup(s->get_id().c_str()), g_free);

        g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* gesture, int n_press, double x, double y, gpointer d) {
            auto* self = static_cast<MainWindow*>(d);
            const char* sid = static_cast<const char*>(g_object_get_data(G_OBJECT(gesture), "session_id"));
            if (!sid) return;
            guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
            self->selected_session_id_ = sid;
            self->update_toolbar_sensitivity();

            // Visually highlight row without rebuilding DOM/gesture
            GtkWidget* c = gtk_widget_get_first_child(self->downloads_container_);
            while (c) {
                const char* csid = static_cast<const char*>(g_object_get_data(G_OBJECT(c), "session_id"));
                if (csid && self->selected_session_id_ == csid) {
                    gtk_widget_add_css_class(c, "selected");
                } else {
                    gtk_widget_remove_css_class(c, "selected");
                }
                c = gtk_widget_get_next_sibling(c);
            }

            if (button == 1) { // Left-click
                if (n_press == 2) {
                    self->on_row_activated(sid);
                }
            } else if (button == 3) { // Right-click context menu
                self->on_row_right_clicked(gesture, n_press, x, y, sid);
            }
        }), this);

        gtk_widget_add_controller(row_box, GTK_EVENT_CONTROLLER(click));
        gtk_box_append(GTK_BOX(downloads_container_), row_box);
    }

    // Modern GTK Empty State when 0 downloads
    if (rendered_count == 0) {
        GtkWidget* empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class(empty_box, "idm-empty-state-box");
        gtk_widget_set_halign(empty_box, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(empty_box, GTK_ALIGN_CENTER);
        gtk_widget_set_vexpand(empty_box, TRUE);

        GtkWidget* empty_icon = gtk_image_new_from_icon_name("folder-download-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 64);
        gtk_widget_add_css_class(empty_icon, "idm-empty-state-icon");
        gtk_box_append(GTK_BOX(empty_box), empty_icon);

        GtkWidget* title = gtk_label_new("No Downloads");
        gtk_widget_add_css_class(title, "idm-empty-state-title");
        gtk_box_append(GTK_BOX(empty_box), title);

        std::string sub_txt = (current_category_ == "All Downloads")
            ? "Click '+ Add URL' or paste a download link to begin"
            : "No downloads found in category '" + current_category_ + "'";
        GtkWidget* subtitle = gtk_label_new(sub_txt.c_str());
        gtk_widget_add_css_class(subtitle, "idm-empty-state-subtitle");
        gtk_box_append(GTK_BOX(empty_box), subtitle);

        if (current_category_ == "All Downloads") {
            GtkWidget* add_btn = gtk_button_new_with_label("+ Add Download");
            gtk_widget_add_css_class(add_btn, "suggested-action");
            gtk_widget_set_halign(add_btn, GTK_ALIGN_CENTER);
            g_signal_connect(add_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
                static_cast<MainWindow*>(d)->on_add_url_clicked();
            }), this);
            gtk_box_append(GTK_BOX(empty_box), add_btn);
        }

        gtk_box_append(GTK_BOX(downloads_container_), empty_box);
    }

    // Update status bar panels
    std::string count_str = std::to_string(sessions.size()) + " files";
    if (active_count > 0) {
        count_str += " (" + std::to_string(active_count) + " downloading)";
    } else {
        count_str += " (Ready)";
    }
    gtk_label_set_text(GTK_LABEL(status_panel_count_), count_str.c_str());
    gtk_label_set_text(GTK_LABEL(status_panel_speed_), ("Total Speed: " + format_speed(total_system_speed)).c_str());
    update_toolbar_sensitivity();
}

void MainWindow::update_toolbar_sensitivity() {
    if (selected_session_id_.empty()) {
        if (btn_resume_) gtk_widget_set_sensitive(btn_resume_, FALSE);
        if (btn_stop_) gtk_widget_set_sensitive(btn_stop_, FALSE);
        if (btn_delete_) gtk_widget_set_sensitive(btn_delete_, FALSE);
        return;
    }

    auto s = QueueManager::instance().find_session(selected_session_id_);
    if (!s) {
        selected_session_id_.clear();
        if (btn_resume_) gtk_widget_set_sensitive(btn_resume_, FALSE);
        if (btn_stop_) gtk_widget_set_sensitive(btn_stop_, FALSE);
        if (btn_delete_) gtk_widget_set_sensitive(btn_delete_, FALSE);
        return;
    }

    if (btn_delete_) gtk_widget_set_sensitive(btn_delete_, TRUE);
    auto prog = s->get_progress();
    if (prog.status == SessionStatus::DOWNLOADING || prog.status == SessionStatus::PROBING || prog.status == SessionStatus::ASSEMBLING) {
        if (btn_stop_) gtk_widget_set_sensitive(btn_stop_, TRUE);
        if (btn_resume_) gtk_widget_set_sensitive(btn_resume_, FALSE);
    } else if (prog.status == SessionStatus::PAUSED || prog.status == SessionStatus::IDLE || prog.status == SessionStatus::ERROR) {
        if (btn_stop_) gtk_widget_set_sensitive(btn_stop_, FALSE);
        if (btn_resume_) gtk_widget_set_sensitive(btn_resume_, TRUE);
    } else { // COMPLETED
        if (btn_stop_) gtk_widget_set_sensitive(btn_stop_, FALSE);
        if (btn_resume_) gtk_widget_set_sensitive(btn_resume_, FALSE);
    }
}

void MainWindow::on_speed_limiter_clicked() {
    static bool limiter_active = false;
    limiter_active = !limiter_active;

    QueueManager::instance().enable_global_speed_limit(limiter_active);
    if (limiter_active) {
        QueueManager::instance().set_global_speed_limit(500 * 1024); // 500 KB/s default
    }

    auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();
    for (auto& s : sessions) {
        s->enable_speed_limit(limiter_active);
        if (limiter_active) {
            s->set_speed_limit(500 * 1024);
        }
    }

    gtk_button_set_label(GTK_BUTTON(status_panel_limiter_),
        limiter_active ? "Speed Limiter: ON (500 KB/s)" : "Speed Limiter: OFF");
}

void MainWindow::prompt_download(const DownloadPromptInfo& info) {
    struct PromptWindowData {
        MainWindow* main_win;
        DownloadPromptInfo info;
    };
    auto* pwd = new PromptWindowData{this, info};
    g_idle_add(+[](gpointer user_data) -> gboolean {
        auto* data = static_cast<PromptWindowData*>(user_data);
        if (data->main_win && data->main_win->window_) {
            GtkWindow* parent = GTK_WINDOW(data->main_win->window_);
            gtk_window_present(parent);

            // Check if URL is an explicit YouTube video watch page URL
            std::string yt_url = data->info.url;
            bool is_direct_stream = data->info.is_streaming_media ||
                                    yt_url.find("googlevideo.com") != std::string::npos ||
                                    yt_url.find("videoplayback") != std::string::npos;
            if (!is_direct_stream && !YouTubeEngine::is_youtube_url(yt_url)) {
                if (YouTubeEngine::is_youtube_url(data->info.referer)) {
                    yt_url = data->info.referer;
                }
            }

            if (!is_direct_stream && YouTubeEngine::is_youtube_url(yt_url)) {
                // Show analyzing dialog with spinner while OpenDM engine resolves formats
                GtkWidget* loading_dlg = gtk_window_new();
                gtk_window_set_title(GTK_WINDOW(loading_dlg), "OpenDM - Analyzing Video");
                gtk_window_set_modal(GTK_WINDOW(loading_dlg), TRUE);
                gtk_window_set_transient_for(GTK_WINDOW(loading_dlg), parent);
                gtk_window_set_default_size(GTK_WINDOW(loading_dlg), 380, 140);
                gtk_window_set_resizable(GTK_WINDOW(loading_dlg), FALSE);

                GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
                gtk_widget_set_margin_start(box, 20);
                gtk_widget_set_margin_end(box, 20);
                gtk_widget_set_margin_top(box, 20);
                gtk_widget_set_margin_bottom(box, 20);
                gtk_window_set_child(GTK_WINDOW(loading_dlg), box);

                GtkWidget* spinner = gtk_spinner_new();
                gtk_spinner_start(GTK_SPINNER(spinner));
                gtk_widget_set_size_request(spinner, 32, 32);
                gtk_box_append(GTK_BOX(box), spinner);

                GtkWidget* lbl = gtk_label_new("Resolving YouTube video streams via OpenDM Engine...");
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.5f);
                gtk_box_append(GTK_BOX(box), lbl);

                gtk_window_present(GTK_WINDOW(loading_dlg));

                struct WorkerData {
                    MainWindow* main_win;
                    GtkWidget* loading_dlg;
                    DownloadPromptInfo original_info;
                    std::string yt_url;
                    YouTubeVideoInfo yt_info;
                    std::string err;
                    bool success{false};
                };

                auto* wd = new WorkerData{data->main_win, loading_dlg, data->info, yt_url, {}, "", false};
                std::thread([wd]() {
                    wd->success = YouTubeEngine::extract_video_info(wd->yt_url, wd->yt_info, &wd->err);

                    g_idle_add(+[](gpointer p) -> gboolean {
                        auto* d = static_cast<WorkerData*>(p);
                        gtk_window_destroy(GTK_WINDOW(d->loading_dlg));

                        if (d->success && d->yt_info.has_playable_formats()) {
                            GtkWindow* win_parent = GTK_WINDOW(d->main_win->window_);
                            YouTubeFormatDialog::show(win_parent, d->yt_info, [win = d->main_win](DownloadInfoAction action, const DownloadPromptInfo& prompt) {
                                if (action == DownloadInfoAction::CANCEL) return;

                                std::string dest_file = (fs::path(prompt.destination_dir) / (prompt.filename.empty() ? "video.mp4" : prompt.filename)).string();
                                std::error_code ec;
                                fs::create_directories(fs::path(dest_file).parent_path(), ec);

                                static uint64_t counter = 1;
                                std::string id = "task_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);

                                auto session = std::make_shared<DownloadSession>(id, prompt.url, dest_file);
                                if (!prompt.audio_stream_url.empty()) {
                                    session->set_audio_stream_url(prompt.audio_stream_url);
                                }
                                if (!prompt.user_agent.empty()) {
                                    session->set_header("User-Agent", prompt.user_agent);
                                }

                                bool start_now = (action == DownloadInfoAction::START_NOW);
                                QueueManager::instance().get_main_queue().add_session(session, start_now);
                                win->refresh_downloads_list();

                                if (start_now) {
                                    win->on_row_activated(id);
                                }
                            });
                        } else {
                            // Fallback to standard prompt dialog if network extraction could not parse formats
                            GtkWindow* win_parent = GTK_WINDOW(d->main_win->window_);
                            DownloadInfoDialog::show(win_parent, d->original_info, [win = d->main_win](DownloadInfoAction action, const DownloadInfoParams& res) {
                                if (action == DownloadInfoAction::CANCEL) return;
                                std::string dest_file = res.save_as_path;
                                if (dest_file.empty()) {
                                    dest_file = (fs::path(res.destination_dir) / (res.filename.empty() ? "download.bin" : res.filename)).string();
                                }
                                std::error_code ec;
                                fs::create_directories(fs::path(dest_file).parent_path(), ec);
                                static uint64_t counter = 1;
                                std::string id = "task_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);
                                auto session = std::make_shared<DownloadSession>(id, res.url, dest_file);
                                if (!res.audio_stream_url.empty()) session->set_audio_stream_url(res.audio_stream_url);
                                SiteLogonManager::instance().apply_to_session(*session, res.url);
                                if (!res.username.empty()) session->set_credentials(res.username, res.password);
                                if (!res.referer.empty()) session->set_header("Referer", res.referer);
                                if (!res.cookies.empty()) session->set_header("Cookie", res.cookies);
                                if (!res.user_agent.empty()) session->set_header("User-Agent", res.user_agent);
                                bool start_now = (action == DownloadInfoAction::START_NOW);
                                QueueManager::instance().get_main_queue().add_session(session, start_now);
                                win->refresh_downloads_list();
                                if (start_now) win->on_row_activated(id);
                            });
                        }
                        delete d;
                        return G_SOURCE_REMOVE;
                    }, wd);
                }).detach();
            } else {
                DownloadInfoDialog::show(parent, data->info, [win = data->main_win](DownloadInfoAction action, const DownloadInfoParams& res) {
                    if (action == DownloadInfoAction::CANCEL) {
                        return;
                    }

                    std::string dest_file = res.save_as_path;
                    if (dest_file.empty()) {
                        dest_file = (fs::path(res.destination_dir) / (res.filename.empty() ? "download.bin" : res.filename)).string();
                    }

                    std::error_code ec;
                    fs::create_directories(fs::path(dest_file).parent_path(), ec);

                    static uint64_t counter = 1;
                    std::string id = "task_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);

                    auto session = std::make_shared<DownloadSession>(id, res.url, dest_file);
                    if (!res.audio_stream_url.empty()) {
                        session->set_audio_stream_url(res.audio_stream_url);
                    }
                    SiteLogonManager::instance().apply_to_session(*session, res.url);
                    if (!res.username.empty()) session->set_credentials(res.username, res.password);
                    if (!res.referer.empty()) session->set_header("Referer", res.referer);
                    if (!res.cookies.empty()) session->set_header("Cookie", res.cookies);
                    if (!res.user_agent.empty()) session->set_header("User-Agent", res.user_agent);

                    bool start_now = (action == DownloadInfoAction::START_NOW);
                    QueueManager::instance().get_main_queue().add_session(session, start_now);
                    win->refresh_downloads_list();

                    if (start_now) {
                        win->on_row_activated(id);
                    }
                });
            }
        }
        delete data;
        return G_SOURCE_REMOVE;
    }, pwd);
}

void MainWindow::on_add_url_clicked() {
    GdkClipboard* clipboard = gtk_widget_get_clipboard(window_);
    gdk_clipboard_read_text_async(clipboard, nullptr, +[](GObject* source, GAsyncResult* res, gpointer data) {
        auto* self = static_cast<MainWindow*>(data);
        GError* err = nullptr;
        char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), res, &err);
        std::string initial_url;
        if (text) {
            std::string t = text;
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t' || t.front() == '\r' || t.front() == '\n')) t.erase(0, 1);
            if (t.rfind("http://", 0) == 0 || t.rfind("https://", 0) == 0 || t.rfind("ftp://", 0) == 0) {
                size_t ws = t.find_first_of(" \t\r\n");
                if (ws != std::string::npos) t = t.substr(0, ws);
                initial_url = t;
            }
            g_free(text);
        }
        if (err) g_error_free(err);

        AddUrlDialog::show(GTK_WINDOW(self->window_), initial_url, [self](const AddUrlResult& res) {
            if (!res.accepted || res.url.empty()) return;

            std::string default_dir = (fs::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string();

            if (res.bypass_prompt) {
                std::string dest_dir = res.path.empty() ? default_dir : res.path;
                std::string dest_file = (fs::path(dest_dir) / (res.filename.empty() ? "video.mp4" : res.filename)).string();

                std::error_code ec;
                fs::create_directories(fs::path(dest_file).parent_path(), ec);

                static uint64_t counter = 1;
                std::string id = "yt_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);

                auto session = std::make_shared<DownloadSession>(id, res.url, dest_file);
                if (!res.audio_stream_url.empty()) {
                    session->set_audio_stream_url(res.audio_stream_url);
                }
                if (!res.user_agent.empty()) session->set_header("User-Agent", res.user_agent);
                session->set_header("Referer", "https://www.youtube.com/");

                QueueManager::instance().get_main_queue().add_session(session, res.start_immediately);
                self->refresh_downloads_list();
                if (res.start_immediately) {
                    self->on_row_activated(id);
                }
                return;
            }

            DownloadPromptInfo info;
            info.url = res.url;
            info.audio_stream_url = res.audio_stream_url;
            info.user_agent = res.user_agent;
            info.is_streaming_media = res.is_streaming_media;
            info.file_size = res.file_size;
            if (res.path != default_dir && !res.path.empty()) {
                info.destination_dir = res.path;
            }
            info.filename = res.filename;
            info.username = res.username;
            info.password = res.password;
            if (info.filename.empty()) {
                size_t slash = res.url.find_last_of('/');
                if (slash != std::string::npos && slash + 1 < res.url.length()) {
                    info.filename = res.url.substr(slash + 1);
                    size_t q = info.filename.find('?');
                    if (q != std::string::npos) info.filename = info.filename.substr(0, q);
                } else {
                    info.filename = "download.bin";
                }
            }
            info.category = QueueManager::detect_category(info.filename);
            self->prompt_download(info);
        });
    }, this);
}

void MainWindow::add_download_url(const std::string& url, const std::string& path,
                                  const std::string& filename, bool start_now) {
    if (start_now) {
        DownloadPromptInfo info;
        info.url = url;
        info.destination_dir = path;
        info.filename = filename;
        if (info.filename.empty()) {
            size_t slash = url.find_last_of('/');
            if (slash != std::string::npos && slash + 1 < url.length()) {
                info.filename = url.substr(slash + 1);
                size_t q = info.filename.find('?');
                if (q != std::string::npos) info.filename = info.filename.substr(0, q);
            } else {
                info.filename = "download.bin";
            }
        }
        info.category = QueueManager::detect_category(info.filename);
        prompt_download(info);
    } else {
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
        SiteLogonManager::instance().apply_to_session(*session, url);
        QueueManager::instance().get_main_queue().add_session(session, false);
        refresh_downloads_list();
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
    auto s = QueueManager::instance().find_session(selected_session_id_);
    if (!s) return;

    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Delete Download");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 380, 160);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window_));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    std::string msg = "Delete \"" + s->get_filename() + "\" from OpenDM?";
    GtkWidget* lbl = gtk_label_new(msg.c_str());
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_box_append(GTK_BOX(vbox), lbl);

    GtkWidget* chk_disk = gtk_check_button_new_with_label("Also delete downloaded file from disk");
    gtk_box_append(GTK_BOX(vbox), chk_disk);

    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);

    GtkWidget* btn_ok = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(btn_ok, "destructive-action");
    GtkWidget* btn_cancel = gtk_button_new_with_label("Cancel");

    gtk_box_append(GTK_BOX(btn_box), btn_cancel);
    gtk_box_append(GTK_BOX(btn_box), btn_ok);
    gtk_box_append(GTK_BOX(vbox), btn_box);

    struct DelData {
        MainWindow* win;
        GtkWidget* dlg;
        GtkWidget* chk;
        std::string sid;
        std::string path;
    };
    auto* dd = new DelData{this, dialog, chk_disk, selected_session_id_, s->get_destination_path()};

    g_signal_connect_swapped(btn_cancel, "clicked", G_CALLBACK(+[](DelData* d) {
        gtk_window_destroy(GTK_WINDOW(d->dlg));
        delete d;
    }), dd);

    g_signal_connect_swapped(btn_ok, "clicked", G_CALLBACK(+[](DelData* d) {
        bool del_file = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->chk));
        if (del_file && !d->path.empty()) {
            std::error_code ec;
            std::filesystem::remove(d->path, ec);
        }
        d->win->on_delete_confirmed(del_file);
        gtk_window_destroy(GTK_WINDOW(d->dlg));
        delete d;
    }), dd);

    gtk_window_present(GTK_WINDOW(dialog));
}

void MainWindow::on_delete_confirmed(bool) {
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

void MainWindow::on_scheduler_clicked() {
    on_options_clicked(3);
}

void MainWindow::on_options_clicked(int initial_tab) {
    OptionsDialog::show(GTK_WINDOW(window_), initial_tab);
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
        std::string path = session->get_destination_path();
        std::string uri = "file://" + path;
        GError* err = nullptr;
        g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &err);
        if (err) {
            g_error_free(err);
        }
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

void MainWindow::on_row_right_clicked(GtkGestureClick* gesture, int, double x, double y, const std::string& session_id) {
    GtkWidget* row_box = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (!row_box) return;

    auto session = QueueManager::instance().find_session(session_id);
    if (!session) return;

    active_context_menu_open_ = true;

    GtkWidget* popover = gtk_popover_new();
    gtk_widget_set_parent(popover, row_box);
    gtk_widget_add_css_class(popover, "idm-context-menu");

    GdkRectangle rect;
    rect.x = static_cast<int>(x);
    rect.y = static_cast<int>(y);
    rect.width = 1;
    rect.height = 1;
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

    g_signal_connect(popover, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer data) {
        auto* self = static_cast<MainWindow*>(data);
        if (self) self->active_context_menu_open_ = false;
        gtk_widget_unparent(GTK_WIDGET(p));
    }), this);

    GtkWidget* menu_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(menu_vbox, 4);
    gtk_widget_set_margin_end(menu_vbox, 4);
    gtk_widget_set_margin_top(menu_vbox, 4);
    gtk_widget_set_margin_bottom(menu_vbox, 4);
    gtk_popover_set_child(GTK_POPOVER(popover), menu_vbox);

    auto add_item = [&](const char* label, const char* icon_name, std::function<void(MainWindow*, const std::string&)> cb, bool enabled = true) {
        GtkWidget* btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_add_css_class(btn, "idm-context-item");
        gtk_widget_set_sensitive(btn, enabled ? TRUE : FALSE);

        GtkWidget* ibox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        if (icon_name) {
            GtkWidget* img = gtk_image_new_from_icon_name(icon_name);
            gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
            gtk_box_append(GTK_BOX(ibox), img);
        }
        GtkWidget* lbl = gtk_label_new(label);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_box_append(GTK_BOX(ibox), lbl);
        gtk_button_set_child(GTK_BUTTON(btn), ibox);

        struct ItemData {
            MainWindow* win;
            GtkWidget* pop;
            std::string sid;
            std::function<void(MainWindow*, const std::string&)> callback;
        };
        auto* id = new ItemData{this, popover, session_id, cb};
        g_signal_connect(btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* d = static_cast<ItemData*>(data);
            gtk_popover_popdown(GTK_POPOVER(d->pop));
            if (d->callback) d->callback(d->win, d->sid);
        }), id);
        g_object_set_data_full(G_OBJECT(btn), "item_data", id, +[](gpointer p) { delete static_cast<ItemData*>(p); });

        gtk_box_append(GTK_BOX(menu_vbox), btn);
    };

    auto add_separator = [&]() {
        GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_box_append(GTK_BOX(menu_vbox), sep);
    };

    auto prog = session->get_progress();
    bool is_active = (prog.status == SessionStatus::DOWNLOADING || prog.status == SessionStatus::PROBING || prog.status == SessionStatus::ASSEMBLING);
    bool is_paused = (prog.status == SessionStatus::PAUSED || prog.status == SessionStatus::IDLE || prog.status == SessionStatus::ERROR);
    bool is_completed = (prog.status == SessionStatus::COMPLETED);

    add_item("Resume Download", "media-playback-start-symbolic", [](MainWindow* w, const std::string&) {
        w->on_resume_clicked();
    }, is_paused);

    add_item("Pause / Stop Download", "media-playback-pause-symbolic", [](MainWindow* w, const std::string&) {
        w->on_stop_clicked();
    }, is_active);

    add_item("Redownload", "view-refresh-symbolic", [](MainWindow* w, const std::string& sid) {
        auto s = QueueManager::instance().find_session(sid);
        if (s) {
            s->redownload();
            w->refresh_downloads_list();
        }
    });

    add_separator();

    add_item("Open File", "document-open-symbolic", [](MainWindow*, const std::string& sid) {
        auto s = QueueManager::instance().find_session(sid);
        if (s) {
            std::string path = s->get_destination_path();
            std::string uri = "file://" + path;
            g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
        }
    }, is_completed);

    add_item("Open Folder", "folder-open-symbolic", [](MainWindow*, const std::string& sid) {
        auto s = QueueManager::instance().find_session(sid);
        if (s) {
            std::string path = s->get_destination_path();
            fs::path p(path);
            std::string parent_dir = p.parent_path().string();
            std::string uri = "file://" + parent_dir;
            g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
        }
    });

    add_separator();

    add_item("Copy URL to Clipboard", "edit-copy-symbolic", [](MainWindow* w, const std::string& sid) {
        auto s = QueueManager::instance().find_session(sid);
        if (s && w->get_widget()) {
            GdkClipboard* clip = gtk_widget_get_clipboard(w->get_widget());
            gdk_clipboard_set_text(clip, s->get_url().c_str());
        }
    });

    add_item("Delete", "edit-delete-symbolic", [](MainWindow* w, const std::string&) {
        w->on_delete_clicked();
    });

    add_separator();

    add_item("Properties", "document-properties-symbolic", [](MainWindow* w, const std::string&) {
        w->on_properties_clicked();
    });

    gtk_popover_popup(GTK_POPOVER(popover));
}

void MainWindow::on_batch_import_clicked() {
    GtkFileChooserNative* chooser = gtk_file_chooser_native_new(
        "Import Batch Downloads (.ef2 or text)",
        GTK_WINDOW(window_),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Import",
        "_Cancel"
    );

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "IDM Export files (*.ef2, *.txt)");
    gtk_file_filter_add_pattern(filter, "*.ef2");
    gtk_file_filter_add_pattern(filter, "*.txt");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

    g_signal_connect(chooser, "response", G_CALLBACK(+[](GtkNativeDialog* native, int resp, gpointer user_data) {
        if (resp == GTK_RESPONSE_ACCEPT) {
            GFile* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native));
            if (file) {
                char* path = g_file_get_path(file);
                if (path) {
                    auto* self = static_cast<MainWindow*>(user_data);
                    std::string err;
                    auto items = BatchImporter::import_file(path, err);
                    for (const auto& item : items) {
                        if (!item.url.empty()) {
                            self->add_download_url(item.url, item.path, item.filename, false);
                        }
                    }
                    self->refresh_downloads_list();
                    g_free(path);
                }
                g_object_unref(file);
            }
        }
        g_object_unref(native);
    }), this);

    gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

void MainWindow::on_properties_clicked() {
    if (selected_session_id_.empty()) return;
    auto s = QueueManager::instance().find_session(selected_session_id_);
    if (s) {
        PropertiesDialog::show(GTK_WINDOW(window_), s);
    }
}

void MainWindow::on_batch_export_clicked() {
    GtkFileChooserNative* chooser = gtk_file_chooser_native_new(
        "Export Batch Downloads to IDM Export File (.ef2)",
        GTK_WINDOW(window_),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Export",
        "_Cancel"
    );
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "export.ef2");

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "IDM Export files (*.ef2)");
    gtk_file_filter_add_pattern(filter, "*.ef2");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

    g_signal_connect(chooser, "response", G_CALLBACK(+[](GtkNativeDialog* native, int resp, gpointer user_data) {
        if (resp == GTK_RESPONSE_ACCEPT) {
            GFile* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native));
            if (file) {
                char* path = g_file_get_path(file);
                if (path) {
                    auto sessions = QueueManager::instance().get_main_queue().get_all_sessions();
                    std::vector<BatchItem> items;
                    for (const auto& s : sessions) {
                        BatchItem it;
                        it.url = s->get_url();
                        it.path = s->get_destination_path();
                        it.filename = s->get_filename();
                        items.push_back(it);
                    }
                    std::string err;
                    BatchImporter::export_ef2(path, items, err);
                    g_free(path);
                }
                g_object_unref(file);
            }
        }
        g_object_unref(native);
    }), this);

    gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

} // namespace opendm
