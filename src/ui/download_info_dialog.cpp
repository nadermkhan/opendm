#include "download_info_dialog.hpp"
#include "ui_assets.hpp"
#include "zip_preview_dialog.hpp"
#include "../core/download_queue.hpp"
#include "../core/site_logon.hpp"
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

namespace opendm {

namespace {

static const char* kCategories[] = {
    "Compressed",
    "Programs",
    "Video",
    "Music",
    "Documents",
    "General",
    nullptr
};

int get_category_index(const std::string& cat) {
    for (int i = 0; kCategories[i] != nullptr; ++i) {
        if (cat == kCategories[i]) return i;
    }
    return 5; // Default to General
}

std::string get_config_dir() {
    const char* home = getenv("HOME");
    std::string base = home ? home : "/home/nader";
    std::string cfg = base + "/.config/opendm";
    std::error_code ec;
    fs::create_directories(cfg, ec);
    return cfg;
}

std::string get_default_downloads_root() {
    const char* home = getenv("HOME");
    std::string base = home ? home : "/home/nader";
    return (fs::path(base) / "Downloads").string();
}

} // namespace

DownloadInfoDialog* DownloadInfoDialog::show(GtkWindow* parent, const DownloadInfoParams& params, DownloadInfoCallback callback) {
    auto* dlg = new DownloadInfoDialog(parent, params, callback);
    dlg->setup_ui();
    return dlg;
}

DownloadInfoDialog::DownloadInfoDialog(GtkWindow* parent, const DownloadInfoParams& params, DownloadInfoCallback callback)
    : parent_(parent), params_(params), callback_(callback) {
    if (params_.category.empty()) {
        params_.category = QueueManager::detect_category(params_.filename);
    }
    if (params_.destination_dir.empty()) {
        params_.destination_dir = load_category_dir(params_.category);
    }
    if (params_.save_as_path.empty()) {
        std::string fname = params_.filename.empty() ? "download.bin" : params_.filename;
        params_.save_as_path = (fs::path(params_.destination_dir) / fname).string();
    }
    if (params_.username.empty()) {
        SiteCredential cred;
        if (SiteLogonManager::instance().get_credential_for_url(params_.url, cred)) {
            params_.username = cred.username;
            params_.password = cred.password;
        }
    }
}

DownloadInfoDialog::~DownloadInfoDialog() {
    destroyed_ = true;
}

std::string DownloadInfoDialog::get_default_category_dir(const std::string& category) {
    std::string root = get_default_downloads_root();
    if (category == "General" || category.empty()) {
        return root;
    }
    return (fs::path(root) / category).string();
}

std::string DownloadInfoDialog::load_category_dir(const std::string& category) {
    std::string cfg_file = get_config_dir() + "/category_paths.cfg";
    std::ifstream in(cfg_file);
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = line.substr(0, eq);
                std::string v = line.substr(eq + 1);
                if (k == category && !v.empty()) {
                    return v;
                }
            }
        }
    }
    return get_default_category_dir(category);
}

void DownloadInfoDialog::save_category_dir(const std::string& category, const std::string& path) {
    if (category.empty() || path.empty()) return;
    std::string cfg_file = get_config_dir() + "/category_paths.cfg";
    std::vector<std::pair<std::string, std::string>> entries;
    bool found = false;

    std::ifstream in(cfg_file);
    if (in.is_open()) {
        std::string line;
        while (std::getline(in, line)) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = line.substr(0, eq);
                std::string v = line.substr(eq + 1);
                if (k == category) {
                    entries.emplace_back(k, path);
                    found = true;
                } else {
                    entries.emplace_back(k, v);
                }
            }
        }
        in.close();
    }

    if (!found) {
        entries.emplace_back(category, path);
    }

    std::ofstream out(cfg_file);
    if (out.is_open()) {
        for (const auto& [k, v] : entries) {
            out << k << "=" << v << "\n";
        }
    }

    // Ensure the folder physically exists
    std::error_code ec;
    fs::create_directories(path, ec);
}

std::string DownloadInfoDialog::format_size(uint64_t bytes) {
    if (bytes == 0) return "Unknown";
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

void DownloadInfoDialog::setup_ui() {
    dialog_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog_), "Download File Info");
    gtk_window_set_modal(GTK_WINDOW(dialog_), TRUE);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog_), parent_);
    }
    gtk_window_set_default_size(GTK_WINDOW(dialog_), 580, 270);
    gtk_window_set_resizable(GTK_WINDOW(dialog_), FALSE);
    gtk_widget_add_css_class(dialog_, "idm-download-info-window");

    // Main layout container
    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(main_vbox, "idm-info-vbox");
    gtk_widget_set_margin_start(main_vbox, 14);
    gtk_widget_set_margin_end(main_vbox, 14);
    gtk_widget_set_margin_top(main_vbox, 12);
    gtk_widget_set_margin_bottom(main_vbox, 12);
    gtk_window_set_child(GTK_WINDOW(dialog_), main_vbox);

    // Grid for URL, Category, Save As, Category Path, Description + Right Column
    GtkWidget* content_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(content_grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(content_grid), 10);
    gtk_box_append(GTK_BOX(main_vbox), content_grid);

    // Row 0: URL
    GtkWidget* lbl_url = gtk_label_new("URL");
    gtk_widget_add_css_class(lbl_url, "idm-info-label");
    gtk_label_set_xalign(GTK_LABEL(lbl_url), 1.0f);
    gtk_grid_attach(GTK_GRID(content_grid), lbl_url, 0, 0, 1, 1);

    entry_url_ = gtk_entry_new();
    gtk_widget_set_hexpand(entry_url_, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry_url_), params_.url.c_str());
    gtk_widget_add_css_class(entry_url_, "idm-info-entry");
    gtk_grid_attach(GTK_GRID(content_grid), entry_url_, 1, 0, 2, 1);

    // Row 1: Format / Quality (for streaming media)
    lbl_format_ = gtk_label_new("Quality");
    gtk_widget_add_css_class(lbl_format_, "idm-info-label");
    gtk_label_set_xalign(GTK_LABEL(lbl_format_), 1.0f);
    gtk_grid_attach(GTK_GRID(content_grid), lbl_format_, 0, 1, 1, 1);
    gtk_widget_set_visible(lbl_format_, FALSE);

    const char* dummy_fmt[] = { "Auto (Best Quality)", nullptr };
    combo_stream_format_ = gtk_drop_down_new_from_strings(dummy_fmt);
    gtk_widget_set_hexpand(combo_stream_format_, TRUE);
    row_stream_format_ = combo_stream_format_;
    gtk_grid_attach(GTK_GRID(content_grid), row_stream_format_, 1, 1, 2, 1);
    gtk_widget_set_visible(row_stream_format_, FALSE);

    // Row 2: Category
    GtkWidget* lbl_category = gtk_label_new("Category");
    gtk_widget_add_css_class(lbl_category, "idm-info-label");
    gtk_label_set_xalign(GTK_LABEL(lbl_category), 1.0f);
    gtk_grid_attach(GTK_GRID(content_grid), lbl_category, 0, 2, 1, 1);

    GtkWidget* cat_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    combo_category_ = gtk_drop_down_new_from_strings(kCategories);
    gtk_widget_set_hexpand(combo_category_, TRUE);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(combo_category_), get_category_index(params_.category));
    gtk_box_append(GTK_BOX(cat_box), combo_category_);

    btn_add_cat_ = gtk_button_new_with_label("+");
    gtk_widget_set_tooltip_text(btn_add_cat_, "Manage Categories");
    gtk_widget_add_css_class(btn_add_cat_, "idm-btn-mini");
    gtk_box_append(GTK_BOX(cat_box), btn_add_cat_);

    gtk_grid_attach(GTK_GRID(content_grid), cat_box, 1, 2, 2, 1);

    // Row 3: Save As
    GtkWidget* lbl_save_as = gtk_label_new("Save As");
    gtk_widget_add_css_class(lbl_save_as, "idm-info-label");
    gtk_label_set_xalign(GTK_LABEL(lbl_save_as), 1.0f);
    gtk_grid_attach(GTK_GRID(content_grid), lbl_save_as, 0, 3, 1, 1);

    GtkWidget* save_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    entry_save_as_ = gtk_entry_new();
    gtk_widget_set_hexpand(entry_save_as_, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry_save_as_), params_.save_as_path.c_str());
    gtk_widget_add_css_class(entry_save_as_, "idm-info-entry");
    gtk_box_append(GTK_BOX(save_box), entry_save_as_);

    btn_browse_ = gtk_button_new_with_label("...");
    gtk_widget_set_tooltip_text(btn_browse_, "Browse destination folder");
    gtk_widget_add_css_class(btn_browse_, "idm-btn-mini");
    gtk_box_append(GTK_BOX(save_box), btn_browse_);

    gtk_grid_attach(GTK_GRID(content_grid), save_box, 1, 3, 2, 1);

    // Row 4: Remember path frame
    GtkWidget* remember_frame = gtk_frame_new(nullptr);
    gtk_widget_add_css_class(remember_frame, "idm-remember-frame");
    GtkWidget* remember_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(remember_vbox, 8);
    gtk_widget_set_margin_end(remember_vbox, 8);
    gtk_widget_set_margin_top(remember_vbox, 6);
    gtk_widget_set_margin_bottom(remember_vbox, 6);
    gtk_frame_set_child(GTK_FRAME(remember_frame), remember_vbox);

    std::string chk_label = "Remember this path for \"" + params_.category + "\" category";
    chk_remember_path_ = gtk_check_button_new_with_label(chk_label.c_str());
    gtk_widget_add_css_class(chk_remember_path_, "idm-remember-chk");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_remember_path_), TRUE);
    gtk_box_append(GTK_BOX(remember_vbox), chk_remember_path_);

    entry_cat_dir_ = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry_cat_dir_), params_.destination_dir.c_str());
    gtk_widget_add_css_class(entry_cat_dir_, "idm-info-entry-sub");
    gtk_box_append(GTK_BOX(remember_vbox), entry_cat_dir_);

    gtk_grid_attach(GTK_GRID(content_grid), remember_frame, 1, 4, 2, 1);

    // Row 5: Description
    GtkWidget* lbl_desc = gtk_label_new("Description");
    gtk_widget_add_css_class(lbl_desc, "idm-info-label");
    gtk_label_set_xalign(GTK_LABEL(lbl_desc), 1.0f);
    gtk_grid_attach(GTK_GRID(content_grid), lbl_desc, 0, 5, 1, 1);

    entry_desc_ = gtk_entry_new();
    gtk_widget_set_hexpand(entry_desc_, TRUE);
    if (!params_.description.empty()) {
        gtk_editable_set_text(GTK_EDITABLE(entry_desc_), params_.description.c_str());
    }
    gtk_widget_add_css_class(entry_desc_, "idm-info-entry");
    gtk_grid_attach(GTK_GRID(content_grid), entry_desc_, 1, 5, 2, 1);

    // Right Column: Icon, File Size, Preview Button (spanning rows 1 to 5)
    right_icon_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(right_icon_box_, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(right_icon_box_, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(right_icon_box_, 90, -1);

    img_icon_ = nullptr;
    update_category_ui(params_.category, false);

    lbl_size_ = gtk_label_new("Unknown");
    gtk_widget_add_css_class(lbl_size_, "idm-info-filesize");
    gtk_box_append(GTK_BOX(right_icon_box_), lbl_size_);
    update_file_size_display();

    btn_preview_ = gtk_button_new_with_mnemonic("_Preview");
    gtk_widget_set_tooltip_text(btn_preview_, "Preview archive contents without downloading full file");
    gtk_widget_add_css_class(btn_preview_, "idm-info-btn");
    gtk_box_append(GTK_BOX(right_icon_box_), btn_preview_);

    gtk_grid_attach(GTK_GRID(content_grid), right_icon_box_, 3, 1, 1, 5);

    // Bottom Action Buttons
    GtkWidget* action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(action_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(action_box, 10);
    gtk_box_append(GTK_BOX(main_vbox), action_box);

    btn_download_later_ = gtk_button_new_with_mnemonic("Download _Later");
    gtk_widget_add_css_class(btn_download_later_, "idm-info-btn");
    gtk_widget_set_size_request(btn_download_later_, 115, 32);
    gtk_box_append(GTK_BOX(action_box), btn_download_later_);

    btn_start_download_ = gtk_button_new_with_mnemonic("_Start Download");
    gtk_widget_add_css_class(btn_start_download_, "idm-info-btn");
    gtk_widget_add_css_class(btn_start_download_, "suggested-action");
    gtk_widget_set_size_request(btn_start_download_, 125, 32);
    gtk_box_append(GTK_BOX(action_box), btn_start_download_);

    btn_cancel_ = gtk_button_new_with_mnemonic("_Cancel");
    gtk_widget_add_css_class(btn_cancel_, "idm-info-btn");
    gtk_widget_set_size_request(btn_cancel_, 95, 32);
    gtk_box_append(GTK_BOX(action_box), btn_cancel_);

    // Connect signals
    g_signal_connect(combo_stream_format_, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        auto* self = static_cast<DownloadInfoDialog*>(data);
        self->on_stream_format_changed();
    }), this);

    g_signal_connect(combo_category_, "notify::selected", G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
        auto* self = static_cast<DownloadInfoDialog*>(data);
        guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
        if (idx < 6) {
            self->update_category_ui(kCategories[idx], true);
        }
    }), this);

    g_signal_connect_swapped(btn_add_cat_, "clicked", G_CALLBACK(+[](DownloadInfoDialog* self) {
        self->on_add_category_clicked();
    }), this);

    g_signal_connect_swapped(btn_browse_, "clicked", G_CALLBACK(+[](DownloadInfoDialog* self) {
        self->on_browse_clicked();
    }), this);

    g_signal_connect_swapped(btn_preview_, "clicked", G_CALLBACK(+[](DownloadInfoDialog* self) {
        self->on_preview_clicked();
    }), this);

    g_signal_connect_swapped(btn_start_download_, "clicked", G_CALLBACK(+[](DownloadInfoDialog* self) {
        self->on_action_clicked(DownloadInfoAction::START_NOW);
    }), this);

    g_signal_connect_swapped(btn_download_later_, "clicked", G_CALLBACK(+[](DownloadInfoDialog* self) {
        self->on_action_clicked(DownloadInfoAction::DOWNLOAD_LATER);
    }), this);

    g_signal_connect_swapped(btn_cancel_, "clicked", G_CALLBACK(+[](DownloadInfoDialog* self) {
        self->on_action_clicked(DownloadInfoAction::CANCEL);
    }), this);

    // Two-way path synchronization between Save As and Category Path
    g_signal_connect(entry_save_as_, "changed", G_CALLBACK(+[](GtkEditable* ed, gpointer data) {
        auto* self = static_cast<DownloadInfoDialog*>(data);
        if (self->syncing_paths_) return;
        self->syncing_paths_ = true;
        const char* txt = gtk_editable_get_text(ed);
        fs::path p(txt ? txt : "");
        std::string parent_dir = p.parent_path().string();
        if (!parent_dir.empty() && self->entry_cat_dir_) {
            gtk_editable_set_text(GTK_EDITABLE(self->entry_cat_dir_), parent_dir.c_str());
        }
        self->syncing_paths_ = false;
    }), this);

    g_signal_connect(entry_cat_dir_, "changed", G_CALLBACK(+[](GtkEditable* ed, gpointer data) {
        auto* self = static_cast<DownloadInfoDialog*>(data);
        if (self->syncing_paths_) return;
        self->syncing_paths_ = true;
        const char* dir_txt = gtk_editable_get_text(ed);
        std::string new_dir = dir_txt ? dir_txt : "";
        if (self->entry_save_as_) {
            const char* cur_save = gtk_editable_get_text(GTK_EDITABLE(self->entry_save_as_));
            fs::path p(cur_save ? cur_save : "");
            std::string fname = p.filename().string();
            if (fname.empty()) fname = self->params_.filename.empty() ? "download.bin" : self->params_.filename;
            std::string new_save = (fs::path(new_dir) / fname).string();
            gtk_editable_set_text(GTK_EDITABLE(self->entry_save_as_), new_save.c_str());
        }
        self->syncing_paths_ = false;
    }), this);

    // Keyboard shortcuts: Enter -> Start Download, Escape -> Cancel
    GtkEventController* key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer user_data) -> gboolean {
        if (keyval == GDK_KEY_Escape) {
            auto* self = static_cast<DownloadInfoDialog*>(user_data);
            self->on_action_clicked(DownloadInfoAction::CANCEL);
            return GDK_EVENT_STOP;
        }
        return GDK_EVENT_PROPAGATE;
    }), this);
    gtk_widget_add_controller(dialog_, key_ctrl);

    g_signal_connect_swapped(entry_url_, "activate", G_CALLBACK(+[](DownloadInfoDialog* self) {
        self->on_action_clicked(DownloadInfoAction::START_NOW);
    }), this);
    g_signal_connect_swapped(entry_save_as_, "activate", G_CALLBACK(+[](DownloadInfoDialog* self) {
        self->on_action_clicked(DownloadInfoAction::START_NOW);
    }), this);
    g_signal_connect_swapped(entry_desc_, "activate", G_CALLBACK(+[](DownloadInfoDialog* self) {
        self->on_action_clicked(DownloadInfoAction::START_NOW);
    }), this);

    // Attach self to dialog_ for automatic deletion when widget is destroyed
    g_object_set_data_full(G_OBJECT(dialog_), "opendm_info_dlg_instance", this, +[](gpointer p) {
        delete static_cast<DownloadInfoDialog*>(p);
    });

    // Close request handler
    g_signal_connect(dialog_, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
        auto* self = static_cast<DownloadInfoDialog*>(data);
        self->destroyed_ = true;
        if (self->alive_token_) {
            self->alive_token_->store(false);
        }
        if (self->callback_) {
            self->callback_(DownloadInfoAction::CANCEL, self->params_);
            self->callback_ = nullptr;
        }
        return FALSE; // Allow close, which triggers dialog destruction and frees self
    }), this);

    gtk_window_present(GTK_WINDOW(dialog_));

    // Probe file size if not known
    if (params_.file_size == 0 && !params_.url.empty()) {
        probe_remote_info();
    }
}

void DownloadInfoDialog::update_category_ui(const std::string& category, bool update_path) {
    params_.category = category;

    if (update_path) {
        params_.destination_dir = load_category_dir(category);
        std::string fname;
        if (entry_save_as_) {
            const char* cur = gtk_editable_get_text(GTK_EDITABLE(entry_save_as_));
            if (cur && *cur) {
                fs::path p(cur);
                fname = p.filename().string();
            }
        }
        if (fname.empty()) {
            fname = params_.filename.empty() ? "download.bin" : params_.filename;
        }
        params_.save_as_path = (fs::path(params_.destination_dir) / fname).string();

        syncing_paths_ = true;
        if (entry_save_as_) {
            gtk_editable_set_text(GTK_EDITABLE(entry_save_as_), params_.save_as_path.c_str());
        }
        if (entry_cat_dir_) {
            gtk_editable_set_text(GTK_EDITABLE(entry_cat_dir_), params_.destination_dir.c_str());
        }
        syncing_paths_ = false;

        if (chk_remember_path_) {
            std::string label = "Remember this path for \"" + category + "\" category";
            gtk_check_button_set_label(GTK_CHECK_BUTTON(chk_remember_path_), label.c_str());
        }
    }

    // Update icon
    std::string icon_asset = "cat_all_32.png";
    const char* fallback = "folder-download";
    if (category == "Compressed") {
        icon_asset = "cat_compressed_32.png";
        fallback = "package-x-generic";
    } else if (category == "Documents") {
        icon_asset = "cat_documents_32.png";
        fallback = "x-office-document";
    } else if (category == "Music") {
        icon_asset = "cat_music_32.png";
        fallback = "audio-x-generic";
    } else if (category == "Programs") {
        icon_asset = "cat_programs_32.png";
        fallback = "application-x-executable";
    } else if (category == "Video") {
        icon_asset = "cat_video_32.png";
        fallback = "video-x-generic";
    }

    if (img_icon_ && right_icon_box_) {
        gtk_box_remove(GTK_BOX(right_icon_box_), img_icon_);
        img_icon_ = nullptr;
    }

    if (right_icon_box_) {
        img_icon_ = UiAssets::create_image(icon_asset, 38, 38, fallback);
        gtk_box_prepend(GTK_BOX(right_icon_box_), img_icon_);
    }

    // Preview button sensitivity: enabled for compressed archives
    bool is_archive = (category == "Compressed");
    std::string lower_fname = params_.filename;
    std::transform(lower_fname.begin(), lower_fname.end(), lower_fname.begin(), ::tolower);
    if (lower_fname.ends_with(".zip") || lower_fname.ends_with(".rar") ||
        lower_fname.ends_with(".7z") || lower_fname.ends_with(".tar") ||
        lower_fname.ends_with(".gz") || lower_fname.ends_with(".bz2")) {
        is_archive = true;
    }
    params_.is_archive = is_archive;
    if (btn_preview_) {
        gtk_widget_set_sensitive(btn_preview_, is_archive ? TRUE : FALSE);
    }
}

void DownloadInfoDialog::update_file_size_display() {
    if (!lbl_size_) return;
    if (params_.file_size > 0) {
        std::string s = format_size(params_.file_size);
        gtk_label_set_text(GTK_LABEL(lbl_size_), s.c_str());
    } else {
        gtk_label_set_text(GTK_LABEL(lbl_size_), "Unknown");
    }
}

void DownloadInfoDialog::on_stream_format_changed() {
    if (!combo_stream_format_ || available_stream_formats_.empty()) return;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(combo_stream_format_));
    if (idx < available_stream_formats_.size()) {
        const auto& fmt = available_stream_formats_[idx];
        params_.url = fmt.video_url;
        params_.audio_stream_url = fmt.audio_url;
        params_.file_size = fmt.filesize;
        params_.is_streaming_media = true;

        if (!params_.filename.empty()) {
            size_t dot = params_.filename.find_last_of('.');
            if (dot != std::string::npos) {
                params_.filename = params_.filename.substr(0, dot + 1) + fmt.ext;
            } else {
                params_.filename += "." + fmt.ext;
            }
            params_.save_as_path = (fs::path(params_.destination_dir) / params_.filename).string();
            if (entry_save_as_) {
                gtk_editable_set_text(GTK_EDITABLE(entry_save_as_), params_.save_as_path.c_str());
            }
        }

        if (fmt.is_audio_only) {
            update_category_ui("Music", true);
        } else {
            update_category_ui("Video", true);
        }
        update_file_size_display();
    }
}

void DownloadInfoDialog::update_stream_formats_ui(const StreamMediaInfo& info) {
    if (destroyed_) return;
    available_stream_formats_ = info.formats;
    if (available_stream_formats_.empty()) {
        gtk_label_set_text(GTK_LABEL(lbl_size_), "Unknown");
        return;
    }

    std::vector<const char*> labels;
    for (const auto& fmt : available_stream_formats_) {
        labels.push_back(fmt.label.c_str());
    }
    labels.push_back(nullptr);

    GtkStringList* str_list = gtk_string_list_new(labels.data());
    gtk_drop_down_set_model(GTK_DROP_DOWN(combo_stream_format_), G_LIST_MODEL(str_list));
    g_object_unref(str_list);

    gtk_drop_down_set_selected(GTK_DROP_DOWN(combo_stream_format_), 0);

    const auto& best = available_stream_formats_[0];
    params_.url = best.video_url;
    params_.audio_stream_url = best.audio_url;
    params_.file_size = best.filesize;
    params_.is_streaming_media = true;

    std::string safe_title = StreamExtractor::sanitize_filename(info.title, best.ext);
    params_.filename = safe_title;
    params_.save_as_path = (fs::path(params_.destination_dir) / safe_title).string();
    if (entry_save_as_) {
        gtk_editable_set_text(GTK_EDITABLE(entry_save_as_), params_.save_as_path.c_str());
    }
    if (entry_desc_ && !info.uploader.empty()) {
        std::string desc = "Uploader: " + info.uploader;
        gtk_editable_set_text(GTK_EDITABLE(entry_desc_), desc.c_str());
        params_.description = desc;
    }

    if (best.is_audio_only) {
        update_category_ui("Music", true);
    } else {
        update_category_ui("Video", true);
    }

    update_file_size_display();

    if (row_stream_format_) gtk_widget_set_visible(row_stream_format_, TRUE);
    if (lbl_format_) gtk_widget_set_visible(lbl_format_, TRUE);
}

void DownloadInfoDialog::probe_remote_info() {
    if (params_.file_size > 0 || params_.url.empty()) return;

    if (StreamExtractor::is_streaming_url(params_.url) && StreamExtractor::is_available()) {
        gtk_label_set_text(GTK_LABEL(lbl_size_), "Extracting...");

        StreamExtractor::extract_info_async(params_.url, [this, alive = alive_token_](bool ok, const StreamMediaInfo& info, const std::string&) {
            if (!alive->load() || destroyed_) return;
            struct StreamProbeData {
                DownloadInfoDialog* self;
                std::shared_ptr<std::atomic<bool>> alive;
                bool ok;
                StreamMediaInfo info;
            };
            auto* data = new StreamProbeData{this, alive, ok, info};
            g_idle_add([](gpointer p) -> gboolean {
                auto* d = static_cast<StreamProbeData*>(p);
                if (d->alive->load() && !d->self->destroyed_) {
                    if (d->ok) {
                        d->self->update_stream_formats_ui(d->info);
                    } else {
                        gtk_label_set_text(GTK_LABEL(d->self->lbl_size_), "Unknown");
                    }
                }
                delete d;
                return G_SOURCE_REMOVE;
            }, data);
        });
        return;
    }

    gtk_label_set_text(GTK_LABEL(lbl_size_), "Checking...");

    struct ProbeContext {
        DownloadInfoDialog* self;
        std::shared_ptr<std::atomic<bool>> alive;
        std::string url;
        std::string referer;
        std::string cookies;
        std::string user_agent;
    };

    auto* ctx = new ProbeContext{this, alive_token_, params_.url, params_.referer, params_.cookies, params_.user_agent};

    std::thread([ctx]() {
        CURL* curl = curl_easy_init();
        uint64_t size = 0;
        std::string cd_filename;

        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, ctx->url.c_str());
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            if (!ctx->referer.empty()) curl_easy_setopt(curl, CURLOPT_REFERER, ctx->referer.c_str());
            if (!ctx->cookies.empty()) curl_easy_setopt(curl, CURLOPT_COOKIE, ctx->cookies.c_str());
            if (!ctx->user_agent.empty()) curl_easy_setopt(curl, CURLOPT_USERAGENT, ctx->user_agent.c_str());

            SiteCredential cred;
            if (SiteLogonManager::instance().get_credential_for_url(ctx->url, cred)) {
                if (!cred.username.empty() && !cred.password.empty()) {
                    std::string userpwd = cred.username + ":" + cred.password;
                    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
                    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
                }
                if (ctx->cookies.empty() && !cred.custom_cookies.empty()) {
                    curl_easy_setopt(curl, CURLOPT_COOKIE, cred.custom_cookies.c_str());
                }
            }

            auto header_cb = +[](char* buffer, size_t size, size_t nitems, void* userdata) -> size_t {
                size_t total = size * nitems;
                std::string line(buffer, total);
                std::string* out_name = static_cast<std::string*>(userdata);
                size_t fn_pos = line.find("filename=");
                if (fn_pos != std::string::npos) {
                    std::string f = line.substr(fn_pos + 9);
                    while (!f.empty() && (f.back() == '\r' || f.back() == '\n' || f.back() == ' ' || f.back() == '"' || f.back() == ';')) f.pop_back();
                    while (!f.empty() && (f.front() == '"' || f.front() == ' ')) f.erase(0, 1);
                    if (!f.empty()) *out_name = f;
                }
                return total;
            };
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cd_filename);

            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                curl_off_t cl = 0;
                curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
                if (cl > 0) size = static_cast<uint64_t>(cl);
            }
            curl_easy_cleanup(curl);
        }

        struct ResultData {
            DownloadInfoDialog* self;
            std::shared_ptr<std::atomic<bool>> alive;
            uint64_t size;
            std::string filename;
        };

        auto* res_data = new ResultData{ctx->self, ctx->alive, size, cd_filename};
        delete ctx;

        g_idle_add(+[](gpointer d) -> gboolean {
            auto* rd = static_cast<ResultData*>(d);
            if (rd->alive && rd->alive->load() && rd->self && !rd->self->destroyed_) {
                if (rd->size > 0) {
                    rd->self->params_.file_size = rd->size;
                    rd->self->update_file_size_display();
                } else {
                    rd->self->update_file_size_display();
                }
                if (!rd->filename.empty() && (rd->self->params_.filename.empty() || rd->self->params_.filename == "download.bin")) {
                    rd->self->params_.filename = rd->filename;
                    std::string cat = QueueManager::detect_category(rd->filename);
                    rd->self->params_.category = cat;
                    if (rd->self->combo_category_) {
                        gtk_drop_down_set_selected(GTK_DROP_DOWN(rd->self->combo_category_), get_category_index(cat));
                    }
                    rd->self->update_category_ui(cat, true);
                }
            }
            delete rd;
            return G_SOURCE_REMOVE;
        }, res_data);
    }).detach();
}

void DownloadInfoDialog::on_browse_clicked() {
    GtkFileChooserNative* native = gtk_file_chooser_native_new(
        "Save Download As",
        dialog_ ? GTK_WINDOW(dialog_) : parent_,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Save",
        "_Cancel"
    );

    std::string cur_save_path = gtk_editable_get_text(GTK_EDITABLE(entry_save_as_));
    fs::path p(cur_save_path);
    std::string fname = p.filename().string();
    std::string parent_dir = p.parent_path().string();

    if (!fname.empty()) {
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(native), fname.c_str());
    }
    if (!parent_dir.empty() && fs::exists(parent_dir)) {
        GFile* folder = g_file_new_for_path(parent_dir.c_str());
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(native), folder, nullptr);
        g_object_unref(folder);
    }

    g_signal_connect(native, "response", G_CALLBACK(+[](GtkNativeDialog* dialog, int response_id, gpointer user_data) {
        if (response_id == GTK_RESPONSE_ACCEPT) {
            GtkFileChooser* chooser = GTK_FILE_CHOOSER(dialog);
            GFile* file = gtk_file_chooser_get_file(chooser);
            if (file) {
                char* chosen_path = g_file_get_path(file);
                if (chosen_path) {
                    auto* self = static_cast<DownloadInfoDialog*>(user_data);
                    if (self && !self->destroyed_) {
                        self->params_.save_as_path = chosen_path;
                        fs::path chosen_fs(chosen_path);
                        self->params_.destination_dir = chosen_fs.parent_path().string();
                        self->params_.filename = chosen_fs.filename().string();

                        gtk_editable_set_text(GTK_EDITABLE(self->entry_save_as_), chosen_path);
                        gtk_editable_set_text(GTK_EDITABLE(self->entry_cat_dir_), self->params_.destination_dir.c_str());
                    }
                    g_free(chosen_path);
                }
                g_object_unref(file);
            }
        }
        g_object_unref(dialog);
    }), this);

    gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));
}

void DownloadInfoDialog::on_add_category_clicked() {
    GtkWidget* modal = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(modal), "Category Properties");
    gtk_window_set_modal(GTK_WINDOW(modal), TRUE);
    if (dialog_) {
        gtk_window_set_transient_for(GTK_WINDOW(modal), GTK_WINDOW(dialog_));
    }
    gtk_window_set_default_size(GTK_WINDOW(modal), 440, 180);
    gtk_window_set_resizable(GTK_WINDOW(modal), FALSE);
    gtk_widget_add_css_class(modal, "idm-download-info-window");

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(modal), vbox);

    GtkWidget* lbl_intro = gtk_label_new("Set default download folder for this category:");
    gtk_label_set_xalign(GTK_LABEL(lbl_intro), 0.0f);
    gtk_widget_add_css_class(lbl_intro, "idm-info-label");
    gtk_box_append(GTK_BOX(vbox), lbl_intro);

    // Current category display
    GtkWidget* cat_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* lbl_cat = gtk_label_new("Category:");
    gtk_widget_add_css_class(lbl_cat, "idm-info-label");
    GtkWidget* lbl_cat_val = gtk_label_new(params_.category.c_str());
    gtk_widget_add_css_class(lbl_cat_val, "idm-info-entry");
    gtk_box_append(GTK_BOX(cat_row), lbl_cat);
    gtk_box_append(GTK_BOX(cat_row), lbl_cat_val);
    gtk_box_append(GTK_BOX(vbox), cat_row);

    // Folder row
    GtkWidget* fld_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* entry_folder = gtk_entry_new();
    gtk_widget_set_hexpand(entry_folder, TRUE);
    std::string cur_dir = load_category_dir(params_.category);
    gtk_editable_set_text(GTK_EDITABLE(entry_folder), cur_dir.c_str());
    gtk_widget_add_css_class(entry_folder, "idm-info-entry");
    gtk_box_append(GTK_BOX(fld_row), entry_folder);

    GtkWidget* btn_pick = gtk_button_new_with_label("...");
    gtk_widget_add_css_class(btn_pick, "idm-btn-mini");
    gtk_box_append(GTK_BOX(fld_row), btn_pick);
    gtk_box_append(GTK_BOX(vbox), fld_row);

    // Pick button opens directory chooser
    struct PickData {
        GtkWindow* parent;
        GtkWidget* target_entry;
    };
    auto* pd = new PickData{GTK_WINDOW(modal), entry_folder};
    g_signal_connect(btn_pick, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* d = static_cast<PickData*>(data);
        GtkFileChooserNative* chooser = gtk_file_chooser_native_new(
            "Select Category Folder",
            d->parent,
            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
            "_Select",
            "_Cancel"
        );
        g_signal_connect(chooser, "response", G_CALLBACK(+[](GtkNativeDialog* native, int resp, gpointer u) {
            if (resp == GTK_RESPONSE_ACCEPT) {
                GFile* f = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native));
                if (f) {
                    char* path = g_file_get_path(f);
                    if (path) {
                        gtk_editable_set_text(GTK_EDITABLE(u), path);
                        g_free(path);
                    }
                    g_object_unref(f);
                }
            }
            g_object_unref(native);
        }), d->target_entry);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
    }), pd);
    g_object_set_data_full(G_OBJECT(modal), "pick_data", pd, +[](gpointer p) { delete static_cast<PickData*>(p); });

    // Buttons
    GtkWidget* bbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(bbox, GTK_ALIGN_END);
    gtk_widget_set_margin_top(bbox, 8);
    gtk_box_append(GTK_BOX(vbox), bbox);

    GtkWidget* btn_ok = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(btn_ok, "idm-info-btn");
    gtk_widget_add_css_class(btn_ok, "suggested-action");
    gtk_widget_set_size_request(btn_ok, 80, 30);
    gtk_box_append(GTK_BOX(bbox), btn_ok);

    GtkWidget* btn_can = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(btn_can, "idm-info-btn");
    gtk_widget_set_size_request(btn_can, 80, 30);
    gtk_box_append(GTK_BOX(bbox), btn_can);

    struct OkData {
        DownloadInfoDialog* self;
        GtkWidget* modal;
        GtkWidget* entry;
    };
    auto* od = new OkData{this, modal, entry_folder};
    g_signal_connect(btn_ok, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* o = static_cast<OkData*>(data);
        const char* txt = gtk_editable_get_text(GTK_EDITABLE(o->entry));
        if (txt && *txt) {
            DownloadInfoDialog::save_category_dir(o->self->params_.category, txt);
            o->self->params_.destination_dir = txt;
            o->self->update_category_ui(o->self->params_.category, true);
        }
        gtk_window_destroy(GTK_WINDOW(o->modal));
    }), od);
    g_object_set_data_full(G_OBJECT(modal), "ok_data", od, +[](gpointer p) { delete static_cast<OkData*>(p); });

    g_signal_connect_swapped(btn_can, "clicked", G_CALLBACK(gtk_window_destroy), modal);

    gtk_window_present(GTK_WINDOW(modal));
}

void DownloadInfoDialog::on_preview_clicked() {
    std::string url = gtk_editable_get_text(GTK_EDITABLE(entry_url_));
    if (url.empty()) return;
    auto* dlg = new ZipPreviewDialog(GTK_WINDOW(dialog_), url);
    dlg->show();
}

void DownloadInfoDialog::on_action_clicked(DownloadInfoAction action) {
    if (action != DownloadInfoAction::CANCEL) {
        // Retrieve latest user-modified fields
        if (!params_.is_streaming_media) {
            params_.url = gtk_editable_get_text(GTK_EDITABLE(entry_url_));
        }

        // If the URL is a streaming site and background extraction hasn't completed yet, do synchronous resolution
        if (StreamExtractor::is_streaming_url(params_.url) && !params_.is_streaming_media && StreamExtractor::is_available()) {
            StreamMediaInfo sinfo;
            if (StreamExtractor::extract_info(params_.url, sinfo) && !sinfo.formats.empty()) {
                const auto& best = sinfo.formats[0];
                params_.url = best.video_url;
                params_.audio_stream_url = best.audio_url;
                params_.file_size = best.filesize;
                params_.is_streaming_media = true;
                std::string fname = StreamExtractor::sanitize_filename(sinfo.title, best.ext);
                params_.filename = fname;
                fs::path cur_save(gtk_editable_get_text(GTK_EDITABLE(entry_save_as_)));
                params_.destination_dir = cur_save.parent_path().string();
                if (params_.destination_dir.empty()) {
                    params_.destination_dir = (fs::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string();
                }
                params_.save_as_path = (fs::path(params_.destination_dir) / fname).string();
            }
        }

        params_.save_as_path = gtk_editable_get_text(GTK_EDITABLE(entry_save_as_));
        params_.description = gtk_editable_get_text(GTK_EDITABLE(entry_desc_));

        fs::path p(params_.save_as_path);
        params_.destination_dir = p.parent_path().string();
        params_.filename = p.filename().string();

        // Remember path for category if checkbox is checked
        if (chk_remember_path_ && gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_remember_path_))) {
            save_category_dir(params_.category, params_.destination_dir);
        }
    }

    destroyed_ = true;
    if (alive_token_) {
        alive_token_->store(false);
    }

    if (callback_) {
        callback_(action, params_);
        callback_ = nullptr;
    }

    if (dialog_) {
        gtk_window_destroy(GTK_WINDOW(dialog_));
        dialog_ = nullptr;
    }
}

bool DownloadInfoDialog::save_snapshot(const std::string& filename) {
    if (!dialog_) return false;
    GtkWidget* target = gtk_window_get_child(GTK_WINDOW(dialog_));
    if (!target) target = dialog_;

    GdkPaintable* paintable = gtk_widget_paintable_new(target);
    int w = gtk_widget_get_width(target);
    int h = gtk_widget_get_height(target);
    if (w <= 0) w = 580;
    if (h <= 0) h = 270;

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

} // namespace opendm
