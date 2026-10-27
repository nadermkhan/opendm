cmake_minimum_required(VERSION 3.20)
project(OpenDM VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CURL_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
)

# Core engine library
add_library(opendm_core STATIC
    src/core/chunk_manager.cpp
    src/core/speed_limiter.cpp
    src/core/download_session.cpp
    src/core/download_queue.cpp
    src/core/zip_previewer.cpp
    src/core/site_grabber.cpp
    src/core/site_logon.cpp
    src/core/batch_importer.cpp
    src/core/media_muxer.cpp
    src/core/dash_parser.cpp
    src/core/post_download.cpp
    src/cli/cli_parser.cpp
    src/ipc/ipc_server.cpp
)

target_link_libraries(opendm_core PUBLIC
    CURL::libcurl
    ZLIB::ZLIB
    Threads::Threads
)

# Main executable (GTK4 + CLI)
add_executable(opendm
    src/ui/ui_assets.cpp
    src/ui/add_url_dialog.cpp
    src/ui/appreciation_dialog.cpp
    src/ui/progress_dialog.cpp
    src/ui/zip_preview_dialog.cpp
    src/ui/grabber_dialog.cpp
    src/ui/main_window.cpp
    src/main.cpp
)

target_link_libraries(opendm PRIVATE
    opendm_core
    PkgConfig::GTK4
)

# Native Messaging Host for Chrome / Firefox WebExtension
add_executable(opendm-native-host
    src/ipc/native_msg_host.cpp
)

target_link_libraries(opendm-native-host PRIVATE
    opendm_core
)

# Test suite
enable_testing()

add_executable(test_chunk_manager
    tests/test_chunk_manager.cpp
)
target_link_libraries(test_chunk_manager PRIVATE
    opendm_core
)
add_test(NAME ChunkManagerTest COMMAND test_chunk_manager)

add_executable(test_zip_previewer
    tests/test_zip_previewer.cpp
)
target_link_libraries(test_zip_previewer PRIVATE
    opendm_core
)
add_test(NAME ZipPreviewerTest COMMAND test_zip_previewer)

add_executable(test_media_muxer
    tests/test_media_muxer.cpp
)
target_link_libraries(test_media_muxer PRIVATE
    opendm_core
)
add_test(NAME MediaMuxerTest COMMAND test_media_muxer)

add_executable(test_integration
    tests/test_integration.cpp
)
target_link_libraries(test_integration PRIVATE
    opendm_core
)
add_test(NAME IntegrationTest COMMAND test_integration)

add_executable(test_dash_parser
    tests/test_dash_parser.cpp
)
target_link_libraries(test_dash_parser PRIVATE
    opendm_core
)
add_test(NAME DashParserTest COMMAND test_dash_parser)

# Installation targets
install(TARGETS opendm opendm-native-host
    RUNTIME DESTINATION bin
)
install(DIRECTORY data/icons/ DESTINATION share/icons/hicolor/scalable/apps)
install(FILES data/opendm.desktop DESTINATION share/applications)
install(DIRECTORY data/ui_assets/ DESTINATION share/opendm/ui_assets)
install(FILES src/ui/style.css DESTINATION share/opendm)



