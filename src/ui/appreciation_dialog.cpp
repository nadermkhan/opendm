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
    gtk_window_set_title(GTK_WINDOW(dialog_), "Thanks");
    gtk_window_set_modal(GTK_WINDOW(dialog_), TRUE);
    if (parent_) {
        gtk_window_set_transient_for(GTK_WINDOW(dialog_), parent_);
    }
    gtk_window_set_default_size(GTK_WINDOW(dialog_), 520, 320);
    gtk_window_set_resizable(GTK_WINDOW(dialog_), FALSE);
    gtk_window_set_deletable(GTK_WINDOW(dialog_), FALSE); // Disables window 'X' button
    gtk_widget_add_css_class(dialog_, "idm-apprec-window");

    // Prevent closing via Alt+F4 or window manager
    g_signal_connect(dialog_, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer) -> gboolean {
        return TRUE;
    }), nullptr);

    g_signal_connect_swapped(dialog_, "destroy", G_CALLBACK(+[](AppreciationDialog* self) {
        delete self;
    }), this);

    GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(main_vbox, "idm-apprec-window");
    gtk_widget_set_margin_start(main_vbox, 24);
    gtk_widget_set_margin_end(main_vbox, 24);
    gtk_widget_set_margin_top(main_vbox, 22);
    gtk_widget_set_margin_bottom(main_vbox, 22);
    gtk_window_set_child(GTK_WINDOW(dialog_), main_vbox);

    // 1. Header with Golden Trophy / Heart Icon
    GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget* icon = UiAssets::create_image("emblem-favorite-symbolic", 44, 44, "starred-symbolic");
    gtk_widget_add_css_class(icon, "idm-apprec-icon");
    gtk_box_append(GTK_BOX(header_box), icon);

    GtkWidget* title_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* lbl_thanks = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(lbl_thanks), "<span size='xx-large' weight='bold'>Thanks</span>");
    gtk_label_set_xalign(GTK_LABEL(lbl_thanks), 0.0f);
    gtk_box_append(GTK_BOX(title_vbox), lbl_thanks);

    GtkWidget* lbl_tagline = gtk_label_new("Developer Recognition & Gratitude");
    gtk_widget_add_css_class(lbl_tagline, "idm-apprec-role");
    gtk_label_set_xalign(GTK_LABEL(lbl_tagline), 0.0f);
    gtk_box_append(GTK_BOX(title_vbox), lbl_tagline);

    gtk_box_append(GTK_BOX(header_box), title_vbox);
    gtk_box_append(GTK_BOX(main_vbox), header_box);

    // 2. The Main Appreciation Message
    GtkWidget* msg_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(msg_card, "idm-apprec-card");
    gtk_box_append(GTK_BOX(main_vbox), msg_card);

    GtkWidget* lbl_msg = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(lbl_msg),
        "<span size='medium' line_height='1.35'>"
        "Thanks to <b>Nader Mahbub Khan</b> who cloned IDM for Linux no one dared to do, "
        "for most of the users worldwide."
        "</span>");
    gtk_label_set_wrap(GTK_LABEL(lbl_msg), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lbl_msg), 0.0f);
    gtk_box_append(GTK_BOX(msg_card), lbl_msg);

    // Developer details row
    GtkWidget* dev_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget* dev_info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(dev_info, TRUE);

    GtkWidget* lbl_name = gtk_label_new("Nader Mahbub Khan");
    gtk_widget_add_css_class(lbl_name, "idm-apprec-name");
    gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0f);
    gtk_box_append(GTK_BOX(dev_info), lbl_name);

    GtkWidget* lbl_role = gtk_label_new("Software Engineer • github.com/nadermkhan/");
    gtk_widget_add_css_class(lbl_role, "idm-apprec-role");
    gtk_label_set_xalign(GTK_LABEL(lbl_role), 0.0f);
    gtk_box_append(GTK_BOX(dev_info), lbl_role);

    gtk_box_append(GTK_BOX(dev_box), dev_info);

    GtkWidget* btn_gh = gtk_button_new_with_label("GitHub Profile ↗");
    gtk_widget_set_valign(btn_gh, GTK_ALIGN_CENTER);
    g_signal_connect_swapped(btn_gh, "clicked", G_CALLBACK(+[](AppreciationDialog* self) {
        self->on_github_clicked();
    }), this);
    gtk_box_append(GTK_BOX(dev_box), btn_gh);

    gtk_box_append(GTK_BOX(msg_card), dev_box);

    // 3. The ONLY Button to close: "I appreciate"
    GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(btn_box, 6);
    gtk_box_append(GTK_BOX(main_vbox), btn_box);

    btn_appreciate_ = gtk_button_new_with_label("I appreciate");
    gtk_widget_add_css_class(btn_appreciate_, "suggested-action");
    gtk_widget_add_css_class(btn_appreciate_, "idm-btn-appreciate");
    gtk_widget_set_hexpand(btn_appreciate_, TRUE);
    g_signal_connect_swapped(btn_appreciate_, "clicked", G_CALLBACK(+[](AppreciationDialog* self) {
        self->on_appreciate_clicked();
    }), this);
    gtk_box_append(GTK_BOX(btn_box), btn_appreciate_);

    gtk_window_present(GTK_WINDOW(dialog_));
}

void AppreciationDialog::on_appreciate_clicked() {
    gtk_window_destroy(GTK_WINDOW(dialog_));
}

void AppreciationDialog::on_github_clicked() {
    g_app_info_launch_default_for_uri("https://github.com/nadermkhan/", nullptr, nullptr);
}

bool AppreciationDialog::save_snapshot(const std::string& filename) {
    if (!dialog_) return false;
    GtkWidget* target = gtk_window_get_child(GTK_WINDOW(dialog_));
    if (!target) target = dialog_;

    GdkPaintable* paintable = gtk_widget_paintable_new(target);
    int w = gtk_widget_get_width(target);
    int h = gtk_widget_get_height(target);
    if (w <= 0) w = 520;
    if (h <= 0) h = 380;

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
