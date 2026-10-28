#include "download_complete_dialog.hpp"
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

} // namespace

void DownloadCompleteDialog::show(GtkWindow* parent, std::shared_ptr<DownloadSession> session) {
    if (!session) return;
    auto* dlg = new DownloadCompleteDialog(parent, session);
    dlg->setup_ui();
}

DownloadCompleteDialog::DownloadCompleteDialog(GtkWindow* parent, std::shared_ptr<DownloadSession> session)
    : parent_(parent), session_(std::move(session)) {}

void DownloadCompleteDialog::setup_ui() {
    window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window_), "Download Complete");
    gtk_window_set_default_size(GTK_WINDOW(window_), 420, 190);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    }

    g_object_set_data_full(G_OBJECT(window_), "opendm_complete_dlg", this, +[](gpointer d) {
        delete static_cast<DownloadCompleteDialog*>(d);
    });

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(main_vbox, 16);
    gtk_widget_set_margin_end(main_vbox, 16);
    gtk_widget_set_margin_top(main_vbox, 16);
    gtk_widget_set_margin_bottom(main_vbox, 16);
    gtk_window_set_child(GTK_WINDOW(window_), main_vbox);

    // Header with icon and filename
    GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* icon = gtk_image_new_from_icon_name("document-save-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 36);
    gtk_box_append(GTK_BOX(header_box), icon);

    GtkWidget* info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(info_box, TRUE);

    std::string fn = session_->get_filename();
    std::string dest = session_->get_destination_path();
    int64_t sz = session_->get_total_size();

    GtkWidget* lbl_fn = gtk_label_new(nullptr);
    std::string markup = "<b>" + fn + "</b>";
    gtk_label_set_markup(GTK_LABEL(lbl_fn), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_fn), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_fn), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(info_box), lbl_fn);

    std::string details = "Size: " + format_size(sz);
    GtkWidget* lbl_size = gtk_label_new(details.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_size), 0.0f);
    gtk_box_append(GTK_BOX(info_box), lbl_size);

    GtkWidget* lbl_path = gtk_label_new(dest.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_path), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_path), PANGO_ELLIPSIZE_START);
    gtk_label_set_selectable(GTK_LABEL(lbl_path), TRUE);
    gtk_widget_add_css_class(lbl_path, "dim-label");
    gtk_box_append(GTK_BOX(info_box), lbl_path);

    gtk_box_append(GTK_BOX(header_box), info_box);
    gtk_box_append(GTK_BOX(main_vbox), header_box);

    // Button actions
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);

    GtkWidget* btn_open = gtk_button_new_with_label("Open");
    GtkWidget* btn_folder = gtk_button_new_with_label("Open Folder");
    GtkWidget* btn_close = gtk_button_new_with_label("Close");

    gtk_box_append(GTK_BOX(btn_box), btn_open);
    gtk_box_append(GTK_BOX(btn_box), btn_folder);
    gtk_box_append(GTK_BOX(btn_box), btn_close);
    gtk_box_append(GTK_BOX(main_vbox), btn_box);

    g_signal_connect_swapped(btn_open, "clicked", G_CALLBACK(+[](DownloadCompleteDialog* self) {
        self->on_open_file();
    }), this);

    g_signal_connect_swapped(btn_folder, "clicked", G_CALLBACK(+[](DownloadCompleteDialog* self) {
        self->on_open_folder();
    }), this);

    g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(+[](DownloadCompleteDialog* self) {
        self->close();
    }), this);

    gtk_window_present(GTK_WINDOW(window_));
}

void DownloadCompleteDialog::on_open_file() {
    if (session_) {
        std::string path = session_->get_destination_path();
        std::string uri = "file://" + path;
        g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
    }
    close();
}

void DownloadCompleteDialog::on_open_folder() {
    if (session_) {
        std::string path = session_->get_destination_path();
        std::string parent_dir = std::filesystem::path(path).parent_path().string();
        std::string uri = "file://" + parent_dir;
        g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
    }
    close();
}

void DownloadCompleteDialog::close() {
    if (window_) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

} // namespace opendm
