#pragma once

#include <gtk/gtk.h>
#include <string>
#include <memory>
#include "core/download_session.hpp"

namespace opendm {

class PropertiesDialog {
public:
    static void show(GtkWindow* parent, std::shared_ptr<DownloadSession> session);

private:
    PropertiesDialog(GtkWindow* parent, std::shared_ptr<DownloadSession> session);
    ~PropertiesDialog() = default;

    void setup_ui();
    void close();

    GtkWindow* parent_{nullptr};
    std::shared_ptr<DownloadSession> session_;
    GtkWidget* window_{nullptr};
};

} // namespace opendm
