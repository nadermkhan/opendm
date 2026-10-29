#pragma once

#include <gtk/gtk.h>
#include <string>
#include <vector>
#include <functional>
#include "../core/youtube_engine.hpp"
#include "../ipc/ipc_server.hpp"
#include "download_info_dialog.hpp"

namespace opendm {

struct YouTubeQualityOption {
    std::string label;
    std::string video_url;
    std::string audio_url;
    std::string user_agent;
    std::string container;
    uint64_t total_size{0};
    bool is_audio_only{false};
    bool is_muxed{false};
};

class YouTubeFormatDialog {
public:
    using Callback = std::function<void(DownloadInfoAction action, const DownloadPromptInfo& info)>;

    static void show(GtkWindow* parent, const YouTubeVideoInfo& info, Callback callback);
    ~YouTubeFormatDialog();

private:
    YouTubeFormatDialog(GtkWindow* parent, const YouTubeVideoInfo& info, Callback callback);

    void setup_ui();
    void build_quality_options();
    void on_quality_changed();
    void on_browse_clicked();
    void on_action_clicked(DownloadInfoAction action);

    static std::string sanitize_filename(const std::string& name);

    GtkWindow* parent_{nullptr};
    YouTubeVideoInfo info_;
    Callback callback_;

    std::vector<YouTubeQualityOption> options_;
    int selected_option_index_{0};

    GtkWidget* dialog_{nullptr};
    GtkWidget* combo_quality_{nullptr};
    GtkWidget* lbl_size_{nullptr};
    GtkWidget* entry_filename_{nullptr};
    GtkWidget* entry_dir_{nullptr};
    GtkWidget* chk_start_now_{nullptr};
    bool destroyed_{false};
};

} // namespace opendm
