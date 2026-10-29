#include "core/download_queue.hpp"
#include "core/download_session.hpp"
#include "core/batch_importer.hpp"
#include "core/site_logon.hpp"
#include "core/youtube_engine.hpp"
#include "cli/cli_parser.hpp"
#include "ipc/ipc_server.hpp"
#include "ui/main_window.hpp"
#include "ui/about_dialog.hpp"
#include "ui/download_info_dialog.hpp"
#include "ui/progress_dialog.hpp"
#include <curl/curl.h>
#include <gtk/gtk.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>

namespace {
std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signum) {
    g_shutdown_requested = true;
}
}

int main(int argc, char* argv[]) {
    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_ALL);

    opendm::CliOptions opts = opendm::CliParser::parse(argc, argv);

    if (opts.show_help) {
        opendm::CliParser::print_help(argv[0]);
        curl_global_cleanup();
        return 0;
    }

    if (opts.show_version) {
        opendm::CliParser::print_version();
        curl_global_cleanup();
        return 0;
    }

    // Try communicating with an existing instance if URL or action provided (skip if screenshot requested)
    if ((!opts.url.empty() || opts.start_queue) && opts.screenshot_path.empty()) {
        std::string req;
        if (!opts.url.empty()) {
            req = "{\"action\": \"download\", \"url\": \"" + opts.url + "\"";
            if (!opts.path.empty()) req += ", \"path\": \"" + opts.path + "\"";
            if (!opts.filename.empty()) req += ", \"filename\": \"" + opts.filename + "\"";
            if (opts.add_only) req += ", \"add_only\": \"true\"";
            if (opts.connections > 0) req += ", \"connections\": \"" + std::to_string(opts.connections) + "\"";
            if (opts.speed_limit_kb > 0) req += ", \"speed_limit\": \"" + std::to_string(opts.speed_limit_kb) + "\"";
            req += "}";
        } else if (opts.start_queue) {
            req = "{\"action\": \"start_queue\"}";
        }

        std::string resp = opendm::IpcServer::send_command(req);
        if (resp.find("Could not connect") == std::string::npos && !resp.empty()) {
            std::cout << "Forwarded request to running OpenDM instance: " << resp << std::endl;
            curl_global_cleanup();
            return 0;
        }
    }

    // Load persisted download state from disk
    opendm::QueueManager::instance().load_from_file();

    // Process batch file import (.ef2 or text file)
    if (!opts.import_file.empty()) {
        std::string err;
        auto items = opendm::BatchImporter::import_file(opts.import_file, err);
        if (items.empty()) {
            std::cerr << "Failed to import batch file: " << err << std::endl;
        } else {
            std::cout << "Imported " << items.size() << " downloads from " << opts.import_file << std::endl;
            for (const auto& item : items) {
                std::string dest_dir = item.path.empty() ? (opts.path.empty() ? "." : opts.path) : item.path;
                std::string dest_file = dest_dir + "/" + (item.filename.empty() ? "download.bin" : item.filename);
                static uint64_t counter = 1;
                std::string id = "import_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(counter++);
                auto s = std::make_shared<opendm::DownloadSession>(id, item.url, dest_file);
                opendm::SiteLogonManager::instance().apply_to_session(*s, item.url);
                if (!item.referer.empty()) s->set_header("Referer", item.referer);
                if (!item.cookies.empty()) s->set_header("Cookie", item.cookies);
                if (!item.user_agent.empty()) s->set_header("User-Agent", item.user_agent);
                opendm::QueueManager::instance().get_main_queue().add_session(s, opts.start_queue);
            }
        }
    }

    // Start background IPC server for incoming downloads
    opendm::IpcServer ipc_server;
    ipc_server.set_gui_mode(opts.show_gui);
    ipc_server.start();

    // Signal handlers for graceful exit
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    int exit_code = 0;

    if (!opts.show_gui) {
        // Headless / CLI / Daemon mode
        std::cout << "OpenDM running in CLI/background mode." << std::endl;

        if (!opts.url.empty()) {
            std::string download_url = opts.url;
            std::string audio_stream_url;
            std::string user_agent;
            std::string default_fname = "download.bin";

            if (opendm::YouTubeEngine::is_youtube_url(opts.url)) {
                std::cout << "Detected YouTube video URL. Analyzing streams via OpenDM Native Engine..." << std::endl;
                opendm::YouTubeVideoInfo yt_info;
                std::string yt_err;
                bool ok = opendm::YouTubeEngine::extract_video_info(opts.url, yt_info, &yt_err);
                if (!ok || !yt_info.has_playable_formats()) {
                    std::cerr << "OpenDM YouTube Engine extraction failed: " << (yt_err.empty() ? "No playable formats found" : yt_err) << std::endl;
                    curl_global_cleanup();
                    return 1;
                }

                std::cout << "  Title: " << yt_info.title << std::endl;
                std::cout << "  Author: " << yt_info.author << std::endl;
                std::cout << "  Duration: " << opendm::YouTubeEngine::format_duration(yt_info.duration_seconds) << std::endl;
                std::cout << "  Engine Client: " << yt_info.client_used << std::endl;

                if (!yt_info.video_formats.empty() && !yt_info.audio_formats.empty()) {
                    const auto& best_v = yt_info.best_video;
                    const auto& best_a = yt_info.best_audio;
                    download_url = best_v.url;
                    audio_stream_url = best_a.url;
                    user_agent = best_v.user_agent.empty() ? best_a.user_agent : best_v.user_agent;
                    default_fname = yt_info.title + "." + (best_v.container == "MP4" ? "mp4" : "mkv");
                    std::cout << "  Selected: " << best_v.quality_label << " (" << best_v.container << ") + Audio (" << opendm::YouTubeEngine::format_bitrate(best_a.bitrate) << ") [Auto-Muxing]" << std::endl;
                } else if (!yt_info.progressive_formats.empty()) {
                    const auto& prog = yt_info.progressive_formats.front();
                    download_url = prog.url;
                    user_agent = prog.user_agent;
                    default_fname = yt_info.title + ".mp4";
                    std::cout << "  Selected: " << prog.quality_label << " (" << prog.container << " - Progressive)" << std::endl;
                }

                // Sanitize filename
                std::string clean_fname;
                for (char c : default_fname) {
                    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                        c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) {
                        clean_fname += '_';
                    } else {
                        clean_fname += c;
                    }
                }
                default_fname = clean_fname;
            }

            std::string dest_dir = opts.path.empty() ? "." : opts.path;
            std::string dest_file = opts.filename.empty() ? (dest_dir + "/" + default_fname) : (dest_dir + "/" + opts.filename);
            
            auto session = std::make_shared<opendm::DownloadSession>("cli_task", download_url, dest_file);
            if (!audio_stream_url.empty()) {
                session->set_audio_stream_url(audio_stream_url);
            }
            if (!user_agent.empty()) {
                session->set_header("User-Agent", user_agent);
            }
            session->set_header("Referer", "https://www.youtube.com/");
            opendm::SiteLogonManager::instance().apply_to_session(*session, download_url);
            session->set_max_connections(opts.connections);
            if (opts.speed_limit_kb > 0) {
                session->set_speed_limit(opts.speed_limit_kb * 1024);
                session->enable_speed_limit(true);
            }

            opendm::QueueManager::instance().get_main_queue().add_session(session, !opts.add_only);

            if (!opts.add_only) {
                // Monitor progress in terminal
                while (!g_shutdown_requested &&
                       session->get_status() != opendm::SessionStatus::COMPLETED &&
                       session->get_status() != opendm::SessionStatus::ERROR) {
                    auto p = session->get_progress();
                    std::cout << "\rProgress: " << static_cast<int>(p.progress_fraction * 100)
                              << "% [" << (p.downloaded_bytes / 1024) << " KB / " << (p.total_size / 1024) << " KB] "
                              << "Speed: " << (p.speed_bytes_per_sec / 1024) << " KB/s "
                              << "Active conns: " << p.active_connections << "    " << std::flush;
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                std::cout << std::endl;
                if (session->get_status() == opendm::SessionStatus::COMPLETED) {
                    std::cout << "Download completed successfully: " << dest_file << std::endl;
                } else if (session->get_status() == opendm::SessionStatus::ERROR) {
                    std::cerr << "Download failed: " << session->get_progress().error_message << std::endl;
                    exit_code = 1;
                }
            }
        } else if (opts.is_daemon) {
            std::cout << "OpenDM daemon active on " << opendm::IpcServer::DEFAULT_SOCKET_PATH << std::endl;
            while (!g_shutdown_requested) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    } else {
        // GTK4 GUI Mode
        GtkApplication* app = nullptr;
        if (!opts.screenshot_path.empty()) {
            app = gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE);
        } else {
            app = gtk_application_new("com.opendm.app", G_APPLICATION_DEFAULT_FLAGS);
        }

        // Register quit action on app for Tasks -> Exit menubar item
        GSimpleAction* act_quit = g_simple_action_new("quit", nullptr);
        g_signal_connect(act_quit, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer a) {
            g_application_quit(G_APPLICATION(a));
        }), app);
        g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(act_quit));

        struct AppContext {
            opendm::CliOptions opts;
            std::unique_ptr<opendm::MainWindow> main_win;
            opendm::IpcServer* ipc{nullptr};
        } ctx{opts, nullptr, &ipc_server};

        g_signal_connect(app, "activate", (GCallback)(+[](GtkApplication* a, gpointer user_data) {
            auto* context = static_cast<AppContext*>(user_data);
            context->main_win = std::make_unique<opendm::MainWindow>(a);
            context->main_win->present();

            if (context->ipc) {
                context->ipc->set_download_prompt_callback([win = context->main_win.get()](const opendm::DownloadPromptInfo& info) {
                    win->prompt_download(info);
                });
            }

            if (context->opts.start_queue) {
                opendm::QueueManager::instance().get_main_queue().start_queue();
            }

            if (context->opts.speed_limit_kb > 0) {
                opendm::QueueManager::instance().enable_global_speed_limit(true);
                opendm::QueueManager::instance().set_global_speed_limit(context->opts.speed_limit_kb * 1024);
            }

            if (context->opts.exit_after_download) {
                g_timeout_add(500, +[](gpointer) -> gboolean {
                    auto sessions = opendm::QueueManager::instance().get_main_queue().get_all_sessions();
                    if (sessions.empty()) return G_SOURCE_CONTINUE;
                    bool all_done = true;
                    for (const auto& s : sessions) {
                        auto st = s->get_status();
                        if (st != opendm::SessionStatus::COMPLETED && st != opendm::SessionStatus::ERROR) {
                            all_done = false;
                            break;
                        }
                    }
                    if (all_done) {
                        std::cout << "All downloads completed, exiting due to -q / --quit-after flag." << std::endl;
                        g_application_quit(G_APPLICATION(g_application_get_default()));
                        return G_SOURCE_REMOVE;
                    }
                    return G_SOURCE_CONTINUE;
                }, nullptr);
            }

            if (context->opts.url == "demo_progress" && !context->opts.screenshot_path.empty()) {
                gtk_window_present(GTK_WINDOW(context->main_win->get_widget()));
                std::string json_state = R"({
                    "id": "demo_prog",
                    "url": "https://www.internetdownloadmanager.com/video2/idm.ogv",
                    "destination_path": "/home/nader/Downloads/Video/idm.ogv",
                    "filename": "idm.ogv",
                    "category": "Video",
                    "total_size": 18607144960,
                    "supports_ranges": true,
                    "status": 2,
                    "chunk_manager": {
                        "total_file_size": 18607144960,
                        "is_streaming": false,
                        "next_chunk_id": 6,
                        "chunks": [
                            {"id": 0, "start_offset": 0, "end_offset": 3101190826, "downloaded_bytes": 671744000, "status": 1, "temp_filepath": ""},
                            {"id": 1, "start_offset": 3101190827, "end_offset": 6202381653, "downloaded_bytes": 808895488, "status": 1, "temp_filepath": ""},
                            {"id": 2, "start_offset": 6202381654, "end_offset": 9303572480, "downloaded_bytes": 656000000, "status": 1, "temp_filepath": ""},
                            {"id": 3, "start_offset": 9303572481, "end_offset": 12404763307, "downloaded_bytes": 592000000, "status": 1, "temp_filepath": ""},
                            {"id": 4, "start_offset": 12404763308, "end_offset": 15505954134, "downloaded_bytes": 464000000, "status": 1, "temp_filepath": ""},
                            {"id": 5, "start_offset": 15505954135, "end_offset": 18607144959, "downloaded_bytes": 439999488, "status": 1, "temp_filepath": ""}
                        ]
                    }
                })";
                auto mock_sess = opendm::DownloadSession::deserialize(json_state);
                if (!mock_sess) {
                    mock_sess = std::make_shared<opendm::DownloadSession>("demo_prog", "https://www.internetdownloadmanager.com/video2/idm.ogv", "/tmp/idm.ogv");
                }
                auto* dlg = new opendm::ProgressDialog(mock_sess, GTK_WINDOW(context->main_win->get_widget()));
                dlg->show();

                struct ProgSnapData {
                    opendm::ProgressDialog* dlg;
                    std::string path;
                    int ticks;
                };
                auto* psd = new ProgSnapData{dlg, context->opts.screenshot_path, 0};
                gtk_widget_add_tick_callback(dlg->get_widget(), +[](GtkWidget*, GdkFrameClock*, gpointer d) -> gboolean {
                    auto* data = static_cast<ProgSnapData*>(d);
                    data->ticks++;
                    if (data->ticks >= 15) {
                        data->dlg->save_snapshot(data->path);
                        g_application_quit(G_APPLICATION(g_application_get_default()));
                        delete data;
                        return G_SOURCE_REMOVE;
                    }
                    return G_SOURCE_CONTINUE;
                }, psd, nullptr);
                return;
            }

            if (!context->opts.url.empty() && !context->opts.screenshot_path.empty()) {
                gtk_window_present(GTK_WINDOW(context->main_win->get_widget()));
                opendm::DownloadPromptInfo prompt_info;
                prompt_info.url = context->opts.url;
                prompt_info.filename = context->opts.filename.empty() ? "idm_al.zip" : context->opts.filename;
                prompt_info.category = opendm::QueueManager::detect_category(prompt_info.filename);
                prompt_info.file_size = 38656; // 37.75 KB

                auto* dlg = opendm::DownloadInfoDialog::show(GTK_WINDOW(context->main_win->get_widget()), prompt_info, nullptr);
                struct InfoSnapData {
                    opendm::DownloadInfoDialog* dlg;
                    std::string path;
                    int ticks;
                };
                auto* isd = new InfoSnapData{dlg, context->opts.screenshot_path, 0};
                gtk_widget_add_tick_callback(dlg->get_widget(), +[](GtkWidget*, GdkFrameClock*, gpointer d) -> gboolean {
                    auto* data = static_cast<InfoSnapData*>(d);
                    data->ticks++;
                    if (data->ticks >= 10) {
                        data->dlg->save_snapshot(data->path);
                        g_application_quit(G_APPLICATION(g_application_get_default()));
                        delete data;
                        return G_SOURCE_REMOVE;
                    }
                    return G_SOURCE_CONTINUE;
                }, isd, nullptr);
                return;
            }

            if (!context->opts.url.empty()) {
                context->main_win->add_download_url(
                    context->opts.url, context->opts.path, context->opts.filename, !context->opts.add_only);
            }

            if (context->opts.show_about || context->opts.show_appreciation) {
                gtk_window_present(GTK_WINDOW(context->main_win->get_widget()));
                auto* dlg = opendm::AboutDialog::show(GTK_WINDOW(context->main_win->get_widget()));
                if (!context->opts.screenshot_path.empty()) {
                    struct AboutSnapData {
                        opendm::AboutDialog* dlg;
                        std::string path;
                        int ticks;
                    };
                    auto* asd = new AboutSnapData{dlg, context->opts.screenshot_path, 0};
                    gtk_widget_add_tick_callback(dlg->get_widget(), +[](GtkWidget*, GdkFrameClock*, gpointer d) -> gboolean {
                        auto* data = static_cast<AboutSnapData*>(d);
                        data->ticks++;
                        if (data->ticks >= 10) {
                            data->dlg->save_snapshot(data->path);
                            g_application_quit(G_APPLICATION(g_application_get_default()));
                            delete data;
                            return G_SOURCE_REMOVE;
                        }
                        return G_SOURCE_CONTINUE;
                    }, asd, nullptr);
                    return;
                }
            }

            if (!context->opts.screenshot_path.empty()) {
                struct SnapData {
                    AppContext* ctx;
                    int ticks;
                };
                auto* snap_data = new SnapData{context, 0};
                gtk_widget_add_tick_callback(context->main_win->get_widget(), +[](GtkWidget*, GdkFrameClock*, gpointer d) -> gboolean {
                    auto* sd = static_cast<SnapData*>(d);
                    sd->ticks++;
                    if (sd->ticks >= 10) {
                        sd->ctx->main_win->save_snapshot(sd->ctx->opts.screenshot_path);
                        g_application_quit(G_APPLICATION(g_application_get_default()));
                        delete sd;
                        return G_SOURCE_REMOVE;
                    }
                    return G_SOURCE_CONTINUE;
                }, snap_data, nullptr);
            }
        }), &ctx);

        // Run GTK main loop (pass no args to gtk so custom cli flags don't conflict)
        char* empty_argv[] = {argv[0], nullptr};
        exit_code = g_application_run(G_APPLICATION(app), 1, empty_argv);
        g_object_unref(app);
    }

    opendm::QueueManager::instance().save_to_file();
    ipc_server.stop();
    curl_global_cleanup();
    return exit_code;
}
