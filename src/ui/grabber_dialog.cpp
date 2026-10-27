#include "grabber_dialog.hpp"
#include "../core/download_queue.hpp"
#include <iostream>
#include <sstream>
#include <filesystem>

namespace opendm {

GrabberDialog::GrabberDialog(GtkWindow* parent) : parent_(parent) {
    setup_ui();
}

GrabberDialog::~GrabberDialog() {
    grabber_.stop();
}

void GrabberDialog::show() {
    gtk_window_present(GTK_WINDOW(window_));
}

void GrabberDialog::close() {
    grabber_.stop();
    gtk_window_destroy(GTK_WINDOW(window_));
}

void GrabberDialog::setup_ui() {
    window_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window_), "OpenDM Site Grabber (Web Spider)");
    gtk_window_set_default_size(GTK_WINDOW(window_), 720, 540);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(window_), parent_);
    }

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(window_), vbox);

    // Configuration Frame
    GtkWidget* cfg_frame = gtk_frame_new("Crawl Settings");
    gtk_box_append(GTK_BOX(vbox), cfg_frame);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_start(grid, 12);
    gtk_widget_set_margin_end(grid, 12);
    gtk_widget_set_margin_top(grid, 8);
    gtk_widget_set_margin_bottom(grid, 8);
    gtk_frame_set_child(GTK_FRAME(cfg_frame), grid);

    // URL
    GtkWidget* lbl_url = gtk_label_new("Start Webpage Address:");
    gtk_label_set_xalign(GTK_LABEL(lbl_url), 0.0f);
    url_entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url_entry_), "https://example.com");
    gtk_widget_set_hexpand(url_entry_, TRUE);
    gtk_grid_attach(GTK_GRID(grid), lbl_url, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), url_entry_, 1, 0, 2, 1);

    // Depth
    GtkWidget* lbl_depth = gtk_label_new("Explore Depth Levels:");
    gtk_label_set_xalign(GTK_LABEL(lbl_depth), 0.0f);
    depth_spin_ = gtk_spin_button_new_with_range(1, 5, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(depth_spin_), 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_depth, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), depth_spin_, 1, 1, 1, 1);

    // Domain scope
    GtkWidget* lbl_domain = gtk_label_new("Domain Boundary:");
    gtk_label_set_xalign(GTK_LABEL(lbl_domain), 0.0f);
    domain_combo_ = gtk_drop_down_new_from_strings((const char*[]){
        "Stay on same domain only",
        "Include all subdomains",
        "Any domain",
        nullptr
    });
    gtk_grid_attach(GTK_GRID(grid), lbl_domain, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), domain_combo_, 1, 2, 2, 1);

    // Filter check boxes
    GtkWidget* filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("File Types to Grab:"), 0, 3, 1, 1);

    chk_video_ = gtk_check_button_new_with_label("Video");
    chk_audio_ = gtk_check_button_new_with_label("Audio");
    chk_compressed_ = gtk_check_button_new_with_label("Archives");
    chk_docs_ = gtk_check_button_new_with_label("Documents");
    chk_images_ = gtk_check_button_new_with_label("Images");

    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_video_), TRUE);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_audio_), TRUE);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_compressed_), TRUE);

    gtk_box_append(GTK_BOX(filter_box), chk_video_);
    gtk_box_append(GTK_BOX(filter_box), chk_audio_);
    gtk_box_append(GTK_BOX(filter_box), chk_compressed_);
    gtk_box_append(GTK_BOX(filter_box), chk_docs_);
    gtk_box_append(GTK_BOX(filter_box), chk_images_);
    gtk_grid_attach(GTK_GRID(grid), filter_box, 1, 3, 2, 1);

    // Action buttons for crawl
    GtkWidget* crawl_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(vbox), crawl_btn_box);

    btn_start_ = gtk_button_new_with_label("▶ Start Crawl");
    btn_stop_ = gtk_button_new_with_label("⏹ Stop");
    gtk_widget_set_sensitive(btn_stop_, FALSE);
    gtk_box_append(GTK_BOX(crawl_btn_box), btn_start_);
    gtk_box_append(GTK_BOX(crawl_btn_box), btn_stop_);

    status_label_ = gtk_label_new("Ready. Enter seed URL and click Start Crawl.");
    gtk_label_set_xalign(GTK_LABEL(status_label_), 0.0f);
    gtk_widget_set_hexpand(status_label_, TRUE);
    gtk_box_append(GTK_BOX(crawl_btn_box), status_label_);

    // Results area
    GtkWidget* results_frame = gtk_frame_new("Discovered Files");
    gtk_widget_set_vexpand(results_frame, TRUE);
    gtk_box_append(GTK_BOX(vbox), results_frame);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_frame_set_child(GTK_FRAME(results_frame), scroll);

    results_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(results_container_, 6);
    gtk_widget_set_margin_end(results_container_, 6);
    gtk_widget_set_margin_top(results_container_, 6);
    gtk_widget_set_margin_bottom(results_container_, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), results_container_);

    // Bottom Action Bar
    GtkWidget* bottom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(bottom_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), bottom_box);

    btn_download_all_ = gtk_button_new_with_label("Add All Found to Download Queue");
    gtk_widget_set_sensitive(btn_download_all_, FALSE);
    gtk_box_append(GTK_BOX(bottom_box), btn_download_all_);

    GtkWidget* btn_close = gtk_button_new_with_label("Close");
    gtk_box_append(GTK_BOX(bottom_box), btn_close);

    g_signal_connect_swapped(btn_start_, "clicked", G_CALLBACK(+[](GrabberDialog* self) {
        self->on_start_crawl_clicked();
    }), this);

    g_signal_connect_swapped(btn_stop_, "clicked", G_CALLBACK(+[](GrabberDialog* self) {
        self->on_stop_crawl_clicked();
    }), this);

    g_signal_connect_swapped(btn_download_all_, "clicked", G_CALLBACK(+[](GrabberDialog* self) {
        self->on_download_all_clicked();
    }), this);

    g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(+[](GrabberDialog* self) {
        self->close();
    }), this);
}

void GrabberDialog::on_start_crawl_clicked() {
    const char* url_text = gtk_editable_get_text(GTK_EDITABLE(url_entry_));
    if (!url_text || strlen(url_text) == 0) return;

    GrabberConfig cfg;
    cfg.seed_url = url_text;
    cfg.max_depth = static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(depth_spin_)));

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(domain_combo_));
    if (sel == 0) cfg.domain_scope = DomainScope::SAME_DOMAIN_ONLY;
    else if (sel == 1) cfg.domain_scope = DomainScope::INCLUDE_SUBDOMAINS;
    else cfg.domain_scope = DomainScope::ANY_DOMAIN;

    std::vector<std::string> exts;
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_video_))) {
        exts.insert(exts.end(), {"mp4", "mkv", "avi", "webm", "flv", "mov", "wmv", "ts"});
    }
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_audio_))) {
        exts.insert(exts.end(), {"mp3", "flac", "wav", "aac", "ogg", "m4a", "opus"});
    }
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_compressed_))) {
        exts.insert(exts.end(), {"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "iso"});
    }
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_docs_))) {
        exts.insert(exts.end(), {"pdf", "docx", "doc", "xlsx", "xls", "txt", "epub"});
    }
    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_images_))) {
        exts.insert(exts.end(), {"jpg", "jpeg", "png", "webp", "gif", "svg"});
    }
    cfg.file_extensions = exts;

    // Clear old results
    GtkWidget* child = gtk_widget_get_first_child(results_container_);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(results_container_), child);
        child = next;
    }
    discovered_.clear();

    gtk_widget_set_sensitive(btn_start_, FALSE);
    gtk_widget_set_sensitive(btn_stop_, TRUE);
    gtk_widget_set_sensitive(btn_download_all_, FALSE);

    grabber_.start(cfg,
        [this](const DiscoveredFile& f) {
            // Marshall to GLib main thread
            struct FileEvent {
                GrabberDialog* self;
                DiscoveredFile file;
            };
            auto* ev = new FileEvent{this, f};
            g_idle_add(+[](gpointer data) -> gboolean {
                auto* e = static_cast<FileEvent*>(data);
                e->self->discovered_.push_back(e->file);

                GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                GtkWidget* icon = gtk_label_new(("[ " + e->file.category + " ]").c_str());
                GtkWidget* fn = gtk_label_new(e->file.filename.c_str());
                gtk_label_set_xalign(GTK_LABEL(fn), 0.0f);
                gtk_label_set_ellipsize(GTK_LABEL(fn), PANGO_ELLIPSIZE_END);
                gtk_widget_set_hexpand(fn, TRUE);

                gtk_box_append(GTK_BOX(row), icon);
                gtk_box_append(GTK_BOX(row), fn);
                gtk_box_append(GTK_BOX(e->self->results_container_), row);

                gtk_widget_set_sensitive(e->self->btn_download_all_, TRUE);
                delete e;
                return G_SOURCE_REMOVE;
            }, ev);
        },
        [this](const std::string& st, size_t pages, size_t files) {
            struct StatEvent {
                GrabberDialog* self;
                std::string status;
            };
            auto* ev = new StatEvent{this, st};
            g_idle_add(+[](gpointer data) -> gboolean {
                auto* e = static_cast<StatEvent*>(data);
                gtk_label_set_text(GTK_LABEL(e->self->status_label_), e->status.c_str());
                if (e->status == "Crawl complete") {
                    gtk_widget_set_sensitive(e->self->btn_start_, TRUE);
                    gtk_widget_set_sensitive(e->self->btn_stop_, FALSE);
                }
                delete e;
                return G_SOURCE_REMOVE;
            }, ev);
        });
}

void GrabberDialog::on_stop_crawl_clicked() {
    grabber_.stop();
    gtk_widget_set_sensitive(btn_start_, TRUE);
    gtk_widget_set_sensitive(btn_stop_, FALSE);
    gtk_label_set_text(GTK_LABEL(status_label_), "Crawl stopped by user.");
}

void GrabberDialog::on_download_all_clicked() {
    std::string dl_dir = (std::filesystem::path(getenv("HOME") ? getenv("HOME") : "/home/nader") / "Downloads").string();
    static uint64_t counter = 1000;

    for (const auto& df : discovered_) {
        std::string full_dest = dl_dir + "/" + df.filename;
        std::string id = "grabber_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);
        auto session = std::make_shared<DownloadSession>(id, df.url, full_dest);
        QueueManager::instance().get_main_queue().add_session(session, false);
    }

    std::ostringstream ss;
    ss << "Enqueued " << discovered_.size() << " files into Download Queue.";
    gtk_label_set_text(GTK_LABEL(status_label_), ss.str().c_str());
    gtk_widget_set_sensitive(btn_download_all_, FALSE);
}

} // namespace opendm
