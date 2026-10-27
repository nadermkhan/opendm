#pragma once

#include <gtk/gtk.h>
#include <string>

namespace opendm {

class UiAssets {
public:
    // Resolves absolute path to a named asset in data/ui_assets/
    static std::string get_path(const std::string& filename);

    // Creates a GtkImage from symbolic icon name or asset file
    static GtkWidget* create_image(const std::string& name_or_file, int width = -1, int height = -1, const char* fallback_icon = nullptr);

    // Creates a modern GTK toolbar button with icon on top and text below
    static GtkWidget* create_toolbar_button(
        const std::string& icon_name,
        const std::string& label_text,
        const std::string& tooltip_text,
        GCallback callback,
        gpointer user_data,
        bool is_primary = false,
        bool has_dropdown = false);

    // Applies the modern GTK CSS theme
    static void apply_theme();
};

} // namespace opendm

