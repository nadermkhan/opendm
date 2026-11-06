#pragma once

#include <windows.h>
#include <commctrl.h>
#include <memory>
#include <string>
#include "../core/download_session.hpp"

namespace opendm {
namespace win32 {

class Win32ProgressDialog {
public:
    explicit Win32ProgressDialog(std::shared_ptr<DownloadSession> session, HWND parent = nullptr);
    ~Win32ProgressDialog();

    void show();
    void close();
    HWND get_hwnd() const { return hwnd_; }

    bool save_snapshot(const std::string& filename);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK VisualizerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void setup_ui();
    void update_ui();
    void on_pause_resume();
    void on_cancel();

    std::shared_ptr<DownloadSession> session_;
    HWND parent_{nullptr};
    HWND hwnd_{nullptr};

    HWND hStaticStatus_{nullptr};
    HWND hStaticSize_{nullptr};
    HWND hStaticDownloaded_{nullptr};
    HWND hStaticSpeed_{nullptr};
    HWND hStaticEta_{nullptr};
    HWND hStaticResume_{nullptr};
    HWND hProgressBar_{nullptr};
    HWND hVisualizer_{nullptr};
    HWND hListConns_{nullptr};
    HWND hBtnPauseResume_{nullptr};
    HWND hBtnCancel_{nullptr};

    HFONT hFont_{nullptr};
    HFONT hFontBold_{nullptr};
    UINT timer_id_{0};
    UINT dpi_{96};
    bool completed_notified_{false};
};

} // namespace win32
} // namespace opendm
