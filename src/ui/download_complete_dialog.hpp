#pragma once

#include <gtk/gtk.h>
#include <string>
#include <memory>
#include "core/download_session.hpp"

namespace opendm {

class DownloadCompleteDialog {
public:
    static void show(GtkWindow* parent, std::shared_ptr<DownloadSession> session);

private:
    DownloadCompleteDialog(GtkWindow* parent, std::shared_ptr<DownloadSession> session);
    ~DownloadCompleteDialog() = default;

    void setup_ui();
    void on_open_file();
    void on_open_folder();
    void close();

    GtkWindow* parent_{nullptr};
    std::shared_ptr<DownloadSession> session_;
    GtkWidget* window_{nullptr};
};

} // namespace opendm
