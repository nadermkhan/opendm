#include "appreciation_dialog.hpp"
#include "ui_assets.hpp"
#include <gio/gio.h>

namespace opendm {

AppreciationDialog* AppreciationDialog::show(GtkWindow* parent) {
    auto* dlg = new AppreciationDialog(parent);
    dlg->setup_ui();
    return dlg;
}

AppreciationDialog::AppreciationDialog(GtkWindow* parent) : parent_(parent) {}

void AppreciationDialog::setup_ui() {
    dialog_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog_), "OpenDM — Developer Appreciation & Registration Status");
    gtk_window_set_modal(GTK_WINDOW(dialog_), TRUE);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog_), parent_);
    }
    gtk_window_set_default_size(GTK_WINDOW(dialog_), 560, 520);
    gtk_window_set_resizable(GTK_WINDOW(dialog_), FALSE);
    gtk_widget_add_css_class(dialog_, "idm-apprec-window");

    g_signal_connect_swapped(dialog_, "destroy", G_CALLBACK(+[](AppreciationDialog* self) {
        delete self;
    }), this);

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(main_vbox, "idm-apprec-window");
    gtk_widget_set_margin_start(main_vbox, 20);
    gtk_widget_set_margin_end(main_vbox, 20);
    gtk_widget_set_margin_top(main_vbox, 18);
    gtk_widget_set_margin_bottom(main_vbox, 18);
    gtk_window_set_child(GTK_WINDOW(dialog_), main_vbox);

    // 1. Header Box
    GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget* icon = UiAssets::create_image("emblem-favorite-symbolic", 36, 36, "starred-symbolic");
    gtk_widget_add_css_class(icon, "idm-apprec-icon");
    gtk_box_append(GTK_BOX(header_box), icon);

    GtkWidget* title_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* lbl_title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(lbl_title), "<span size='large' weight='bold'>OpenDM — 100% Free &amp; Open Source</span>");
    gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0f);
    gtk_box_append(GTK_BOX(title_vbox), lbl_title);

    GtkWidget* lbl_sub = gtk_label_new("Registration is NOT required • Developer Appreciation is MANDATORY!");
    gtk_widget_add_css_class(lbl_sub, "idm-apprec-role");
    gtk_label_set_xalign(GTK_LABEL(lbl_sub), 0.0f);
    gtk_box_append(GTK_BOX(title_vbox), lbl_sub);

    gtk_box_append(GTK_BOX(header_box), title_vbox);
    gtk_box_append(GTK_BOX(main_vbox), header_box);

    // 2. Developer Showcase & License Card
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(card, "idm-apprec-card");
    gtk_box_append(GTK_BOX(main_vbox), card);

    // Dev Info row
    GtkWidget* dev_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* dev_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(dev_vbox, TRUE);

    GtkWidget* lbl_dev = gtk_label_new("Nader Mahbub Khan");
    gtk_widget_add_css_class(lbl_dev, "idm-apprec-name");
    gtk_label_set_xalign(GTK_LABEL(lbl_dev), 0.0f);
    gtk_box_append(GTK_BOX(dev_vbox), lbl_dev);

    GtkWidget* lbl_role = gtk_label_new("Software Engineer • Creator of OpenDM");
    gtk_widget_add_css_class(lbl_role, "idm-apprec-link");
    gtk_label_set_xalign(GTK_LABEL(lbl_role), 0.0f);
    gtk_box_append(GTK_BOX(dev_vbox), lbl_role);

    GtkWidget* lbl_gh = gtk_label_new("github.com/nadermkhan/");
    gtk_widget_add_css_class(lbl_gh, "idm-apprec-role");
    gtk_label_set_xalign(GTK_LABEL(lbl_gh), 0.0f);
    gtk_box_append(GTK_BOX(dev_vbox), lbl_gh);

    gtk_box_append(GTK_BOX(dev_row), dev_vbox);

    // Button to open GitHub directly from card
    btn_github_ = gtk_button_new_with_label("GitHub Profile ↗");
    gtk_widget_set_valign(btn_github_, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(btn_github_, "clicked", G_CALLBACK(+[](AppreciationDialog* self) {
        self->on_github_clicked();
    }), this);
    gtk_box_append(GTK_BOX(dev_row), btn_github_);
    gtk_box_append(GTK_BOX(card), dev_row);

    // License & Serial Parody
    GtkWidget* serial_lbl = gtk_label_new("Lifetime License & Appreciation Key:");
    gtk_label_set_xalign(GTK_LABEL(serial_lbl), 0.0f);
    gtk_box_append(GTK_BOX(card), serial_lbl);

    GtkWidget* serial_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* serial_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(serial_entry), "THANKS-NADER-MAHBUB-KHAN-SUPER-DEV");
    gtk_editable_set_editable(GTK_EDITABLE(serial_entry), FALSE);
    gtk_widget_add_css_class(serial_entry, "idm-apprec-serial");
    gtk_widget_set_hexpand(serial_entry, TRUE);
    gtk_box_append(GTK_BOX(serial_box), serial_entry);

    GtkWidget* badge_lbl = gtk_label_new("✔ Lifetime Valid");
    gtk_widget_add_css_class(badge_lbl, "idm-badge");
    gtk_widget_add_css_class(badge_lbl, "idm-badge-complete");
    gtk_widget_set_valign(badge_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(serial_box), badge_lbl);
    gtk_box_append(GTK_BOX(card), serial_box);

    // 3. Tricky Appreciation Questionnaire
    GtkWidget* pledge_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(main_vbox), pledge_box);

    GtkWidget* lbl_pledge = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(lbl_pledge), "<b>Mandatory Appreciation Selection:</b>");
    gtk_label_set_xalign(GTK_LABEL(lbl_pledge), 0.0f);
    gtk_box_append(GTK_BOX(pledge_box), lbl_pledge);

    radio_legend_ = gtk_check_button_new_with_label("I sincerely appreciate Nader Mahbub Khan for building OpenDM!");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(radio_legend_), TRUE);
    gtk_box_append(GTK_BOX(pledge_box), radio_legend_);

    radio_forever_ = gtk_check_button_new_with_label("The above, plus eternal gratitude and starring on GitHub!");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(radio_forever_), GTK_CHECK_BUTTON(radio_legend_));
    gtk_box_append(GTK_BOX(pledge_box), radio_forever_);

    // 4. Feedback / Status Label
    status_label_ = gtk_label_new("Status: Lifetime free license active. No fake serial numbers, ever!");
    gtk_widget_add_css_class(status_label_, "idm-apprec-feedback");
    gtk_label_set_xalign(GTK_LABEL(status_label_), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(status_label_), TRUE);
    gtk_box_append(GTK_BOX(main_vbox), status_label_);

    // 5. Button Actions
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(btn_box, 6);
    gtk_box_append(GTK_BOX(main_vbox), btn_box);

    btn_not_appreciate_ = gtk_button_new_with_label("Refuse to Appreciate ❌");
    g_signal_connect_swapped(btn_not_appreciate_, "clicked", G_CALLBACK(+[](AppreciationDialog* self) {
        self->on_not_appreciate_clicked();
    }), this);
    gtk_box_append(GTK_BOX(btn_box), btn_not_appreciate_);

    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(btn_box), spacer);

    btn_appreciate_ = gtk_button_new_with_label("Appreciate Developer ❤️");
    gtk_widget_add_css_class(btn_appreciate_, "suggested-action");
    g_signal_connect_swapped(btn_appreciate_, "clicked", G_CALLBACK(+[](AppreciationDialog* self) {
        self->on_appreciate_clicked();
    }), this);
    gtk_box_append(GTK_BOX(btn_box), btn_appreciate_);

    GtkWidget* btn_close = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(+[](AppreciationDialog* self) {
        self->on_close_clicked();
    }), this);
    gtk_box_append(GTK_BOX(btn_box), btn_close);

    gtk_window_present(GTK_WINDOW(dialog_));
}

void AppreciationDialog::on_appreciate_clicked() {
    gtk_widget_remove_css_class(status_label_, "error");
    gtk_widget_add_css_class(status_label_, "success");
    gtk_label_set_text(GTK_LABEL(status_label_),
        "✔ Sincere appreciation officially recorded! Thank you for supporting Nader Mahbub Khan and OpenDM!");

    gtk_button_set_label(GTK_BUTTON(btn_appreciate_), "Appreciation Sent! ★");
    gtk_widget_set_sensitive(btn_appreciate_, FALSE);
}

void AppreciationDialog::on_not_appreciate_clicked() {
    gtk_widget_remove_css_class(status_label_, "success");
    gtk_widget_add_css_class(status_label_, "error");
    gtk_label_set_text(GTK_LABEL(status_label_),
        "[!] Error 404: Ingratitude Not Found! Zero-ingratitude policy triggered: Automatically upgraded your appreciation for Nader Mahbub Khan to 1000%!");

    gtk_check_button_set_active(GTK_CHECK_BUTTON(radio_forever_), TRUE);
    gtk_button_set_label(GTK_BUTTON(btn_not_appreciate_), "Ingratitude Disallowed :)");
    gtk_widget_set_sensitive(btn_not_appreciate_, FALSE);
}

void AppreciationDialog::on_github_clicked() {
    g_app_info_launch_default_for_uri("https://github.com/nadermkhan/", nullptr, nullptr);
}

void AppreciationDialog::on_close_clicked() {
    gtk_window_destroy(GTK_WINDOW(dialog_));
}

bool AppreciationDialog::save_snapshot(const std::string& filename) {
    if (!dialog_) return false;
    GtkWidget* target = gtk_window_get_child(GTK_WINDOW(dialog_));
    if (!target) target = dialog_;

    GdkPaintable* paintable = gtk_widget_paintable_new(target);
    int w = gtk_widget_get_width(target);
    int h = gtk_widget_get_height(target);
    if (w <= 0) w = 560;
    if (h <= 0) h = 520;

    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), static_cast<double>(w), static_cast<double>(h));
    GskRenderNode* node = gtk_snapshot_to_node(snapshot);
    if (!node) {
        node = gtk_snapshot_free_to_node(snapshot);
    }

    bool ok = false;
    if (node) {
        GtkNative* native = gtk_widget_get_native(target);
        GskRenderer* renderer = native ? gtk_native_get_renderer(native) : nullptr;
        if (renderer && gsk_renderer_is_realized(renderer)) {
            graphene_rect_t bounds;
            graphene_rect_init(&bounds, 0.0f, 0.0f, (float)w, (float)h);
            GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &bounds);
            if (texture) {
                ok = gdk_texture_save_to_png(texture, filename.c_str());
                g_object_unref(texture);
            }
        }
        gsk_render_node_unref(node);
    }
    g_object_unref(paintable);
    return ok;
}

} // namespace opendm
