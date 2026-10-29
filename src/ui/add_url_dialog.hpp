#pragma once

#include <gtk/gtk.h>
#include <string>
#include <functional>

namespace opendm {

struct AddUrlResult {
    bool accepted{false};
    std::string url;
    std::string path;
    std::string filename;
    bool start_immediately{true};
    bool use_auth{false};
    std::string username;
    std::string password;
    std::string audio_stream_url;
    std::string user_agent;
    bool is_streaming_media{false};
    uint64_t file_size{0};
    bool bypass_prompt{false};
};

class AddUrlDialog {
public:
    using Callback = std::function<void(const AddUrlResult& result)>;

    static void show(GtkWindow* parent, const std::string& default_url, Callback callback);

private:
    AddUrlDialog(GtkWindow* parent, const std::string& default_url, Callback callback);

    void setup_ui();
    void on_ok_clicked();
    void on_cancel_clicked();

    GtkWidget* dialog_{nullptr};
    GtkWindow* parent_{nullptr};
    std::string default_url_;
    Callback callback_;

    GtkWidget* entry_url_{nullptr};
    GtkWidget* entry_path_{nullptr};
    GtkWidget* chk_auth_{nullptr};
    GtkWidget* entry_user_{nullptr};
    GtkWidget* entry_pass_{nullptr};
    GtkWidget* chk_start_now_{nullptr};
};

} // namespace opendm
