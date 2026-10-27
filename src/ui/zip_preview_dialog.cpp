#include "zip_preview_dialog.hpp"
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

} // namespace

ZipPreviewDialog::ZipPreviewDialog(GtkWindow* parent, const std::string& initial_url)
    : parent_(parent), current_url_(initial_url) {
    setup_ui();
}

ZipPreviewDialog::~ZipPreviewDialog() {}

void ZipPreviewDialog::show() {
    gtk_window_present(GTK_WINDOW(window_));
    if (!current_url_.empty()) {
        on_fetch_clicked();
    }
}

void ZipPreviewDialog::close() {
    gtk_window_destroy(GTK_WINDOW(window_));
}

void ZipPreviewDialog::setup_ui() {
    window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window_), "OpenDM - Remote ZIP Content Previewer");
    gtk_window_set_default_size(GTK_WINDOW(window_), 640, 480);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    }

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(window_), vbox);

    // URL Entry + Fetch button
    GtkWidget* url_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(vbox), url_hbox);

    url_entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url_entry_), "https://example.com/archive.zip");
    if (!current_url_.empty()) {
        gtk_editable_set_text(GTK_EDITABLE(url_entry_), current_url_.c_str());
    }
    gtk_widget_set_hexpand(url_entry_, TRUE);
    gtk_box_append(GTK_BOX(url_hbox), url_entry_);

    btn_fetch_ = gtk_button_new_with_label("Inspect Archive");
    gtk_box_append(GTK_BOX(url_hbox), btn_fetch_);

    status_label_ = gtk_label_new("Enter remote ZIP URL to inspect internal files without downloading the archive.");
    gtk_label_set_xalign(GTK_LABEL(status_label_), 0.0f);
    gtk_box_append(GTK_BOX(vbox), status_label_);

    // Scrolled window for entries
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);

    files_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), files_container_);

    // Bottom Action Bar
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), btn_box);

    btn_extract_ = gtk_button_new_with_label("Extract Selected File");
    gtk_widget_set_sensitive(btn_extract_, FALSE);
    gtk_box_append(GTK_BOX(btn_box), btn_extract_);

    GtkWidget* btn_close = gtk_button_new_with_label("Close");
    gtk_box_append(GTK_BOX(btn_box), btn_close);

    g_signal_connect_swapped(btn_fetch_, "clicked", G_CALLBACK(+[](ZipPreviewDialog* self) {
        self->on_fetch_clicked();
    }), this);

    g_signal_connect_swapped(btn_extract_, "clicked", G_CALLBACK(+[](ZipPreviewDialog* self) {
        self->on_extract_clicked();
    }), this);

    g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(+[](ZipPreviewDialog* self) {
        self->close();
    }), this);
}

void ZipPreviewDialog::on_fetch_clicked() {
    const char* text = gtk_editable_get_text(GTK_EDITABLE(url_entry_));
    if (!text || strlen(text) == 0) return;

    current_url_ = text;
    gtk_label_set_text(GTK_LABEL(status_label_), "Sending HTTP Range request for Central Directory...");
    gtk_widget_set_sensitive(btn_fetch_, FALSE);

    std::string err;
    bool ok = previewer_.fetch_entries(current_url_, entries_, err);
    gtk_widget_set_sensitive(btn_fetch_, TRUE);

    if (!ok) {
        gtk_label_set_text(GTK_LABEL(status_label_), ("Error: " + err).c_str());
        return;
    }

    std::ostringstream ss;
    ss << "Found " << entries_.size() << " files in remote archive (Tail Range probed successfully).";
    gtk_label_set_text(GTK_LABEL(status_label_), ss.str().c_str());

    // Clear old rows
    GtkWidget* child = gtk_widget_get_first_child(files_container_);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(files_container_), child);
        child = next;
    }

    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        GtkWidget* row = gtk_frame_new(nullptr);
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(hbox, 8);
        gtk_widget_set_margin_end(hbox, 8);
        gtk_widget_set_margin_top(hbox, 4);
        gtk_widget_set_margin_bottom(hbox, 4);
        gtk_frame_set_child(GTK_FRAME(row), hbox);

        GtkWidget* icon_lbl = gtk_label_new(e.is_directory ? "📁" : "📄");
        gtk_box_append(GTK_BOX(hbox), icon_lbl);

        GtkWidget* fn_lbl = gtk_label_new(e.filename.c_str());
        gtk_label_set_xalign(GTK_LABEL(fn_lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(fn_lbl), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand(fn_lbl, TRUE);
        gtk_box_append(GTK_BOX(hbox), fn_lbl);

        std::string size_str = format_bytes(e.uncompressed_size);
        if (e.compressed_size > 0 && e.compressed_size != e.uncompressed_size) {
            size_str += " (comp: " + format_bytes(e.compressed_size) + ")";
        }
        GtkWidget* sz_lbl = gtk_label_new(size_str.c_str());
        gtk_box_append(GTK_BOX(hbox), sz_lbl);

        // Click gesture for row selection
        g_object_set_data(G_OBJECT(row), "entry_idx", GINT_TO_POINTER(static_cast<int>(i)));
        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int n_press, double x, double y, gpointer user_data) {
            auto* self = static_cast<ZipPreviewDialog*>(user_data);
            GtkWidget* w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
            int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "entry_idx"));
            self->selected_entry_index_ = idx;
            gtk_widget_set_sensitive(self->btn_extract_, !self->entries_[idx].is_directory);
            if (n_press == 2 && !self->entries_[idx].is_directory) {
                self->on_extract_clicked();
            }
        }), this);
        gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

        gtk_box_append(GTK_BOX(files_container_), row);
    }
}

void ZipPreviewDialog::on_extract_clicked() {
    if (selected_entry_index_ < 0 || selected_entry_index_ >= static_cast<int>(entries_.size())) return;

    const auto& entry = entries_[selected_entry_index_];
    std::string base_dir = (std::filesystem::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string();
    std::string dest_file = base_dir + "/" + std::filesystem::path(entry.filename).filename().string();

    gtk_label_set_text(GTK_LABEL(status_label_), ("Extracting " + entry.filename + " via HTTP Range...").c_str());

    std::string err;
    bool ok = previewer_.extract_entry(current_url_, entry, dest_file, nullptr, err);
    if (ok) {
        gtk_label_set_text(GTK_LABEL(status_label_), ("Extracted successfully: " + dest_file).c_str());
    } else {
        gtk_label_set_text(GTK_LABEL(status_label_), ("Extraction error: " + err).c_str());
    }
}

} // namespace opendm
