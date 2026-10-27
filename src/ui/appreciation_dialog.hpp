#pragma once

#include <gtk/gtk.h>
#include <string>

namespace opendm {

class AppreciationDialog {
public:
    static AppreciationDialog* show(GtkWindow* parent);
    bool save_snapshot(const std::string& filename);
    GtkWidget* get_widget() const { return dialog_; }
    void trigger_not_appreciate() { on_not_appreciate_clicked(); }
    void trigger_appreciate() { on_appreciate_clicked(); }

private:
    explicit AppreciationDialog(GtkWindow* parent);
    ~AppreciationDialog() = default;

    void setup_ui();
    void on_appreciate_clicked();
    void on_not_appreciate_clicked();
    void on_github_clicked();
    void on_close_clicked();

    GtkWindow* parent_{nullptr};
    GtkWidget* dialog_{nullptr};
    GtkWidget* status_label_{nullptr};
    GtkWidget* btn_appreciate_{nullptr};
    GtkWidget* btn_not_appreciate_{nullptr};
    GtkWidget* btn_github_{nullptr};
    GtkWidget* radio_legend_{nullptr};
    GtkWidget* radio_forever_{nullptr};
};

} // namespace opendm
