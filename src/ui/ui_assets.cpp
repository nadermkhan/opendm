#include "ui_assets.hpp"
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace opendm {

std::string UiAssets::get_path(const std::string& filename) {
    // 1. Current working directory data/ui_assets/
    if (fs::exists("data/ui_assets/" + filename)) {
        return fs::absolute("data/ui_assets/" + filename).string();
    }

    // 2. Relative to executable path (/proc/self/exe)
    char self_path[4096];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len > 0) {
        self_path[len] = '\0';
        fs::path exe_dir = fs::path(self_path).parent_path();
        
        fs::path candidate1 = exe_dir / "data" / "ui_assets" / filename;
        if (fs::exists(candidate1)) return candidate1.string();

        fs::path candidate2 = exe_dir.parent_path() / "data" / "ui_assets" / filename;
        if (fs::exists(candidate2)) return candidate2.string();
    }

    // 3. User local share ~/.local/share/opendm/ui_assets/
    const char* home = getenv("HOME");
    if (home) {
        fs::path user_share = fs::path(home) / ".local" / "share" / "opendm" / "ui_assets" / filename;
        if (fs::exists(user_share)) return user_share.string();
    }

    // 4. System share /usr/share/opendm/ui_assets/
    fs::path sys_share = fs::path("/usr/share/opendm/ui_assets") / filename;
    if (fs::exists(sys_share)) return sys_share.string();

    return "";
}

GtkWidget* UiAssets::create_image(
    const std::string& filename,
    int width,
    int height,
    const char* fallback_icon) {

    std::string full_path = get_path(filename);
    if (!full_path.empty()) {
        GtkWidget* img = gtk_image_new_from_file(full_path.c_str());
        if (width > 0 && height > 0) {
            gtk_image_set_pixel_size(GTK_IMAGE(img), width);
        }
        return img;
    }

    if (fallback_icon) {
        GtkWidget* img = gtk_image_new_from_icon_name(fallback_icon);
        if (width > 0) {
            gtk_image_set_pixel_size(GTK_IMAGE(img), width);
        }
        return img;
    }

    return gtk_image_new_from_icon_name("application-x-executable");
}

GtkWidget* UiAssets::create_toolbar_button(
    const std::string& icon_filename,
    const std::string& label_text,
    const std::string& tooltip_text,
    GCallback callback,
    gpointer user_data) {

    GtkWidget* btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "idm-toolbar-btn");
    gtk_widget_set_tooltip_text(btn, tooltip_text.c_str());

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget* img = create_image(icon_filename, 32, 32, "document-save");
    gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), img);

    GtkWidget* lbl = gtk_label_new(label_text.c_str());
    gtk_widget_add_css_class(lbl, "idm-toolbar-label");
    gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), lbl);

    gtk_button_set_child(GTK_BUTTON(btn), box);

    if (callback) {
        g_signal_connect(btn, "clicked", callback, user_data);
    }

    return btn;
}

void UiAssets::apply_theme() {
    std::string css_path = "";
    if (fs::exists("src/ui/style.css")) {
        css_path = fs::absolute("src/ui/style.css").string();
    } else {
        char self_path[4096];
        ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
        if (len > 0) {
            self_path[len] = '\0';
            fs::path p = fs::path(self_path).parent_path().parent_path() / "src" / "ui" / "style.css";
            if (fs::exists(p)) css_path = p.string();
        }
    }

    if (!css_path.empty()) {
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_path(provider, css_path.c_str());
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider);
    }
}

} // namespace opendm
