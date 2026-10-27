#pragma once

#include <gtk/gtk.h>
#include <string>

namespace opendm {

class AppreciationDialog {
public:
    static AppreciationDialog* show(GtkWindow* parent);
    bool save_snapshot(const std::string& filename);
    GtkWidget* get_widget() const { return dialog_; }

private:
    explicit AppreciationDialog(GtkWindow* parent);
    ~AppreciationDialog() = default;

    void setup_ui();
    void on_appreciate_clicked();
    void on_github_clicked();

    GtkWindow* parent_{nullptr};
    GtkWidget* dialog_{nullptr};
    GtkWidget* btn_appreciate_{nullptr};
};

} // namespace opendm
