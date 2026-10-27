#include "ui_assets.hpp"
#include <filesystem>
#include <iostream>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace opendm {

std::string UiAssets::get_path(const std::string& filename) {
    std::vector<std::string> subdirs = {"", "neon/"};

    for (const auto& sub : subdirs) {
        std::string candidate = sub + filename;

        // 1. Current working directory data/ui_assets/
        if (fs::exists("data/ui_assets/" + candidate)) {
            return fs::absolute("data/ui_assets/" + candidate).string();
        }

        // 2. Relative to executable path (/proc/self/exe)
        char self_path[4096];
        ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
        if (len > 0) {
            self_path[len] = '\0';
            fs::path exe_dir = fs::path(self_path).parent_path();
            
            fs::path c1 = exe_dir / "data" / "ui_assets" / candidate;
            if (fs::exists(c1)) return c1.string();

            fs::path c2 = exe_dir.parent_path() / "data" / "ui_assets" / candidate;
            if (fs::exists(c2)) return c2.string();
        }

        // 3. User local share ~/.local/share/opendm/ui_assets/
        const char* home = getenv("HOME");
        if (home) {
            fs::path user_share = fs::path(home) / ".local" / "share" / "opendm" / "ui_assets" / candidate;
            if (fs::exists(user_share)) return user_share.string();
        }

        // 4. System share /usr/share/opendm/ui_assets/
        fs::path sys_share = fs::path("/usr/share/opendm/ui_assets") / candidate;
        if (fs::exists(sys_share)) return sys_share.string();
    }

    return "";
}

GtkWidget* UiAssets::create_image(
    const std::string& name_or_file,
    int width,
    int height,
    const char* fallback_icon) {

    int target_size = (width > 0) ? width : 20;

    // 1. If it has a file extension or slash, try to load from assets
    if (name_or_file.find('/') != std::string::npos || name_or_file.find(".png") != std::string::npos || name_or_file.find(".svg") != std::string::npos) {
        std::string full_path = get_path(name_or_file);
        if (!full_path.empty()) {
            GtkWidget* img = gtk_image_new_from_file(full_path.c_str());
            if (width > 0) {
                gtk_image_set_pixel_size(GTK_IMAGE(img), width);
            }
            return img;
        }
    }

    // 2. Otherwise treat name_or_file as GTK icon name
    GtkWidget* img = gtk_image_new_from_icon_name(name_or_file.c_str());
    gtk_image_set_pixel_size(GTK_IMAGE(img), target_size);
    return img;
}

GtkWidget* UiAssets::create_toolbar_button(
    const std::string& icon_name,
    const std::string& label_text,
    const std::string& tooltip_text,
    GCallback callback,
    gpointer user_data,
    bool is_primary,
    bool has_dropdown) {

    GtkWidget* btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "idm-toolbar-btn");
    if (is_primary) {
        gtk_widget_add_css_class(btn, "suggested-action");
    }
    gtk_widget_set_tooltip_text(btn, tooltip_text.c_str());

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget* img = create_image(icon_name, 22, 22, "document-save-symbolic");
    gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), img);

    GtkWidget* lbl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_halign(lbl_box, GTK_ALIGN_CENTER);

    GtkWidget* lbl = gtk_label_new(label_text.c_str());
    gtk_widget_add_css_class(lbl, "idm-toolbar-label");
    gtk_box_append(GTK_BOX(lbl_box), lbl);

    if (has_dropdown) {
        GtkWidget* chevron = gtk_label_new("▾");
        gtk_widget_add_css_class(chevron, "idm-toolbar-chevron");
        gtk_box_append(GTK_BOX(lbl_box), chevron);
    }

    gtk_box_append(GTK_BOX(box), lbl_box);
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
