#include "youtube_format_dialog.hpp"
#include "ui_assets.hpp"
#include <filesystem>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

namespace opendm {

namespace {

std::string get_default_downloads_folder() {
    const char* home = getenv("HOME");
    std::string base = home ? home : "/home/nader";
    return (fs::path(base) / "Downloads").string();
}

} // namespace

void YouTubeFormatDialog::show(GtkWindow* parent, const YouTubeVideoInfo& info, Callback callback) {
    auto* dlg = new YouTubeFormatDialog(parent, info, callback);
    dlg->setup_ui();
}

YouTubeFormatDialog::YouTubeFormatDialog(GtkWindow* parent, const YouTubeVideoInfo& info, Callback callback)
    : parent_(parent), info_(info), callback_(callback) {
    build_quality_options();
}

YouTubeFormatDialog::~YouTubeFormatDialog() {
    destroyed_ = true;
}

std::string YouTubeFormatDialog::sanitize_filename(const std::string& name) {
    std::string clean;
    clean.reserve(name.size());
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) {
            clean += '_';
        } else {
            clean += c;
        }
    }
    // Trim leading/trailing whitespace and periods
    size_t start = clean.find_first_not_of(" ._");
    if (start == std::string::npos) return "youtube_video";
    size_t end = clean.find_last_not_of(" ._");
    clean = clean.substr(start, end - start + 1);
    if (clean.empty()) return "youtube_video";
    return clean;
}

void YouTubeFormatDialog::build_quality_options() {
    options_.clear();

    // 1. Adaptive video formats (muxed with best audio)
    if (!info_.video_formats.empty() && !info_.audio_formats.empty()) {
        const auto& best_audio = info_.best_audio;
        for (const auto& vf : info_.video_formats) {
            YouTubeQualityOption opt;
            uint64_t total = vf.content_length + (best_audio.content_length > 0 ? best_audio.content_length : 0);
            opt.total_size = total;
            opt.video_url = vf.url;
            opt.audio_url = best_audio.url;
            opt.user_agent = vf.user_agent.empty() ? best_audio.user_agent : vf.user_agent;
            opt.container = (vf.container == "MP4") ? "mp4" : "mkv";
            opt.is_audio_only = false;
            opt.is_muxed = true;

            std::string label = vf.quality_label;
            if (vf.fps > 30) {
                label += " (" + std::to_string(vf.fps) + "fps, " + vf.container + ")";
            } else {
                label += " (" + vf.container + ")";
            }
            label += " [Video + " + YouTubeEngine::format_bitrate(best_audio.bitrate) + " Audio]";
            if (total > 0) {
                label += " ~ " + DownloadInfoDialog::format_size(total);
            }
            opt.label = label;
            options_.push_back(opt);
        }
    }

    // 2. Progressive formats (single stream video+audio)
    for (const auto& pf : info_.progressive_formats) {
        YouTubeQualityOption opt;
        opt.total_size = pf.content_length;
        opt.video_url = pf.url;
        opt.audio_url = "";
        opt.user_agent = pf.user_agent;
        opt.container = "mp4";
        opt.is_audio_only = false;
        opt.is_muxed = false;

        std::string label = pf.quality_label + " (" + pf.container + " - Progressive)";
        if (pf.content_length > 0) {
            label += " ~ " + DownloadInfoDialog::format_size(pf.content_length);
        }
        opt.label = label;
        options_.push_back(opt);
    }

    // 3. Audio only formats
    for (const auto& af : info_.audio_formats) {
        YouTubeQualityOption opt;
        opt.total_size = af.content_length;
        opt.video_url = af.url;
        opt.audio_url = "";
        opt.user_agent = af.user_agent;
        opt.container = (af.container == "M4A") ? "m4a" : "opus";
        opt.is_audio_only = true;
        opt.is_muxed = false;

        std::string label = "Audio Only - " + af.container + " (" + YouTubeEngine::format_bitrate(af.bitrate) + ")";
        if (af.content_length > 0) {
            label += " ~ " + DownloadInfoDialog::format_size(af.content_length);
        }
        opt.label = label;
        options_.push_back(opt);
    }

    if (options_.empty()) {
        YouTubeQualityOption fallback;
        fallback.label = "Default Stream";
        fallback.container = "mp4";
        options_.push_back(fallback);
    }
}

void YouTubeFormatDialog::setup_ui() {
    dialog_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog_), "OpenDM - Download YouTube Video");
    gtk_window_set_modal(GTK_WINDOW(dialog_), TRUE);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog_), parent_);
    }
    gtk_window_set_default_size(GTK_WINDOW(dialog_), 540, 360);
    gtk_window_set_resizable(GTK_WINDOW(dialog_), FALSE);

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(main_vbox, 18);
    gtk_widget_set_margin_end(main_vbox, 18);
    gtk_widget_set_margin_top(main_vbox, 16);
    gtk_widget_set_margin_bottom(main_vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dialog_), main_vbox);

    // Header banner with IDM Video icon and metadata
    GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* icon = UiAssets::create_image("cat_video_32.png", 40, 40, "video-x-generic");
    gtk_box_append(GTK_BOX(header_box), icon);

    GtkWidget* meta_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget* lbl_title = gtk_label_new(nullptr);
    std::string title_markup = "<b>" + info_.title + "</b>";
    gtk_label_set_markup(GTK_LABEL(lbl_title), title_markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(lbl_title), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl_title), 50);
    gtk_label_set_lines(GTK_LABEL(lbl_title), 2);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_title), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(meta_vbox), lbl_title);

    std::string meta_line = "Channel: " + (info_.author.empty() ? "YouTube" : info_.author) +
                            "  |  Duration: " + YouTubeEngine::format_duration(info_.duration_seconds) +
                            "  |  Client: " + (info_.client_used.empty() ? "ANDROID" : info_.client_used);
    GtkWidget* lbl_sub = gtk_label_new(meta_line.c_str());
    gtk_widget_add_css_class(lbl_sub, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(lbl_sub), 0.0f);
    gtk_box_append(GTK_BOX(meta_vbox), lbl_sub);

    gtk_box_append(GTK_BOX(header_box), meta_vbox);
    gtk_box_append(GTK_BOX(main_vbox), header_box);

    // Separator
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(main_vbox), sep);

    // Settings grid
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(main_vbox), grid);

    // 1. Quality dropdown
    GtkWidget* lbl_q = gtk_label_new("Quality / Format:");
    gtk_label_set_xalign(GTK_LABEL(lbl_q), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl_q, 0, 0, 1, 1);

    combo_quality_ = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(combo_quality_, TRUE);
    for (const auto& opt : options_) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_quality_), opt.label.c_str());
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_quality_), 0);
    gtk_grid_attach(GTK_GRID(grid), combo_quality_, 1, 0, 2, 1);

    // 2. File size label
    GtkWidget* lbl_s_tag = gtk_label_new("Estimated Size:");
    gtk_label_set_xalign(GTK_LABEL(lbl_s_tag), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl_s_tag, 0, 1, 1, 1);

    lbl_size_ = gtk_label_new(options_.empty() || options_[0].total_size == 0
                                  ? "Unknown"
                                  : DownloadInfoDialog::format_size(options_[0].total_size).c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_size_), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl_size_, 1, 1, 2, 1);

    // 3. Filename entry
    GtkWidget* lbl_fn = gtk_label_new("Save As:");
    gtk_label_set_xalign(GTK_LABEL(lbl_fn), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl_fn, 0, 2, 1, 1);

    std::string base_title = sanitize_filename(info_.title);
    std::string init_ext = options_.empty() ? ".mp4" : ("." + options_[0].container);
    std::string init_fname = base_title + init_ext;

    entry_filename_ = gtk_entry_new();
    gtk_widget_set_hexpand(entry_filename_, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry_filename_), init_fname.c_str());
    gtk_grid_attach(GTK_GRID(grid), entry_filename_, 1, 2, 2, 1);

    // 4. Save Folder entry + Browse button
    GtkWidget* lbl_dir = gtk_label_new("Save To:");
    gtk_label_set_xalign(GTK_LABEL(lbl_dir), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl_dir, 0, 3, 1, 1);

    entry_dir_ = gtk_entry_new();
    gtk_widget_set_hexpand(entry_dir_, TRUE);
    std::string default_dir = DownloadInfoDialog::load_category_dir("Video");
    if (default_dir.empty()) default_dir = get_default_downloads_folder();
    gtk_editable_set_text(GTK_EDITABLE(entry_dir_), default_dir.c_str());
    gtk_grid_attach(GTK_GRID(grid), entry_dir_, 1, 3, 1, 1);

    GtkWidget* btn_browse = gtk_button_new_with_label("Browse...");
    gtk_grid_attach(GTK_GRID(grid), btn_browse, 2, 3, 1, 1);

    g_signal_connect_swapped(btn_browse, "clicked", G_CALLBACK(+[](YouTubeFormatDialog* self) {
        self->on_browse_clicked();
    }), this);

    // 5. Options: Start immediately
    chk_start_now_ = gtk_check_button_new_with_label("Start download immediately");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_start_now_), TRUE);
    gtk_grid_attach(GTK_GRID(grid), chk_start_now_, 0, 4, 3, 1);

    // Handle combo quality selection change
    g_signal_connect_swapped(combo_quality_, "changed", G_CALLBACK(+[](YouTubeFormatDialog* self) {
        self->on_quality_changed();
    }), this);

    // Buttons: Download, Download Later, Cancel
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 10);

    GtkWidget* btn_download = gtk_button_new_with_label("Download");
    gtk_widget_add_css_class(btn_download, "suggested-action");
    GtkWidget* btn_later = gtk_button_new_with_label("Download Later");
    GtkWidget* btn_cancel = gtk_button_new_with_label("Cancel");

    gtk_box_append(GTK_BOX(btn_box), btn_download);
    gtk_box_append(GTK_BOX(btn_box), btn_later);
    gtk_box_append(GTK_BOX(btn_box), btn_cancel);
    gtk_box_append(GTK_BOX(main_vbox), btn_box);

    g_signal_connect_swapped(btn_download, "clicked", G_CALLBACK(+[](YouTubeFormatDialog* self) {
        bool start_now = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->chk_start_now_));
        self->on_action_clicked(start_now ? DownloadInfoAction::START_NOW : DownloadInfoAction::DOWNLOAD_LATER);
    }), this);

    g_signal_connect_swapped(btn_later, "clicked", G_CALLBACK(+[](YouTubeFormatDialog* self) {
        self->on_action_clicked(DownloadInfoAction::DOWNLOAD_LATER);
    }), this);

    g_signal_connect_swapped(btn_cancel, "clicked", G_CALLBACK(+[](YouTubeFormatDialog* self) {
        self->on_action_clicked(DownloadInfoAction::CANCEL);
    }), this);

    gtk_window_present(GTK_WINDOW(dialog_));
}

void YouTubeFormatDialog::on_quality_changed() {
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo_quality_));
    if (idx < 0 || idx >= static_cast<int>(options_.size())) return;
    selected_option_index_ = idx;

    const auto& opt = options_[idx];
    if (opt.total_size > 0) {
        gtk_label_set_text(GTK_LABEL(lbl_size_), DownloadInfoDialog::format_size(opt.total_size).c_str());
    } else {
        gtk_label_set_text(GTK_LABEL(lbl_size_), "Unknown");
    }

    // Update extension in filename entry
    std::string cur_name = gtk_editable_get_text(GTK_EDITABLE(entry_filename_));
    size_t dot = cur_name.find_last_of('.');
    std::string stem = (dot != std::string::npos) ? cur_name.substr(0, dot) : cur_name;
    std::string new_name = stem + "." + opt.container;
    gtk_editable_set_text(GTK_EDITABLE(entry_filename_), new_name.c_str());

    // Switch default folder to Music if audio only, or Video if video
    if (opt.is_audio_only) {
        std::string music_dir = DownloadInfoDialog::load_category_dir("Music");
        if (!music_dir.empty()) {
            gtk_editable_set_text(GTK_EDITABLE(entry_dir_), music_dir.c_str());
        }
    } else {
        std::string video_dir = DownloadInfoDialog::load_category_dir("Video");
        if (!video_dir.empty()) {
            gtk_editable_set_text(GTK_EDITABLE(entry_dir_), video_dir.c_str());
        }
    }
}

void YouTubeFormatDialog::on_browse_clicked() {
    GtkFileChooserNative* native = gtk_file_chooser_native_new(
        "Select Destination Directory",
        dialog_ ? GTK_WINDOW(dialog_) : parent_,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Select",
        "_Cancel"
    );

    std::string cur_dir = gtk_editable_get_text(GTK_EDITABLE(entry_dir_));
    if (!cur_dir.empty() && fs::exists(cur_dir)) {
        GFile* folder = g_file_new_for_path(cur_dir.c_str());
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(native), folder, nullptr);
        g_object_unref(folder);
    }

    g_signal_connect(native, "response", G_CALLBACK(+[](GtkNativeDialog* dlg, int response_id, gpointer user_data) {
        if (response_id == GTK_RESPONSE_ACCEPT) {
            GtkFileChooser* chooser = GTK_FILE_CHOOSER(dlg);
            GFile* file = gtk_file_chooser_get_file(chooser);
            if (file) {
                char* chosen_path = g_file_get_path(file);
                if (chosen_path) {
                    auto* self = static_cast<YouTubeFormatDialog*>(user_data);
                    if (self && !self->destroyed_) {
                        gtk_editable_set_text(GTK_EDITABLE(self->entry_dir_), chosen_path);
                    }
                    g_free(chosen_path);
                }
                g_object_unref(file);
            }
        }
        g_object_unref(dlg);
    }), this);

    gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));
}

void YouTubeFormatDialog::on_action_clicked(DownloadInfoAction action) {
    if (action == DownloadInfoAction::CANCEL) {
        gtk_window_destroy(GTK_WINDOW(dialog_));
        delete this;
        return;
    }

    if (selected_option_index_ < 0 || selected_option_index_ >= static_cast<int>(options_.size())) {
        gtk_window_destroy(GTK_WINDOW(dialog_));
        delete this;
        return;
    }

    const auto& opt = options_[selected_option_index_];
    std::string chosen_dir = gtk_editable_get_text(GTK_EDITABLE(entry_dir_));
    std::string chosen_fname = gtk_editable_get_text(GTK_EDITABLE(entry_filename_));
    if (chosen_fname.empty()) {
        chosen_fname = "video." + opt.container;
    }
    if (chosen_dir.empty()) {
        chosen_dir = get_default_downloads_folder();
    }

    DownloadPromptInfo prompt;
    prompt.url = opt.video_url;
    prompt.audio_stream_url = opt.audio_url;
    prompt.destination_dir = chosen_dir;
    prompt.filename = chosen_fname;
    prompt.save_as_path = (fs::path(chosen_dir) / chosen_fname).string();
    prompt.category = opt.is_audio_only ? "Music" : "Video";
    prompt.file_size = opt.total_size;
    prompt.user_agent = opt.user_agent;
    prompt.referer = "https://www.youtube.com/";
    prompt.is_streaming_media = true;
    prompt.description = info_.title;

    if (callback_) {
        callback_(action, prompt);
    }

    gtk_window_destroy(GTK_WINDOW(dialog_));
    delete this;
}

} // namespace opendm
