#pragma once

#include <gtk/gtk.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include "../core/zip_previewer.hpp"

namespace opendm {

class ZipPreviewDialog {
public:
    explicit ZipPreviewDialog(GtkWindow* parent = nullptr, const std::string& initial_url = "");
    ~ZipPreviewDialog();

    void show();
    void close();

private:
    void setup_ui();
    void on_fetch_clicked();
    void on_extract_clicked();
    void populate_entries();

    GtkWindow* parent_{nullptr};
    std::string current_url_;

    GtkWidget* window_{nullptr};
    GtkWidget* url_entry_{nullptr};
    GtkWidget* btn_fetch_{nullptr};
    GtkWidget* btn_extract_{nullptr};
    GtkWidget* status_label_{nullptr};
    GtkWidget* files_container_{nullptr};

    std::vector<ZipEntry> entries_;
    int selected_entry_index_{-1};
    ZipPreviewer previewer_;
    bool destroyed_{false};
    std::shared_ptr<std::atomic<bool>> alive_token_{std::make_shared<std::atomic<bool>>(true)};
};

} // namespace opendm
