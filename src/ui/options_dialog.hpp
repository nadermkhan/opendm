#pragma once

#include <gtk/gtk.h>
#include <string>

namespace opendm {

class OptionsDialog {
public:
    static void show(GtkWindow* parent, int initial_tab = 0);

    GtkWidget* get_widget() const { return dialog_; }

private:
    explicit OptionsDialog(GtkWindow* parent);
    ~OptionsDialog();

    void setup_ui(int initial_tab);
    GtkWidget* build_categories_tab();
    GtkWidget* build_connection_tab();
    GtkWidget* build_site_logins_tab();
    GtkWidget* build_scheduler_tab();

    void on_save_clicked();
    void on_add_site_login_clicked();
    void reload_credentials();

    GtkWindow* parent_{nullptr};
    GtkWidget* dialog_{nullptr};
    GtkWidget* notebook_{nullptr};

    // Category widgets
    GtkWidget* cat_combo_{nullptr};
    GtkWidget* cat_path_entry_{nullptr};

    // Connection widgets
    GtkWidget* spin_connections_{nullptr};
    GtkWidget* spin_speed_limit_{nullptr};
    GtkWidget* chk_enable_limiter_{nullptr};

    // Site login widgets
    GtkWidget* login_list_box_{nullptr};

    // Scheduler widgets
    GtkWidget* chk_sched_enabled_{nullptr};
    GtkWidget* spin_max_active_{nullptr};
    GtkWidget* chk_sched_turn_off_{nullptr};
};

} // namespace opendm
