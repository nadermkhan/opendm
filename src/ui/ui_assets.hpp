#pragma once

#include <gtk/gtk.h>
#include <string>

namespace opendm {

class UiAssets {
public:
    // Resolves absolute path to a named asset in data/ui_assets/
    static std::string get_path(const std::string& filename);

    // Creates a GtkImage from the extracted authentic asset
    static GtkWidget* create_image(const std::string& filename, int width = -1, int height = -1, const char* fallback_icon = nullptr);

    // Creates an authentic IDM-style toolbar button with icon on top and text below
    static GtkWidget* create_toolbar_button(
        const std::string& icon_filename,
        const std::string& label_text,
        const std::string& tooltip_text,
        GCallback callback,
        gpointer user_data);

    // Applies the authentic IDM CSS theme
    static void apply_theme();
};

} // namespace opendm
