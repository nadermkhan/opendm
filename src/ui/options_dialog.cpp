#include "options_dialog.hpp"
#include "download_info_dialog.hpp"
#include "../core/download_queue.hpp"
#include "../core/site_logon.hpp"
#include "ui_assets.hpp"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace opendm {

namespace {

static const char* kCategories[] = {
    "Compressed",
    "Programs",
    "Video",
    "Music",
    "Documents",
    "General",
    nullptr
};

} // namespace

void OptionsDialog::show(GtkWindow* parent, int initial_tab) {
    auto* dlg = new OptionsDialog(parent);
    dlg->setup_ui(initial_tab);
}

OptionsDialog::OptionsDialog(GtkWindow* parent) : parent_(parent) {}

OptionsDialog::~OptionsDialog() = default;

void OptionsDialog::setup_ui(int initial_tab) {
    dialog_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog_), "Options");
    gtk_window_set_modal(GTK_WINDOW(dialog_), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog_), parent_);
    gtk_window_set_default_size(GTK_WINDOW(dialog_), 580, 480);
    gtk_window_set_resizable(GTK_WINDOW(dialog_), TRUE);

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(main_vbox, 12);
    gtk_widget_set_margin_end(main_vbox, 12);
    gtk_widget_set_margin_top(main_vbox, 12);
    gtk_widget_set_margin_bottom(main_vbox, 12);
    gtk_window_set_child(GTK_WINDOW(dialog_), main_vbox);

    notebook_ = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook_, TRUE);
    gtk_box_append(GTK_BOX(main_vbox), notebook_);

    // Tab 1: Save To / Categories
    GtkWidget* tab1 = build_categories_tab();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook_), tab1, gtk_label_new("Save To"));

    // Tab 2: Connection
    GtkWidget* tab2 = build_connection_tab();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook_), tab2, gtk_label_new("Connection"));

    // Tab 3: Site Logins
    GtkWidget* tab3 = build_site_logins_tab();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook_), tab3, gtk_label_new("Site Logins"));

    // Tab 4: Scheduler
    GtkWidget* tab4 = build_scheduler_tab();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook_), tab4, gtk_label_new("Scheduler"));

    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), initial_tab);

    // Bottom Action Buttons: OK, Cancel, Apply
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);

    GtkWidget* btn_ok = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(btn_ok, "suggested-action");
    GtkWidget* btn_cancel = gtk_button_new_with_label("Cancel");
    GtkWidget* btn_apply = gtk_button_new_with_label("Apply");

    gtk_box_append(GTK_BOX(btn_box), btn_ok);
    gtk_box_append(GTK_BOX(btn_box), btn_cancel);
    gtk_box_append(GTK_BOX(btn_box), btn_apply);
    gtk_box_append(GTK_BOX(main_vbox), btn_box);

    g_signal_connect_swapped(btn_ok, "clicked", G_CALLBACK(+[](OptionsDialog* self) {
        self->on_save_clicked();
        gtk_window_destroy(GTK_WINDOW(self->dialog_));
    }), this);

    g_signal_connect_swapped(btn_cancel, "clicked", G_CALLBACK(+[](OptionsDialog* self) {
        gtk_window_destroy(GTK_WINDOW(self->dialog_));
    }), this);

    g_signal_connect_swapped(btn_apply, "clicked", G_CALLBACK(+[](OptionsDialog* self) {
        self->on_save_clicked();
    }), this);

    // Auto-free this on destroy
    g_object_set_data_full(G_OBJECT(dialog_), "opendm_options_dlg", this, +[](gpointer p) {
        delete static_cast<OptionsDialog*>(p);
    });

    gtk_window_present(GTK_WINDOW(dialog_));
}

GtkWidget* OptionsDialog::build_categories_tab() {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    GtkWidget* lbl_intro = gtk_label_new("Default download directories for file categories:");
    gtk_label_set_xalign(GTK_LABEL(lbl_intro), 0.0f);
    gtk_box_append(GTK_BOX(vbox), lbl_intro);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_append(GTK_BOX(vbox), grid);

    GtkWidget* lbl_cat = gtk_label_new("Category:");
    gtk_label_set_xalign(GTK_LABEL(lbl_cat), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl_cat, 0, 0, 1, 1);

    cat_combo_ = gtk_drop_down_new_from_strings(kCategories);
    gtk_widget_set_hexpand(cat_combo_, TRUE);
    gtk_grid_attach(GTK_GRID(grid), cat_combo_, 1, 0, 2, 1);

    GtkWidget* lbl_path = gtk_label_new("Folder:");
    gtk_label_set_xalign(GTK_LABEL(lbl_path), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl_path, 0, 1, 1, 1);

    cat_path_entry_ = gtk_entry_new();
    gtk_widget_set_hexpand(cat_path_entry_, TRUE);
    gtk_grid_attach(GTK_GRID(grid), cat_path_entry_, 1, 1, 1, 1);

    GtkWidget* btn_browse = gtk_button_new_with_label("Browse...");
    gtk_grid_attach(GTK_GRID(grid), btn_browse, 2, 1, 1, 1);

    // Initialize with first category
    std::string init_cat = kCategories[0];
    gtk_editable_set_text(GTK_EDITABLE(cat_path_entry_), DownloadInfoDialog::load_category_dir(init_cat).c_str());

    g_signal_connect(cat_combo_, "notify::selected", G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
        auto* self = static_cast<OptionsDialog*>(data);
        guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
        if (kCategories[sel]) {
            std::string cat = kCategories[sel];
            std::string p = DownloadInfoDialog::load_category_dir(cat);
            gtk_editable_set_text(GTK_EDITABLE(self->cat_path_entry_), p.c_str());
        }
    }), this);

    g_signal_connect(btn_browse, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<OptionsDialog*>(data);
        GtkFileChooserNative* chooser = gtk_file_chooser_native_new(
            "Select Category Download Folder",
            GTK_WINDOW(self->dialog_),
            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
            "_Select",
            "_Cancel"
        );
        g_signal_connect(chooser, "response", G_CALLBACK(+[](GtkNativeDialog* native, int resp, gpointer user_data) {
            if (resp == GTK_RESPONSE_ACCEPT) {
                GFile* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native));
                if (file) {
                    char* path = g_file_get_path(file);
                    if (path) {
                        auto* dlg = static_cast<OptionsDialog*>(user_data);
                        gtk_editable_set_text(GTK_EDITABLE(dlg->cat_path_entry_), path);
                        g_free(path);
                    }
                    g_object_unref(file);
                }
            }
            g_object_unref(native);
        }), self);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
    }), this);

    return vbox;
}

GtkWidget* OptionsDialog::build_connection_tab() {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(vbox), grid);

    // Max connections
    GtkWidget* lbl_conns = gtk_label_new("Default max connections number:");
    gtk_label_set_xalign(GTK_LABEL(lbl_conns), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl_conns, 0, 0, 1, 1);

    spin_connections_ = gtk_spin_button_new_with_range(1, 32, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_connections_), 8);
    gtk_grid_attach(GTK_GRID(grid), spin_connections_, 1, 0, 1, 1);

    // Global speed limiter
    GtkWidget* lbl_limit = gtk_label_new("Speed Limiter rate (KB/s):");
    gtk_label_set_xalign(GTK_LABEL(lbl_limit), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl_limit, 0, 1, 1, 1);

    spin_speed_limit_ = gtk_spin_button_new_with_range(10, 1000000, 50);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_speed_limit_), 500);
    gtk_grid_attach(GTK_GRID(grid), spin_speed_limit_, 1, 1, 1, 1);

    chk_enable_limiter_ = gtk_check_button_new_with_label("Enable global speed limiter");
    gtk_grid_attach(GTK_GRID(grid), chk_enable_limiter_, 0, 2, 2, 1);

    return vbox;
}

void OptionsDialog::reload_credentials() {
    if (!login_list_box_) return;

    GtkWidget* child = gtk_widget_get_first_child(login_list_box_);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(login_list_box_), child);
        child = next;
    }

    auto creds = SiteLogonManager::instance().get_all_credentials();
    for (const auto& c : creds) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(row, 8);
        gtk_widget_set_margin_end(row, 8);
        gtk_widget_set_margin_top(row, 4);
        gtk_widget_set_margin_bottom(row, 4);

        GtkWidget* lbl_d = gtk_label_new(c.domain.c_str());
        gtk_widget_set_hexpand(lbl_d, TRUE);
        gtk_label_set_xalign(GTK_LABEL(lbl_d), 0.0f);

        GtkWidget* lbl_u = gtk_label_new(c.username.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_u), 0.0f);

        gtk_box_append(GTK_BOX(row), lbl_d);
        gtk_box_append(GTK_BOX(row), lbl_u);

        g_object_set_data_full(G_OBJECT(row), "site_domain", g_strdup(c.domain.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(login_list_box_), row);
    }
}

void OptionsDialog::on_add_site_login_clicked() {
    GtkWidget* add_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(add_win), "New Site Credential");
    gtk_window_set_modal(GTK_WINDOW(add_win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(add_win), GTK_WINDOW(dialog_));
    gtk_window_set_default_size(GTK_WINDOW(add_win), 360, 200);

    GtkWidget* gbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(gbox, 14);
    gtk_widget_set_margin_end(gbox, 14);
    gtk_widget_set_margin_top(gbox, 14);
    gtk_widget_set_margin_bottom(gbox, 14);
    gtk_window_set_child(GTK_WINDOW(add_win), gbox);

    GtkWidget* e_site = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e_site), "Domain / Site (e.g. example.com)");
    GtkWidget* e_user = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e_user), "Username");
    GtkWidget* e_pass = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e_pass), "Password");
    gtk_entry_set_visibility(GTK_ENTRY(e_pass), FALSE);

    gtk_box_append(GTK_BOX(gbox), e_site);
    gtk_box_append(GTK_BOX(gbox), e_user);
    gtk_box_append(GTK_BOX(gbox), e_pass);

    GtkWidget* b_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(b_row, GTK_ALIGN_END);
    GtkWidget* b_ok = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(b_ok, "suggested-action");
    GtkWidget* b_cancel = gtk_button_new_with_label("Cancel");

    gtk_box_append(GTK_BOX(b_row), b_ok);
    gtk_box_append(GTK_BOX(b_row), b_cancel);
    gtk_box_append(GTK_BOX(gbox), b_row);

    struct AddData {
        OptionsDialog* dlg;
        GtkWidget* win;
        GtkWidget* e_site;
        GtkWidget* e_user;
        GtkWidget* e_pass;
    };
    auto* ad = new AddData{this, add_win, e_site, e_user, e_pass};

    g_signal_connect(b_ok, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* a = static_cast<AddData*>(d);
        const char* s = gtk_editable_get_text(GTK_EDITABLE(a->e_site));
        const char* u = gtk_editable_get_text(GTK_EDITABLE(a->e_user));
        const char* p = gtk_editable_get_text(GTK_EDITABLE(a->e_pass));
        if (s && *s) {
            SiteCredential sc;
            sc.domain = s;
            sc.username = u ? u : "";
            sc.password = p ? p : "";
            SiteLogonManager::instance().set_credential(sc);
            a->dlg->reload_credentials();
        }
        gtk_window_destroy(GTK_WINDOW(a->win));
        delete a;
    }), ad);

    g_signal_connect(b_cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer d) {
        auto* a = static_cast<AddData*>(d);
        gtk_window_destroy(GTK_WINDOW(a->win));
        delete a;
    }), ad);

    gtk_window_present(GTK_WINDOW(add_win));
}

GtkWidget* OptionsDialog::build_site_logins_tab() {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    GtkWidget* lbl_intro = gtk_label_new("Stored passwords and credentials for download servers:");
    gtk_label_set_xalign(GTK_LABEL(lbl_intro), 0.0f);
    gtk_box_append(GTK_BOX(vbox), lbl_intro);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), scroll);

    login_list_box_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(login_list_box_), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), login_list_box_);

    reload_credentials();

    // Buttons: New, Delete
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* btn_new = gtk_button_new_with_label("New Site...");
    GtkWidget* btn_del = gtk_button_new_with_label("Delete");

    gtk_box_append(GTK_BOX(btn_box), btn_new);
    gtk_box_append(GTK_BOX(btn_box), btn_del);
    gtk_box_append(GTK_BOX(vbox), btn_box);

    g_signal_connect_swapped(btn_new, "clicked", G_CALLBACK(+[](OptionsDialog* self) {
        self->on_add_site_login_clicked();
    }), this);

    g_signal_connect(btn_del, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<OptionsDialog*>(data);
        GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->login_list_box_));
        if (row) {
            GtkWidget* child = gtk_list_box_row_get_child(row);
            const char* domain = static_cast<const char*>(g_object_get_data(G_OBJECT(child), "site_domain"));
            if (domain) {
                SiteLogonManager::instance().remove_credential(domain);
                gtk_list_box_remove(GTK_LIST_BOX(self->login_list_box_), GTK_WIDGET(row));
            }
        }
    }), this);

    return vbox;
}

GtkWidget* OptionsDialog::build_scheduler_tab() {
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    chk_sched_enabled_ = gtk_check_button_new_with_label("Enable download queue scheduler");
    gtk_box_append(GTK_BOX(vbox), chk_sched_enabled_);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_append(GTK_BOX(vbox), grid);

    GtkWidget* lbl_active = gtk_label_new("Maximum concurrent downloads:");
    gtk_label_set_xalign(GTK_LABEL(lbl_active), 0.0f);
    gtk_grid_attach(GTK_GRID(grid), lbl_active, 0, 0, 1, 1);

    spin_max_active_ = gtk_spin_button_new_with_range(1, 10, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_max_active_), 2);
    gtk_grid_attach(GTK_GRID(grid), spin_max_active_, 1, 0, 1, 1);

    chk_sched_turn_off_ = gtk_check_button_new_with_label("Turn off computer when downloads finish");
    gtk_box_append(GTK_BOX(vbox), chk_sched_turn_off_);

    return vbox;
}

void OptionsDialog::on_save_clicked() {
    // 1. Save selected category path
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(cat_combo_));
    if (kCategories[sel]) {
        std::string cat = kCategories[sel];
        const char* p = gtk_editable_get_text(GTK_EDITABLE(cat_path_entry_));
        if (p && *p) {
            DownloadInfoDialog::save_category_dir(cat, p);
        }
    }

    // 2. Save connections & speed limiter
    int conns = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_connections_));
    int limit_kb = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_speed_limit_));
    bool enable_limiter = gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_enable_limiter_));

    QueueManager::instance().enable_global_speed_limit(enable_limiter);
    if (limit_kb > 0) {
        QueueManager::instance().set_global_speed_limit(static_cast<uint64_t>(limit_kb) * 1024);
    }

    // 3. Save scheduler queue config
    int max_active = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_max_active_));
    QueueManager::instance().get_main_queue().set_max_concurrent(max_active);
}

} // namespace opendm
