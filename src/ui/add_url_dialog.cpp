#include "add_url_dialog.hpp"
#include "ui_assets.hpp"
#include "youtube_format_dialog.hpp"
#include "../core/youtube_engine.hpp"
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace opendm {

void AddUrlDialog::show(GtkWindow* parent, const std::string& default_url, Callback callback) {
    auto* dlg = new AddUrlDialog(parent, default_url, callback);
    dlg->setup_ui();
}

AddUrlDialog::AddUrlDialog(GtkWindow* parent, const std::string& default_url, Callback callback)
    : parent_(parent), default_url_(default_url), callback_(callback) {}

void AddUrlDialog::setup_ui() {
    dialog_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog_), "Enter new address to download");
    gtk_window_set_modal(GTK_WINDOW(dialog_), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog_), parent_);
    gtk_window_set_default_size(GTK_WINDOW(dialog_), 480, 260);
    gtk_window_set_resizable(GTK_WINDOW(dialog_), FALSE);

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(main_vbox, 16);
    gtk_widget_set_margin_end(main_vbox, 16);
    gtk_widget_set_margin_top(main_vbox, 16);
    gtk_widget_set_margin_bottom(main_vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dialog_), main_vbox);

    // Header with authentic IDM add icon
    GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* icon = UiAssets::create_image("toolbar_add_url.png", 36, 36, "list-add");
    GtkWidget* lbl_header = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(lbl_header), "<b>Enter new address to download:</b>");
    gtk_label_set_xalign(GTK_LABEL(lbl_header), 0.0f);
    gtk_box_append(GTK_BOX(header_box), icon);
    gtk_box_append(GTK_BOX(header_box), lbl_header);
    gtk_box_append(GTK_BOX(main_vbox), header_box);

    // URL Entry
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_append(GTK_BOX(main_vbox), grid);

    GtkWidget* lbl_url = gtk_label_new("Address:");
    gtk_label_set_xalign(GTK_LABEL(lbl_url), 0.0f);
    entry_url_ = gtk_entry_new();
    gtk_widget_set_hexpand(entry_url_, TRUE);
    if (!default_url_.empty()) {
        gtk_editable_set_text(GTK_EDITABLE(entry_url_), default_url_.c_str());
    }

    gtk_grid_attach(GTK_GRID(grid), lbl_url, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_url_, 1, 0, 2, 1);

    // Save To directory
    GtkWidget* lbl_path = gtk_label_new("Save To:");
    gtk_label_set_xalign(GTK_LABEL(lbl_path), 0.0f);
    entry_path_ = gtk_entry_new();
    std::string default_dir = (fs::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string();
    gtk_editable_set_text(GTK_EDITABLE(entry_path_), default_dir.c_str());

    gtk_grid_attach(GTK_GRID(grid), lbl_path, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry_path_, 1, 1, 2, 1);

    // Authorization box
    chk_auth_ = gtk_check_button_new_with_label("Use authorization");
    gtk_grid_attach(GTK_GRID(grid), chk_auth_, 0, 2, 3, 1);

    GtkWidget* auth_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(auth_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(auth_grid), 10);
    gtk_widget_set_margin_start(auth_grid, 20);

    GtkWidget* lbl_u = gtk_label_new("User:");
    gtk_label_set_xalign(GTK_LABEL(lbl_u), 0.0f);
    entry_user_ = gtk_entry_new();
    gtk_widget_set_sensitive(entry_user_, FALSE);

    GtkWidget* lbl_p = gtk_label_new("Password:");
    gtk_label_set_xalign(GTK_LABEL(lbl_p), 0.0f);
    entry_pass_ = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry_pass_), FALSE);
    gtk_widget_set_sensitive(entry_pass_, FALSE);

    gtk_grid_attach(GTK_GRID(auth_grid), lbl_u, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(auth_grid), entry_user_, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(auth_grid), lbl_p, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(auth_grid), entry_pass_, 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), auth_grid, 0, 3, 3, 1);

    g_signal_connect(chk_auth_, "toggled", G_CALLBACK(+[](GtkCheckButton* b, gpointer data) {
        auto* self = static_cast<AddUrlDialog*>(data);
        gboolean active = gtk_check_button_get_active(b);
        gtk_widget_set_sensitive(self->entry_user_, active);
        gtk_widget_set_sensitive(self->entry_pass_, active);
    }), this);

    // Options: Start immediately
    chk_start_now_ = gtk_check_button_new_with_label("Start download immediately");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_start_now_), TRUE);
    gtk_grid_attach(GTK_GRID(grid), chk_start_now_, 0, 4, 3, 1);

    // Buttons: OK and Cancel
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 10);

    GtkWidget* btn_ok = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(btn_ok, "suggested-action");
    GtkWidget* btn_cancel = gtk_button_new_with_label("Cancel");

    gtk_box_append(GTK_BOX(btn_box), btn_ok);
    gtk_box_append(GTK_BOX(btn_box), btn_cancel);
    gtk_box_append(GTK_BOX(main_vbox), btn_box);

    g_signal_connect_swapped(btn_ok, "clicked", G_CALLBACK(+[](AddUrlDialog* self) {
        self->on_ok_clicked();
    }), this);

    g_signal_connect_swapped(btn_cancel, "clicked", G_CALLBACK(+[](AddUrlDialog* self) {
        self->on_cancel_clicked();
    }), this);

    // Activate on Enter
    g_signal_connect_swapped(entry_url_, "activate", G_CALLBACK(+[](AddUrlDialog* self) {
        self->on_ok_clicked();
    }), this);

    gtk_window_present(GTK_WINDOW(dialog_));
}

void AddUrlDialog::on_ok_clicked() {
    AddUrlResult res;
    res.accepted = true;
    res.url = gtk_editable_get_text(GTK_EDITABLE(entry_url_));
    res.path = gtk_editable_get_text(GTK_EDITABLE(entry_path_));
    res.start_immediately = gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_start_now_));
    res.use_auth = gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_auth_));
    if (res.use_auth) {
        res.username = gtk_editable_get_text(GTK_EDITABLE(entry_user_));
        res.password = gtk_editable_get_text(GTK_EDITABLE(entry_pass_));
    }

    std::string u = res.url;
    if (YouTubeEngine::is_youtube_url(u)) {
        // Show lightweight loading dialog
        GtkWidget* loading_dlg = gtk_window_new();
        gtk_window_set_title(GTK_WINDOW(loading_dlg), "OpenDM - Analyzing Video");
        gtk_window_set_modal(GTK_WINDOW(loading_dlg), TRUE);
        gtk_window_set_transient_for(GTK_WINDOW(loading_dlg), GTK_WINDOW(dialog_));
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
            AddUrlDialog* self;
            GtkWidget* loading_dlg;
            std::string url;
            std::string path;
            YouTubeVideoInfo info;
            std::string err;
            bool success{false};
        };

        auto* wd = new WorkerData{this, loading_dlg, res.url, res.path, {}, "", false};
        std::thread([wd]() {
            wd->success = YouTubeEngine::extract_video_info(wd->url, wd->info, &wd->err);

            g_idle_add(+[](gpointer p) -> gboolean {
                auto* d = static_cast<WorkerData*>(p);
                gtk_window_destroy(GTK_WINDOW(d->loading_dlg));

                if (d->success && d->info.has_playable_formats()) {
                    GtkWindow* win_parent = d->self->parent_;
                    auto cb = d->self->callback_;
                    gtk_window_destroy(GTK_WINDOW(d->self->dialog_));
                    delete d->self;

                    YouTubeFormatDialog::show(win_parent, d->info, [cb](DownloadInfoAction action, const DownloadPromptInfo& prompt) {
                        if (action == DownloadInfoAction::CANCEL) return;
                        AddUrlResult r;
                        r.accepted = true;
                        r.url = prompt.url;
                        r.audio_stream_url = prompt.audio_stream_url;
                        r.user_agent = prompt.user_agent;
                        r.path = prompt.destination_dir;
                        r.filename = prompt.filename;
                        r.start_immediately = (action == DownloadInfoAction::START_NOW);
                        r.is_streaming_media = true;
                        r.file_size = prompt.file_size;
                        r.bypass_prompt = true;
                        if (cb) cb(r);
                    });
                } else {
                    // Show error dialog with option to open in browser
                    GtkWidget* alert = gtk_window_new();
                    gtk_window_set_title(GTK_WINDOW(alert), "OpenDM - Video Resolution Error");
                    gtk_window_set_modal(GTK_WINDOW(alert), TRUE);
                    gtk_window_set_transient_for(GTK_WINDOW(alert), GTK_WINDOW(d->self->dialog_));
                    gtk_window_set_default_size(GTK_WINDOW(alert), 440, 180);
                    gtk_window_set_resizable(GTK_WINDOW(alert), FALSE);

                    GtkWidget* abox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
                    gtk_widget_set_margin_start(abox, 16);
                    gtk_widget_set_margin_end(abox, 16);
                    gtk_widget_set_margin_top(abox, 16);
                    gtk_widget_set_margin_bottom(abox, 16);
                    gtk_window_set_child(GTK_WINDOW(alert), abox);

                    std::string err_msg = "Could not resolve stream URLs using OpenDM Native Processing Engine:\n" +
                                          (d->err.empty() ? "No playable formats found" : d->err) + "\n\n" +
                                          "Would you like to open this video in your web browser instead?";
                    GtkWidget* l = gtk_label_new(err_msg.c_str());
                    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
                    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
                    gtk_box_append(GTK_BOX(abox), l);

                    GtkWidget* bbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                    gtk_widget_set_halign(bbox, GTK_ALIGN_END);
                    GtkWidget* btn_open = gtk_button_new_with_label("Open in Browser");
                    gtk_widget_add_css_class(btn_open, "suggested-action");
                    GtkWidget* btn_close = gtk_button_new_with_label("Close");
                    gtk_box_append(GTK_BOX(bbox), btn_open);
                    gtk_box_append(GTK_BOX(bbox), btn_close);
                    gtk_box_append(GTK_BOX(abox), bbox);

                    struct AlertCtx { GtkWidget* alert; std::string url; };
                    auto* actx = new AlertCtx{alert, d->url};
                    g_signal_connect_swapped(btn_open, "clicked", G_CALLBACK(+[](AlertCtx* ac) {
                        std::string open_cmd = "xdg-open \"" + ac->url + "\" >/dev/null 2>&1 &";
                        (void)system(open_cmd.c_str());
                        gtk_window_destroy(GTK_WINDOW(ac->alert));
                        delete ac;
                    }), actx);
                    g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(+[](AlertCtx* ac) {
                        gtk_window_destroy(GTK_WINDOW(ac->alert));
                        delete ac;
                    }), actx);
                    gtk_window_present(GTK_WINDOW(alert));
                }
                delete d;
                return G_SOURCE_REMOVE;
            }, wd);
        }).detach();
        return;
    }

    if (callback_) {
        callback_(res);
    }

    gtk_window_destroy(GTK_WINDOW(dialog_));
    delete this;
}

void AddUrlDialog::on_cancel_clicked() {
    gtk_window_destroy(GTK_WINDOW(dialog_));
    delete this;
}

} // namespace opendm
