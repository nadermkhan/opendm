# OpenDM Bug Audit & Architectural Remediation Report

### 1. Concurrency, Deadlocks, Race Conditions & Use-After-Free Bugs

#### 1.1 Self-Deadlock & UI Starvation on `ChunkManager::mutex_` During File Assembly

* **Location:** `core/download_session.cpp` (`DownloadSession::assemble_file`, `DownloadSession::get_progress`) & `core/chunk_manager.cpp` (`ChunkManager::join_file`)
* **Mechanism:**
  1. `ChunkManager::join_file()` acquires `std::lock_guard<std::mutex> lock(mutex_)` at entry and holds this non-recursive mutex for the entire multi-file disk stitching loop.
  2. Inside the loop, while still holding `ChunkManager::mutex_`, `join_file()` invokes `progress_cb(...)`.
  3. The lambda in `DownloadSession::assemble_file()` calls `DownloadProgress prog = get_progress()`, which in turn calls `chunk_manager_.get_total_downloaded_bytes()` and `chunk_manager_.get_chunks_snapshot()`.
  4. Both methods attempt to lock `ChunkManager::mutex_` again on the **same thread**, triggering immediate undefined behavior / self-deadlock (`EDEADLK`) whenever `progress_callback_` is registered.
  5. Even without `progress_callback_` set, holding `ChunkManager::mutex_` across all disk I/O in `join_file()` blocks the GTK main thread whenever `MainWindow::refresh_downloads_list()` (every 400ms) or `ProgressDialog::update_ui()` (every 150ms) calls `session->get_progress()`.

#### 1.2 Lifecycle Race in `pause()` -> `start()` / `resume()` (and Broken "Redownload")

* **Location:** `core/download_session.cpp` (`DownloadSession::pause`, `DownloadSession::start`, `DownloadSession::run_session`) & `ui/main_window.cpp` (`MainWindow::on_row_right_clicked`)
* **Mechanism:**
  1. `DownloadSession::pause()` sets `pause_requested_ = true` and `status_ = SessionStatus::PAUSED` without acquiring `lifecycle_mutex_` and without joining `coordinator_thread_` or `worker_threads_`.
  2. If `start()` or `resume()` is called immediately after `pause()` (such as in the context menu's **"Redownload"** handler, which executes `s->pause(); s->start();`, or a rapid Pause/Resume click by the user), `start()` sees `status_ == SessionStatus::PAUSED` and resets `stop_requested_ = false`, `pause_requested_ = false`, and `status_ = SessionStatus::PROBING` **before** calling `coordinator_thread_.join()`.
  3. Because the abort flags are cleared before the old `coordinator_thread_` is joined, the still-running `coordinator_thread_` and its `worker_threads_` no longer see `pause_requested_ == true` or `status_ == PAUSED`. They continue downloading while `coordinator_thread_.join()` blocks the GTK UI thread for the remainder of the transfer.
  4. Additionally, calling **Redownload** on a `COMPLETED` session fails: `pause()` returns early on `COMPLETED` sessions, and `run_session()` sees `existing_chunks` non-empty with `all_chunks_completed() == true`. It immediately calls `assemble_file()`, which fails with `"Failed to assemble file parts."` because `join_file()` already deleted the `.part` files when the download first completed.

#### 1.3 Detached Thread Use-After-Free (UAF) & Destroyed-Widget Access in Dialogs

* **Location:**
  * `ui/download_info_dialog.cpp` (`DownloadInfoDialog::probe_remote_info`, `on_action_clicked`, `setup_ui`)
  * `ui/grabber_dialog.cpp` (`GrabberDialog::on_start_crawl_clicked`, `setup_ui`, `close`)
  * `ui/zip_preview_dialog.cpp` (`ZipPreviewDialog::on_fetch_clicked`, `on_extract_clicked`)
* **Mechanism:**
  1. **`DownloadInfoDialog` UAF & Destroyed Widget Access:**
     * Closing the dialog via the window manager 'X' button triggers `"close-request"`, which runs `delete self;`. If the detached curl thread in `probe_remote_info()` completes afterward, its `g_idle_add` callback dereferences the dangling `rd->self` pointer (`!rd->self->destroyed_`), causing a **heap Use-After-Free**.
     * Conversely, clicking Start, Download Later, or Cancel calls `on_action_clicked()`, which calls `gtk_window_destroy(GTK_WINDOW(dialog_))` and sets `dialog_ = nullptr`—neither deleting `this` (memory leak) nor setting `destroyed_ = true`. When the `probe_remote_info()` idle callback fires, `rd->self->destroyed_` is still `false`, so it calls `gtk_label_set_text` and `gtk_drop_down_set_selected` on already-destroyed GTK widgets.
  2. **`GrabberDialog` Unstopped Background Thread & Pending Idle Callbacks:**
     * `GrabberDialog::window_` has no `"close-request"` or `"destroy"` signal handler connected. If closed via the titlebar 'X' button while crawling, `grabber_.stop()` is never called, and the background thread continues posting `g_idle_add` callbacks that append rows to the destroyed `results_container_` and update the destroyed `status_label_`.
     * Even when closed via the "Close" button (`GrabberDialog::close()`), `grabber_.stop()` only joins the worker thread; any `g_idle_add` callbacks already queued prior to `join()` execute on the next main-loop tick against the widgets just destroyed by `gtk_window_destroy(GTK_WINDOW(window_))`.
  3. **`ProgressDialog` Timer Running After Window Destruction:**
     * `MainWindow` holds `std::shared_ptr<ProgressDialog>` in `active_dialogs_`. When the user closes `ProgressDialog` via its titlebar 'X' button or clicks "Cancel" (`close()` -> `gtk_window_destroy(GTK_WINDOW(window_))`), the `ProgressDialog` C++ object is not destroyed because `active_dialogs_` retains its `shared_ptr`, and `timer_id_` (150ms interval) is not removed.
     * Every 150ms thereafter, `ProgressDialog::on_tick_update` calls `update_ui()` on destroyed GTK widgets (`window_`, `label_status_`, `main_progress_bar_`, `visualizer_area_`). Re-activating the row later calls `gtk_window_present(GTK_WINDOW(window_))` on the dangling `window_` pointer.

#### 1.4 Unsynchronized Data Races on `DownloadSession` Members

* **Location:** `core/download_session.hpp`, `core/download_session.cpp`
* **Mechanism:**
  * `filename_` and `destination_path_` (`std::string`) are mutated by `coordinator_thread_` inside `probe_resource()` when parsing `Content-Disposition` headers while concurrently read without any lock by the GTK thread (`s->get_filename()` every 400ms in `MainWindow` and 150ms in `ProgressDialog`) and the IPC server thread (`action == "status"`).
  * `last_error_` (`std::string`), `total_size_` (`int64_t`), and `supports_ranges_` (`bool`) are non-atomic and written on `coordinator_thread_` while read without synchronization on UI/CLI/IPC threads in `get_progress()`.

#### 1.5 `DownloadQueue` Mutex Deadlock Hazard & Infinite Post-Download Execution Loop

* **Location:** `core/download_queue.cpp` (`move_item_up`, `move_item_down`, `process_pending_items`)
* **Mechanism:**
  * `move_item_up()` and `move_item_down()` invoke `update_callback_()` **while holding `DownloadQueue::mutex_`** (whereas `add_session` and `remove_session` release `mutex_` first). Any callback querying queue state will deadlock.
  * In `process_pending_items()` (called every 500ms by `queue_scheduler_loop()`), when all sessions finish (`all_finished && !snapshot.empty() && status_ == QueueStatus::RUNNING`), `status_` is **never transitioned to `QueueStatus::STOPPED`**. If `scheduler_.enabled` is true, `std::system(cfg.post_download_command.c_str())` and `std::system("shutdown -h now")` will be executed repeatedly every 500ms.

---

### 2. Core Download Engine, Chunking, IPC & Security Bugs

| Component | Location | Bug / Defect Verified in Code |
| :--- | :--- | :--- |
| **Dynamic Chunk Split Accounting** | `core/download_session.cpp:428-436` (`dynamic_split_loop`) | `split_chunk()` initializes the new chunk with `ChunkStatus::PENDING`, and `dynamic_split_loop()` spawns the worker thread **without** setting the chunk status to `ChunkStatus::DOWNLOADING` (unlike `run_session()`). Because `active_workers` only counts `ChunkStatus::DOWNLOADING`, newly spawned threads that haven't yet opened their file/CURL handle are not counted in `active_workers`, allowing the coordinator to over-split and spawn more than `max_connections_` worker threads. |
| **HTTP Error Pages Saved as Valid Chunks** | `core/download_session.cpp:407-411` (`download_chunk_worker`) | `CURLOPT_FAILONERROR` is not set, and the completion check `if (res == CURLE_OK || ...)` does not check `http_code` or verify `reached_boundary`. If a server responds with `HTTP 403/404/429/503` on a chunk request, `curl_easy_perform` returns `CURLE_OK`, writing the HTML error body into the `.partN` file and marking the chunk `COMPLETED`, corrupting the assembled output. |
| **Stale / Zero Chunk Speed Reporting** | `core/download_session.cpp:102-109` (`write_callback`) | Speed is only calculated when `diff_ms >= 250` inside `write_callback`. Chunks that finish in `< 250ms` always report `0 B/s`. Conversely, if a connection stalls or fails (`ChunkStatus::FAILED`), its last non-zero `current_speed` is never decayed or zeroed out. |
| **Per-Session vs. Global Speed Limiter** | `core/speed_limiter.cpp`, `core/download_queue.cpp` | Each `DownloadSession` owns a separate `SpeedLimiter` instance. `QueueManager::set_global_speed_limit(500 * 1024)` sets a 500 KB/s limit on *each* session individually rather than sharing a global limiter across active sessions. Inside a single session with $N$ connections, all $N$ threads share one `window_start_` and reset it independently after sleeping >= 1000 ms, skewing multi-thread bandwidth accounting. |
| **Orphaned `.part` Files on Cancel** | `core/download_session.cpp:200` (`cancel`), `core/chunk_manager.cpp:277` (`join_file`) | `DownloadSession::cancel()` stops threads but never deletes `.part` files or `temp_dir_` (`~/.cache/opendm/chunks/<id>`). Even on completion, `join_file()` removes `.part` files but leaves the empty `<id>` directory behind. |
| **Path Traversal & Shell Command Injection** | `core/download_session.cpp:281-293`, `ui/main_window.cpp:620`, `ui/progress_dialog.cpp:277,283`, `core/media_muxer.cpp:113` | `probe_resource()` extracts `filename` from `Content-Disposition` without stripping path separators (`/` or `..`), allowing a remote server to traverse directories or overwrite arbitrary paths via `(p.parent_path() / filename_)`. In addition, `probe_resource()` unconditionally overwrites any custom filename the user explicitly chose. Double-clicking a completed download or clicking "Open" in `ProgressDialog` passes `destination_path_` directly into `system("xdg-open \"" + path + "\" &")`, enabling shell execution via `$(...)` or backticks in filenames. |
| **`native_msg_host` `execl` Failure & Zombies** | `ipc/native_msg_host.cpp:104-121` (`ensure_opendm_running`) | When `~/.local/bin/opendm` does not exist, `opendm_bin` defaults to `"opendm"`, and the code calls `execl("opendm", "opendm", nullptr)` instead of `execlp`. Because `execl` does not search `$PATH`, auto-starting OpenDM from `/usr/bin` or `/usr/local/bin` always fails with `ENOENT`. Furthermore, the parent process never reaps the child PID (`waitpid` / `SIGCHLD`), leaving zombie processes. |
| **IPC Partial Socket Reads** | `ipc/ipc_server.cpp:117-125` (`server_loop`) | `server_loop()` breaks out of `read()` as soon as `bytes_read < sizeof(buffer) - 1` even if `'\n'` has not been received yet, truncating fragmented messages over `AF_UNIX` stream sockets. |
| **`ZipPreviewer` Underflow on 0-Byte Files** | `core/zip_previewer.cpp:85, 162` | In `extract_entry()`, `uint32_t payload_end = payload_start + entry.compressed_size - 1;` underflows when `entry.compressed_size == 0` (requesting an invalid byte range `start-(start-1)`), and `fetch_entries()` similarly underflows `cd_offset + cd_size - 1` on an empty archive (`cd_size == 0`). |

---

### 3. Broken UI Interactivity & Widget Lifecycle Bugs

#### 3.1 Double-Click Activation (`n_press == 2`) and Right-Click Context Menus Are Broken by the 400ms Full-List Rebuild

* **Location:** `ui/main_window.cpp` (`MainWindow::refresh_downloads_list`, `on_row_right_clicked`)
* **Mechanism:**
  1. **Double-Click Broken:** Inside `refresh_downloads_list()`, the `GtkGestureClick` `"pressed"` callback on each `row_box` handles left-click (`button == 1`):
     On the first click (`n_press == 1`) of a double-click, calling `self->refresh_downloads_list()` immediately removes and destroys `row_box` and its `GtkGestureClick` controller. When the second click lands ~100ms later, it hits a newly created `row_box` and a fresh `GtkGestureClick` instance that has no record of the first click, registering `n_press == 1` again.
  2. **Right-Click Context Menu Destroyed Within 400ms:** `on_row_right_clicked()` attaches a `GtkPopover` directly to `row_box` via `gtk_widget_set_parent(popover, row_box)`. At most 400ms later, `on_tick_refresh()` calls `refresh_downloads_list()`, which destroys all `row_box` widgets in `downloads_container_`, forcefully closing the context menu before the user can click an item.

#### 3.2 Cancel in `ProgressDialog` Restarts Automatically When Queue is Active

* **Location:** `ui/progress_dialog.cpp` (`ProgressDialog::on_cancel_clicked`) & `core/download_queue.cpp` (`process_pending_items`)
* **Mechanism:** Clicking "Cancel" in `ProgressDialog` calls `session_->cancel()`, which resets `status_` to `SessionStatus::IDLE` while leaving the session inside `DownloadQueue`. If `Main download queue` is running, `DownloadQueue::process_pending_items()` sees `st == SessionStatus::IDLE` within 500ms and automatically restarts the download.

#### 3.3 `AddUrlDialog` Ignores "Start download immediately" & Overrides Category Folders

* **Location:** `ui/add_url_dialog.cpp` (`setup_ui`, `on_ok_clicked`) & `ui/main_window.cpp` (`on_add_url_clicked`)
* **Mechanism:**
  * `AddUrlDialog` provides a `"Start download immediately"` checkbox (`res.start_immediately`), but `MainWindow::on_add_url_clicked()` ignores `res.start_immediately` completely and always chains into `prompt_download(info)` (`DownloadInfoDialog`).
  * Because `AddUrlDialog` pre-fills `entry_path_` with `~/Downloads`, `info.destination_dir` is non-empty when passed to `DownloadInfoDialog`, bypassing `load_category_dir(params_.category)` so category-specific directories (e.g., `~/Downloads/Compressed`) are not applied initially.

---

### 4. Missing Interactivity & Unwired / Stubbed Components

1. **Menu Item `Tasks -> Exit` (`"app.quit"`) Is Unwired:**
   * `MainWindow::setup_menubar()` binds `"Exit"` to `"app.quit"`, but no `"quit"` action is ever registered on `G_ACTION_MAP(app_)` in `main.cpp` or `main_window.cpp`.
2. **Toolbar & Menu `"Scheduler"` and `"Options"` Buttons Are Stubs:**
   * Both the `"Scheduler"` and `"Options"` toolbar buttons (and `Downloads -> Scheduler...` in the menubar) are wired to `MainWindow::on_scheduler_clicked()`, which only prints to stdout.
3. **`ProgressDialog` "Options on completion" Tab Is Completely Unwired:**
   * The four checkboxes in `ProgressDialog::setup_options_tab()` (`chk_show_complete_`, `chk_hang_up_`, `chk_turn_off_`, `chk_force_close_`) have no signal handlers and are never read when a download finishes.
4. **`PostDownloadManager::execute_completion_action()` Is Never Called:**
   * Implemented in `core/post_download.cpp` (`EXIT_APP`, `SUSPEND_SYSTEM`, `SHUTDOWN_SYSTEM`), but never invoked by `DownloadSession`, `DownloadQueue`, or `ProgressDialog`.
5. **`SiteLogonManager` (`core/site_logon.cpp`) Is Never Wired to Sessions or UI:**
   * `SiteLogonManager::apply_to_session()` and `set_credential()` are never called anywhere in the application.
6. **Session & Chunk State Persistence (`serialize` / `deserialize`) Is Never Called:**
   * `DownloadSession::serialize()` / `deserialize()` and `ChunkManager::serialize_state()` / `deserialize_state()` are implemented in `core/`, but neither `main.cpp` nor `QueueManager` saves state on exit or loads state on startup.
7. **`DashParser` (`core/dash_parser.cpp`) and `BatchImporter::export_ef2()` (`core/batch_importer.cpp`) Are Unused:**
   * `DashParser` is never referenced outside `dash_parser.cpp`. `BatchImporter::export_ef2()` has no corresponding UI menu action or CLI switch.
8. **Sidebar Categories & Custom Categories Are Not Persisted on `DownloadSession`:**
   * Selecting `"Grabber projects"` in the sidebar filters by `QueueManager::detect_category(filename) == "Grabber projects"`, which always evaluates to `false`. `DownloadSession` does not store the category chosen in `DownloadInfoDialog`.
9. **Unwired CLI Flags (`main.cpp` & `cli/cli_parser.cpp`):**
   * `-q` / `--quit-after` is parsed in `CliParser` but never checked anywhere in `main.cpp`.
   * `-c` and `-l` are only applied in headless CLI mode and ignored in GUI mode.
   * `--about` only opens `AboutDialog` when `--screenshot` is also non-empty.
