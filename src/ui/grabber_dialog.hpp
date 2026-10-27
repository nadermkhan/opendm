#pragma once

#include <gtk/gtk.h>
#include <string>
#include <vector>
#include <memory>
#include "../core/site_grabber.hpp"

namespace opendm {

class GrabberDialog {
public:
    explicit GrabberDialog(GtkWindow* parent = nullptr);
    ~GrabberDialog();

    void show();
    void close();

private:
    void setup_ui();
    void on_start_crawl_clicked();
    void on_stop_crawl_clicked();
    void on_download_all_clicked();

    GtkWindow* parent_{nullptr};
    GtkWidget* window_{nullptr};
    GtkWidget* url_entry_{nullptr};
    GtkWidget* depth_spin_{nullptr};
    GtkWidget* domain_combo_{nullptr};
    GtkWidget* chk_video_{nullptr};
    GtkWidget* chk_audio_{nullptr};
    GtkWidget* chk_compressed_{nullptr};
    GtkWidget* chk_docs_{nullptr};
    GtkWidget* chk_images_{nullptr};

    GtkWidget* btn_start_{nullptr};
    GtkWidget* btn_stop_{nullptr};
    GtkWidget* btn_download_all_{nullptr};
    GtkWidget* status_label_{nullptr};
    GtkWidget* results_container_{nullptr};

    SiteGrabber grabber_;
    std::vector<DiscoveredFile> discovered_;
};

} // namespace opendm
