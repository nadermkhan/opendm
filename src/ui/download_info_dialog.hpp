#pragma once

#include <gtk/gtk.h>
#include <string>
#include <functional>
#include <cstdint>
#include <memory>
#include <atomic>
#include <vector>

#include "../ipc/ipc_server.hpp"
#include "../core/stream_extractor.hpp"

namespace opendm {

using DownloadInfoParams = DownloadPromptInfo;

enum class DownloadInfoAction {
    START_NOW,
    DOWNLOAD_LATER,
    CANCEL
};

using DownloadInfoCallback = std::function<void(DownloadInfoAction action, const DownloadInfoParams& result)>;

class DownloadInfoDialog {
public:
    static DownloadInfoDialog* show(GtkWindow* parent, const DownloadInfoParams& params, DownloadInfoCallback callback);
    ~DownloadInfoDialog();

    GtkWidget* get_widget() const { return dialog_; }
    bool save_snapshot(const std::string& filename);

    static std::string get_default_category_dir(const std::string& category);
    static void save_category_dir(const std::string& category, const std::string& path);
    static std::string load_category_dir(const std::string& category);
    static std::string format_size(uint64_t bytes);

private:
    DownloadInfoDialog(GtkWindow* parent, const DownloadInfoParams& params, DownloadInfoCallback callback);

    void setup_ui();
    void update_category_ui(const std::string& category, bool update_path = true);
    void update_file_size_display();
    void probe_remote_info();
    void update_stream_formats_ui(const StreamMediaInfo& info);
    void on_stream_format_changed();
    void on_browse_clicked();
    void on_add_category_clicked();
    void on_preview_clicked();
    void on_action_clicked(DownloadInfoAction action);

    GtkWindow* parent_{nullptr};
    DownloadInfoParams params_;
    DownloadInfoCallback callback_;
    bool syncing_paths_{false};

    GtkWidget* dialog_{nullptr};
    GtkWidget* entry_url_{nullptr};
    GtkWidget* combo_category_{nullptr};
    GtkWidget* btn_add_cat_{nullptr};
    GtkWidget* entry_save_as_{nullptr};
    GtkWidget* btn_browse_{nullptr};
    GtkWidget* chk_remember_path_{nullptr};
    GtkWidget* entry_cat_dir_{nullptr};
    GtkWidget* entry_desc_{nullptr};
    GtkWidget* right_icon_box_{nullptr};
    GtkWidget* img_icon_{nullptr};
    GtkWidget* lbl_size_{nullptr};
    GtkWidget* btn_preview_{nullptr};
    GtkWidget* btn_download_later_{nullptr};
    GtkWidget* btn_start_download_{nullptr};
    GtkWidget* btn_cancel_{nullptr};
    GtkWidget* row_stream_format_{nullptr};
    GtkWidget* lbl_format_{nullptr};
    GtkWidget* combo_stream_format_{nullptr};
    std::vector<StreamFormat> available_stream_formats_;

    bool destroyed_{false};
    std::shared_ptr<std::atomic<bool>> alive_token_{std::make_shared<std::atomic<bool>>(true)};
};

} // namespace opendm
