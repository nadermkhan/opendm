#include "add_url_dialog.hpp"
#include "ui_assets.hpp"
#include <filesystem>

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
    bool is_yt = (u.find("youtube.com/watch") != std::string::npos ||
                  u.find("youtu.be/") != std::string::npos ||
                  u.find("youtube.com/shorts/") != std::string::npos ||
                  u.find("youtube.com/live/") != std::string::npos);
    if (is_yt) {
        GtkWidget* alert = gtk_window_new();
        gtk_window_set_title(GTK_WINDOW(alert), "OpenDM - Web Video Download");
        gtk_window_set_modal(GTK_WINDOW(alert), TRUE);
        gtk_window_set_transient_for(GTK_WINDOW(alert), GTK_WINDOW(dialog_));
        gtk_window_set_default_size(GTK_WINDOW(alert), 420, 200);
        gtk_window_set_resizable(GTK_WINDOW(alert), FALSE);

        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_start(vbox, 16);
        gtk_widget_set_margin_end(vbox, 16);
        gtk_widget_set_margin_top(vbox, 16);
        gtk_widget_set_margin_bottom(vbox, 16);
        gtk_window_set_child(GTK_WINDOW(alert), vbox);

        GtkWidget* msg_lbl = gtk_label_new(
            "OpenDM captures video and audio streams dynamically using its browser extension.\n\n"
            "To download this YouTube video with audio:\n"
            "  1. Play the video in your web browser (Chrome, Firefox, Edge, etc.)\n"
            "  2. Click the 'Download this video' panel located above the video player.\n\n"
            "Would you like to open this video in your browser now?"
        );
        gtk_label_set_wrap(GTK_LABEL(msg_lbl), TRUE);
        gtk_label_set_xalign(GTK_LABEL(msg_lbl), 0.0f);
        gtk_box_append(GTK_BOX(vbox), msg_lbl);

        GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
        gtk_widget_set_margin_top(btn_box, 10);

        GtkWidget* btn_open = gtk_button_new_with_label("Open in Browser");
        gtk_widget_add_css_class(btn_open, "suggested-action");
        GtkWidget* btn_anyway = gtk_button_new_with_label("Download Webpage");
        GtkWidget* btn_cancel = gtk_button_new_with_label("Cancel");

        gtk_box_append(GTK_BOX(btn_box), btn_open);
        gtk_box_append(GTK_BOX(btn_box), btn_anyway);
        gtk_box_append(GTK_BOX(btn_box), btn_cancel);
        gtk_box_append(GTK_BOX(vbox), btn_box);

        struct AlertData {
            AddUrlDialog* self;
            GtkWidget* alert_win;
            AddUrlResult res;
        };
        auto* data = new AlertData{this, alert, res};

        g_signal_connect_swapped(btn_open, "clicked", G_CALLBACK(+[](AlertData* d) {
            std::string open_cmd = "xdg-open \"" + d->res.url + "\" >/dev/null 2>&1 &";
            (void)system(open_cmd.c_str());
            gtk_window_destroy(GTK_WINDOW(d->alert_win));
            gtk_window_destroy(GTK_WINDOW(d->self->dialog_));
            delete d->self;
            delete d;
        }), data);

        g_signal_connect_swapped(btn_anyway, "clicked", G_CALLBACK(+[](AlertData* d) {
            if (d->self->callback_) {
                d->self->callback_(d->res);
            }
            gtk_window_destroy(GTK_WINDOW(d->alert_win));
            gtk_window_destroy(GTK_WINDOW(d->self->dialog_));
            delete d->self;
            delete d;
        }), data);

        g_signal_connect_swapped(btn_cancel, "clicked", G_CALLBACK(+[](AlertData* d) {
            gtk_window_destroy(GTK_WINDOW(d->alert_win));
            delete d;
        }), data);

        gtk_window_present(GTK_WINDOW(alert));
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
