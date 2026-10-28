#include "properties_dialog.hpp"
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace opendm {

namespace {

std::string format_size(int64_t bytes) {
    if (bytes < 0) return "Unknown size";
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int idx = 0;
    double d = static_cast<double>(bytes);
    while (d >= 1024.0 && idx < 4) {
        d /= 1024.0;
        idx++;
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << d << " " << units[idx];
    return ss.str();
}

std::string status_to_string(SessionStatus st) {
    switch (st) {
        case SessionStatus::IDLE: return "Idle / Queued";
        case SessionStatus::PROBING: return "Connecting / Probing";
        case SessionStatus::DOWNLOADING: return "Downloading";
        case SessionStatus::PAUSED: return "Paused";
        case SessionStatus::ASSEMBLING: return "Assembling / Stitching";
        case SessionStatus::COMPLETED: return "Completed";
        case SessionStatus::ERROR: return "Error";
    }
    return "Unknown";
}

} // namespace

void PropertiesDialog::show(GtkWindow* parent, std::shared_ptr<DownloadSession> session) {
    if (!session) return;
    auto* dlg = new PropertiesDialog(parent, session);
    dlg->setup_ui();
}

PropertiesDialog::PropertiesDialog(GtkWindow* parent, std::shared_ptr<DownloadSession> session)
    : parent_(parent), session_(std::move(session)) {}

void PropertiesDialog::setup_ui() {
    window_ = gtk_window_new();
    std::string title = "Download Properties - " + session_->get_filename();
    gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(window_), 520, 360);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    }

    g_object_set_data_full(G_OBJECT(window_), "opendm_props_dlg", this, +[](gpointer d) {
        delete static_cast<PropertiesDialog*>(d);
    });

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(main_vbox, 16);
    gtk_widget_set_margin_end(main_vbox, 16);
    gtk_widget_set_margin_top(main_vbox, 16);
    gtk_widget_set_margin_bottom(main_vbox, 16);
    gtk_window_set_child(GTK_WINDOW(window_), main_vbox);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_box_append(GTK_BOX(main_vbox), grid);

    int row = 0;
    auto add_prop = [&](const char* label_text, const std::string& val_text) {
        GtkWidget* lbl = gtk_label_new(label_text);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_add_css_class(lbl, "bold-label");
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget* val = gtk_label_new(val_text.c_str());
        gtk_label_set_xalign(GTK_LABEL(val), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(val), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(val), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(val, TRUE);
        gtk_grid_attach(GTK_GRID(grid), val, 1, row, 1, 1);
        row++;
    };

    add_prop("File Name:", session_->get_filename());
    add_prop("Category:", session_->get_category());
    add_prop("Status:", status_to_string(session_->get_status()));
    add_prop("File Size:", format_size(session_->get_total_size()));
    add_prop("Resumable:", session_->supports_ranges() ? "Yes" : "No");
    add_prop("URL:", session_->get_url());
    add_prop("Save To:", session_->get_destination_path());

    std::string lm = session_->get_last_modified();
    if (!lm.empty()) {
        add_prop("Last-Modified:", lm);
    }
    std::string et = session_->get_etag();
    if (!et.empty()) {
        add_prop("ETag:", et);
    }

    std::string err = session_->get_last_error();
    if (!err.empty()) {
        add_prop("Last Error:", err);
    }

    // Button actions
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 12);

    GtkWidget* btn_folder = gtk_button_new_with_label("Open Folder");
    GtkWidget* btn_close = gtk_button_new_with_label("Close");

    gtk_box_append(GTK_BOX(btn_box), btn_folder);
    gtk_box_append(GTK_BOX(btn_box), btn_close);
    gtk_box_append(GTK_BOX(main_vbox), btn_box);

    g_signal_connect_swapped(btn_folder, "clicked", G_CALLBACK(+[](PropertiesDialog* self) {
        if (self->session_) {
            std::string path = self->session_->get_destination_path();
            std::string parent_dir = std::filesystem::path(path).parent_path().string();
            std::string uri = "file://" + parent_dir;
            g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
        }
        self->close();
    }), this);

    g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(+[](PropertiesDialog* self) {
        self->close();
    }), this);

    gtk_window_present(GTK_WINDOW(window_));
}

void PropertiesDialog::close() {
    if (window_) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

} // namespace opendm
