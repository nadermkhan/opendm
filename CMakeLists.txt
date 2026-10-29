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
    src/core/youtube_engine.cpp
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
    src/ui/download_info_dialog.cpp
    src/ui/options_dialog.cpp
    src/ui/about_dialog.cpp
    src/ui/progress_dialog.cpp
    src/ui/download_complete_dialog.cpp
    src/ui/properties_dialog.cpp
    src/ui/zip_preview_dialog.cpp
    src/ui/youtube_format_dialog.cpp
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

add_executable(test_session_pause
    tests/test_session_pause.cpp
)
target_link_libraries(test_session_pause PRIVATE
    opendm_core
)
add_test(NAME SessionPauseTest COMMAND test_session_pause)

add_executable(test_youtube_engine
    tests/test_youtube_engine.cpp
)
target_link_libraries(test_youtube_engine PRIVATE
    opendm_core
)
add_test(NAME YouTubeEngineTest COMMAND test_youtube_engine)

# Ensure assertions remain enabled in test suites across all build types
foreach(test_tgt test_chunk_manager test_zip_previewer test_media_muxer test_integration test_dash_parser test_session_pause test_youtube_engine)
    target_compile_options(${test_tgt} PRIVATE -UNDEBUG)
endforeach()

# Installation targets
include(GNUInstallDirs)
install(TARGETS opendm opendm-native-host
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY data/icons/hicolor/ DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor)
install(FILES data/opendm.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/io.github.opendm.OpenDM.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(DIRECTORY data/ui_assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/opendm/ui_assets)
install(FILES src/ui/style.css DESTINATION ${CMAKE_INSTALL_DATADIR}/opendm)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/opendm_extension.xpi")
    install(FILES opendm_extension.xpi DESTINATION ${CMAKE_INSTALL_DATADIR}/opendm)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extension_extracted")
    install(DIRECTORY extension_extracted/ DESTINATION ${CMAKE_INSTALL_DATADIR}/opendm/extension)
endif()

# Browser Native Messaging Manifests
install(FILES data/native-messaging/com.tonec.idm.firefox.json
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/mozilla/native-messaging-hosts
    RENAME com.tonec.idm.json
)

# CPack Debian Package Configuration
set(CPACK_PACKAGE_NAME "opendm")
set(CPACK_PACKAGE_VENDOR "OpenDM Development Team")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "High-speed dynamic multipart download manager (IDM Architecture)")
set(CPACK_PACKAGE_VERSION_MAJOR 1)
set(CPACK_PACKAGE_VERSION_MINOR 0)
set(CPACK_PACKAGE_VERSION_PATCH 0)
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_CONTACT "OpenDM Development Team <contact@opendm.org>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/nadermkhan/opendm")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_NAME "opendm")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "OpenDM Development Team <contact@opendm.org>")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/nadermkhan/opendm")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "ffmpeg")

include(CPack)



